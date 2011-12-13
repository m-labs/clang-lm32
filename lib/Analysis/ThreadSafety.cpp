//===- ThreadSafety.cpp ----------------------------------------*- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// A intra-procedural analysis for thread safety (e.g. deadlocks and race
// conditions), based off of an annotation system.
//
// See http://clang.llvm.org/docs/LanguageExtensions.html#threadsafety for more
// information.
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/ThreadSafety.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/AnalysisContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/CFGStmtMap.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <algorithm>
#include <vector>

using namespace clang;
using namespace thread_safety;

// Key method definition
ThreadSafetyHandler::~ThreadSafetyHandler() {}

namespace {

/// \brief A MutexID object uniquely identifies a particular mutex, and
/// is built from an Expr* (i.e. calling a lock function).
///
/// Thread-safety analysis works by comparing lock expressions.  Within the
/// body of a function, an expression such as "x->foo->bar.mu" will resolve to
/// a particular mutex object at run-time.  Subsequent occurrences of the same
/// expression (where "same" means syntactic equality) will refer to the same
/// run-time object if three conditions hold:
/// (1) Local variables in the expression, such as "x" have not changed.
/// (2) Values on the heap that affect the expression have not changed.
/// (3) The expression involves only pure function calls.
///
/// The current implementation assumes, but does not verify, that multiple uses
/// of the same lock expression satisfies these criteria.
///
/// Clang introduces an additional wrinkle, which is that it is difficult to
/// derive canonical expressions, or compare expressions directly for equality.
/// Thus, we identify a mutex not by an Expr, but by the set of named
/// declarations that are referenced by the Expr.  In other words,
/// x->foo->bar.mu will be a four element vector with the Decls for
/// mu, bar, and foo, and x.  The vector will uniquely identify the expression
/// for all practical purposes.
///
/// Note we will need to perform substitution on "this" and function parameter
/// names when constructing a lock expression.
///
/// For example:
/// class C { Mutex Mu;  void lock() EXCLUSIVE_LOCK_FUNCTION(this->Mu); };
/// void myFunc(C *X) { ... X->lock() ... }
/// The original expression for the mutex acquired by myFunc is "this->Mu", but
/// "X" is substituted for "this" so we get X->Mu();
///
/// For another example:
/// foo(MyList *L) EXCLUSIVE_LOCKS_REQUIRED(L->Mu) { ... }
/// MyList *MyL;
/// foo(MyL);  // requires lock MyL->Mu to be held
class MutexID {
  SmallVector<NamedDecl*, 2> DeclSeq;

  /// Build a Decl sequence representing the lock from the given expression.
  /// Recursive function that terminates on DeclRefExpr.
  /// Note: this function merely creates a MutexID; it does not check to
  /// ensure that the original expression is a valid mutex expression.
  void buildMutexID(Expr *Exp, Expr *Parent, int NumArgs,
                    const NamedDecl **FunArgDecls, Expr **FunArgs) {
    if (!Exp) {
      DeclSeq.clear();
      return;
    }

    if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Exp)) {
      if (FunArgDecls) {
        // Substitute call arguments for references to function parameters
        for (int i = 0; i < NumArgs; ++i) {
          if (DRE->getDecl() == FunArgDecls[i]) {
            buildMutexID(FunArgs[i], 0, 0, 0, 0);
            return;
          }
        }
      }
      NamedDecl *ND = cast<NamedDecl>(DRE->getDecl()->getCanonicalDecl());
      DeclSeq.push_back(ND);
    } else if (MemberExpr *ME = dyn_cast<MemberExpr>(Exp)) {
      NamedDecl *ND = ME->getMemberDecl();
      DeclSeq.push_back(ND);
      buildMutexID(ME->getBase(), Parent, NumArgs, FunArgDecls, FunArgs);
    } else if (isa<CXXThisExpr>(Exp)) {
      if (Parent)
        buildMutexID(Parent, 0, 0, 0, 0);
      else
        return;  // mutexID is still valid in this case
    } else if (UnaryOperator *UOE = dyn_cast<UnaryOperator>(Exp))
      buildMutexID(UOE->getSubExpr(), Parent, NumArgs, FunArgDecls, FunArgs);
    else if (CastExpr *CE = dyn_cast<CastExpr>(Exp))
      buildMutexID(CE->getSubExpr(), Parent, NumArgs, FunArgDecls, FunArgs);
    else
      DeclSeq.clear(); // Mark as invalid lock expression.
  }

  /// \brief Construct a MutexID from an expression.
  /// \param MutexExp The original mutex expression within an attribute
  /// \param DeclExp An expression involving the Decl on which the attribute
  ///        occurs.
  /// \param D  The declaration to which the lock/unlock attribute is attached.
  void buildMutexIDFromExp(Expr *MutexExp, Expr *DeclExp, const NamedDecl *D) {
    Expr *Parent = 0;
    unsigned NumArgs = 0;
    Expr **FunArgs = 0;
    SmallVector<const NamedDecl*, 8> FunArgDecls;

    // If we are processing a raw attribute expression, with no substitutions.
    if (DeclExp == 0) {
      buildMutexID(MutexExp, 0, 0, 0, 0);
      return;
    }

    // Examine DeclExp to find Parent and FunArgs, which are used to substitute
    // for formal parameters when we call buildMutexID later.
    if (MemberExpr *ME = dyn_cast<MemberExpr>(DeclExp)) {
      Parent = ME->getBase();
    } else if (CXXMemberCallExpr *CE = dyn_cast<CXXMemberCallExpr>(DeclExp)) {
      Parent = CE->getImplicitObjectArgument();
      NumArgs = CE->getNumArgs();
      FunArgs = CE->getArgs();
    } else if (CXXConstructExpr *CE = dyn_cast<CXXConstructExpr>(DeclExp)) {
      Parent = 0;  // FIXME -- get the parent from DeclStmt
      NumArgs = CE->getNumArgs();
      FunArgs = CE->getArgs();
    } else if (D && isa<CXXDestructorDecl>(D)) {
      // There's no such thing as a "destructor call" in the AST.
      Parent = DeclExp;
    }

    // If the attribute has no arguments, then assume the argument is "this".
    if (MutexExp == 0) {
      buildMutexID(Parent, 0, 0, 0, 0);
      return;
    }

    // FIXME: handle default arguments
    if (const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(D)) {
      for (unsigned i = 0, ni = FD->getNumParams(); i < ni && i < NumArgs; ++i) {
        FunArgDecls.push_back(FD->getParamDecl(i));
      }
    }
    buildMutexID(MutexExp, Parent, NumArgs, &FunArgDecls.front(), FunArgs);
  }

public:
  explicit MutexID(clang::Decl::EmptyShell e) {
    DeclSeq.clear();
  }

  /// \param MutexExp The original mutex expression within an attribute
  /// \param DeclExp An expression involving the Decl on which the attribute
  ///        occurs.
  /// \param D  The declaration to which the lock/unlock attribute is attached.
  /// Caller must check isValid() after construction.
  MutexID(Expr* MutexExp, Expr *DeclExp, const NamedDecl* D) {
    buildMutexIDFromExp(MutexExp, DeclExp, D);
  }

  /// Return true if this is a valid decl sequence.
  /// Caller must call this by hand after construction to handle errors.
  bool isValid() const {
    return !DeclSeq.empty();
  }

  /// Issue a warning about an invalid lock expression
  static void warnInvalidLock(ThreadSafetyHandler &Handler, Expr* MutexExp,
                              Expr *DeclExp, const NamedDecl* D) {
    SourceLocation Loc;
    if (DeclExp)
      Loc = DeclExp->getExprLoc();

    // FIXME: add a note about the attribute location in MutexExp or D
    if (Loc.isValid())
      Handler.handleInvalidLockExp(Loc);
  }

  bool operator==(const MutexID &other) const {
    return DeclSeq == other.DeclSeq;
  }

  bool operator!=(const MutexID &other) const {
    return !(*this == other);
  }

  // SmallVector overloads Operator< to do lexicographic ordering. Note that
  // we use pointer equality (and <) to compare NamedDecls. This means the order
  // of MutexIDs in a lockset is nondeterministic. In order to output
  // diagnostics in a deterministic ordering, we must order all diagnostics to
  // output by SourceLocation when iterating through this lockset.
  bool operator<(const MutexID &other) const {
    return DeclSeq < other.DeclSeq;
  }

  /// \brief Returns the name of the first Decl in the list for a given MutexID;
  /// e.g. the lock expression foo.bar() has name "bar".
  /// The caret will point unambiguously to the lock expression, so using this
  /// name in diagnostics is a way to get simple, and consistent, mutex names.
  /// We do not want to output the entire expression text for security reasons.
  StringRef getName() const {
    assert(isValid());
    return DeclSeq.front()->getName();
  }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    for (SmallVectorImpl<NamedDecl*>::const_iterator I = DeclSeq.begin(),
         E = DeclSeq.end(); I != E; ++I) {
      ID.AddPointer(*I);
    }
  }
};


/// \brief This is a helper class that stores info about the most recent
/// accquire of a Lock.
///
/// The main body of the analysis maps MutexIDs to LockDatas.
struct LockData {
  SourceLocation AcquireLoc;

  /// \brief LKind stores whether a lock is held shared or exclusively.
  /// Note that this analysis does not currently support either re-entrant
  /// locking or lock "upgrading" and "downgrading" between exclusive and
  /// shared.
  ///
  /// FIXME: add support for re-entrant locking and lock up/downgrading
  LockKind LKind;
  MutexID UnderlyingMutex;  // for ScopedLockable objects

  LockData(SourceLocation AcquireLoc, LockKind LKind)
    : AcquireLoc(AcquireLoc), LKind(LKind), UnderlyingMutex(Decl::EmptyShell())
  {}

  LockData(SourceLocation AcquireLoc, LockKind LKind, const MutexID &Mu)
    : AcquireLoc(AcquireLoc), LKind(LKind), UnderlyingMutex(Mu) {}

  bool operator==(const LockData &other) const {
    return AcquireLoc == other.AcquireLoc && LKind == other.LKind;
  }

  bool operator!=(const LockData &other) const {
    return !(*this == other);
  }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(AcquireLoc.getRawEncoding());
    ID.AddInteger(LKind);
  }
};


/// A Lockset maps each MutexID (defined above) to information about how it has
/// been locked.
typedef llvm::ImmutableMap<MutexID, LockData> Lockset;

/// \brief We use this class to visit different types of expressions in
/// CFGBlocks, and build up the lockset.
/// An expression may cause us to add or remove locks from the lockset, or else
/// output error messages related to missing locks.
/// FIXME: In future, we may be able to not inherit from a visitor.
class BuildLockset : public StmtVisitor<BuildLockset> {
  friend class ThreadSafetyAnalyzer;

  ThreadSafetyHandler &Handler;
  Lockset LSet;
  Lockset::Factory &LocksetFactory;

  // Helper functions
  void addLock(const MutexID &Mutex, const LockData &LDat);
  void removeLock(const MutexID &Mutex, SourceLocation UnlockLoc);

  template <class AttrType>
  void addLocksToSet(LockKind LK, AttrType *Attr,
                     Expr *Exp, NamedDecl *D, VarDecl *VD = 0);
  void removeLocksFromSet(UnlockFunctionAttr *Attr,
                          Expr *Exp, NamedDecl* FunDecl);

  const ValueDecl *getValueDecl(Expr *Exp);
  void warnIfMutexNotHeld (const NamedDecl *D, Expr *Exp, AccessKind AK,
                           Expr *MutexExp, ProtectedOperationKind POK);
  void checkAccess(Expr *Exp, AccessKind AK);
  void checkDereference(Expr *Exp, AccessKind AK);
  void handleCall(Expr *Exp, NamedDecl *D, VarDecl *VD = 0);

  /// \brief Returns true if the lockset contains a lock, regardless of whether
  /// the lock is held exclusively or shared.
  bool locksetContains(const MutexID &Lock) const {
    return LSet.lookup(Lock);
  }

  /// \brief Returns true if the lockset contains a lock with the passed in
  /// locktype.
  bool locksetContains(const MutexID &Lock, LockKind KindRequested) const {
    const LockData *LockHeld = LSet.lookup(Lock);
    return (LockHeld && KindRequested == LockHeld->LKind);
  }

  /// \brief Returns true if the lockset contains a lock with at least the
  /// passed in locktype. So for example, if we pass in LK_Shared, this function
  /// returns true if the lock is held LK_Shared or LK_Exclusive. If we pass in
  /// LK_Exclusive, this function returns true if the lock is held LK_Exclusive.
  bool locksetContainsAtLeast(const MutexID &Lock,
                              LockKind KindRequested) const {
    switch (KindRequested) {
      case LK_Shared:
        return locksetContains(Lock);
      case LK_Exclusive:
        return locksetContains(Lock, KindRequested);
    }
    llvm_unreachable("Unknown LockKind");
  }

public:
  BuildLockset(ThreadSafetyHandler &Handler, Lockset LS, Lockset::Factory &F)
    : StmtVisitor<BuildLockset>(), Handler(Handler), LSet(LS),
      LocksetFactory(F) {}

  Lockset getLockset() {
    return LSet;
  }

  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitCastExpr(CastExpr *CE);
  void VisitCXXMemberCallExpr(CXXMemberCallExpr *Exp);
  void VisitCXXConstructExpr(CXXConstructExpr *Exp);
  void VisitDeclStmt(DeclStmt *S);
};

/// \brief Add a new lock to the lockset, warning if the lock is already there.
/// \param Mutex -- the Mutex expression for the lock
/// \param LDat  -- the LockData for the lock
void BuildLockset::addLock(const MutexID &Mutex, const LockData& LDat) {
  // FIXME: deal with acquired before/after annotations.
  // FIXME: Don't always warn when we have support for reentrant locks.
  if (locksetContains(Mutex))
    Handler.handleDoubleLock(Mutex.getName(), LDat.AcquireLoc);
  else
    LSet = LocksetFactory.add(LSet, Mutex, LDat);
}

/// \brief Remove a lock from the lockset, warning if the lock is not there.
/// \param LockExp The lock expression corresponding to the lock to be removed
/// \param UnlockLoc The source location of the unlock (only used in error msg)
void BuildLockset::removeLock(const MutexID &Mutex, SourceLocation UnlockLoc) {
  const LockData *LDat = LSet.lookup(Mutex);
  if (!LDat)
    Handler.handleUnmatchedUnlock(Mutex.getName(), UnlockLoc);
  else {
    // For scoped-lockable vars, remove the mutex associated with this var.
    if (LDat->UnderlyingMutex.isValid())
      removeLock(LDat->UnderlyingMutex, UnlockLoc);
    LSet = LocksetFactory.remove(LSet, Mutex);
  }
}

/// \brief This function, parameterized by an attribute type, is used to add a
/// set of locks specified as attribute arguments to the lockset.
template <typename AttrType>
void BuildLockset::addLocksToSet(LockKind LK, AttrType *Attr,
                                 Expr *Exp, NamedDecl* FunDecl, VarDecl *VD) {
  typedef typename AttrType::args_iterator iterator_type;

  SourceLocation ExpLocation = Exp->getExprLoc();

  // Figure out if we're calling the constructor of scoped lockable class
  bool isScopedVar = false;
  if (VD) {
    if (CXXConstructorDecl *CD = dyn_cast<CXXConstructorDecl>(FunDecl)) {
      CXXRecordDecl* PD = CD->getParent();
      if (PD && PD->getAttr<ScopedLockableAttr>())
        isScopedVar = true;
    }
  }

  if (Attr->args_size() == 0) {
    // The mutex held is the "this" object.
    MutexID Mutex(0, Exp, FunDecl);
    if (!Mutex.isValid())
      MutexID::warnInvalidLock(Handler, 0, Exp, FunDecl);
    else
      addLock(Mutex, LockData(ExpLocation, LK));
    return;
  }

  for (iterator_type I=Attr->args_begin(), E=Attr->args_end(); I != E; ++I) {
    MutexID Mutex(*I, Exp, FunDecl);
    if (!Mutex.isValid())
      MutexID::warnInvalidLock(Handler, *I, Exp, FunDecl);
    else {
      addLock(Mutex, LockData(ExpLocation, LK));
      if (isScopedVar) {
        // For scoped lockable vars, map this var to its underlying mutex.
        DeclRefExpr DRE(VD, VD->getType(), VK_LValue, VD->getLocation());
        MutexID SMutex(&DRE, 0, 0);
        addLock(SMutex, LockData(VD->getLocation(), LK, Mutex));
      }
    }
  }
}

/// \brief This function removes a set of locks specified as attribute
/// arguments from the lockset.
void BuildLockset::removeLocksFromSet(UnlockFunctionAttr *Attr,
                                      Expr *Exp, NamedDecl* FunDecl) {
  SourceLocation ExpLocation;
  if (Exp) ExpLocation = Exp->getExprLoc();

  if (Attr->args_size() == 0) {
    // The mutex held is the "this" object.
    MutexID Mu(0, Exp, FunDecl);
    if (!Mu.isValid())
      MutexID::warnInvalidLock(Handler, 0, Exp, FunDecl);
    else
      removeLock(Mu, ExpLocation);
    return;
  }

  for (UnlockFunctionAttr::args_iterator I = Attr->args_begin(),
       E = Attr->args_end(); I != E; ++I) {
    MutexID Mutex(*I, Exp, FunDecl);
    if (!Mutex.isValid())
      MutexID::warnInvalidLock(Handler, *I, Exp, FunDecl);
    else
      removeLock(Mutex, ExpLocation);
  }
}

/// \brief Gets the value decl pointer from DeclRefExprs or MemberExprs
const ValueDecl *BuildLockset::getValueDecl(Expr *Exp) {
  if (const DeclRefExpr *DR = dyn_cast<DeclRefExpr>(Exp))
    return DR->getDecl();

  if (const MemberExpr *ME = dyn_cast<MemberExpr>(Exp))
    return ME->getMemberDecl();

  return 0;
}

/// \brief Warn if the LSet does not contain a lock sufficient to protect access
/// of at least the passed in AccessKind.
void BuildLockset::warnIfMutexNotHeld(const NamedDecl *D, Expr *Exp,
                                      AccessKind AK, Expr *MutexExp,
                                      ProtectedOperationKind POK) {
  LockKind LK = getLockKindFromAccessKind(AK);

  MutexID Mutex(MutexExp, Exp, D);
  if (!Mutex.isValid())
    MutexID::warnInvalidLock(Handler, MutexExp, Exp, D);
  else if (!locksetContainsAtLeast(Mutex, LK))
    Handler.handleMutexNotHeld(D, POK, Mutex.getName(), LK, Exp->getExprLoc());
}

/// \brief This method identifies variable dereferences and checks pt_guarded_by
/// and pt_guarded_var annotations. Note that we only check these annotations
/// at the time a pointer is dereferenced.
/// FIXME: We need to check for other types of pointer dereferences
/// (e.g. [], ->) and deal with them here.
/// \param Exp An expression that has been read or written.
void BuildLockset::checkDereference(Expr *Exp, AccessKind AK) {
  UnaryOperator *UO = dyn_cast<UnaryOperator>(Exp);
  if (!UO || UO->getOpcode() != clang::UO_Deref)
    return;
  Exp = UO->getSubExpr()->IgnoreParenCasts();

  const ValueDecl *D = getValueDecl(Exp);
  if(!D || !D->hasAttrs())
    return;

  if (D->getAttr<PtGuardedVarAttr>() && LSet.isEmpty())
    Handler.handleNoMutexHeld(D, POK_VarDereference, AK, Exp->getExprLoc());

  const AttrVec &ArgAttrs = D->getAttrs();
  for(unsigned i = 0, Size = ArgAttrs.size(); i < Size; ++i)
    if (PtGuardedByAttr *PGBAttr = dyn_cast<PtGuardedByAttr>(ArgAttrs[i]))
      warnIfMutexNotHeld(D, Exp, AK, PGBAttr->getArg(), POK_VarDereference);
}

/// \brief Checks guarded_by and guarded_var attributes.
/// Whenever we identify an access (read or write) of a DeclRefExpr or
/// MemberExpr, we need to check whether there are any guarded_by or
/// guarded_var attributes, and make sure we hold the appropriate mutexes.
void BuildLockset::checkAccess(Expr *Exp, AccessKind AK) {
  const ValueDecl *D = getValueDecl(Exp);
  if(!D || !D->hasAttrs())
    return;

  if (D->getAttr<GuardedVarAttr>() && LSet.isEmpty())
    Handler.handleNoMutexHeld(D, POK_VarAccess, AK, Exp->getExprLoc());

  const AttrVec &ArgAttrs = D->getAttrs();
  for(unsigned i = 0, Size = ArgAttrs.size(); i < Size; ++i)
    if (GuardedByAttr *GBAttr = dyn_cast<GuardedByAttr>(ArgAttrs[i]))
      warnIfMutexNotHeld(D, Exp, AK, GBAttr->getArg(), POK_VarAccess);
}

/// \brief Process a function call, method call, constructor call,
/// or destructor call.  This involves looking at the attributes on the
/// corresponding function/method/constructor/destructor, issuing warnings,
/// and updating the locksets accordingly.
///
/// FIXME: For classes annotated with one of the guarded annotations, we need
/// to treat const method calls as reads and non-const method calls as writes,
/// and check that the appropriate locks are held. Non-const method calls with
/// the same signature as const method calls can be also treated as reads.
///
/// FIXME: We need to also visit CallExprs to catch/check global functions.
///
/// FIXME: Do not flag an error for member variables accessed in constructors/
/// destructors
void BuildLockset::handleCall(Expr *Exp, NamedDecl *D, VarDecl *VD) {
  AttrVec &ArgAttrs = D->getAttrs();
  for(unsigned i = 0; i < ArgAttrs.size(); ++i) {
    Attr *Attr = ArgAttrs[i];
    switch (Attr->getKind()) {
      // When we encounter an exclusive lock function, we need to add the lock
      // to our lockset with kind exclusive.
      case attr::ExclusiveLockFunction: {
        ExclusiveLockFunctionAttr *A = cast<ExclusiveLockFunctionAttr>(Attr);
        addLocksToSet(LK_Exclusive, A, Exp, D, VD);
        break;
      }

      // When we encounter a shared lock function, we need to add the lock
      // to our lockset with kind shared.
      case attr::SharedLockFunction: {
        SharedLockFunctionAttr *A = cast<SharedLockFunctionAttr>(Attr);
        addLocksToSet(LK_Shared, A, Exp, D, VD);
        break;
      }

      // When we encounter an unlock function, we need to remove unlocked
      // mutexes from the lockset, and flag a warning if they are not there.
      case attr::UnlockFunction: {
        UnlockFunctionAttr *UFAttr = cast<UnlockFunctionAttr>(Attr);
        removeLocksFromSet(UFAttr, Exp, D);
        break;
      }

      case attr::ExclusiveLocksRequired: {
        ExclusiveLocksRequiredAttr *ELRAttr =
            cast<ExclusiveLocksRequiredAttr>(Attr);

        for (ExclusiveLocksRequiredAttr::args_iterator
             I = ELRAttr->args_begin(), E = ELRAttr->args_end(); I != E; ++I)
          warnIfMutexNotHeld(D, Exp, AK_Written, *I, POK_FunctionCall);
        break;
      }

      case attr::SharedLocksRequired: {
        SharedLocksRequiredAttr *SLRAttr = cast<SharedLocksRequiredAttr>(Attr);

        for (SharedLocksRequiredAttr::args_iterator I = SLRAttr->args_begin(),
             E = SLRAttr->args_end(); I != E; ++I)
          warnIfMutexNotHeld(D, Exp, AK_Read, *I, POK_FunctionCall);
        break;
      }

      case attr::LocksExcluded: {
        LocksExcludedAttr *LEAttr = cast<LocksExcludedAttr>(Attr);
        for (LocksExcludedAttr::args_iterator I = LEAttr->args_begin(),
            E = LEAttr->args_end(); I != E; ++I) {
          MutexID Mutex(*I, Exp, D);
          if (!Mutex.isValid())
            MutexID::warnInvalidLock(Handler, *I, Exp, D);
          else if (locksetContains(Mutex))
            Handler.handleFunExcludesLock(D->getName(), Mutex.getName(),
                                          Exp->getExprLoc());
        }
        break;
      }

      // Ignore other (non thread-safety) attributes
      default:
        break;
    }
  }
}

/// \brief For unary operations which read and write a variable, we need to
/// check whether we hold any required mutexes. Reads are checked in
/// VisitCastExpr.
void BuildLockset::VisitUnaryOperator(UnaryOperator *UO) {
  switch (UO->getOpcode()) {
    case clang::UO_PostDec:
    case clang::UO_PostInc:
    case clang::UO_PreDec:
    case clang::UO_PreInc: {
      Expr *SubExp = UO->getSubExpr()->IgnoreParenCasts();
      checkAccess(SubExp, AK_Written);
      checkDereference(SubExp, AK_Written);
      break;
    }
    default:
      break;
  }
}

/// For binary operations which assign to a variable (writes), we need to check
/// whether we hold any required mutexes.
/// FIXME: Deal with non-primitive types.
void BuildLockset::VisitBinaryOperator(BinaryOperator *BO) {
  if (!BO->isAssignmentOp())
    return;
  Expr *LHSExp = BO->getLHS()->IgnoreParenCasts();
  checkAccess(LHSExp, AK_Written);
  checkDereference(LHSExp, AK_Written);
}

/// Whenever we do an LValue to Rvalue cast, we are reading a variable and
/// need to ensure we hold any required mutexes.
/// FIXME: Deal with non-primitive types.
void BuildLockset::VisitCastExpr(CastExpr *CE) {
  if (CE->getCastKind() != CK_LValueToRValue)
    return;
  Expr *SubExp = CE->getSubExpr()->IgnoreParenCasts();
  checkAccess(SubExp, AK_Read);
  checkDereference(SubExp, AK_Read);
}


void BuildLockset::VisitCXXMemberCallExpr(CXXMemberCallExpr *Exp) {
  NamedDecl *D = dyn_cast_or_null<NamedDecl>(Exp->getCalleeDecl());
  if(!D || !D->hasAttrs())
    return;
  handleCall(Exp, D);
}

void BuildLockset::VisitCXXConstructExpr(CXXConstructExpr *Exp) {
  // FIXME -- only handles constructors in DeclStmt below.
}

void BuildLockset::VisitDeclStmt(DeclStmt *S) {
  DeclGroupRef DGrp = S->getDeclGroup();
  for (DeclGroupRef::iterator I = DGrp.begin(), E = DGrp.end(); I != E; ++I) {
    Decl *D = *I;
    if (VarDecl *VD = dyn_cast_or_null<VarDecl>(D)) {
      Expr *E = VD->getInit();
      if (CXXConstructExpr *CE = dyn_cast_or_null<CXXConstructExpr>(E)) {
        NamedDecl *CtorD = dyn_cast_or_null<NamedDecl>(CE->getConstructor());
        if (!CtorD || !CtorD->hasAttrs())
          return;
        handleCall(CE, CtorD, VD);
      }
    }
  }
}


/// \brief Class which implements the core thread safety analysis routines.
class ThreadSafetyAnalyzer {
  ThreadSafetyHandler &Handler;
  Lockset::Factory    LocksetFactory;

public:
  ThreadSafetyAnalyzer(ThreadSafetyHandler &H) : Handler(H) {}

  Lockset intersectAndWarn(const Lockset LSet1, const Lockset LSet2,
                           LockErrorKind LEK);

  Lockset addLock(Lockset &LSet, Expr *MutexExp, const NamedDecl *D,
                  LockKind LK, SourceLocation Loc);

  void runAnalysis(AnalysisDeclContext &AC);
};

/// \brief Compute the intersection of two locksets and issue warnings for any
/// locks in the symmetric difference.
///
/// This function is used at a merge point in the CFG when comparing the lockset
/// of each branch being merged. For example, given the following sequence:
/// A; if () then B; else C; D; we need to check that the lockset after B and C
/// are the same. In the event of a difference, we use the intersection of these
/// two locksets at the start of D.
Lockset ThreadSafetyAnalyzer::intersectAndWarn(const Lockset LSet1,
                                               const Lockset LSet2,
                                               LockErrorKind LEK) {
  Lockset Intersection = LSet1;
  for (Lockset::iterator I = LSet2.begin(), E = LSet2.end(); I != E; ++I) {
    const MutexID &LSet2Mutex = I.getKey();
    const LockData &LSet2LockData = I.getData();
    if (const LockData *LD = LSet1.lookup(LSet2Mutex)) {
      if (LD->LKind != LSet2LockData.LKind) {
        Handler.handleExclusiveAndShared(LSet2Mutex.getName(),
                                         LSet2LockData.AcquireLoc,
                                         LD->AcquireLoc);
        if (LD->LKind != LK_Exclusive)
          Intersection = LocksetFactory.add(Intersection, LSet2Mutex,
                                            LSet2LockData);
      }
    } else {
      Handler.handleMutexHeldEndOfScope(LSet2Mutex.getName(),
                                        LSet2LockData.AcquireLoc, LEK);
    }
  }

  for (Lockset::iterator I = LSet1.begin(), E = LSet1.end(); I != E; ++I) {
    if (!LSet2.contains(I.getKey())) {
      const MutexID &Mutex = I.getKey();
      const LockData &MissingLock = I.getData();
      Handler.handleMutexHeldEndOfScope(Mutex.getName(),
                                        MissingLock.AcquireLoc, LEK);
      Intersection = LocksetFactory.remove(Intersection, Mutex);
    }
  }
  return Intersection;
}

Lockset ThreadSafetyAnalyzer::addLock(Lockset &LSet, Expr *MutexExp,
                                      const NamedDecl *D,
                                      LockKind LK, SourceLocation Loc) {
  MutexID Mutex(MutexExp, 0, D);
  if (!Mutex.isValid()) {
    MutexID::warnInvalidLock(Handler, MutexExp, 0, D);
    return LSet;
  }
  LockData NewLock(Loc, LK);
  return LocksetFactory.add(LSet, Mutex, NewLock);
}

/// \brief Check a function's CFG for thread-safety violations.
///
/// We traverse the blocks in the CFG, compute the set of mutexes that are held
/// at the end of each block, and issue warnings for thread safety violations.
/// Each block in the CFG is traversed exactly once.
void ThreadSafetyAnalyzer::runAnalysis(AnalysisDeclContext &AC) {
  CFG *CFGraph = AC.getCFG();
  if (!CFGraph) return;
  const NamedDecl *D = dyn_cast_or_null<NamedDecl>(AC.getDecl());

  if (!D)
    return;  // Ignore anonymous functions for now.
  if (D->getAttr<NoThreadSafetyAnalysisAttr>())
    return;

  // FIXME: Switch to SmallVector? Otherwise improve performance impact?
  std::vector<Lockset> EntryLocksets(CFGraph->getNumBlockIDs(),
                                     LocksetFactory.getEmptyMap());
  std::vector<Lockset> ExitLocksets(CFGraph->getNumBlockIDs(),
                                    LocksetFactory.getEmptyMap());

  // We need to explore the CFG via a "topological" ordering.
  // That way, we will be guaranteed to have information about required
  // predecessor locksets when exploring a new block.
  PostOrderCFGView *SortedGraph = AC.getAnalysis<PostOrderCFGView>();
  PostOrderCFGView::CFGBlockSet VisitedBlocks(CFGraph);

  // Add locks from exclusive_locks_required and shared_locks_required
  // to initial lockset.
  if (!SortedGraph->empty() && D->hasAttrs()) {
    const CFGBlock *FirstBlock = *SortedGraph->begin();
    Lockset &InitialLockset = EntryLocksets[FirstBlock->getBlockID()];
    const AttrVec &ArgAttrs = D->getAttrs();
    for(unsigned i = 0; i < ArgAttrs.size(); ++i) {
      Attr *Attr = ArgAttrs[i];
      SourceLocation AttrLoc = Attr->getLocation();
      if (SharedLocksRequiredAttr *SLRAttr
            = dyn_cast<SharedLocksRequiredAttr>(Attr)) {
        for (SharedLocksRequiredAttr::args_iterator
            SLRIter = SLRAttr->args_begin(),
            SLREnd = SLRAttr->args_end(); SLRIter != SLREnd; ++SLRIter)
          InitialLockset = addLock(InitialLockset,
                                   *SLRIter, D, LK_Shared,
                                   AttrLoc);
      } else if (ExclusiveLocksRequiredAttr *ELRAttr
                   = dyn_cast<ExclusiveLocksRequiredAttr>(Attr)) {
        for (ExclusiveLocksRequiredAttr::args_iterator
            ELRIter = ELRAttr->args_begin(),
            ELREnd = ELRAttr->args_end(); ELRIter != ELREnd; ++ELRIter)
          InitialLockset = addLock(InitialLockset,
                                   *ELRIter, D, LK_Exclusive,
                                   AttrLoc);
      }
    }
  }

  for (PostOrderCFGView::iterator I = SortedGraph->begin(),
       E = SortedGraph->end(); I!= E; ++I) {
    const CFGBlock *CurrBlock = *I;
    int CurrBlockID = CurrBlock->getBlockID();

    VisitedBlocks.insert(CurrBlock);

    // Use the default initial lockset in case there are no predecessors.
    Lockset &Entryset = EntryLocksets[CurrBlockID];
    Lockset &Exitset = ExitLocksets[CurrBlockID];

    // Iterate through the predecessor blocks and warn if the lockset for all
    // predecessors is not the same. We take the entry lockset of the current
    // block to be the intersection of all previous locksets.
    // FIXME: By keeping the intersection, we may output more errors in future
    // for a lock which is not in the intersection, but was in the union. We
    // may want to also keep the union in future. As an example, let's say
    // the intersection contains Mutex L, and the union contains L and M.
    // Later we unlock M. At this point, we would output an error because we
    // never locked M; although the real error is probably that we forgot to
    // lock M on all code paths. Conversely, let's say that later we lock M.
    // In this case, we should compare against the intersection instead of the
    // union because the real error is probably that we forgot to unlock M on
    // all code paths.
    bool LocksetInitialized = false;
    for (CFGBlock::const_pred_iterator PI = CurrBlock->pred_begin(),
         PE  = CurrBlock->pred_end(); PI != PE; ++PI) {

      // if *PI -> CurrBlock is a back edge
      if (*PI == 0 || !VisitedBlocks.alreadySet(*PI))
        continue;

      int PrevBlockID = (*PI)->getBlockID();
      if (!LocksetInitialized) {
        Entryset = ExitLocksets[PrevBlockID];
        LocksetInitialized = true;
      } else {
        Entryset = intersectAndWarn(Entryset, ExitLocksets[PrevBlockID],
                                    LEK_LockedSomePredecessors);
      }
    }

    BuildLockset LocksetBuilder(Handler, Entryset, LocksetFactory);
    for (CFGBlock::const_iterator BI = CurrBlock->begin(),
         BE = CurrBlock->end(); BI != BE; ++BI) {
      switch (BI->getKind()) {
        case CFGElement::Statement: {
          const CFGStmt *CS = cast<CFGStmt>(&*BI);
          LocksetBuilder.Visit(const_cast<Stmt*>(CS->getStmt()));
          break;
        }
        // Ignore BaseDtor, MemberDtor, and TemporaryDtor for now.
        case CFGElement::AutomaticObjectDtor: {
          const CFGAutomaticObjDtor *AD = cast<CFGAutomaticObjDtor>(&*BI);
          CXXDestructorDecl *DD = const_cast<CXXDestructorDecl*>(
            AD->getDestructorDecl(AC.getASTContext()));
          if (!DD->hasAttrs())
            break;

          // Create a dummy expression,
          VarDecl *VD = const_cast<VarDecl*>(AD->getVarDecl());
          DeclRefExpr DRE(VD, VD->getType(), VK_LValue,
                          AD->getTriggerStmt()->getLocEnd());
          LocksetBuilder.handleCall(&DRE, DD);
          break;
        }
        default:
          break;
      }
    }
    Exitset = LocksetBuilder.getLockset();

    // For every back edge from CurrBlock (the end of the loop) to another block
    // (FirstLoopBlock) we need to check that the Lockset of Block is equal to
    // the one held at the beginning of FirstLoopBlock. We can look up the
    // Lockset held at the beginning of FirstLoopBlock in the EntryLockSets map.
    for (CFGBlock::const_succ_iterator SI = CurrBlock->succ_begin(),
         SE  = CurrBlock->succ_end(); SI != SE; ++SI) {

      // if CurrBlock -> *SI is *not* a back edge
      if (*SI == 0 || !VisitedBlocks.alreadySet(*SI))
        continue;

      CFGBlock *FirstLoopBlock = *SI;
      Lockset PreLoop = EntryLocksets[FirstLoopBlock->getBlockID()];
      Lockset LoopEnd = ExitLocksets[CurrBlockID];
      intersectAndWarn(LoopEnd, PreLoop, LEK_LockedSomeLoopIterations);
    }
  }

  Lockset InitialLockset = EntryLocksets[CFGraph->getEntry().getBlockID()];
  Lockset FinalLockset = ExitLocksets[CFGraph->getExit().getBlockID()];

  // FIXME: Should we call this function for all blocks which exit the function?
  intersectAndWarn(InitialLockset, FinalLockset, LEK_LockedAtEndOfFunction);
}

} // end anonymous namespace


namespace clang {
namespace thread_safety {

/// \brief Check a function's CFG for thread-safety violations.
///
/// We traverse the blocks in the CFG, compute the set of mutexes that are held
/// at the end of each block, and issue warnings for thread safety violations.
/// Each block in the CFG is traversed exactly once.
void runThreadSafetyAnalysis(AnalysisDeclContext &AC,
                             ThreadSafetyHandler &Handler) {
  ThreadSafetyAnalyzer Analyzer(Handler);
  Analyzer.runAnalysis(AC);
}

/// \brief Helper function that returns a LockKind required for the given level
/// of access.
LockKind getLockKindFromAccessKind(AccessKind AK) {
  switch (AK) {
    case AK_Read :
      return LK_Shared;
    case AK_Written :
      return LK_Exclusive;
  }
  llvm_unreachable("Unknown AccessKind");
}

}} // end namespace clang::thread_safety
