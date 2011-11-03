//===--- ExprConstant.cpp - Expression Constant Evaluator -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Expr constant evaluator.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/SmallString.h"
#include <cstring>

using namespace clang;
using llvm::APSInt;
using llvm::APFloat;

/// EvalInfo - This is a private struct used by the evaluator to capture
/// information about a subexpression as it is folded.  It retains information
/// about the AST context, but also maintains information about the folded
/// expression.
///
/// If an expression could be evaluated, it is still possible it is not a C
/// "integer constant expression" or constant expression.  If not, this struct
/// captures information about how and why not.
///
/// One bit of information passed *into* the request for constant folding
/// indicates whether the subexpression is "evaluated" or not according to C
/// rules.  For example, the RHS of (0 && foo()) is not evaluated.  We can
/// evaluate the expression regardless of what the RHS is, but C only allows
/// certain things in certain situations.
namespace {
  struct CallStackFrame;
  struct EvalInfo;

  /// A core constant value. This can be the value of any constant expression,
  /// or a pointer or reference to a non-static object or function parameter.
  class CCValue : public APValue {
    typedef llvm::APSInt APSInt;
    typedef llvm::APFloat APFloat;
    /// If the value is a reference or pointer into a parameter or temporary,
    /// this is the corresponding call stack frame.
    CallStackFrame *CallFrame;
  public:
    struct GlobalValue {};

    CCValue() {}
    explicit CCValue(const APSInt &I) : APValue(I) {}
    explicit CCValue(const APFloat &F) : APValue(F) {}
    CCValue(const APValue *E, unsigned N) : APValue(E, N) {}
    CCValue(const APSInt &R, const APSInt &I) : APValue(R, I) {}
    CCValue(const APFloat &R, const APFloat &I) : APValue(R, I) {}
    CCValue(const CCValue &V) : APValue(V), CallFrame(V.CallFrame) {}
    CCValue(const Expr *B, const CharUnits &O, CallStackFrame *F) :
      APValue(B, O), CallFrame(F) {}
    CCValue(const APValue &V, GlobalValue) :
      APValue(V), CallFrame(0) {}

    CallStackFrame *getLValueFrame() const {
      assert(getKind() == LValue);
      return CallFrame;
    }
  };

  /// A stack frame in the constexpr call stack.
  struct CallStackFrame {
    EvalInfo &Info;

    /// Parent - The caller of this stack frame.
    CallStackFrame *Caller;

    /// ParmBindings - Parameter bindings for this function call, indexed by
    /// parameters' function scope indices.
    const CCValue *Arguments;

    typedef llvm::DenseMap<const Expr*, CCValue> MapTy;
    typedef MapTy::const_iterator temp_iterator;
    /// Temporaries - Temporary lvalues materialized within this stack frame.
    MapTy Temporaries;

    CallStackFrame(EvalInfo &Info, const CCValue *Arguments);
    ~CallStackFrame();
  };

  struct EvalInfo {
    const ASTContext &Ctx;

    /// EvalStatus - Contains information about the evaluation.
    Expr::EvalStatus &EvalStatus;

    /// CurrentCall - The top of the constexpr call stack.
    CallStackFrame *CurrentCall;

    /// NumCalls - The number of calls we've evaluated so far.
    unsigned NumCalls;

    /// CallStackDepth - The number of calls in the call stack right now.
    unsigned CallStackDepth;

    typedef llvm::DenseMap<const OpaqueValueExpr*, CCValue> MapTy;
    /// OpaqueValues - Values used as the common expression in a
    /// BinaryConditionalOperator.
    MapTy OpaqueValues;

    /// BottomFrame - The frame in which evaluation started. This must be
    /// initialized last.
    CallStackFrame BottomFrame;


    EvalInfo(const ASTContext &C, Expr::EvalStatus &S)
      : Ctx(C), EvalStatus(S), CurrentCall(0), NumCalls(0), CallStackDepth(0),
        BottomFrame(*this, 0) {}

    const CCValue *getOpaqueValue(const OpaqueValueExpr *e) const {
      MapTy::const_iterator i = OpaqueValues.find(e);
      if (i == OpaqueValues.end()) return 0;
      return &i->second;
    }

    const LangOptions &getLangOpts() { return Ctx.getLangOptions(); }
  };

  CallStackFrame::CallStackFrame(EvalInfo &Info, const CCValue *Arguments)
      : Info(Info), Caller(Info.CurrentCall), Arguments(Arguments) {
    Info.CurrentCall = this;
    ++Info.CallStackDepth;
  }

  CallStackFrame::~CallStackFrame() {
    assert(Info.CurrentCall == this && "calls retired out of order");
    --Info.CallStackDepth;
    Info.CurrentCall = Caller;
  }

  struct ComplexValue {
  private:
    bool IsInt;

  public:
    APSInt IntReal, IntImag;
    APFloat FloatReal, FloatImag;

    ComplexValue() : FloatReal(APFloat::Bogus), FloatImag(APFloat::Bogus) {}

    void makeComplexFloat() { IsInt = false; }
    bool isComplexFloat() const { return !IsInt; }
    APFloat &getComplexFloatReal() { return FloatReal; }
    APFloat &getComplexFloatImag() { return FloatImag; }

    void makeComplexInt() { IsInt = true; }
    bool isComplexInt() const { return IsInt; }
    APSInt &getComplexIntReal() { return IntReal; }
    APSInt &getComplexIntImag() { return IntImag; }

    void moveInto(CCValue &v) const {
      if (isComplexFloat())
        v = CCValue(FloatReal, FloatImag);
      else
        v = CCValue(IntReal, IntImag);
    }
    void setFrom(const CCValue &v) {
      assert(v.isComplexFloat() || v.isComplexInt());
      if (v.isComplexFloat()) {
        makeComplexFloat();
        FloatReal = v.getComplexFloatReal();
        FloatImag = v.getComplexFloatImag();
      } else {
        makeComplexInt();
        IntReal = v.getComplexIntReal();
        IntImag = v.getComplexIntImag();
      }
    }
  };

  struct LValue {
    const Expr *Base;
    CharUnits Offset;
    CallStackFrame *Frame;

    const Expr *getLValueBase() const { return Base; }
    CharUnits &getLValueOffset() { return Offset; }
    const CharUnits &getLValueOffset() const { return Offset; }
    CallStackFrame *getLValueFrame() const { return Frame; }

    void moveInto(CCValue &V) const {
      V = CCValue(Base, Offset, Frame);
    }
    void setFrom(const CCValue &V) {
      assert(V.isLValue());
      Base = V.getLValueBase();
      Offset = V.getLValueOffset();
      Frame = V.getLValueFrame();
    }
  };
}

static bool Evaluate(CCValue &Result, EvalInfo &Info, const Expr *E);
static bool EvaluateLValue(const Expr *E, LValue &Result, EvalInfo &Info);
static bool EvaluatePointer(const Expr *E, LValue &Result, EvalInfo &Info);
static bool EvaluateInteger(const Expr *E, APSInt  &Result, EvalInfo &Info);
static bool EvaluateIntegerOrLValue(const Expr *E, CCValue &Result,
                                    EvalInfo &Info);
static bool EvaluateFloat(const Expr *E, APFloat &Result, EvalInfo &Info);
static bool EvaluateComplex(const Expr *E, ComplexValue &Res, EvalInfo &Info);

//===----------------------------------------------------------------------===//
// Misc utilities
//===----------------------------------------------------------------------===//

static bool IsGlobalLValue(const Expr* E) {
  if (!E) return true;

  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (isa<FunctionDecl>(DRE->getDecl()))
      return true;
    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return VD->hasGlobalStorage();
    return false;
  }

  if (const CompoundLiteralExpr *CLE = dyn_cast<CompoundLiteralExpr>(E))
    return CLE->isFileScope();

  if (isa<MemberExpr>(E) || isa<MaterializeTemporaryExpr>(E))
    return false;

  return true;
}

/// Check that this core constant expression value is a valid value for a
/// constant expression.
static bool CheckConstantExpression(const CCValue &Value) {
  return !Value.isLValue() || IsGlobalLValue(Value.getLValueBase());
}

const ValueDecl *GetLValueBaseDecl(const LValue &LVal) {
  if (!LVal.Base)
    return 0;

  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(LVal.Base))
    return DRE->getDecl();

  // FIXME: Static data members accessed via a MemberExpr are represented as
  // that MemberExpr. We should use the Decl directly instead.
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(LVal.Base)) {
    assert(!isa<FieldDecl>(ME->getMemberDecl()) && "shouldn't see fields here");
    return ME->getMemberDecl();
  }

  return 0;
}

static bool IsLiteralLValue(const LValue &Value) {
  return Value.Base &&
         !isa<DeclRefExpr>(Value.Base) &&
         !isa<MemberExpr>(Value.Base) &&
         !isa<MaterializeTemporaryExpr>(Value.Base);
}

static bool IsWeakDecl(const ValueDecl *Decl) {
  return Decl->hasAttr<WeakAttr>() ||
         Decl->hasAttr<WeakRefAttr>() ||
         Decl->isWeakImported();
}

static bool IsWeakLValue(const LValue &Value) {
  const ValueDecl *Decl = GetLValueBaseDecl(Value);
  return Decl && IsWeakDecl(Decl);
}

static bool EvalPointerValueAsBool(const LValue &Value, bool &Result) {
  const Expr* Base = Value.Base;

  // A null base expression indicates a null pointer.  These are always
  // evaluatable, and they are false unless the offset is zero.
  if (!Base) {
    Result = !Value.Offset.isZero();
    return true;
  }

  // Require the base expression to be a global l-value.
  // FIXME: C++11 requires such conversions. Remove this check.
  if (!IsGlobalLValue(Base)) return false;

  // We have a non-null base expression.  These are generally known to
  // be true, but if it'a decl-ref to a weak symbol it can be null at
  // runtime.
  Result = true;
  return !IsWeakLValue(Value);
}

static bool HandleConversionToBool(const CCValue &Val, bool &Result) {
  switch (Val.getKind()) {
  case APValue::Uninitialized:
    return false;
  case APValue::Int:
    Result = Val.getInt().getBoolValue();
    return true;
  case APValue::Float:
    Result = !Val.getFloat().isZero();
    return true;
  case APValue::ComplexInt:
    Result = Val.getComplexIntReal().getBoolValue() ||
             Val.getComplexIntImag().getBoolValue();
    return true;
  case APValue::ComplexFloat:
    Result = !Val.getComplexFloatReal().isZero() ||
             !Val.getComplexFloatImag().isZero();
    return true;
  case APValue::LValue: {
    LValue PointerResult;
    PointerResult.setFrom(Val);
    return EvalPointerValueAsBool(PointerResult, Result);
  }
  case APValue::Vector:
    return false;
  }

  llvm_unreachable("unknown APValue kind");
}

static bool EvaluateAsBooleanCondition(const Expr *E, bool &Result,
                                       EvalInfo &Info) {
  assert(E->isRValue() && "missing lvalue-to-rvalue conv in bool condition");
  CCValue Val;
  if (!Evaluate(Val, Info, E))
    return false;
  return HandleConversionToBool(Val, Result);
}

static APSInt HandleFloatToIntCast(QualType DestType, QualType SrcType,
                                   APFloat &Value, const ASTContext &Ctx) {
  unsigned DestWidth = Ctx.getIntWidth(DestType);
  // Determine whether we are converting to unsigned or signed.
  bool DestSigned = DestType->isSignedIntegerOrEnumerationType();

  // FIXME: Warning for overflow.
  APSInt Result(DestWidth, !DestSigned);
  bool ignored;
  (void)Value.convertToInteger(Result, llvm::APFloat::rmTowardZero, &ignored);
  return Result;
}

static APFloat HandleFloatToFloatCast(QualType DestType, QualType SrcType,
                                      APFloat &Value, const ASTContext &Ctx) {
  bool ignored;
  APFloat Result = Value;
  Result.convert(Ctx.getFloatTypeSemantics(DestType),
                 APFloat::rmNearestTiesToEven, &ignored);
  return Result;
}

static APSInt HandleIntToIntCast(QualType DestType, QualType SrcType,
                                 APSInt &Value, const ASTContext &Ctx) {
  unsigned DestWidth = Ctx.getIntWidth(DestType);
  APSInt Result = Value;
  // Figure out if this is a truncate, extend or noop cast.
  // If the input is signed, do a sign extend, noop, or truncate.
  Result = Result.extOrTrunc(DestWidth);
  Result.setIsUnsigned(DestType->isUnsignedIntegerOrEnumerationType());
  return Result;
}

static APFloat HandleIntToFloatCast(QualType DestType, QualType SrcType,
                                    APSInt &Value, const ASTContext &Ctx) {

  APFloat Result(Ctx.getFloatTypeSemantics(DestType), 1);
  Result.convertFromAPInt(Value, Value.isSigned(),
                          APFloat::rmNearestTiesToEven);
  return Result;
}

/// Try to evaluate the initializer for a variable declaration.
static bool EvaluateVarDeclInit(EvalInfo &Info, const VarDecl *VD,
                                CallStackFrame *Frame, CCValue &Result) {
  // If this is a parameter to an active constexpr function call, perform
  // argument substitution.
  if (const ParmVarDecl *PVD = dyn_cast<ParmVarDecl>(VD)) {
    if (!Frame || !Frame->Arguments)
      return false;
    Result = Frame->Arguments[PVD->getFunctionScopeIndex()];
    return true;
  }

  // Never evaluate the initializer of a weak variable. We can't be sure that
  // this is the definition which will be used.
  if (IsWeakDecl(VD))
    return false;

  const Expr *Init = VD->getAnyInitializer();
  if (!Init)
    return false;

  if (APValue *V = VD->getEvaluatedValue()) {
    Result = CCValue(*V, CCValue::GlobalValue());
    return !Result.isUninit();
  }

  if (VD->isEvaluatingValue())
    return false;

  VD->setEvaluatingValue();

  Expr::EvalStatus EStatus;
  EvalInfo InitInfo(Info.Ctx, EStatus);
  // FIXME: The caller will need to know whether the value was a constant
  // expression. If not, we should propagate up a diagnostic.
  if (!Evaluate(Result, InitInfo, Init) || !CheckConstantExpression(Result)) {
    VD->setEvaluatedValue(APValue());
    return false;
  }

  VD->setEvaluatedValue(Result);
  return true;
}

static bool IsConstNonVolatile(QualType T) {
  Qualifiers Quals = T.getQualifiers();
  return Quals.hasConst() && !Quals.hasVolatile();
}

bool HandleLValueToRValueConversion(EvalInfo &Info, QualType Type,
                                    const LValue &LVal, CCValue &RVal) {
  const Expr *Base = LVal.Base;
  CallStackFrame *Frame = LVal.Frame;

  // FIXME: Indirection through a null pointer deserves a diagnostic.
  if (!Base)
    return false;

  // FIXME: Support accessing subobjects of objects of literal types. A simple
  // byte offset is insufficient for C++11 semantics: we need to know how the
  // reference was formed (which union member was named, for instance).
  // FIXME: Support subobjects of StringLiteral and PredefinedExpr.
  if (!LVal.Offset.isZero())
    return false;

  if (const ValueDecl *D = GetLValueBaseDecl(LVal)) {
    // If the lvalue has been cast to some other type, don't try to read it.
    // FIXME: Could simulate a bitcast here.
    if (!Info.Ctx.hasSameUnqualifiedType(Type, D->getType()))
      return 0;

    // In C++98, const, non-volatile integers initialized with ICEs are ICEs.
    // In C++11, constexpr, non-volatile variables initialized with constant
    // expressions are constant expressions too. Inside constexpr functions,
    // parameters are constant expressions even if they're non-const.
    // In C, such things can also be folded, although they are not ICEs.
    //
    // FIXME: volatile-qualified ParmVarDecls need special handling. A literal
    // interpretation of C++11 suggests that volatile parameters are OK if
    // they're never read (there's no prohibition against constructing volatile
    // objects in constant expressions), but lvalue-to-rvalue conversions on
    // them are not permitted.
    const VarDecl *VD = dyn_cast<VarDecl>(D);
    if (!VD || !(IsConstNonVolatile(VD->getType()) || isa<ParmVarDecl>(VD)) ||
        !(Type->isIntegralOrEnumerationType() || Type->isRealFloatingType()) ||
        !EvaluateVarDeclInit(Info, VD, Frame, RVal))
      return false;

    if (isa<ParmVarDecl>(VD) || !VD->getAnyInitializer()->isLValue())
      return true;

    // The declaration was initialized by an lvalue, with no lvalue-to-rvalue
    // conversion. This happens when the declaration and the lvalue should be
    // considered synonymous, for instance when initializing an array of char
    // from a string literal. Continue as if the initializer lvalue was the
    // value we were originally given.
    if (!RVal.getLValueOffset().isZero())
      return false;
    Base = RVal.getLValueBase();
    Frame = RVal.getLValueFrame();
  }

  // If this is a temporary expression with a nontrivial initializer, grab the
  // value from the relevant stack frame.
  if (Frame) {
    RVal = Frame->Temporaries[Base];
    return true;
  }

  // In C99, a CompoundLiteralExpr is an lvalue, and we defer evaluating the
  // initializer until now for such expressions. Such an expression can't be
  // an ICE in C, so this only matters for fold.
  if (const CompoundLiteralExpr *CLE = dyn_cast<CompoundLiteralExpr>(Base)) {
    assert(!Info.getLangOpts().CPlusPlus && "lvalue compound literal in c++?");
    return Evaluate(RVal, Info, CLE->getInitializer());
  }

  return false;
}

namespace {
enum EvalStmtResult {
  /// Evaluation failed.
  ESR_Failed,
  /// Hit a 'return' statement.
  ESR_Returned,
  /// Evaluation succeeded.
  ESR_Succeeded
};
}

// Evaluate a statement.
static EvalStmtResult EvaluateStmt(CCValue &Result, EvalInfo &Info,
                                   const Stmt *S) {
  switch (S->getStmtClass()) {
  default:
    return ESR_Failed;

  case Stmt::NullStmtClass:
  case Stmt::DeclStmtClass:
    return ESR_Succeeded;

  case Stmt::ReturnStmtClass:
    if (Evaluate(Result, Info, cast<ReturnStmt>(S)->getRetValue()))
      return ESR_Returned;
    return ESR_Failed;

  case Stmt::CompoundStmtClass: {
    const CompoundStmt *CS = cast<CompoundStmt>(S);
    for (CompoundStmt::const_body_iterator BI = CS->body_begin(),
           BE = CS->body_end(); BI != BE; ++BI) {
      EvalStmtResult ESR = EvaluateStmt(Result, Info, *BI);
      if (ESR != ESR_Succeeded)
        return ESR;
    }
    return ESR_Succeeded;
  }
  }
}

/// Evaluate a function call.
static bool HandleFunctionCall(ArrayRef<const Expr*> Args, const Stmt *Body,
                               EvalInfo &Info, CCValue &Result) {
  // FIXME: Implement a proper call limit, along with a command-line flag.
  if (Info.NumCalls >= 1000000 || Info.CallStackDepth >= 512)
    return false;

  SmallVector<CCValue, 16> ArgValues(Args.size());
  // FIXME: Deal with default arguments and 'this'.
  for (ArrayRef<const Expr*>::iterator I = Args.begin(), E = Args.end();
       I != E; ++I)
    if (!Evaluate(ArgValues[I - Args.begin()], Info, *I))
      return false;

  CallStackFrame Frame(Info, ArgValues.data());
  return EvaluateStmt(Result, Info, Body) == ESR_Returned;
}

namespace {
class HasSideEffect
  : public ConstStmtVisitor<HasSideEffect, bool> {
  const ASTContext &Ctx;
public:

  HasSideEffect(const ASTContext &C) : Ctx(C) {}

  // Unhandled nodes conservatively default to having side effects.
  bool VisitStmt(const Stmt *S) {
    return true;
  }

  bool VisitParenExpr(const ParenExpr *E) { return Visit(E->getSubExpr()); }
  bool VisitGenericSelectionExpr(const GenericSelectionExpr *E) {
    return Visit(E->getResultExpr());
  }
  bool VisitDeclRefExpr(const DeclRefExpr *E) {
    if (Ctx.getCanonicalType(E->getType()).isVolatileQualified())
      return true;
    return false;
  }
  bool VisitObjCIvarRefExpr(const ObjCIvarRefExpr *E) {
    if (Ctx.getCanonicalType(E->getType()).isVolatileQualified())
      return true;
    return false;
  }
  bool VisitBlockDeclRefExpr (const BlockDeclRefExpr *E) {
    if (Ctx.getCanonicalType(E->getType()).isVolatileQualified())
      return true;
    return false;
  }

  // We don't want to evaluate BlockExprs multiple times, as they generate
  // a ton of code.
  bool VisitBlockExpr(const BlockExpr *E) { return true; }
  bool VisitPredefinedExpr(const PredefinedExpr *E) { return false; }
  bool VisitCompoundLiteralExpr(const CompoundLiteralExpr *E)
    { return Visit(E->getInitializer()); }
  bool VisitMemberExpr(const MemberExpr *E) { return Visit(E->getBase()); }
  bool VisitIntegerLiteral(const IntegerLiteral *E) { return false; }
  bool VisitFloatingLiteral(const FloatingLiteral *E) { return false; }
  bool VisitStringLiteral(const StringLiteral *E) { return false; }
  bool VisitCharacterLiteral(const CharacterLiteral *E) { return false; }
  bool VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *E)
    { return false; }
  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *E)
    { return Visit(E->getLHS()) || Visit(E->getRHS()); }
  bool VisitChooseExpr(const ChooseExpr *E)
    { return Visit(E->getChosenSubExpr(Ctx)); }
  bool VisitCastExpr(const CastExpr *E) { return Visit(E->getSubExpr()); }
  bool VisitBinAssign(const BinaryOperator *E) { return true; }
  bool VisitCompoundAssignOperator(const BinaryOperator *E) { return true; }
  bool VisitBinaryOperator(const BinaryOperator *E)
  { return Visit(E->getLHS()) || Visit(E->getRHS()); }
  bool VisitUnaryPreInc(const UnaryOperator *E) { return true; }
  bool VisitUnaryPostInc(const UnaryOperator *E) { return true; }
  bool VisitUnaryPreDec(const UnaryOperator *E) { return true; }
  bool VisitUnaryPostDec(const UnaryOperator *E) { return true; }
  bool VisitUnaryDeref(const UnaryOperator *E) {
    if (Ctx.getCanonicalType(E->getType()).isVolatileQualified())
      return true;
    return Visit(E->getSubExpr());
  }
  bool VisitUnaryOperator(const UnaryOperator *E) { return Visit(E->getSubExpr()); }
    
  // Has side effects if any element does.
  bool VisitInitListExpr(const InitListExpr *E) {
    for (unsigned i = 0, e = E->getNumInits(); i != e; ++i)
      if (Visit(E->getInit(i))) return true;
    if (const Expr *filler = E->getArrayFiller())
      return Visit(filler);
    return false;
  }
    
  bool VisitSizeOfPackExpr(const SizeOfPackExpr *) { return false; }
};

class OpaqueValueEvaluation {
  EvalInfo &info;
  OpaqueValueExpr *opaqueValue;

public:
  OpaqueValueEvaluation(EvalInfo &info, OpaqueValueExpr *opaqueValue,
                        Expr *value)
    : info(info), opaqueValue(opaqueValue) {

    // If evaluation fails, fail immediately.
    if (!Evaluate(info.OpaqueValues[opaqueValue], info, value)) {
      this->opaqueValue = 0;
      return;
    }
  }

  bool hasError() const { return opaqueValue == 0; }

  ~OpaqueValueEvaluation() {
    // FIXME: This will not work for recursive constexpr functions using opaque
    // values. Restore the former value.
    if (opaqueValue) info.OpaqueValues.erase(opaqueValue);
  }
};
  
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Generic Evaluation
//===----------------------------------------------------------------------===//
namespace {

template <class Derived, typename RetTy=void>
class ExprEvaluatorBase
  : public ConstStmtVisitor<Derived, RetTy> {
private:
  RetTy DerivedSuccess(const CCValue &V, const Expr *E) {
    return static_cast<Derived*>(this)->Success(V, E);
  }
  RetTy DerivedError(const Expr *E) {
    return static_cast<Derived*>(this)->Error(E);
  }
  RetTy DerivedValueInitialization(const Expr *E) {
    return static_cast<Derived*>(this)->ValueInitialization(E);
  }

protected:
  EvalInfo &Info;
  typedef ConstStmtVisitor<Derived, RetTy> StmtVisitorTy;
  typedef ExprEvaluatorBase ExprEvaluatorBaseTy;

  RetTy ValueInitialization(const Expr *E) { return DerivedError(E); }

  bool MakeTemporary(const Expr *Key, const Expr *Value, LValue &Result) {
    if (!Evaluate(Info.CurrentCall->Temporaries[Key], Info, Value))
      return false;
    Result.Base = Key;
    Result.Offset = CharUnits::Zero();
    Result.Frame = Info.CurrentCall;
    return true;
  }
public:
  ExprEvaluatorBase(EvalInfo &Info) : Info(Info) {}

  RetTy VisitStmt(const Stmt *) {
    llvm_unreachable("Expression evaluator should not be called on stmts");
  }
  RetTy VisitExpr(const Expr *E) {
    return DerivedError(E);
  }

  RetTy VisitParenExpr(const ParenExpr *E)
    { return StmtVisitorTy::Visit(E->getSubExpr()); }
  RetTy VisitUnaryExtension(const UnaryOperator *E)
    { return StmtVisitorTy::Visit(E->getSubExpr()); }
  RetTy VisitUnaryPlus(const UnaryOperator *E)
    { return StmtVisitorTy::Visit(E->getSubExpr()); }
  RetTy VisitChooseExpr(const ChooseExpr *E)
    { return StmtVisitorTy::Visit(E->getChosenSubExpr(Info.Ctx)); }
  RetTy VisitGenericSelectionExpr(const GenericSelectionExpr *E)
    { return StmtVisitorTy::Visit(E->getResultExpr()); }
  RetTy VisitSubstNonTypeTemplateParmExpr(const SubstNonTypeTemplateParmExpr *E)
    { return StmtVisitorTy::Visit(E->getReplacement()); }

  RetTy VisitBinaryConditionalOperator(const BinaryConditionalOperator *E) {
    OpaqueValueEvaluation opaque(Info, E->getOpaqueValue(), E->getCommon());
    if (opaque.hasError())
      return DerivedError(E);

    bool cond;
    if (!EvaluateAsBooleanCondition(E->getCond(), cond, Info))
      return DerivedError(E);

    return StmtVisitorTy::Visit(cond ? E->getTrueExpr() : E->getFalseExpr());
  }

  RetTy VisitConditionalOperator(const ConditionalOperator *E) {
    bool BoolResult;
    if (!EvaluateAsBooleanCondition(E->getCond(), BoolResult, Info))
      return DerivedError(E);

    Expr *EvalExpr = BoolResult ? E->getTrueExpr() : E->getFalseExpr();
    return StmtVisitorTy::Visit(EvalExpr);
  }

  RetTy VisitOpaqueValueExpr(const OpaqueValueExpr *E) {
    const CCValue *Value = Info.getOpaqueValue(E);
    if (!Value)
      return (E->getSourceExpr() ? StmtVisitorTy::Visit(E->getSourceExpr())
                                 : DerivedError(E));
    return DerivedSuccess(*Value, E);
  }

  RetTy VisitCallExpr(const CallExpr *E) {
    const Expr *Callee = E->getCallee();
    QualType CalleeType = Callee->getType();

    // FIXME: Handle the case where Callee is a (parenthesized) MemberExpr for a
    // non-static member function.
    if (CalleeType->isSpecificBuiltinType(BuiltinType::BoundMember))
      return DerivedError(E);

    if (!CalleeType->isFunctionType() && !CalleeType->isFunctionPointerType())
      return DerivedError(E);

    CCValue Call;
    if (!Evaluate(Call, Info, Callee) || !Call.isLValue() ||
        !Call.getLValueBase() || !Call.getLValueOffset().isZero())
      return DerivedError(Callee);

    const FunctionDecl *FD = 0;
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Call.getLValueBase()))
      FD = dyn_cast<FunctionDecl>(DRE->getDecl());
    else if (const MemberExpr *ME = dyn_cast<MemberExpr>(Call.getLValueBase()))
      FD = dyn_cast<FunctionDecl>(ME->getMemberDecl());
    if (!FD)
      return DerivedError(Callee);

    // Don't call function pointers which have been cast to some other type.
    if (!Info.Ctx.hasSameType(CalleeType->getPointeeType(), FD->getType()))
      return DerivedError(E);

    const FunctionDecl *Definition;
    Stmt *Body = FD->getBody(Definition);
    CCValue Result;
    llvm::ArrayRef<const Expr*> Args(E->getArgs(), E->getNumArgs());

    if (Body && Definition->isConstexpr() && !Definition->isInvalidDecl() &&
        HandleFunctionCall(Args, Body, Info, Result) &&
        CheckConstantExpression(Result))
      return DerivedSuccess(Result, E);

    return DerivedError(E);
  }

  RetTy VisitCompoundLiteralExpr(const CompoundLiteralExpr *E) {
    return StmtVisitorTy::Visit(E->getInitializer());
  }
  RetTy VisitInitListExpr(const InitListExpr *E) {
    if (Info.getLangOpts().CPlusPlus0x) {
      if (E->getNumInits() == 0)
        return DerivedValueInitialization(E);
      if (E->getNumInits() == 1)
        return StmtVisitorTy::Visit(E->getInit(0));
    }
    return DerivedError(E);
  }
  RetTy VisitImplicitValueInitExpr(const ImplicitValueInitExpr *E) {
    return DerivedValueInitialization(E);
  }
  RetTy VisitCXXScalarValueInitExpr(const CXXScalarValueInitExpr *E) {
    return DerivedValueInitialization(E);
  }

  RetTy VisitCastExpr(const CastExpr *E) {
    switch (E->getCastKind()) {
    default:
      break;

    case CK_NoOp:
      return StmtVisitorTy::Visit(E->getSubExpr());

    case CK_LValueToRValue: {
      LValue LVal;
      if (EvaluateLValue(E->getSubExpr(), LVal, Info)) {
        CCValue RVal;
        if (HandleLValueToRValueConversion(Info, E->getType(), LVal, RVal))
          return DerivedSuccess(RVal, E);
      }
      break;
    }
    }

    return DerivedError(E);
  }

  /// Visit a value which is evaluated, but whose value is ignored.
  void VisitIgnoredValue(const Expr *E) {
    CCValue Scratch;
    if (!Evaluate(Scratch, Info, E))
      Info.EvalStatus.HasSideEffects = true;
  }
};

}

//===----------------------------------------------------------------------===//
// LValue Evaluation
//
// This is used for evaluating lvalues (in C and C++), xvalues (in C++11),
// function designators (in C), decl references to void objects (in C), and
// temporaries (if building with -Wno-address-of-temporary).
//
// LValue evaluation produces values comprising a base expression of one of the
// following types:
//  * DeclRefExpr
//  * MemberExpr for a static member
//  * CompoundLiteralExpr in C
//  * StringLiteral
//  * PredefinedExpr
//  * ObjCEncodeExpr
//  * AddrLabelExpr
//  * BlockExpr
//  * CallExpr for a MakeStringConstant builtin
// plus an offset in bytes. It can also produce lvalues referring to locals. In
// that case, the Frame will point to a stack frame, and the Expr is used as a
// key to find the relevant temporary's value.
//===----------------------------------------------------------------------===//
namespace {
class LValueExprEvaluator
  : public ExprEvaluatorBase<LValueExprEvaluator, bool> {
  LValue &Result;
  const Decl *PrevDecl;

  bool Success(const Expr *E) {
    Result.Base = E;
    Result.Offset = CharUnits::Zero();
    Result.Frame = 0;
    return true;
  }
public:

  LValueExprEvaluator(EvalInfo &info, LValue &Result) :
    ExprEvaluatorBaseTy(info), Result(Result), PrevDecl(0) {}

  bool Success(const CCValue &V, const Expr *E) {
    Result.setFrom(V);
    return true;
  }
  bool Error(const Expr *E) {
    return false;
  }
  
  bool VisitVarDecl(const Expr *E, const VarDecl *VD);

  bool VisitDeclRefExpr(const DeclRefExpr *E);
  bool VisitPredefinedExpr(const PredefinedExpr *E) { return Success(E); }
  bool VisitMaterializeTemporaryExpr(const MaterializeTemporaryExpr *E);
  bool VisitCompoundLiteralExpr(const CompoundLiteralExpr *E);
  bool VisitMemberExpr(const MemberExpr *E);
  bool VisitStringLiteral(const StringLiteral *E) { return Success(E); }
  bool VisitObjCEncodeExpr(const ObjCEncodeExpr *E) { return Success(E); }
  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *E);
  bool VisitUnaryDeref(const UnaryOperator *E);

  bool VisitCastExpr(const CastExpr *E) {
    switch (E->getCastKind()) {
    default:
      return ExprEvaluatorBaseTy::VisitCastExpr(E);

    case CK_LValueBitCast:
      return Visit(E->getSubExpr());

    // FIXME: Support CK_DerivedToBase and CK_UncheckedDerivedToBase.
    // Reuse PointerExprEvaluator::VisitCastExpr for these.
    }
  }

  // FIXME: Missing: __real__, __imag__

};
} // end anonymous namespace

/// Evaluate an expression as an lvalue. This can be legitimately called on
/// expressions which are not glvalues, in a few cases:
///  * function designators in C,
///  * "extern void" objects,
///  * temporaries, if building with -Wno-address-of-temporary.
static bool EvaluateLValue(const Expr* E, LValue& Result, EvalInfo &Info) {
  assert((E->isGLValue() || E->getType()->isFunctionType() ||
          E->getType()->isVoidType() || isa<CXXTemporaryObjectExpr>(E)) &&
         "can't evaluate expression as an lvalue");
  return LValueExprEvaluator(Info, Result).Visit(E);
}

bool LValueExprEvaluator::VisitDeclRefExpr(const DeclRefExpr *E) {
  if (isa<FunctionDecl>(E->getDecl()))
    return Success(E);
  if (const VarDecl* VD = dyn_cast<VarDecl>(E->getDecl()))
    return VisitVarDecl(E, VD);
  return Error(E);
}

bool LValueExprEvaluator::VisitVarDecl(const Expr *E, const VarDecl *VD) {
  if (!VD->getType()->isReferenceType()) {
    if (isa<ParmVarDecl>(VD)) {
      Result.Base = E;
      Result.Offset = CharUnits::Zero();
      Result.Frame = Info.CurrentCall;
      return true;
    }
    return Success(E);
  }

  CCValue V;
  if (EvaluateVarDeclInit(Info, VD, Info.CurrentCall, V))
    return Success(V, E);

  return Error(E);
}

bool LValueExprEvaluator::VisitMaterializeTemporaryExpr(
    const MaterializeTemporaryExpr *E) {
  return MakeTemporary(E, E->GetTemporaryExpr(), Result);
}

bool
LValueExprEvaluator::VisitCompoundLiteralExpr(const CompoundLiteralExpr *E) {
  assert(!Info.getLangOpts().CPlusPlus && "lvalue compound literal in c++?");
  // Defer visiting the literal until the lvalue-to-rvalue conversion. We can
  // only see this when folding in C, so there's no standard to follow here.
  return Success(E);
}

bool LValueExprEvaluator::VisitMemberExpr(const MemberExpr *E) {
  // Handle static data members.
  if (const VarDecl *VD = dyn_cast<VarDecl>(E->getMemberDecl())) {
    VisitIgnoredValue(E->getBase());
    return VisitVarDecl(E, VD);
  }

  // Handle static member functions.
  if (const CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(E->getMemberDecl())) {
    if (MD->isStatic()) {
      VisitIgnoredValue(E->getBase());
      return Success(E);
    }
  }

  QualType Ty;
  if (E->isArrow()) {
    if (!EvaluatePointer(E->getBase(), Result, Info))
      return false;
    Ty = E->getBase()->getType()->getAs<PointerType>()->getPointeeType();
  } else {
    if (!Visit(E->getBase()))
      return false;
    Ty = E->getBase()->getType();
  }

  const RecordDecl *RD = Ty->getAs<RecordType>()->getDecl();
  const ASTRecordLayout &RL = Info.Ctx.getASTRecordLayout(RD);

  const FieldDecl *FD = dyn_cast<FieldDecl>(E->getMemberDecl());
  if (!FD) // FIXME: deal with other kinds of member expressions
    return false;

  if (FD->getType()->isReferenceType())
    return false;

  unsigned i = FD->getFieldIndex();
  Result.Offset += Info.Ctx.toCharUnitsFromBits(RL.getFieldOffset(i));
  return true;
}

bool LValueExprEvaluator::VisitArraySubscriptExpr(const ArraySubscriptExpr *E) {
  // FIXME: Deal with vectors as array subscript bases.
  if (E->getBase()->getType()->isVectorType())
    return false;

  if (!EvaluatePointer(E->getBase(), Result, Info))
    return false;

  APSInt Index;
  if (!EvaluateInteger(E->getIdx(), Index, Info))
    return false;

  CharUnits ElementSize = Info.Ctx.getTypeSizeInChars(E->getType());
  Result.Offset += Index.getSExtValue() * ElementSize;
  return true;
}

bool LValueExprEvaluator::VisitUnaryDeref(const UnaryOperator *E) {
  return EvaluatePointer(E->getSubExpr(), Result, Info);
}

//===----------------------------------------------------------------------===//
// Pointer Evaluation
//===----------------------------------------------------------------------===//

namespace {
class PointerExprEvaluator
  : public ExprEvaluatorBase<PointerExprEvaluator, bool> {
  LValue &Result;

  bool Success(const Expr *E) {
    Result.Base = E;
    Result.Offset = CharUnits::Zero();
    Result.Frame = 0;
    return true;
  }
public:

  PointerExprEvaluator(EvalInfo &info, LValue &Result)
    : ExprEvaluatorBaseTy(info), Result(Result) {}

  bool Success(const CCValue &V, const Expr *E) {
    Result.setFrom(V);
    return true;
  }
  bool Error(const Stmt *S) {
    return false;
  }
  bool ValueInitialization(const Expr *E) {
    return Success((Expr*)0);
  }

  bool VisitBinaryOperator(const BinaryOperator *E);
  bool VisitCastExpr(const CastExpr* E);
  bool VisitUnaryAddrOf(const UnaryOperator *E);
  bool VisitObjCStringLiteral(const ObjCStringLiteral *E)
      { return Success(E); }
  bool VisitAddrLabelExpr(const AddrLabelExpr *E)
      { return Success(E); }
  bool VisitCallExpr(const CallExpr *E);
  bool VisitBlockExpr(const BlockExpr *E) {
    if (!E->getBlockDecl()->hasCaptures())
      return Success(E);
    return false;
  }
  bool VisitCXXNullPtrLiteralExpr(const CXXNullPtrLiteralExpr *E)
      { return ValueInitialization(E); }

  // FIXME: Missing: @protocol, @selector
};
} // end anonymous namespace

static bool EvaluatePointer(const Expr* E, LValue& Result, EvalInfo &Info) {
  assert(E->isRValue() && E->getType()->hasPointerRepresentation());
  return PointerExprEvaluator(Info, Result).Visit(E);
}

bool PointerExprEvaluator::VisitBinaryOperator(const BinaryOperator *E) {
  if (E->getOpcode() != BO_Add &&
      E->getOpcode() != BO_Sub)
    return false;

  const Expr *PExp = E->getLHS();
  const Expr *IExp = E->getRHS();
  if (IExp->getType()->isPointerType())
    std::swap(PExp, IExp);

  if (!EvaluatePointer(PExp, Result, Info))
    return false;

  llvm::APSInt Offset;
  if (!EvaluateInteger(IExp, Offset, Info))
    return false;
  int64_t AdditionalOffset
    = Offset.isSigned() ? Offset.getSExtValue()
                        : static_cast<int64_t>(Offset.getZExtValue());

  // Compute the new offset in the appropriate width.

  QualType PointeeType =
    PExp->getType()->getAs<PointerType>()->getPointeeType();
  CharUnits SizeOfPointee;

  // Explicitly handle GNU void* and function pointer arithmetic extensions.
  if (PointeeType->isVoidType() || PointeeType->isFunctionType())
    SizeOfPointee = CharUnits::One();
  else
    SizeOfPointee = Info.Ctx.getTypeSizeInChars(PointeeType);

  if (E->getOpcode() == BO_Add)
    Result.Offset += AdditionalOffset * SizeOfPointee;
  else
    Result.Offset -= AdditionalOffset * SizeOfPointee;

  return true;
}

bool PointerExprEvaluator::VisitUnaryAddrOf(const UnaryOperator *E) {
  return EvaluateLValue(E->getSubExpr(), Result, Info);
}


bool PointerExprEvaluator::VisitCastExpr(const CastExpr* E) {
  const Expr* SubExpr = E->getSubExpr();

  switch (E->getCastKind()) {
  default:
    break;

  case CK_BitCast:
  case CK_CPointerToObjCPointerCast:
  case CK_BlockPointerToObjCPointerCast:
  case CK_AnyPointerToBlockPointerCast:
    return Visit(SubExpr);

  case CK_DerivedToBase:
  case CK_UncheckedDerivedToBase: {
    if (!EvaluatePointer(E->getSubExpr(), Result, Info))
      return false;

    // Now figure out the necessary offset to add to the baseLV to get from
    // the derived class to the base class.
    CharUnits Offset = CharUnits::Zero();

    QualType Ty = E->getSubExpr()->getType();
    const CXXRecordDecl *DerivedDecl = 
      Ty->getAs<PointerType>()->getPointeeType()->getAsCXXRecordDecl();

    for (CastExpr::path_const_iterator PathI = E->path_begin(), 
         PathE = E->path_end(); PathI != PathE; ++PathI) {
      const CXXBaseSpecifier *Base = *PathI;

      // FIXME: If the base is virtual, we'd need to determine the type of the
      // most derived class and we don't support that right now.
      if (Base->isVirtual())
        return false;

      const CXXRecordDecl *BaseDecl = Base->getType()->getAsCXXRecordDecl();
      const ASTRecordLayout &Layout = Info.Ctx.getASTRecordLayout(DerivedDecl);

      Result.getLValueOffset() += Layout.getBaseClassOffset(BaseDecl);
      DerivedDecl = BaseDecl;
    }

    return true;
  }

  case CK_NullToPointer:
    return ValueInitialization(E);

  case CK_IntegralToPointer: {
    CCValue Value;
    if (!EvaluateIntegerOrLValue(SubExpr, Value, Info))
      break;

    if (Value.isInt()) {
      unsigned Size = Info.Ctx.getTypeSize(E->getType());
      uint64_t N = Value.getInt().extOrTrunc(Size).getZExtValue();
      Result.Base = 0;
      Result.Offset = CharUnits::fromQuantity(N);
      Result.Frame = 0;
      return true;
    } else {
      // Cast is of an lvalue, no need to change value.
      Result.setFrom(Value);
      return true;
    }
  }
  case CK_ArrayToPointerDecay:
    // FIXME: Support array-to-pointer decay on array rvalues.
    if (!SubExpr->isGLValue())
      return Error(E);
    return EvaluateLValue(SubExpr, Result, Info);

  case CK_FunctionToPointerDecay:
    return EvaluateLValue(SubExpr, Result, Info);
  }

  return ExprEvaluatorBaseTy::VisitCastExpr(E);
}

bool PointerExprEvaluator::VisitCallExpr(const CallExpr *E) {
  if (E->isBuiltinCall(Info.Ctx) ==
        Builtin::BI__builtin___CFStringMakeConstantString ||
      E->isBuiltinCall(Info.Ctx) ==
        Builtin::BI__builtin___NSStringMakeConstantString)
    return Success(E);

  return ExprEvaluatorBaseTy::VisitCallExpr(E);
}

//===----------------------------------------------------------------------===//
// Vector Evaluation
//===----------------------------------------------------------------------===//

namespace {
  class VectorExprEvaluator
  : public ExprEvaluatorBase<VectorExprEvaluator, bool> {
    APValue &Result;
  public:

    VectorExprEvaluator(EvalInfo &info, APValue &Result)
      : ExprEvaluatorBaseTy(info), Result(Result) {}

    bool Success(const ArrayRef<APValue> &V, const Expr *E) {
      assert(V.size() == E->getType()->castAs<VectorType>()->getNumElements());
      // FIXME: remove this APValue copy.
      Result = APValue(V.data(), V.size());
      return true;
    }
    bool Success(const APValue &V, const Expr *E) {
      Result = V;
      return true;
    }
    bool Error(const Expr *E) { return false; }
    bool ValueInitialization(const Expr *E);

    bool VisitUnaryReal(const UnaryOperator *E)
      { return Visit(E->getSubExpr()); }
    bool VisitCastExpr(const CastExpr* E);
    bool VisitInitListExpr(const InitListExpr *E);
    bool VisitUnaryImag(const UnaryOperator *E);
    // FIXME: Missing: unary -, unary ~, binary add/sub/mul/div,
    //                 binary comparisons, binary and/or/xor,
    //                 shufflevector, ExtVectorElementExpr
    //        (Note that these require implementing conversions
    //         between vector types.)
  };
} // end anonymous namespace

static bool EvaluateVector(const Expr* E, APValue& Result, EvalInfo &Info) {
  assert(E->isRValue() && E->getType()->isVectorType() &&"not a vector rvalue");
  return VectorExprEvaluator(Info, Result).Visit(E);
}

bool VectorExprEvaluator::VisitCastExpr(const CastExpr* E) {
  const VectorType *VTy = E->getType()->castAs<VectorType>();
  QualType EltTy = VTy->getElementType();
  unsigned NElts = VTy->getNumElements();
  unsigned EltWidth = Info.Ctx.getTypeSize(EltTy);

  const Expr* SE = E->getSubExpr();
  QualType SETy = SE->getType();

  switch (E->getCastKind()) {
  case CK_VectorSplat: {
    APValue Val = APValue();
    if (SETy->isIntegerType()) {
      APSInt IntResult;
      if (!EvaluateInteger(SE, IntResult, Info))
         return Error(E);
      Val = APValue(IntResult);
    } else if (SETy->isRealFloatingType()) {
       APFloat F(0.0);
       if (!EvaluateFloat(SE, F, Info))
         return Error(E);
       Val = APValue(F);
    } else {
      return Error(E);
    }

    // Splat and create vector APValue.
    SmallVector<APValue, 4> Elts(NElts, Val);
    return Success(Elts, E);
  }
  case CK_BitCast: {
    // FIXME: this is wrong for any cast other than a no-op cast.
    if (SETy->isVectorType())
      return Visit(SE);

    if (!SETy->isIntegerType())
      return Error(E);

    APSInt Init;
    if (!EvaluateInteger(SE, Init, Info))
      return Error(E);

    assert((EltTy->isIntegerType() || EltTy->isRealFloatingType()) &&
           "Vectors must be composed of ints or floats");

    SmallVector<APValue, 4> Elts;
    for (unsigned i = 0; i != NElts; ++i) {
      APSInt Tmp = Init.extOrTrunc(EltWidth);

      if (EltTy->isIntegerType())
        Elts.push_back(APValue(Tmp));
      else
        Elts.push_back(APValue(APFloat(Tmp)));

      Init >>= EltWidth;
    }
    return Success(Elts, E);
  }
  default:
    return ExprEvaluatorBaseTy::VisitCastExpr(E);
  }
}

bool
VectorExprEvaluator::VisitInitListExpr(const InitListExpr *E) {
  const VectorType *VT = E->getType()->castAs<VectorType>();
  unsigned NumInits = E->getNumInits();
  unsigned NumElements = VT->getNumElements();

  QualType EltTy = VT->getElementType();
  SmallVector<APValue, 4> Elements;

  // If a vector is initialized with a single element, that value
  // becomes every element of the vector, not just the first.
  // This is the behavior described in the IBM AltiVec documentation.
  if (NumInits == 1) {

    // Handle the case where the vector is initialized by another
    // vector (OpenCL 6.1.6).
    if (E->getInit(0)->getType()->isVectorType())
      return Visit(E->getInit(0));

    APValue InitValue;
    if (EltTy->isIntegerType()) {
      llvm::APSInt sInt(32);
      if (!EvaluateInteger(E->getInit(0), sInt, Info))
        return Error(E);
      InitValue = APValue(sInt);
    } else {
      llvm::APFloat f(0.0);
      if (!EvaluateFloat(E->getInit(0), f, Info))
        return Error(E);
      InitValue = APValue(f);
    }
    for (unsigned i = 0; i < NumElements; i++) {
      Elements.push_back(InitValue);
    }
  } else {
    for (unsigned i = 0; i < NumElements; i++) {
      if (EltTy->isIntegerType()) {
        llvm::APSInt sInt(32);
        if (i < NumInits) {
          if (!EvaluateInteger(E->getInit(i), sInt, Info))
            return Error(E);
        } else {
          sInt = Info.Ctx.MakeIntValue(0, EltTy);
        }
        Elements.push_back(APValue(sInt));
      } else {
        llvm::APFloat f(0.0);
        if (i < NumInits) {
          if (!EvaluateFloat(E->getInit(i), f, Info))
            return Error(E);
        } else {
          f = APFloat::getZero(Info.Ctx.getFloatTypeSemantics(EltTy));
        }
        Elements.push_back(APValue(f));
      }
    }
  }
  return Success(Elements, E);
}

bool
VectorExprEvaluator::ValueInitialization(const Expr *E) {
  const VectorType *VT = E->getType()->getAs<VectorType>();
  QualType EltTy = VT->getElementType();
  APValue ZeroElement;
  if (EltTy->isIntegerType())
    ZeroElement = APValue(Info.Ctx.MakeIntValue(0, EltTy));
  else
    ZeroElement =
        APValue(APFloat::getZero(Info.Ctx.getFloatTypeSemantics(EltTy)));

  SmallVector<APValue, 4> Elements(VT->getNumElements(), ZeroElement);
  return Success(Elements, E);
}

bool VectorExprEvaluator::VisitUnaryImag(const UnaryOperator *E) {
  VisitIgnoredValue(E->getSubExpr());
  return ValueInitialization(E);
}

//===----------------------------------------------------------------------===//
// Integer Evaluation
//
// As a GNU extension, we support casting pointers to sufficiently-wide integer
// types and back in constant folding. Integer values are thus represented
// either as an integer-valued APValue, or as an lvalue-valued APValue.
//===----------------------------------------------------------------------===//

namespace {
class IntExprEvaluator
  : public ExprEvaluatorBase<IntExprEvaluator, bool> {
  CCValue &Result;
public:
  IntExprEvaluator(EvalInfo &info, CCValue &result)
    : ExprEvaluatorBaseTy(info), Result(result) {}

  bool Success(const llvm::APSInt &SI, const Expr *E) {
    assert(E->getType()->isIntegralOrEnumerationType() &&
           "Invalid evaluation result.");
    assert(SI.isSigned() == E->getType()->isSignedIntegerOrEnumerationType() &&
           "Invalid evaluation result.");
    assert(SI.getBitWidth() == Info.Ctx.getIntWidth(E->getType()) &&
           "Invalid evaluation result.");
    Result = CCValue(SI);
    return true;
  }

  bool Success(const llvm::APInt &I, const Expr *E) {
    assert(E->getType()->isIntegralOrEnumerationType() && 
           "Invalid evaluation result.");
    assert(I.getBitWidth() == Info.Ctx.getIntWidth(E->getType()) &&
           "Invalid evaluation result.");
    Result = CCValue(APSInt(I));
    Result.getInt().setIsUnsigned(
                            E->getType()->isUnsignedIntegerOrEnumerationType());
    return true;
  }

  bool Success(uint64_t Value, const Expr *E) {
    assert(E->getType()->isIntegralOrEnumerationType() && 
           "Invalid evaluation result.");
    Result = CCValue(Info.Ctx.MakeIntValue(Value, E->getType()));
    return true;
  }

  bool Success(CharUnits Size, const Expr *E) {
    return Success(Size.getQuantity(), E);
  }


  bool Error(SourceLocation L, diag::kind D, const Expr *E) {
    // Take the first error.
    if (Info.EvalStatus.Diag == 0) {
      Info.EvalStatus.DiagLoc = L;
      Info.EvalStatus.Diag = D;
      Info.EvalStatus.DiagExpr = E;
    }
    return false;
  }

  bool Success(const CCValue &V, const Expr *E) {
    if (V.isLValue()) {
      Result = V;
      return true;
    }
    return Success(V.getInt(), E);
  }
  bool Error(const Expr *E) {
    return Error(E->getLocStart(), diag::note_invalid_subexpr_in_ice, E);
  }

  bool ValueInitialization(const Expr *E) { return Success(0, E); }

  //===--------------------------------------------------------------------===//
  //                            Visitor Methods
  //===--------------------------------------------------------------------===//

  bool VisitIntegerLiteral(const IntegerLiteral *E) {
    return Success(E->getValue(), E);
  }
  bool VisitCharacterLiteral(const CharacterLiteral *E) {
    return Success(E->getValue(), E);
  }

  bool CheckReferencedDecl(const Expr *E, const Decl *D);
  bool VisitDeclRefExpr(const DeclRefExpr *E) {
    if (CheckReferencedDecl(E, E->getDecl()))
      return true;

    return ExprEvaluatorBaseTy::VisitDeclRefExpr(E);
  }
  bool VisitMemberExpr(const MemberExpr *E) {
    if (CheckReferencedDecl(E, E->getMemberDecl())) {
      VisitIgnoredValue(E->getBase());
      return true;
    }

    return ExprEvaluatorBaseTy::VisitMemberExpr(E);
  }

  bool VisitCallExpr(const CallExpr *E);
  bool VisitBinaryOperator(const BinaryOperator *E);
  bool VisitOffsetOfExpr(const OffsetOfExpr *E);
  bool VisitUnaryOperator(const UnaryOperator *E);

  bool VisitCastExpr(const CastExpr* E);
  bool VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *E);

  bool VisitCXXBoolLiteralExpr(const CXXBoolLiteralExpr *E) {
    return Success(E->getValue(), E);
  }

  // Note, GNU defines __null as an integer, not a pointer.
  bool VisitGNUNullExpr(const GNUNullExpr *E) {
    return ValueInitialization(E);
  }

  bool VisitUnaryTypeTraitExpr(const UnaryTypeTraitExpr *E) {
    return Success(E->getValue(), E);
  }

  bool VisitBinaryTypeTraitExpr(const BinaryTypeTraitExpr *E) {
    return Success(E->getValue(), E);
  }

  bool VisitArrayTypeTraitExpr(const ArrayTypeTraitExpr *E) {
    return Success(E->getValue(), E);
  }

  bool VisitExpressionTraitExpr(const ExpressionTraitExpr *E) {
    return Success(E->getValue(), E);
  }

  bool VisitUnaryReal(const UnaryOperator *E);
  bool VisitUnaryImag(const UnaryOperator *E);

  bool VisitCXXNoexceptExpr(const CXXNoexceptExpr *E);
  bool VisitSizeOfPackExpr(const SizeOfPackExpr *E);

private:
  CharUnits GetAlignOfExpr(const Expr *E);
  CharUnits GetAlignOfType(QualType T);
  static QualType GetObjectType(const Expr *E);
  bool TryEvaluateBuiltinObjectSize(const CallExpr *E);
  // FIXME: Missing: array subscript of vector, member of vector
};
} // end anonymous namespace

/// EvaluateIntegerOrLValue - Evaluate an rvalue integral-typed expression, and
/// produce either the integer value or a pointer.
///
/// GCC has a heinous extension which folds casts between pointer types and
/// pointer-sized integral types. We support this by allowing the evaluation of
/// an integer rvalue to produce a pointer (represented as an lvalue) instead.
/// Some simple arithmetic on such values is supported (they are treated much
/// like char*).
static bool EvaluateIntegerOrLValue(const Expr* E, CCValue &Result,
                                    EvalInfo &Info) {
  assert(E->isRValue() && E->getType()->isIntegralOrEnumerationType());
  return IntExprEvaluator(Info, Result).Visit(E);
}

static bool EvaluateInteger(const Expr* E, APSInt &Result, EvalInfo &Info) {
  CCValue Val;
  if (!EvaluateIntegerOrLValue(E, Val, Info) || !Val.isInt())
    return false;
  Result = Val.getInt();
  return true;
}

bool IntExprEvaluator::CheckReferencedDecl(const Expr* E, const Decl* D) {
  // Enums are integer constant exprs.
  if (const EnumConstantDecl *ECD = dyn_cast<EnumConstantDecl>(D)) {
    // Check for signedness/width mismatches between E type and ECD value.
    bool SameSign = (ECD->getInitVal().isSigned()
                     == E->getType()->isSignedIntegerOrEnumerationType());
    bool SameWidth = (ECD->getInitVal().getBitWidth()
                      == Info.Ctx.getIntWidth(E->getType()));
    if (SameSign && SameWidth)
      return Success(ECD->getInitVal(), E);
    else {
      // Get rid of mismatch (otherwise Success assertions will fail)
      // by computing a new value matching the type of E.
      llvm::APSInt Val = ECD->getInitVal();
      if (!SameSign)
        Val.setIsSigned(!ECD->getInitVal().isSigned());
      if (!SameWidth)
        Val = Val.extOrTrunc(Info.Ctx.getIntWidth(E->getType()));
      return Success(Val, E);
    }
  }
  return false;
}

/// EvaluateBuiltinClassifyType - Evaluate __builtin_classify_type the same way
/// as GCC.
static int EvaluateBuiltinClassifyType(const CallExpr *E) {
  // The following enum mimics the values returned by GCC.
  // FIXME: Does GCC differ between lvalue and rvalue references here?
  enum gcc_type_class {
    no_type_class = -1,
    void_type_class, integer_type_class, char_type_class,
    enumeral_type_class, boolean_type_class,
    pointer_type_class, reference_type_class, offset_type_class,
    real_type_class, complex_type_class,
    function_type_class, method_type_class,
    record_type_class, union_type_class,
    array_type_class, string_type_class,
    lang_type_class
  };

  // If no argument was supplied, default to "no_type_class". This isn't
  // ideal, however it is what gcc does.
  if (E->getNumArgs() == 0)
    return no_type_class;

  QualType ArgTy = E->getArg(0)->getType();
  if (ArgTy->isVoidType())
    return void_type_class;
  else if (ArgTy->isEnumeralType())
    return enumeral_type_class;
  else if (ArgTy->isBooleanType())
    return boolean_type_class;
  else if (ArgTy->isCharType())
    return string_type_class; // gcc doesn't appear to use char_type_class
  else if (ArgTy->isIntegerType())
    return integer_type_class;
  else if (ArgTy->isPointerType())
    return pointer_type_class;
  else if (ArgTy->isReferenceType())
    return reference_type_class;
  else if (ArgTy->isRealType())
    return real_type_class;
  else if (ArgTy->isComplexType())
    return complex_type_class;
  else if (ArgTy->isFunctionType())
    return function_type_class;
  else if (ArgTy->isStructureOrClassType())
    return record_type_class;
  else if (ArgTy->isUnionType())
    return union_type_class;
  else if (ArgTy->isArrayType())
    return array_type_class;
  else if (ArgTy->isUnionType())
    return union_type_class;
  else  // FIXME: offset_type_class, method_type_class, & lang_type_class?
    llvm_unreachable("CallExpr::isBuiltinClassifyType(): unimplemented type");
  return -1;
}

/// Retrieves the "underlying object type" of the given expression,
/// as used by __builtin_object_size.
QualType IntExprEvaluator::GetObjectType(const Expr *E) {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return VD->getType();
  } else if (isa<CompoundLiteralExpr>(E)) {
    return E->getType();
  }

  return QualType();
}

bool IntExprEvaluator::TryEvaluateBuiltinObjectSize(const CallExpr *E) {
  // TODO: Perhaps we should let LLVM lower this?
  LValue Base;
  if (!EvaluatePointer(E->getArg(0), Base, Info))
    return false;

  // If we can prove the base is null, lower to zero now.
  const Expr *LVBase = Base.getLValueBase();
  if (!LVBase) return Success(0, E);

  QualType T = GetObjectType(LVBase);
  if (T.isNull() ||
      T->isIncompleteType() ||
      T->isFunctionType() ||
      T->isVariablyModifiedType() ||
      T->isDependentType())
    return false;

  CharUnits Size = Info.Ctx.getTypeSizeInChars(T);
  CharUnits Offset = Base.getLValueOffset();

  if (!Offset.isNegative() && Offset <= Size)
    Size -= Offset;
  else
    Size = CharUnits::Zero();
  return Success(Size, E);
}

bool IntExprEvaluator::VisitCallExpr(const CallExpr *E) {
  switch (E->isBuiltinCall(Info.Ctx)) {
  default:
    return ExprEvaluatorBaseTy::VisitCallExpr(E);

  case Builtin::BI__builtin_object_size: {
    if (TryEvaluateBuiltinObjectSize(E))
      return true;

    // If evaluating the argument has side-effects we can't determine
    // the size of the object and lower it to unknown now.
    if (E->getArg(0)->HasSideEffects(Info.Ctx)) {
      if (E->getArg(1)->EvaluateKnownConstInt(Info.Ctx).getZExtValue() <= 1)
        return Success(-1ULL, E);
      return Success(0, E);
    }

    return Error(E->getLocStart(), diag::note_invalid_subexpr_in_ice, E);
  }

  case Builtin::BI__builtin_classify_type:
    return Success(EvaluateBuiltinClassifyType(E), E);

  case Builtin::BI__builtin_constant_p:
    // __builtin_constant_p always has one operand: it returns true if that
    // operand can be folded, false otherwise.
    return Success(E->getArg(0)->isEvaluatable(Info.Ctx), E);
      
  case Builtin::BI__builtin_eh_return_data_regno: {
    int Operand = E->getArg(0)->EvaluateKnownConstInt(Info.Ctx).getZExtValue();
    Operand = Info.Ctx.getTargetInfo().getEHDataRegisterNumber(Operand);
    return Success(Operand, E);
  }

  case Builtin::BI__builtin_expect:
    return Visit(E->getArg(0));
      
  case Builtin::BIstrlen:
  case Builtin::BI__builtin_strlen:
    // As an extension, we support strlen() and __builtin_strlen() as constant
    // expressions when the argument is a string literal.
    if (const StringLiteral *S
               = dyn_cast<StringLiteral>(E->getArg(0)->IgnoreParenImpCasts())) {
      // The string literal may have embedded null characters. Find the first
      // one and truncate there.
      StringRef Str = S->getString();
      StringRef::size_type Pos = Str.find(0);
      if (Pos != StringRef::npos)
        Str = Str.substr(0, Pos);
      
      return Success(Str.size(), E);
    }
      
    return Error(E->getLocStart(), diag::note_invalid_subexpr_in_ice, E);

  case Builtin::BI__atomic_is_lock_free: {
    APSInt SizeVal;
    if (!EvaluateInteger(E->getArg(0), SizeVal, Info))
      return false;

    // For __atomic_is_lock_free(sizeof(_Atomic(T))), if the size is a power
    // of two less than the maximum inline atomic width, we know it is
    // lock-free.  If the size isn't a power of two, or greater than the
    // maximum alignment where we promote atomics, we know it is not lock-free
    // (at least not in the sense of atomic_is_lock_free).  Otherwise,
    // the answer can only be determined at runtime; for example, 16-byte
    // atomics have lock-free implementations on some, but not all,
    // x86-64 processors.

    // Check power-of-two.
    CharUnits Size = CharUnits::fromQuantity(SizeVal.getZExtValue());
    if (!Size.isPowerOfTwo())
#if 0
      // FIXME: Suppress this folding until the ABI for the promotion width
      // settles.
      return Success(0, E);
#else
      return Error(E->getLocStart(), diag::note_invalid_subexpr_in_ice, E);
#endif

#if 0
    // Check against promotion width.
    // FIXME: Suppress this folding until the ABI for the promotion width
    // settles.
    unsigned PromoteWidthBits =
        Info.Ctx.getTargetInfo().getMaxAtomicPromoteWidth();
    if (Size > Info.Ctx.toCharUnitsFromBits(PromoteWidthBits))
      return Success(0, E);
#endif

    // Check against inlining width.
    unsigned InlineWidthBits =
        Info.Ctx.getTargetInfo().getMaxAtomicInlineWidth();
    if (Size <= Info.Ctx.toCharUnitsFromBits(InlineWidthBits))
      return Success(1, E);

    return Error(E->getLocStart(), diag::note_invalid_subexpr_in_ice, E);
  }
  }
}

static bool HasSameBase(const LValue &A, const LValue &B) {
  if (!A.getLValueBase())
    return !B.getLValueBase();
  if (!B.getLValueBase())
    return false;

  if (A.getLValueBase() != B.getLValueBase()) {
    const Decl *ADecl = GetLValueBaseDecl(A);
    if (!ADecl)
      return false;
    const Decl *BDecl = GetLValueBaseDecl(B);
    if (ADecl != BDecl)
      return false;
  }

  return IsGlobalLValue(A.getLValueBase()) ||
         A.getLValueFrame() == B.getLValueFrame();
}

bool IntExprEvaluator::VisitBinaryOperator(const BinaryOperator *E) {
  if (E->isAssignmentOp())
    return Error(E->getOperatorLoc(), diag::note_invalid_subexpr_in_ice, E);

  if (E->getOpcode() == BO_Comma) {
    VisitIgnoredValue(E->getLHS());
    return Visit(E->getRHS());
  }

  if (E->isLogicalOp()) {
    // These need to be handled specially because the operands aren't
    // necessarily integral
    bool lhsResult, rhsResult;

    if (EvaluateAsBooleanCondition(E->getLHS(), lhsResult, Info)) {
      // We were able to evaluate the LHS, see if we can get away with not
      // evaluating the RHS: 0 && X -> 0, 1 || X -> 1
      if (lhsResult == (E->getOpcode() == BO_LOr))
        return Success(lhsResult, E);

      if (EvaluateAsBooleanCondition(E->getRHS(), rhsResult, Info)) {
        if (E->getOpcode() == BO_LOr)
          return Success(lhsResult || rhsResult, E);
        else
          return Success(lhsResult && rhsResult, E);
      }
    } else {
      if (EvaluateAsBooleanCondition(E->getRHS(), rhsResult, Info)) {
        // We can't evaluate the LHS; however, sometimes the result
        // is determined by the RHS: X && 0 -> 0, X || 1 -> 1.
        if (rhsResult == (E->getOpcode() == BO_LOr) ||
            !rhsResult == (E->getOpcode() == BO_LAnd)) {
          // Since we weren't able to evaluate the left hand side, it
          // must have had side effects.
          Info.EvalStatus.HasSideEffects = true;

          return Success(rhsResult, E);
        }
      }
    }

    return false;
  }

  QualType LHSTy = E->getLHS()->getType();
  QualType RHSTy = E->getRHS()->getType();

  if (LHSTy->isAnyComplexType()) {
    assert(RHSTy->isAnyComplexType() && "Invalid comparison");
    ComplexValue LHS, RHS;

    if (!EvaluateComplex(E->getLHS(), LHS, Info))
      return false;

    if (!EvaluateComplex(E->getRHS(), RHS, Info))
      return false;

    if (LHS.isComplexFloat()) {
      APFloat::cmpResult CR_r =
        LHS.getComplexFloatReal().compare(RHS.getComplexFloatReal());
      APFloat::cmpResult CR_i =
        LHS.getComplexFloatImag().compare(RHS.getComplexFloatImag());

      if (E->getOpcode() == BO_EQ)
        return Success((CR_r == APFloat::cmpEqual &&
                        CR_i == APFloat::cmpEqual), E);
      else {
        assert(E->getOpcode() == BO_NE &&
               "Invalid complex comparison.");
        return Success(((CR_r == APFloat::cmpGreaterThan ||
                         CR_r == APFloat::cmpLessThan ||
                         CR_r == APFloat::cmpUnordered) ||
                        (CR_i == APFloat::cmpGreaterThan ||
                         CR_i == APFloat::cmpLessThan ||
                         CR_i == APFloat::cmpUnordered)), E);
      }
    } else {
      if (E->getOpcode() == BO_EQ)
        return Success((LHS.getComplexIntReal() == RHS.getComplexIntReal() &&
                        LHS.getComplexIntImag() == RHS.getComplexIntImag()), E);
      else {
        assert(E->getOpcode() == BO_NE &&
               "Invalid compex comparison.");
        return Success((LHS.getComplexIntReal() != RHS.getComplexIntReal() ||
                        LHS.getComplexIntImag() != RHS.getComplexIntImag()), E);
      }
    }
  }

  if (LHSTy->isRealFloatingType() &&
      RHSTy->isRealFloatingType()) {
    APFloat RHS(0.0), LHS(0.0);

    if (!EvaluateFloat(E->getRHS(), RHS, Info))
      return false;

    if (!EvaluateFloat(E->getLHS(), LHS, Info))
      return false;

    APFloat::cmpResult CR = LHS.compare(RHS);

    switch (E->getOpcode()) {
    default:
      llvm_unreachable("Invalid binary operator!");
    case BO_LT:
      return Success(CR == APFloat::cmpLessThan, E);
    case BO_GT:
      return Success(CR == APFloat::cmpGreaterThan, E);
    case BO_LE:
      return Success(CR == APFloat::cmpLessThan || CR == APFloat::cmpEqual, E);
    case BO_GE:
      return Success(CR == APFloat::cmpGreaterThan || CR == APFloat::cmpEqual,
                     E);
    case BO_EQ:
      return Success(CR == APFloat::cmpEqual, E);
    case BO_NE:
      return Success(CR == APFloat::cmpGreaterThan
                     || CR == APFloat::cmpLessThan
                     || CR == APFloat::cmpUnordered, E);
    }
  }

  if (LHSTy->isPointerType() && RHSTy->isPointerType()) {
    if (E->getOpcode() == BO_Sub || E->isComparisonOp()) {
      LValue LHSValue;
      if (!EvaluatePointer(E->getLHS(), LHSValue, Info))
        return false;

      LValue RHSValue;
      if (!EvaluatePointer(E->getRHS(), RHSValue, Info))
        return false;

      // Reject differing bases from the normal codepath; we special-case
      // comparisons to null.
      if (!HasSameBase(LHSValue, RHSValue)) {
        // Inequalities and subtractions between unrelated pointers have
        // unspecified or undefined behavior.
        if (!E->isEqualityOp())
          return false;
        // A constant address may compare equal to the address of a symbol.
        // The one exception is that address of an object cannot compare equal
        // to a null pointer constant.
        if ((!LHSValue.Base && !LHSValue.Offset.isZero()) ||
            (!RHSValue.Base && !RHSValue.Offset.isZero()))
          return false;
        // It's implementation-defined whether distinct literals will have
        // distinct addresses. In clang, we do not guarantee the addresses are
        // distinct.
        if (IsLiteralLValue(LHSValue) || IsLiteralLValue(RHSValue))
          return false;
        // We can't tell whether weak symbols will end up pointing to the same
        // object.
        if (IsWeakLValue(LHSValue) || IsWeakLValue(RHSValue))
          return false;
        // Pointers with different bases cannot represent the same object.
        // (Note that clang defaults to -fmerge-all-constants, which can
        // lead to inconsistent results for comparisons involving the address
        // of a constant; this generally doesn't matter in practice.)
        return Success(E->getOpcode() == BO_NE, E);
      }

      if (E->getOpcode() == BO_Sub) {
        QualType Type = E->getLHS()->getType();
        QualType ElementType = Type->getAs<PointerType>()->getPointeeType();

        CharUnits ElementSize = CharUnits::One();
        if (!ElementType->isVoidType() && !ElementType->isFunctionType())
          ElementSize = Info.Ctx.getTypeSizeInChars(ElementType);

        CharUnits Diff = LHSValue.getLValueOffset() - 
                             RHSValue.getLValueOffset();
        return Success(Diff / ElementSize, E);
      }

      const CharUnits &LHSOffset = LHSValue.getLValueOffset();
      const CharUnits &RHSOffset = RHSValue.getLValueOffset();
      switch (E->getOpcode()) {
      default: llvm_unreachable("missing comparison operator");
      case BO_LT: return Success(LHSOffset < RHSOffset, E);
      case BO_GT: return Success(LHSOffset > RHSOffset, E);
      case BO_LE: return Success(LHSOffset <= RHSOffset, E);
      case BO_GE: return Success(LHSOffset >= RHSOffset, E);
      case BO_EQ: return Success(LHSOffset == RHSOffset, E);
      case BO_NE: return Success(LHSOffset != RHSOffset, E);
      }
    }
  }
  if (!LHSTy->isIntegralOrEnumerationType() ||
      !RHSTy->isIntegralOrEnumerationType()) {
    // We can't continue from here for non-integral types, and they
    // could potentially confuse the following operations.
    return false;
  }

  // The LHS of a constant expr is always evaluated and needed.
  CCValue LHSVal;
  if (!EvaluateIntegerOrLValue(E->getLHS(), LHSVal, Info))
    return false; // error in subexpression.

  if (!Visit(E->getRHS()))
    return false;
  CCValue &RHSVal = Result;

  // Handle cases like (unsigned long)&a + 4.
  if (E->isAdditiveOp() && LHSVal.isLValue() && RHSVal.isInt()) {
    CharUnits AdditionalOffset = CharUnits::fromQuantity(
                                     RHSVal.getInt().getZExtValue());
    if (E->getOpcode() == BO_Add)
      LHSVal.getLValueOffset() += AdditionalOffset;
    else
      LHSVal.getLValueOffset() -= AdditionalOffset;
    Result = LHSVal;
    return true;
  }

  // Handle cases like 4 + (unsigned long)&a
  if (E->getOpcode() == BO_Add &&
        RHSVal.isLValue() && LHSVal.isInt()) {
    RHSVal.getLValueOffset() += CharUnits::fromQuantity(
                                    LHSVal.getInt().getZExtValue());
    // Note that RHSVal is Result.
    return true;
  }

  // All the following cases expect both operands to be an integer
  if (!LHSVal.isInt() || !RHSVal.isInt())
    return false;

  APSInt &LHS = LHSVal.getInt();
  APSInt &RHS = RHSVal.getInt();

  switch (E->getOpcode()) {
  default:
    return Error(E->getOperatorLoc(), diag::note_invalid_subexpr_in_ice, E);
  case BO_Mul: return Success(LHS * RHS, E);
  case BO_Add: return Success(LHS + RHS, E);
  case BO_Sub: return Success(LHS - RHS, E);
  case BO_And: return Success(LHS & RHS, E);
  case BO_Xor: return Success(LHS ^ RHS, E);
  case BO_Or:  return Success(LHS | RHS, E);
  case BO_Div:
    if (RHS == 0)
      return Error(E->getOperatorLoc(), diag::note_expr_divide_by_zero, E);
    return Success(LHS / RHS, E);
  case BO_Rem:
    if (RHS == 0)
      return Error(E->getOperatorLoc(), diag::note_expr_divide_by_zero, E);
    return Success(LHS % RHS, E);
  case BO_Shl: {
    // During constant-folding, a negative shift is an opposite shift.
    if (RHS.isSigned() && RHS.isNegative()) {
      RHS = -RHS;
      goto shift_right;
    }

  shift_left:
    unsigned SA
      = (unsigned) RHS.getLimitedValue(LHS.getBitWidth()-1);
    return Success(LHS << SA, E);
  }
  case BO_Shr: {
    // During constant-folding, a negative shift is an opposite shift.
    if (RHS.isSigned() && RHS.isNegative()) {
      RHS = -RHS;
      goto shift_left;
    }

  shift_right:
    unsigned SA =
      (unsigned) RHS.getLimitedValue(LHS.getBitWidth()-1);
    return Success(LHS >> SA, E);
  }

  case BO_LT: return Success(LHS < RHS, E);
  case BO_GT: return Success(LHS > RHS, E);
  case BO_LE: return Success(LHS <= RHS, E);
  case BO_GE: return Success(LHS >= RHS, E);
  case BO_EQ: return Success(LHS == RHS, E);
  case BO_NE: return Success(LHS != RHS, E);
  }
}

CharUnits IntExprEvaluator::GetAlignOfType(QualType T) {
  // C++ [expr.sizeof]p2: "When applied to a reference or a reference type,
  //   the result is the size of the referenced type."
  // C++ [expr.alignof]p3: "When alignof is applied to a reference type, the
  //   result shall be the alignment of the referenced type."
  if (const ReferenceType *Ref = T->getAs<ReferenceType>())
    T = Ref->getPointeeType();

  // __alignof is defined to return the preferred alignment.
  return Info.Ctx.toCharUnitsFromBits(
    Info.Ctx.getPreferredTypeAlign(T.getTypePtr()));
}

CharUnits IntExprEvaluator::GetAlignOfExpr(const Expr *E) {
  E = E->IgnoreParens();

  // alignof decl is always accepted, even if it doesn't make sense: we default
  // to 1 in those cases.
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
    return Info.Ctx.getDeclAlign(DRE->getDecl(), 
                                 /*RefAsPointee*/true);

  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E))
    return Info.Ctx.getDeclAlign(ME->getMemberDecl(),
                                 /*RefAsPointee*/true);

  return GetAlignOfType(E->getType());
}


/// VisitUnaryExprOrTypeTraitExpr - Evaluate a sizeof, alignof or vec_step with
/// a result as the expression's type.
bool IntExprEvaluator::VisitUnaryExprOrTypeTraitExpr(
                                    const UnaryExprOrTypeTraitExpr *E) {
  switch(E->getKind()) {
  case UETT_AlignOf: {
    if (E->isArgumentType())
      return Success(GetAlignOfType(E->getArgumentType()), E);
    else
      return Success(GetAlignOfExpr(E->getArgumentExpr()), E);
  }

  case UETT_VecStep: {
    QualType Ty = E->getTypeOfArgument();

    if (Ty->isVectorType()) {
      unsigned n = Ty->getAs<VectorType>()->getNumElements();

      // The vec_step built-in functions that take a 3-component
      // vector return 4. (OpenCL 1.1 spec 6.11.12)
      if (n == 3)
        n = 4;

      return Success(n, E);
    } else
      return Success(1, E);
  }

  case UETT_SizeOf: {
    QualType SrcTy = E->getTypeOfArgument();
    // C++ [expr.sizeof]p2: "When applied to a reference or a reference type,
    //   the result is the size of the referenced type."
    // C++ [expr.alignof]p3: "When alignof is applied to a reference type, the
    //   result shall be the alignment of the referenced type."
    if (const ReferenceType *Ref = SrcTy->getAs<ReferenceType>())
      SrcTy = Ref->getPointeeType();

    // sizeof(void), __alignof__(void), sizeof(function) = 1 as a gcc
    // extension.
    if (SrcTy->isVoidType() || SrcTy->isFunctionType())
      return Success(1, E);

    // sizeof(vla) is not a constantexpr: C99 6.5.3.4p2.
    if (!SrcTy->isConstantSizeType())
      return false;

    // Get information about the size.
    return Success(Info.Ctx.getTypeSizeInChars(SrcTy), E);
  }
  }

  llvm_unreachable("unknown expr/type trait");
  return false;
}

bool IntExprEvaluator::VisitOffsetOfExpr(const OffsetOfExpr *OOE) {
  CharUnits Result;
  unsigned n = OOE->getNumComponents();
  if (n == 0)
    return false;
  QualType CurrentType = OOE->getTypeSourceInfo()->getType();
  for (unsigned i = 0; i != n; ++i) {
    OffsetOfExpr::OffsetOfNode ON = OOE->getComponent(i);
    switch (ON.getKind()) {
    case OffsetOfExpr::OffsetOfNode::Array: {
      const Expr *Idx = OOE->getIndexExpr(ON.getArrayExprIndex());
      APSInt IdxResult;
      if (!EvaluateInteger(Idx, IdxResult, Info))
        return false;
      const ArrayType *AT = Info.Ctx.getAsArrayType(CurrentType);
      if (!AT)
        return false;
      CurrentType = AT->getElementType();
      CharUnits ElementSize = Info.Ctx.getTypeSizeInChars(CurrentType);
      Result += IdxResult.getSExtValue() * ElementSize;
        break;
    }
        
    case OffsetOfExpr::OffsetOfNode::Field: {
      FieldDecl *MemberDecl = ON.getField();
      const RecordType *RT = CurrentType->getAs<RecordType>();
      if (!RT) 
        return false;
      RecordDecl *RD = RT->getDecl();
      const ASTRecordLayout &RL = Info.Ctx.getASTRecordLayout(RD);
      unsigned i = MemberDecl->getFieldIndex();
      assert(i < RL.getFieldCount() && "offsetof field in wrong type");
      Result += Info.Ctx.toCharUnitsFromBits(RL.getFieldOffset(i));
      CurrentType = MemberDecl->getType().getNonReferenceType();
      break;
    }
        
    case OffsetOfExpr::OffsetOfNode::Identifier:
      llvm_unreachable("dependent __builtin_offsetof");
      return false;
        
    case OffsetOfExpr::OffsetOfNode::Base: {
      CXXBaseSpecifier *BaseSpec = ON.getBase();
      if (BaseSpec->isVirtual())
        return false;

      // Find the layout of the class whose base we are looking into.
      const RecordType *RT = CurrentType->getAs<RecordType>();
      if (!RT) 
        return false;
      RecordDecl *RD = RT->getDecl();
      const ASTRecordLayout &RL = Info.Ctx.getASTRecordLayout(RD);

      // Find the base class itself.
      CurrentType = BaseSpec->getType();
      const RecordType *BaseRT = CurrentType->getAs<RecordType>();
      if (!BaseRT)
        return false;
      
      // Add the offset to the base.
      Result += RL.getBaseClassOffset(cast<CXXRecordDecl>(BaseRT->getDecl()));
      break;
    }
    }
  }
  return Success(Result, OOE);
}

bool IntExprEvaluator::VisitUnaryOperator(const UnaryOperator *E) {
  if (E->getOpcode() == UO_LNot) {
    // LNot's operand isn't necessarily an integer, so we handle it specially.
    bool bres;
    if (!EvaluateAsBooleanCondition(E->getSubExpr(), bres, Info))
      return false;
    return Success(!bres, E);
  }

  // Only handle integral operations...
  if (!E->getSubExpr()->getType()->isIntegralOrEnumerationType())
    return false;

  // Get the operand value.
  CCValue Val;
  if (!Evaluate(Val, Info, E->getSubExpr()))
    return false;

  switch (E->getOpcode()) {
  default:
    // Address, indirect, pre/post inc/dec, etc are not valid constant exprs.
    // See C99 6.6p3.
    return Error(E->getOperatorLoc(), diag::note_invalid_subexpr_in_ice, E);
  case UO_Extension:
    // FIXME: Should extension allow i-c-e extension expressions in its scope?
    // If so, we could clear the diagnostic ID.
    return Success(Val, E);
  case UO_Plus:
    // The result is just the value.
    return Success(Val, E);
  case UO_Minus:
    if (!Val.isInt()) return false;
    return Success(-Val.getInt(), E);
  case UO_Not:
    if (!Val.isInt()) return false;
    return Success(~Val.getInt(), E);
  }
}

/// HandleCast - This is used to evaluate implicit or explicit casts where the
/// result type is integer.
bool IntExprEvaluator::VisitCastExpr(const CastExpr *E) {
  const Expr *SubExpr = E->getSubExpr();
  QualType DestType = E->getType();
  QualType SrcType = SubExpr->getType();

  switch (E->getCastKind()) {
  case CK_BaseToDerived:
  case CK_DerivedToBase:
  case CK_UncheckedDerivedToBase:
  case CK_Dynamic:
  case CK_ToUnion:
  case CK_ArrayToPointerDecay:
  case CK_FunctionToPointerDecay:
  case CK_NullToPointer:
  case CK_NullToMemberPointer:
  case CK_BaseToDerivedMemberPointer:
  case CK_DerivedToBaseMemberPointer:
  case CK_ConstructorConversion:
  case CK_IntegralToPointer:
  case CK_ToVoid:
  case CK_VectorSplat:
  case CK_IntegralToFloating:
  case CK_FloatingCast:
  case CK_CPointerToObjCPointerCast:
  case CK_BlockPointerToObjCPointerCast:
  case CK_AnyPointerToBlockPointerCast:
  case CK_ObjCObjectLValueCast:
  case CK_FloatingRealToComplex:
  case CK_FloatingComplexToReal:
  case CK_FloatingComplexCast:
  case CK_FloatingComplexToIntegralComplex:
  case CK_IntegralRealToComplex:
  case CK_IntegralComplexCast:
  case CK_IntegralComplexToFloatingComplex:
    llvm_unreachable("invalid cast kind for integral value");

  case CK_BitCast:
  case CK_Dependent:
  case CK_GetObjCProperty:
  case CK_LValueBitCast:
  case CK_UserDefinedConversion:
  case CK_ARCProduceObject:
  case CK_ARCConsumeObject:
  case CK_ARCReclaimReturnedObject:
  case CK_ARCExtendBlockObject:
    return false;

  case CK_LValueToRValue:
  case CK_NoOp:
    return ExprEvaluatorBaseTy::VisitCastExpr(E);

  case CK_MemberPointerToBoolean:
  case CK_PointerToBoolean:
  case CK_IntegralToBoolean:
  case CK_FloatingToBoolean:
  case CK_FloatingComplexToBoolean:
  case CK_IntegralComplexToBoolean: {
    bool BoolResult;
    if (!EvaluateAsBooleanCondition(SubExpr, BoolResult, Info))
      return false;
    return Success(BoolResult, E);
  }

  case CK_IntegralCast: {
    if (!Visit(SubExpr))
      return false;

    if (!Result.isInt()) {
      // Only allow casts of lvalues if they are lossless.
      return Info.Ctx.getTypeSize(DestType) == Info.Ctx.getTypeSize(SrcType);
    }

    return Success(HandleIntToIntCast(DestType, SrcType,
                                      Result.getInt(), Info.Ctx), E);
  }

  case CK_PointerToIntegral: {
    LValue LV;
    if (!EvaluatePointer(SubExpr, LV, Info))
      return false;

    if (LV.getLValueBase()) {
      // Only allow based lvalue casts if they are lossless.
      if (Info.Ctx.getTypeSize(DestType) != Info.Ctx.getTypeSize(SrcType))
        return false;

      LV.moveInto(Result);
      return true;
    }

    APSInt AsInt = Info.Ctx.MakeIntValue(LV.getLValueOffset().getQuantity(), 
                                         SrcType);
    return Success(HandleIntToIntCast(DestType, SrcType, AsInt, Info.Ctx), E);
  }

  case CK_IntegralComplexToReal: {
    ComplexValue C;
    if (!EvaluateComplex(SubExpr, C, Info))
      return false;
    return Success(C.getComplexIntReal(), E);
  }

  case CK_FloatingToIntegral: {
    APFloat F(0.0);
    if (!EvaluateFloat(SubExpr, F, Info))
      return false;

    return Success(HandleFloatToIntCast(DestType, SrcType, F, Info.Ctx), E);
  }
  }

  llvm_unreachable("unknown cast resulting in integral value");
  return false;
}

bool IntExprEvaluator::VisitUnaryReal(const UnaryOperator *E) {
  if (E->getSubExpr()->getType()->isAnyComplexType()) {
    ComplexValue LV;
    if (!EvaluateComplex(E->getSubExpr(), LV, Info) || !LV.isComplexInt())
      return Error(E->getExprLoc(), diag::note_invalid_subexpr_in_ice, E);
    return Success(LV.getComplexIntReal(), E);
  }

  return Visit(E->getSubExpr());
}

bool IntExprEvaluator::VisitUnaryImag(const UnaryOperator *E) {
  if (E->getSubExpr()->getType()->isComplexIntegerType()) {
    ComplexValue LV;
    if (!EvaluateComplex(E->getSubExpr(), LV, Info) || !LV.isComplexInt())
      return Error(E->getExprLoc(), diag::note_invalid_subexpr_in_ice, E);
    return Success(LV.getComplexIntImag(), E);
  }

  VisitIgnoredValue(E->getSubExpr());
  return Success(0, E);
}

bool IntExprEvaluator::VisitSizeOfPackExpr(const SizeOfPackExpr *E) {
  return Success(E->getPackLength(), E);
}

bool IntExprEvaluator::VisitCXXNoexceptExpr(const CXXNoexceptExpr *E) {
  return Success(E->getValue(), E);
}

//===----------------------------------------------------------------------===//
// Float Evaluation
//===----------------------------------------------------------------------===//

namespace {
class FloatExprEvaluator
  : public ExprEvaluatorBase<FloatExprEvaluator, bool> {
  APFloat &Result;
public:
  FloatExprEvaluator(EvalInfo &info, APFloat &result)
    : ExprEvaluatorBaseTy(info), Result(result) {}

  bool Success(const CCValue &V, const Expr *e) {
    Result = V.getFloat();
    return true;
  }
  bool Error(const Stmt *S) {
    return false;
  }

  bool ValueInitialization(const Expr *E) {
    Result = APFloat::getZero(Info.Ctx.getFloatTypeSemantics(E->getType()));
    return true;
  }

  bool VisitCallExpr(const CallExpr *E);

  bool VisitUnaryOperator(const UnaryOperator *E);
  bool VisitBinaryOperator(const BinaryOperator *E);
  bool VisitFloatingLiteral(const FloatingLiteral *E);
  bool VisitCastExpr(const CastExpr *E);

  bool VisitUnaryReal(const UnaryOperator *E);
  bool VisitUnaryImag(const UnaryOperator *E);

  // FIXME: Missing: array subscript of vector, member of vector,
  //                 ImplicitValueInitExpr
};
} // end anonymous namespace

static bool EvaluateFloat(const Expr* E, APFloat& Result, EvalInfo &Info) {
  assert(E->isRValue() && E->getType()->isRealFloatingType());
  return FloatExprEvaluator(Info, Result).Visit(E);
}

static bool TryEvaluateBuiltinNaN(const ASTContext &Context,
                                  QualType ResultTy,
                                  const Expr *Arg,
                                  bool SNaN,
                                  llvm::APFloat &Result) {
  const StringLiteral *S = dyn_cast<StringLiteral>(Arg->IgnoreParenCasts());
  if (!S) return false;

  const llvm::fltSemantics &Sem = Context.getFloatTypeSemantics(ResultTy);

  llvm::APInt fill;

  // Treat empty strings as if they were zero.
  if (S->getString().empty())
    fill = llvm::APInt(32, 0);
  else if (S->getString().getAsInteger(0, fill))
    return false;

  if (SNaN)
    Result = llvm::APFloat::getSNaN(Sem, false, &fill);
  else
    Result = llvm::APFloat::getQNaN(Sem, false, &fill);
  return true;
}

bool FloatExprEvaluator::VisitCallExpr(const CallExpr *E) {
  switch (E->isBuiltinCall(Info.Ctx)) {
  default:
    return ExprEvaluatorBaseTy::VisitCallExpr(E);

  case Builtin::BI__builtin_huge_val:
  case Builtin::BI__builtin_huge_valf:
  case Builtin::BI__builtin_huge_vall:
  case Builtin::BI__builtin_inf:
  case Builtin::BI__builtin_inff:
  case Builtin::BI__builtin_infl: {
    const llvm::fltSemantics &Sem =
      Info.Ctx.getFloatTypeSemantics(E->getType());
    Result = llvm::APFloat::getInf(Sem);
    return true;
  }

  case Builtin::BI__builtin_nans:
  case Builtin::BI__builtin_nansf:
  case Builtin::BI__builtin_nansl:
    return TryEvaluateBuiltinNaN(Info.Ctx, E->getType(), E->getArg(0),
                                 true, Result);

  case Builtin::BI__builtin_nan:
  case Builtin::BI__builtin_nanf:
  case Builtin::BI__builtin_nanl:
    // If this is __builtin_nan() turn this into a nan, otherwise we
    // can't constant fold it.
    return TryEvaluateBuiltinNaN(Info.Ctx, E->getType(), E->getArg(0),
                                 false, Result);

  case Builtin::BI__builtin_fabs:
  case Builtin::BI__builtin_fabsf:
  case Builtin::BI__builtin_fabsl:
    if (!EvaluateFloat(E->getArg(0), Result, Info))
      return false;

    if (Result.isNegative())
      Result.changeSign();
    return true;

  case Builtin::BI__builtin_copysign:
  case Builtin::BI__builtin_copysignf:
  case Builtin::BI__builtin_copysignl: {
    APFloat RHS(0.);
    if (!EvaluateFloat(E->getArg(0), Result, Info) ||
        !EvaluateFloat(E->getArg(1), RHS, Info))
      return false;
    Result.copySign(RHS);
    return true;
  }
  }
}

bool FloatExprEvaluator::VisitUnaryReal(const UnaryOperator *E) {
  if (E->getSubExpr()->getType()->isAnyComplexType()) {
    ComplexValue CV;
    if (!EvaluateComplex(E->getSubExpr(), CV, Info))
      return false;
    Result = CV.FloatReal;
    return true;
  }

  return Visit(E->getSubExpr());
}

bool FloatExprEvaluator::VisitUnaryImag(const UnaryOperator *E) {
  if (E->getSubExpr()->getType()->isAnyComplexType()) {
    ComplexValue CV;
    if (!EvaluateComplex(E->getSubExpr(), CV, Info))
      return false;
    Result = CV.FloatImag;
    return true;
  }

  VisitIgnoredValue(E->getSubExpr());
  const llvm::fltSemantics &Sem = Info.Ctx.getFloatTypeSemantics(E->getType());
  Result = llvm::APFloat::getZero(Sem);
  return true;
}

bool FloatExprEvaluator::VisitUnaryOperator(const UnaryOperator *E) {
  switch (E->getOpcode()) {
  default: return false;
  case UO_Plus:
    return EvaluateFloat(E->getSubExpr(), Result, Info);
  case UO_Minus:
    if (!EvaluateFloat(E->getSubExpr(), Result, Info))
      return false;
    Result.changeSign();
    return true;
  }
}

bool FloatExprEvaluator::VisitBinaryOperator(const BinaryOperator *E) {
  if (E->getOpcode() == BO_Comma) {
    VisitIgnoredValue(E->getLHS());
    return Visit(E->getRHS());
  }

  // We can't evaluate pointer-to-member operations or assignments.
  if (E->isPtrMemOp() || E->isAssignmentOp())
    return false;

  // FIXME: Diagnostics?  I really don't understand how the warnings
  // and errors are supposed to work.
  APFloat RHS(0.0);
  if (!EvaluateFloat(E->getLHS(), Result, Info))
    return false;
  if (!EvaluateFloat(E->getRHS(), RHS, Info))
    return false;

  switch (E->getOpcode()) {
  default: return false;
  case BO_Mul:
    Result.multiply(RHS, APFloat::rmNearestTiesToEven);
    return true;
  case BO_Add:
    Result.add(RHS, APFloat::rmNearestTiesToEven);
    return true;
  case BO_Sub:
    Result.subtract(RHS, APFloat::rmNearestTiesToEven);
    return true;
  case BO_Div:
    Result.divide(RHS, APFloat::rmNearestTiesToEven);
    return true;
  }
}

bool FloatExprEvaluator::VisitFloatingLiteral(const FloatingLiteral *E) {
  Result = E->getValue();
  return true;
}

bool FloatExprEvaluator::VisitCastExpr(const CastExpr *E) {
  const Expr* SubExpr = E->getSubExpr();

  switch (E->getCastKind()) {
  default:
    return ExprEvaluatorBaseTy::VisitCastExpr(E);

  case CK_IntegralToFloating: {
    APSInt IntResult;
    if (!EvaluateInteger(SubExpr, IntResult, Info))
      return false;
    Result = HandleIntToFloatCast(E->getType(), SubExpr->getType(),
                                  IntResult, Info.Ctx);
    return true;
  }

  case CK_FloatingCast: {
    if (!Visit(SubExpr))
      return false;
    Result = HandleFloatToFloatCast(E->getType(), SubExpr->getType(),
                                    Result, Info.Ctx);
    return true;
  }

  case CK_FloatingComplexToReal: {
    ComplexValue V;
    if (!EvaluateComplex(SubExpr, V, Info))
      return false;
    Result = V.getComplexFloatReal();
    return true;
  }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Complex Evaluation (for float and integer)
//===----------------------------------------------------------------------===//

namespace {
class ComplexExprEvaluator
  : public ExprEvaluatorBase<ComplexExprEvaluator, bool> {
  ComplexValue &Result;

public:
  ComplexExprEvaluator(EvalInfo &info, ComplexValue &Result)
    : ExprEvaluatorBaseTy(info), Result(Result) {}

  bool Success(const CCValue &V, const Expr *e) {
    Result.setFrom(V);
    return true;
  }
  bool Error(const Expr *E) {
    return false;
  }

  //===--------------------------------------------------------------------===//
  //                            Visitor Methods
  //===--------------------------------------------------------------------===//

  bool VisitImaginaryLiteral(const ImaginaryLiteral *E);

  bool VisitCastExpr(const CastExpr *E);

  bool VisitBinaryOperator(const BinaryOperator *E);
  bool VisitUnaryOperator(const UnaryOperator *E);
  // FIXME Missing: ImplicitValueInitExpr, InitListExpr
};
} // end anonymous namespace

static bool EvaluateComplex(const Expr *E, ComplexValue &Result,
                            EvalInfo &Info) {
  assert(E->isRValue() && E->getType()->isAnyComplexType());
  return ComplexExprEvaluator(Info, Result).Visit(E);
}

bool ComplexExprEvaluator::VisitImaginaryLiteral(const ImaginaryLiteral *E) {
  const Expr* SubExpr = E->getSubExpr();

  if (SubExpr->getType()->isRealFloatingType()) {
    Result.makeComplexFloat();
    APFloat &Imag = Result.FloatImag;
    if (!EvaluateFloat(SubExpr, Imag, Info))
      return false;

    Result.FloatReal = APFloat(Imag.getSemantics());
    return true;
  } else {
    assert(SubExpr->getType()->isIntegerType() &&
           "Unexpected imaginary literal.");

    Result.makeComplexInt();
    APSInt &Imag = Result.IntImag;
    if (!EvaluateInteger(SubExpr, Imag, Info))
      return false;

    Result.IntReal = APSInt(Imag.getBitWidth(), !Imag.isSigned());
    return true;
  }
}

bool ComplexExprEvaluator::VisitCastExpr(const CastExpr *E) {

  switch (E->getCastKind()) {
  case CK_BitCast:
  case CK_BaseToDerived:
  case CK_DerivedToBase:
  case CK_UncheckedDerivedToBase:
  case CK_Dynamic:
  case CK_ToUnion:
  case CK_ArrayToPointerDecay:
  case CK_FunctionToPointerDecay:
  case CK_NullToPointer:
  case CK_NullToMemberPointer:
  case CK_BaseToDerivedMemberPointer:
  case CK_DerivedToBaseMemberPointer:
  case CK_MemberPointerToBoolean:
  case CK_ConstructorConversion:
  case CK_IntegralToPointer:
  case CK_PointerToIntegral:
  case CK_PointerToBoolean:
  case CK_ToVoid:
  case CK_VectorSplat:
  case CK_IntegralCast:
  case CK_IntegralToBoolean:
  case CK_IntegralToFloating:
  case CK_FloatingToIntegral:
  case CK_FloatingToBoolean:
  case CK_FloatingCast:
  case CK_CPointerToObjCPointerCast:
  case CK_BlockPointerToObjCPointerCast:
  case CK_AnyPointerToBlockPointerCast:
  case CK_ObjCObjectLValueCast:
  case CK_FloatingComplexToReal:
  case CK_FloatingComplexToBoolean:
  case CK_IntegralComplexToReal:
  case CK_IntegralComplexToBoolean:
  case CK_ARCProduceObject:
  case CK_ARCConsumeObject:
  case CK_ARCReclaimReturnedObject:
  case CK_ARCExtendBlockObject:
    llvm_unreachable("invalid cast kind for complex value");

  case CK_LValueToRValue:
  case CK_NoOp:
    return ExprEvaluatorBaseTy::VisitCastExpr(E);

  case CK_Dependent:
  case CK_GetObjCProperty:
  case CK_LValueBitCast:
  case CK_UserDefinedConversion:
    return false;

  case CK_FloatingRealToComplex: {
    APFloat &Real = Result.FloatReal;
    if (!EvaluateFloat(E->getSubExpr(), Real, Info))
      return false;

    Result.makeComplexFloat();
    Result.FloatImag = APFloat(Real.getSemantics());
    return true;
  }

  case CK_FloatingComplexCast: {
    if (!Visit(E->getSubExpr()))
      return false;

    QualType To = E->getType()->getAs<ComplexType>()->getElementType();
    QualType From
      = E->getSubExpr()->getType()->getAs<ComplexType>()->getElementType();

    Result.FloatReal
      = HandleFloatToFloatCast(To, From, Result.FloatReal, Info.Ctx);
    Result.FloatImag
      = HandleFloatToFloatCast(To, From, Result.FloatImag, Info.Ctx);
    return true;
  }

  case CK_FloatingComplexToIntegralComplex: {
    if (!Visit(E->getSubExpr()))
      return false;

    QualType To = E->getType()->getAs<ComplexType>()->getElementType();
    QualType From
      = E->getSubExpr()->getType()->getAs<ComplexType>()->getElementType();
    Result.makeComplexInt();
    Result.IntReal = HandleFloatToIntCast(To, From, Result.FloatReal, Info.Ctx);
    Result.IntImag = HandleFloatToIntCast(To, From, Result.FloatImag, Info.Ctx);
    return true;
  }

  case CK_IntegralRealToComplex: {
    APSInt &Real = Result.IntReal;
    if (!EvaluateInteger(E->getSubExpr(), Real, Info))
      return false;

    Result.makeComplexInt();
    Result.IntImag = APSInt(Real.getBitWidth(), !Real.isSigned());
    return true;
  }

  case CK_IntegralComplexCast: {
    if (!Visit(E->getSubExpr()))
      return false;

    QualType To = E->getType()->getAs<ComplexType>()->getElementType();
    QualType From
      = E->getSubExpr()->getType()->getAs<ComplexType>()->getElementType();

    Result.IntReal = HandleIntToIntCast(To, From, Result.IntReal, Info.Ctx);
    Result.IntImag = HandleIntToIntCast(To, From, Result.IntImag, Info.Ctx);
    return true;
  }

  case CK_IntegralComplexToFloatingComplex: {
    if (!Visit(E->getSubExpr()))
      return false;

    QualType To = E->getType()->getAs<ComplexType>()->getElementType();
    QualType From
      = E->getSubExpr()->getType()->getAs<ComplexType>()->getElementType();
    Result.makeComplexFloat();
    Result.FloatReal = HandleIntToFloatCast(To, From, Result.IntReal, Info.Ctx);
    Result.FloatImag = HandleIntToFloatCast(To, From, Result.IntImag, Info.Ctx);
    return true;
  }
  }

  llvm_unreachable("unknown cast resulting in complex value");
  return false;
}

bool ComplexExprEvaluator::VisitBinaryOperator(const BinaryOperator *E) {
  if (E->getOpcode() == BO_Comma) {
    VisitIgnoredValue(E->getLHS());
    return Visit(E->getRHS());
  }
  if (!Visit(E->getLHS()))
    return false;

  ComplexValue RHS;
  if (!EvaluateComplex(E->getRHS(), RHS, Info))
    return false;

  assert(Result.isComplexFloat() == RHS.isComplexFloat() &&
         "Invalid operands to binary operator.");
  switch (E->getOpcode()) {
  default: return false;
  case BO_Add:
    if (Result.isComplexFloat()) {
      Result.getComplexFloatReal().add(RHS.getComplexFloatReal(),
                                       APFloat::rmNearestTiesToEven);
      Result.getComplexFloatImag().add(RHS.getComplexFloatImag(),
                                       APFloat::rmNearestTiesToEven);
    } else {
      Result.getComplexIntReal() += RHS.getComplexIntReal();
      Result.getComplexIntImag() += RHS.getComplexIntImag();
    }
    break;
  case BO_Sub:
    if (Result.isComplexFloat()) {
      Result.getComplexFloatReal().subtract(RHS.getComplexFloatReal(),
                                            APFloat::rmNearestTiesToEven);
      Result.getComplexFloatImag().subtract(RHS.getComplexFloatImag(),
                                            APFloat::rmNearestTiesToEven);
    } else {
      Result.getComplexIntReal() -= RHS.getComplexIntReal();
      Result.getComplexIntImag() -= RHS.getComplexIntImag();
    }
    break;
  case BO_Mul:
    if (Result.isComplexFloat()) {
      ComplexValue LHS = Result;
      APFloat &LHS_r = LHS.getComplexFloatReal();
      APFloat &LHS_i = LHS.getComplexFloatImag();
      APFloat &RHS_r = RHS.getComplexFloatReal();
      APFloat &RHS_i = RHS.getComplexFloatImag();

      APFloat Tmp = LHS_r;
      Tmp.multiply(RHS_r, APFloat::rmNearestTiesToEven);
      Result.getComplexFloatReal() = Tmp;
      Tmp = LHS_i;
      Tmp.multiply(RHS_i, APFloat::rmNearestTiesToEven);
      Result.getComplexFloatReal().subtract(Tmp, APFloat::rmNearestTiesToEven);

      Tmp = LHS_r;
      Tmp.multiply(RHS_i, APFloat::rmNearestTiesToEven);
      Result.getComplexFloatImag() = Tmp;
      Tmp = LHS_i;
      Tmp.multiply(RHS_r, APFloat::rmNearestTiesToEven);
      Result.getComplexFloatImag().add(Tmp, APFloat::rmNearestTiesToEven);
    } else {
      ComplexValue LHS = Result;
      Result.getComplexIntReal() =
        (LHS.getComplexIntReal() * RHS.getComplexIntReal() -
         LHS.getComplexIntImag() * RHS.getComplexIntImag());
      Result.getComplexIntImag() =
        (LHS.getComplexIntReal() * RHS.getComplexIntImag() +
         LHS.getComplexIntImag() * RHS.getComplexIntReal());
    }
    break;
  case BO_Div:
    if (Result.isComplexFloat()) {
      ComplexValue LHS = Result;
      APFloat &LHS_r = LHS.getComplexFloatReal();
      APFloat &LHS_i = LHS.getComplexFloatImag();
      APFloat &RHS_r = RHS.getComplexFloatReal();
      APFloat &RHS_i = RHS.getComplexFloatImag();
      APFloat &Res_r = Result.getComplexFloatReal();
      APFloat &Res_i = Result.getComplexFloatImag();

      APFloat Den = RHS_r;
      Den.multiply(RHS_r, APFloat::rmNearestTiesToEven);
      APFloat Tmp = RHS_i;
      Tmp.multiply(RHS_i, APFloat::rmNearestTiesToEven);
      Den.add(Tmp, APFloat::rmNearestTiesToEven);

      Res_r = LHS_r;
      Res_r.multiply(RHS_r, APFloat::rmNearestTiesToEven);
      Tmp = LHS_i;
      Tmp.multiply(RHS_i, APFloat::rmNearestTiesToEven);
      Res_r.add(Tmp, APFloat::rmNearestTiesToEven);
      Res_r.divide(Den, APFloat::rmNearestTiesToEven);

      Res_i = LHS_i;
      Res_i.multiply(RHS_r, APFloat::rmNearestTiesToEven);
      Tmp = LHS_r;
      Tmp.multiply(RHS_i, APFloat::rmNearestTiesToEven);
      Res_i.subtract(Tmp, APFloat::rmNearestTiesToEven);
      Res_i.divide(Den, APFloat::rmNearestTiesToEven);
    } else {
      if (RHS.getComplexIntReal() == 0 && RHS.getComplexIntImag() == 0) {
        // FIXME: what about diagnostics?
        return false;
      }
      ComplexValue LHS = Result;
      APSInt Den = RHS.getComplexIntReal() * RHS.getComplexIntReal() +
        RHS.getComplexIntImag() * RHS.getComplexIntImag();
      Result.getComplexIntReal() =
        (LHS.getComplexIntReal() * RHS.getComplexIntReal() +
         LHS.getComplexIntImag() * RHS.getComplexIntImag()) / Den;
      Result.getComplexIntImag() =
        (LHS.getComplexIntImag() * RHS.getComplexIntReal() -
         LHS.getComplexIntReal() * RHS.getComplexIntImag()) / Den;
    }
    break;
  }

  return true;
}

bool ComplexExprEvaluator::VisitUnaryOperator(const UnaryOperator *E) {
  // Get the operand value into 'Result'.
  if (!Visit(E->getSubExpr()))
    return false;

  switch (E->getOpcode()) {
  default:
    // FIXME: what about diagnostics?
    return false;
  case UO_Extension:
    return true;
  case UO_Plus:
    // The result is always just the subexpr.
    return true;
  case UO_Minus:
    if (Result.isComplexFloat()) {
      Result.getComplexFloatReal().changeSign();
      Result.getComplexFloatImag().changeSign();
    }
    else {
      Result.getComplexIntReal() = -Result.getComplexIntReal();
      Result.getComplexIntImag() = -Result.getComplexIntImag();
    }
    return true;
  case UO_Not:
    if (Result.isComplexFloat())
      Result.getComplexFloatImag().changeSign();
    else
      Result.getComplexIntImag() = -Result.getComplexIntImag();
    return true;
  }
}

//===----------------------------------------------------------------------===//
// Top level Expr::EvaluateAsRValue method.
//===----------------------------------------------------------------------===//

static bool Evaluate(CCValue &Result, EvalInfo &Info, const Expr *E) {
  // In C, function designators are not lvalues, but we evaluate them as if they
  // are.
  if (E->isGLValue() || E->getType()->isFunctionType()) {
    LValue LV;
    if (!EvaluateLValue(E, LV, Info))
      return false;
    LV.moveInto(Result);
  } else if (E->getType()->isVectorType()) {
    if (!EvaluateVector(E, Result, Info))
      return false;
  } else if (E->getType()->isIntegralOrEnumerationType()) {
    if (!IntExprEvaluator(Info, Result).Visit(E))
      return false;
  } else if (E->getType()->hasPointerRepresentation()) {
    LValue LV;
    if (!EvaluatePointer(E, LV, Info))
      return false;
    LV.moveInto(Result);
  } else if (E->getType()->isRealFloatingType()) {
    llvm::APFloat F(0.0);
    if (!EvaluateFloat(E, F, Info))
      return false;
    Result = CCValue(F);
  } else if (E->getType()->isAnyComplexType()) {
    ComplexValue C;
    if (!EvaluateComplex(E, C, Info))
      return false;
    C.moveInto(Result);
  } else
    return false;

  return true;
}


/// EvaluateAsRValue - Return true if this is a constant which we can fold using
/// any crazy technique (that has nothing to do with language standards) that
/// we want to.  If this function returns true, it returns the folded constant
/// in Result. If this expression is a glvalue, an lvalue-to-rvalue conversion
/// will be applied to the result.
bool Expr::EvaluateAsRValue(EvalResult &Result, const ASTContext &Ctx) const {
  EvalInfo Info(Ctx, Result);

  CCValue Value;
  if (!::Evaluate(Value, Info, this))
    return false;

  if (isGLValue()) {
    LValue LV;
    LV.setFrom(Value);
    if (!HandleLValueToRValueConversion(Info, getType(), LV, Value))
      return false;
  }

  // Check this core constant expression is a constant expression, and if so,
  // slice it down to one.
  if (!CheckConstantExpression(Value))
    return false;
  Result.Val = Value;
  return true;
}

bool Expr::EvaluateAsBooleanCondition(bool &Result,
                                      const ASTContext &Ctx) const {
  EvalResult Scratch;
  return EvaluateAsRValue(Scratch, Ctx) &&
         HandleConversionToBool(CCValue(Scratch.Val, CCValue::GlobalValue()),
                                Result);
}

bool Expr::EvaluateAsInt(APSInt &Result, const ASTContext &Ctx) const {
  EvalResult ExprResult;
  if (!EvaluateAsRValue(ExprResult, Ctx) || ExprResult.HasSideEffects ||
      !ExprResult.Val.isInt()) {
    return false;
  }
  Result = ExprResult.Val.getInt();
  return true;
}

bool Expr::EvaluateAsLValue(EvalResult &Result, const ASTContext &Ctx) const {
  EvalInfo Info(Ctx, Result);

  LValue LV;
  if (EvaluateLValue(this, LV, Info) && !Result.HasSideEffects &&
      IsGlobalLValue(LV.Base)) {
    Result.Val = APValue(LV.Base, LV.Offset);
    return true;
  }
  return false;
}

bool Expr::EvaluateAsAnyLValue(EvalResult &Result,
                               const ASTContext &Ctx) const {
  EvalInfo Info(Ctx, Result);

  LValue LV;
  if (EvaluateLValue(this, LV, Info)) {
    Result.Val = APValue(LV.Base, LV.Offset);
    return true;
  }
  return false;
}

/// isEvaluatable - Call EvaluateAsRValue to see if this expression can be
/// constant folded, but discard the result.
bool Expr::isEvaluatable(const ASTContext &Ctx) const {
  EvalResult Result;
  return EvaluateAsRValue(Result, Ctx) && !Result.HasSideEffects;
}

bool Expr::HasSideEffects(const ASTContext &Ctx) const {
  return HasSideEffect(Ctx).Visit(this);
}

APSInt Expr::EvaluateKnownConstInt(const ASTContext &Ctx) const {
  EvalResult EvalResult;
  bool Result = EvaluateAsRValue(EvalResult, Ctx);
  (void)Result;
  assert(Result && "Could not evaluate expression");
  assert(EvalResult.Val.isInt() && "Expression did not evaluate to integer");

  return EvalResult.Val.getInt();
}

 bool Expr::EvalResult::isGlobalLValue() const {
   assert(Val.isLValue());
   return IsGlobalLValue(Val.getLValueBase());
 }


/// isIntegerConstantExpr - this recursive routine will test if an expression is
/// an integer constant expression.

/// FIXME: Pass up a reason why! Invalid operation in i-c-e, division by zero,
/// comma, etc
///
/// FIXME: Handle offsetof.  Two things to do:  Handle GCC's __builtin_offsetof
/// to support gcc 4.0+  and handle the idiom GCC recognizes with a null pointer
/// cast+dereference.

// CheckICE - This function does the fundamental ICE checking: the returned
// ICEDiag contains a Val of 0, 1, or 2, and a possibly null SourceLocation.
// Note that to reduce code duplication, this helper does no evaluation
// itself; the caller checks whether the expression is evaluatable, and
// in the rare cases where CheckICE actually cares about the evaluated
// value, it calls into Evalute.
//
// Meanings of Val:
// 0: This expression is an ICE.
// 1: This expression is not an ICE, but if it isn't evaluated, it's
//    a legal subexpression for an ICE. This return value is used to handle
//    the comma operator in C99 mode.
// 2: This expression is not an ICE, and is not a legal subexpression for one.

namespace {

struct ICEDiag {
  unsigned Val;
  SourceLocation Loc;

  public:
  ICEDiag(unsigned v, SourceLocation l) : Val(v), Loc(l) {}
  ICEDiag() : Val(0) {}
};

}

static ICEDiag NoDiag() { return ICEDiag(); }

static ICEDiag CheckEvalInICE(const Expr* E, ASTContext &Ctx) {
  Expr::EvalResult EVResult;
  if (!E->EvaluateAsRValue(EVResult, Ctx) || EVResult.HasSideEffects ||
      !EVResult.Val.isInt()) {
    return ICEDiag(2, E->getLocStart());
  }
  return NoDiag();
}

static ICEDiag CheckICE(const Expr* E, ASTContext &Ctx) {
  assert(!E->isValueDependent() && "Should not see value dependent exprs!");
  if (!E->getType()->isIntegralOrEnumerationType()) {
    return ICEDiag(2, E->getLocStart());
  }

  switch (E->getStmtClass()) {
#define ABSTRACT_STMT(Node)
#define STMT(Node, Base) case Expr::Node##Class:
#define EXPR(Node, Base)
#include "clang/AST/StmtNodes.inc"
  case Expr::PredefinedExprClass:
  case Expr::FloatingLiteralClass:
  case Expr::ImaginaryLiteralClass:
  case Expr::StringLiteralClass:
  case Expr::ArraySubscriptExprClass:
  case Expr::MemberExprClass:
  case Expr::CompoundAssignOperatorClass:
  case Expr::CompoundLiteralExprClass:
  case Expr::ExtVectorElementExprClass:
  case Expr::DesignatedInitExprClass:
  case Expr::ImplicitValueInitExprClass:
  case Expr::ParenListExprClass:
  case Expr::VAArgExprClass:
  case Expr::AddrLabelExprClass:
  case Expr::StmtExprClass:
  case Expr::CXXMemberCallExprClass:
  case Expr::CUDAKernelCallExprClass:
  case Expr::CXXDynamicCastExprClass:
  case Expr::CXXTypeidExprClass:
  case Expr::CXXUuidofExprClass:
  case Expr::CXXNullPtrLiteralExprClass:
  case Expr::CXXThisExprClass:
  case Expr::CXXThrowExprClass:
  case Expr::CXXNewExprClass:
  case Expr::CXXDeleteExprClass:
  case Expr::CXXPseudoDestructorExprClass:
  case Expr::UnresolvedLookupExprClass:
  case Expr::DependentScopeDeclRefExprClass:
  case Expr::CXXConstructExprClass:
  case Expr::CXXBindTemporaryExprClass:
  case Expr::ExprWithCleanupsClass:
  case Expr::CXXTemporaryObjectExprClass:
  case Expr::CXXUnresolvedConstructExprClass:
  case Expr::CXXDependentScopeMemberExprClass:
  case Expr::UnresolvedMemberExprClass:
  case Expr::ObjCStringLiteralClass:
  case Expr::ObjCEncodeExprClass:
  case Expr::ObjCMessageExprClass:
  case Expr::ObjCSelectorExprClass:
  case Expr::ObjCProtocolExprClass:
  case Expr::ObjCIvarRefExprClass:
  case Expr::ObjCPropertyRefExprClass:
  case Expr::ObjCIsaExprClass:
  case Expr::ShuffleVectorExprClass:
  case Expr::BlockExprClass:
  case Expr::BlockDeclRefExprClass:
  case Expr::NoStmtClass:
  case Expr::OpaqueValueExprClass:
  case Expr::PackExpansionExprClass:
  case Expr::SubstNonTypeTemplateParmPackExprClass:
  case Expr::AsTypeExprClass:
  case Expr::ObjCIndirectCopyRestoreExprClass:
  case Expr::MaterializeTemporaryExprClass:
  case Expr::AtomicExprClass:
    return ICEDiag(2, E->getLocStart());

  case Expr::InitListExprClass:
    if (Ctx.getLangOptions().CPlusPlus0x) {
      const InitListExpr *ILE = cast<InitListExpr>(E);
      if (ILE->getNumInits() == 0)
        return NoDiag();
      if (ILE->getNumInits() == 1)
        return CheckICE(ILE->getInit(0), Ctx);
      // Fall through for more than 1 expression.
    }
    return ICEDiag(2, E->getLocStart());

  case Expr::SizeOfPackExprClass:
  case Expr::GNUNullExprClass:
    // GCC considers the GNU __null value to be an integral constant expression.
    return NoDiag();

  case Expr::SubstNonTypeTemplateParmExprClass:
    return
      CheckICE(cast<SubstNonTypeTemplateParmExpr>(E)->getReplacement(), Ctx);

  case Expr::ParenExprClass:
    return CheckICE(cast<ParenExpr>(E)->getSubExpr(), Ctx);
  case Expr::GenericSelectionExprClass:
    return CheckICE(cast<GenericSelectionExpr>(E)->getResultExpr(), Ctx);
  case Expr::IntegerLiteralClass:
  case Expr::CharacterLiteralClass:
  case Expr::CXXBoolLiteralExprClass:
  case Expr::CXXScalarValueInitExprClass:
  case Expr::UnaryTypeTraitExprClass:
  case Expr::BinaryTypeTraitExprClass:
  case Expr::ArrayTypeTraitExprClass:
  case Expr::ExpressionTraitExprClass:
  case Expr::CXXNoexceptExprClass:
    return NoDiag();
  case Expr::CallExprClass:
  case Expr::CXXOperatorCallExprClass: {
    // C99 6.6/3 allows function calls within unevaluated subexpressions of
    // constant expressions, but they can never be ICEs because an ICE cannot
    // contain an operand of (pointer to) function type.
    const CallExpr *CE = cast<CallExpr>(E);
    if (CE->isBuiltinCall(Ctx))
      return CheckEvalInICE(E, Ctx);
    return ICEDiag(2, E->getLocStart());
  }
  case Expr::DeclRefExprClass:
    if (isa<EnumConstantDecl>(cast<DeclRefExpr>(E)->getDecl()))
      return NoDiag();
    if (Ctx.getLangOptions().CPlusPlus && IsConstNonVolatile(E->getType())) {
      const NamedDecl *D = cast<DeclRefExpr>(E)->getDecl();

      // Parameter variables are never constants.  Without this check,
      // getAnyInitializer() can find a default argument, which leads
      // to chaos.
      if (isa<ParmVarDecl>(D))
        return ICEDiag(2, cast<DeclRefExpr>(E)->getLocation());

      // C++ 7.1.5.1p2
      //   A variable of non-volatile const-qualified integral or enumeration
      //   type initialized by an ICE can be used in ICEs.
      if (const VarDecl *Dcl = dyn_cast<VarDecl>(D)) {
        // Look for a declaration of this variable that has an initializer.
        const VarDecl *ID = 0;
        const Expr *Init = Dcl->getAnyInitializer(ID);
        if (Init) {
          if (ID->isInitKnownICE()) {
            // We have already checked whether this subexpression is an
            // integral constant expression.
            if (ID->isInitICE())
              return NoDiag();
            else
              return ICEDiag(2, cast<DeclRefExpr>(E)->getLocation());
          }

          // It's an ICE whether or not the definition we found is
          // out-of-line.  See DR 721 and the discussion in Clang PR
          // 6206 for details.

          if (Dcl->isCheckingICE()) {
            return ICEDiag(2, cast<DeclRefExpr>(E)->getLocation());
          }

          Dcl->setCheckingICE();
          ICEDiag Result = CheckICE(Init, Ctx);
          // Cache the result of the ICE test.
          Dcl->setInitKnownICE(Result.Val == 0);
          return Result;
        }
      }
    }
    return ICEDiag(2, E->getLocStart());
  case Expr::UnaryOperatorClass: {
    const UnaryOperator *Exp = cast<UnaryOperator>(E);
    switch (Exp->getOpcode()) {
    case UO_PostInc:
    case UO_PostDec:
    case UO_PreInc:
    case UO_PreDec:
    case UO_AddrOf:
    case UO_Deref:
      // C99 6.6/3 allows increment and decrement within unevaluated
      // subexpressions of constant expressions, but they can never be ICEs
      // because an ICE cannot contain an lvalue operand.
      return ICEDiag(2, E->getLocStart());
    case UO_Extension:
    case UO_LNot:
    case UO_Plus:
    case UO_Minus:
    case UO_Not:
    case UO_Real:
    case UO_Imag:
      return CheckICE(Exp->getSubExpr(), Ctx);
    }
    
    // OffsetOf falls through here.
  }
  case Expr::OffsetOfExprClass: {
      // Note that per C99, offsetof must be an ICE. And AFAIK, using
      // EvaluateAsRValue matches the proposed gcc behavior for cases like
      // "offsetof(struct s{int x[4];}, x[1.0])".  This doesn't affect
      // compliance: we should warn earlier for offsetof expressions with
      // array subscripts that aren't ICEs, and if the array subscripts
      // are ICEs, the value of the offsetof must be an integer constant.
      return CheckEvalInICE(E, Ctx);
  }
  case Expr::UnaryExprOrTypeTraitExprClass: {
    const UnaryExprOrTypeTraitExpr *Exp = cast<UnaryExprOrTypeTraitExpr>(E);
    if ((Exp->getKind() ==  UETT_SizeOf) &&
        Exp->getTypeOfArgument()->isVariableArrayType())
      return ICEDiag(2, E->getLocStart());
    return NoDiag();
  }
  case Expr::BinaryOperatorClass: {
    const BinaryOperator *Exp = cast<BinaryOperator>(E);
    switch (Exp->getOpcode()) {
    case BO_PtrMemD:
    case BO_PtrMemI:
    case BO_Assign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
    case BO_AndAssign:
    case BO_XorAssign:
    case BO_OrAssign:
      // C99 6.6/3 allows assignments within unevaluated subexpressions of
      // constant expressions, but they can never be ICEs because an ICE cannot
      // contain an lvalue operand.
      return ICEDiag(2, E->getLocStart());

    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_Add:
    case BO_Sub:
    case BO_Shl:
    case BO_Shr:
    case BO_LT:
    case BO_GT:
    case BO_LE:
    case BO_GE:
    case BO_EQ:
    case BO_NE:
    case BO_And:
    case BO_Xor:
    case BO_Or:
    case BO_Comma: {
      ICEDiag LHSResult = CheckICE(Exp->getLHS(), Ctx);
      ICEDiag RHSResult = CheckICE(Exp->getRHS(), Ctx);
      if (Exp->getOpcode() == BO_Div ||
          Exp->getOpcode() == BO_Rem) {
        // EvaluateAsRValue gives an error for undefined Div/Rem, so make sure
        // we don't evaluate one.
        if (LHSResult.Val == 0 && RHSResult.Val == 0) {
          llvm::APSInt REval = Exp->getRHS()->EvaluateKnownConstInt(Ctx);
          if (REval == 0)
            return ICEDiag(1, E->getLocStart());
          if (REval.isSigned() && REval.isAllOnesValue()) {
            llvm::APSInt LEval = Exp->getLHS()->EvaluateKnownConstInt(Ctx);
            if (LEval.isMinSignedValue())
              return ICEDiag(1, E->getLocStart());
          }
        }
      }
      if (Exp->getOpcode() == BO_Comma) {
        if (Ctx.getLangOptions().C99) {
          // C99 6.6p3 introduces a strange edge case: comma can be in an ICE
          // if it isn't evaluated.
          if (LHSResult.Val == 0 && RHSResult.Val == 0)
            return ICEDiag(1, E->getLocStart());
        } else {
          // In both C89 and C++, commas in ICEs are illegal.
          return ICEDiag(2, E->getLocStart());
        }
      }
      if (LHSResult.Val >= RHSResult.Val)
        return LHSResult;
      return RHSResult;
    }
    case BO_LAnd:
    case BO_LOr: {
      ICEDiag LHSResult = CheckICE(Exp->getLHS(), Ctx);

      // C++0x [expr.const]p2:
      //   [...] subexpressions of logical AND (5.14), logical OR
      //   (5.15), and condi- tional (5.16) operations that are not
      //   evaluated are not considered.
      if (Ctx.getLangOptions().CPlusPlus0x && LHSResult.Val == 0) {
        if (Exp->getOpcode() == BO_LAnd && 
            Exp->getLHS()->EvaluateKnownConstInt(Ctx) == 0)
          return LHSResult;

        if (Exp->getOpcode() == BO_LOr &&
            Exp->getLHS()->EvaluateKnownConstInt(Ctx) != 0)
          return LHSResult;
      }

      ICEDiag RHSResult = CheckICE(Exp->getRHS(), Ctx);
      if (LHSResult.Val == 0 && RHSResult.Val == 1) {
        // Rare case where the RHS has a comma "side-effect"; we need
        // to actually check the condition to see whether the side
        // with the comma is evaluated.
        if ((Exp->getOpcode() == BO_LAnd) !=
            (Exp->getLHS()->EvaluateKnownConstInt(Ctx) == 0))
          return RHSResult;
        return NoDiag();
      }

      if (LHSResult.Val >= RHSResult.Val)
        return LHSResult;
      return RHSResult;
    }
    }
  }
  case Expr::ImplicitCastExprClass:
  case Expr::CStyleCastExprClass:
  case Expr::CXXFunctionalCastExprClass:
  case Expr::CXXStaticCastExprClass:
  case Expr::CXXReinterpretCastExprClass:
  case Expr::CXXConstCastExprClass:
  case Expr::ObjCBridgedCastExprClass: {
    const Expr *SubExpr = cast<CastExpr>(E)->getSubExpr();
    if (isa<ExplicitCastExpr>(E) &&
        isa<FloatingLiteral>(SubExpr->IgnoreParenImpCasts()))
      return NoDiag();
    switch (cast<CastExpr>(E)->getCastKind()) {
    case CK_LValueToRValue:
    case CK_NoOp:
    case CK_IntegralToBoolean:
    case CK_IntegralCast:
      return CheckICE(SubExpr, Ctx);
    default:
      return ICEDiag(2, E->getLocStart());
    }
  }
  case Expr::BinaryConditionalOperatorClass: {
    const BinaryConditionalOperator *Exp = cast<BinaryConditionalOperator>(E);
    ICEDiag CommonResult = CheckICE(Exp->getCommon(), Ctx);
    if (CommonResult.Val == 2) return CommonResult;
    ICEDiag FalseResult = CheckICE(Exp->getFalseExpr(), Ctx);
    if (FalseResult.Val == 2) return FalseResult;
    if (CommonResult.Val == 1) return CommonResult;
    if (FalseResult.Val == 1 &&
        Exp->getCommon()->EvaluateKnownConstInt(Ctx) == 0) return NoDiag();
    return FalseResult;
  }
  case Expr::ConditionalOperatorClass: {
    const ConditionalOperator *Exp = cast<ConditionalOperator>(E);
    // If the condition (ignoring parens) is a __builtin_constant_p call,
    // then only the true side is actually considered in an integer constant
    // expression, and it is fully evaluated.  This is an important GNU
    // extension.  See GCC PR38377 for discussion.
    if (const CallExpr *CallCE
        = dyn_cast<CallExpr>(Exp->getCond()->IgnoreParenCasts()))
      if (CallCE->isBuiltinCall(Ctx) == Builtin::BI__builtin_constant_p) {
        Expr::EvalResult EVResult;
        if (!E->EvaluateAsRValue(EVResult, Ctx) || EVResult.HasSideEffects ||
            !EVResult.Val.isInt()) {
          return ICEDiag(2, E->getLocStart());
        }
        return NoDiag();
      }
    ICEDiag CondResult = CheckICE(Exp->getCond(), Ctx);
    if (CondResult.Val == 2)
      return CondResult;

    // C++0x [expr.const]p2:
    //   subexpressions of [...] conditional (5.16) operations that
    //   are not evaluated are not considered
    bool TrueBranch = Ctx.getLangOptions().CPlusPlus0x
      ? Exp->getCond()->EvaluateKnownConstInt(Ctx) != 0
      : false;
    ICEDiag TrueResult = NoDiag();
    if (!Ctx.getLangOptions().CPlusPlus0x || TrueBranch)
      TrueResult = CheckICE(Exp->getTrueExpr(), Ctx);
    ICEDiag FalseResult = NoDiag();
    if (!Ctx.getLangOptions().CPlusPlus0x || !TrueBranch)
      FalseResult = CheckICE(Exp->getFalseExpr(), Ctx);

    if (TrueResult.Val == 2)
      return TrueResult;
    if (FalseResult.Val == 2)
      return FalseResult;
    if (CondResult.Val == 1)
      return CondResult;
    if (TrueResult.Val == 0 && FalseResult.Val == 0)
      return NoDiag();
    // Rare case where the diagnostics depend on which side is evaluated
    // Note that if we get here, CondResult is 0, and at least one of
    // TrueResult and FalseResult is non-zero.
    if (Exp->getCond()->EvaluateKnownConstInt(Ctx) == 0) {
      return FalseResult;
    }
    return TrueResult;
  }
  case Expr::CXXDefaultArgExprClass:
    return CheckICE(cast<CXXDefaultArgExpr>(E)->getExpr(), Ctx);
  case Expr::ChooseExprClass: {
    return CheckICE(cast<ChooseExpr>(E)->getChosenSubExpr(Ctx), Ctx);
  }
  }

  // Silence a GCC warning
  return ICEDiag(2, E->getLocStart());
}

bool Expr::isIntegerConstantExpr(llvm::APSInt &Result, ASTContext &Ctx,
                                 SourceLocation *Loc, bool isEvaluated) const {
  ICEDiag d = CheckICE(this, Ctx);
  if (d.Val != 0) {
    if (Loc) *Loc = d.Loc;
    return false;
  }
  if (!EvaluateAsInt(Result, Ctx))
    llvm_unreachable("ICE cannot be evaluated!");
  return true;
}
