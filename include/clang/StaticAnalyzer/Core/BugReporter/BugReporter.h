//===---  BugReporter.h - Generate PathDiagnostics --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines BugReporter, a utility class for generating
//  PathDiagnostics for analyses based on GRState.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_GR_BUGREPORTER
#define LLVM_CLANG_GR_BUGREPORTER

#include "clang/Basic/SourceLocation.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/GRState.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ImmutableList.h"
#include "llvm/ADT/ImmutableSet.h"
#include "llvm/ADT/SmallSet.h"
#include <list>

namespace clang {

class ASTContext;
class Diagnostic;
class Stmt;
class ParentMap;

namespace ento {

class PathDiagnostic;
class PathDiagnosticPiece;
class PathDiagnosticClient;
class ExplodedNode;
class ExplodedGraph;
class BugReporter;
class BugReporterContext;
class ExprEngine;
class GRState;
class BugType;

//===----------------------------------------------------------------------===//
// Interface for individual bug reports.
//===----------------------------------------------------------------------===//

class BugReporterVisitor : public llvm::FoldingSetNode {
public:
  virtual ~BugReporterVisitor();
  virtual PathDiagnosticPiece* VisitNode(const ExplodedNode* N,
                                         const ExplodedNode* PrevN,
                                         BugReporterContext& BRC) = 0;

  virtual bool isOwnedByReporterContext() { return true; }
  virtual void Profile(llvm::FoldingSetNodeID &ID) const = 0;
};

// FIXME: Combine this with RangedBugReport and remove RangedBugReport.
class BugReport : public BugReporterVisitor {
protected:
  BugType& BT;
  std::string ShortDescription;
  std::string Description;
  const ExplodedNode *ErrorNode;
  mutable SourceRange R;

protected:
  friend class BugReporter;
  friend class BugReportEquivClass;

  virtual void Profile(llvm::FoldingSetNodeID& hash) const {
    hash.AddPointer(&BT);
    hash.AddInteger(getLocation().getRawEncoding());
    hash.AddString(Description);
  }

public:
  class NodeResolver {
  public:
    virtual ~NodeResolver() {}
    virtual const ExplodedNode*
            getOriginalNode(const ExplodedNode* N) = 0;
  };

  BugReport(BugType& bt, StringRef desc, const ExplodedNode *errornode)
    : BT(bt), Description(desc), ErrorNode(errornode) {}

  BugReport(BugType& bt, StringRef shortDesc, StringRef desc,
            const ExplodedNode *errornode)
  : BT(bt), ShortDescription(shortDesc), Description(desc),
    ErrorNode(errornode) {}

  virtual ~BugReport();

  virtual bool isOwnedByReporterContext() { return false; }

  const BugType& getBugType() const { return BT; }
  BugType& getBugType() { return BT; }

  // FIXME: Perhaps this should be moved into a subclass?
  const ExplodedNode* getErrorNode() const { return ErrorNode; }

  // FIXME: Do we need this?  Maybe getLocation() should return a ProgramPoint
  // object.
  // FIXME: If we do need it, we can probably just make it private to
  // BugReporter.
  const Stmt* getStmt() const;

  const StringRef getDescription() const { return Description; }

  const StringRef getShortDescription() const {
    return ShortDescription.empty() ? Description : ShortDescription;
  }

  // FIXME: Is this needed?
  virtual std::pair<const char**,const char**> getExtraDescriptiveText() {
    return std::make_pair((const char**)0,(const char**)0);
  }

  // FIXME: Perhaps move this into a subclass.
  virtual PathDiagnosticPiece* getEndPath(BugReporterContext& BRC,
                                          const ExplodedNode* N);

  /// getLocation - Return the "definitive" location of the reported bug.
  ///  While a bug can span an entire path, usually there is a specific
  ///  location that can be used to identify where the key issue occurred.
  ///  This location is used by clients rendering diagnostics.
  virtual SourceLocation getLocation() const;

  typedef const SourceRange *ranges_iterator;

  /// getRanges - Returns the source ranges associated with this bug.
  virtual std::pair<ranges_iterator, ranges_iterator> getRanges() const;

  virtual PathDiagnosticPiece* VisitNode(const ExplodedNode* N,
                                         const ExplodedNode* PrevN,
                                         BugReporterContext& BR);

  virtual void registerInitialVisitors(BugReporterContext& BRC,
                                       const ExplodedNode* N) {}
};

//===----------------------------------------------------------------------===//
// BugTypes (collections of related reports).
//===----------------------------------------------------------------------===//

class BugReportEquivClass : public llvm::FoldingSetNode {
  // List of *owned* BugReport objects.
  std::list<BugReport*> Reports;

  friend class BugReporter;
  void AddReport(BugReport* R) { Reports.push_back(R); }
public:
  BugReportEquivClass(BugReport* R) { Reports.push_back(R); }
  ~BugReportEquivClass();

  void Profile(llvm::FoldingSetNodeID& ID) const {
    assert(!Reports.empty());
    (*Reports.begin())->Profile(ID);
  }

  class iterator {
    std::list<BugReport*>::iterator impl;
  public:
    iterator(std::list<BugReport*>::iterator i) : impl(i) {}
    iterator& operator++() { ++impl; return *this; }
    bool operator==(const iterator& I) const { return I.impl == impl; }
    bool operator!=(const iterator& I) const { return I.impl != impl; }
    BugReport* operator*() const { return *impl; }
    BugReport* operator->() const { return *impl; }
  };

  class const_iterator {
    std::list<BugReport*>::const_iterator impl;
  public:
    const_iterator(std::list<BugReport*>::const_iterator i) : impl(i) {}
    const_iterator& operator++() { ++impl; return *this; }
    bool operator==(const const_iterator& I) const { return I.impl == impl; }
    bool operator!=(const const_iterator& I) const { return I.impl != impl; }
    const BugReport* operator*() const { return *impl; }
    const BugReport* operator->() const { return *impl; }
  };

  iterator begin() { return iterator(Reports.begin()); }
  iterator end() { return iterator(Reports.end()); }

  const_iterator begin() const { return const_iterator(Reports.begin()); }
  const_iterator end() const { return const_iterator(Reports.end()); }
};


//===----------------------------------------------------------------------===//
// Specialized subclasses of BugReport.
//===----------------------------------------------------------------------===//

// FIXME: Collapse this with the default BugReport class.
class RangedBugReport : public BugReport {
  SmallVector<SourceRange, 4> Ranges;
public:
  RangedBugReport(BugType& D, StringRef description,
                  ExplodedNode *errornode)
    : BugReport(D, description, errornode) {}

  RangedBugReport(BugType& D, StringRef shortDescription,
                  StringRef description, ExplodedNode *errornode)
  : BugReport(D, shortDescription, description, errornode) {}

  ~RangedBugReport();

  // FIXME: Move this out of line.
  void addRange(SourceRange R) {
    assert(R.isValid());
    Ranges.push_back(R);
  }

  virtual std::pair<ranges_iterator, ranges_iterator> getRanges() const {
    return std::make_pair(Ranges.begin(), Ranges.end());
  }
  
  virtual void Profile(llvm::FoldingSetNodeID& hash) const {
    BugReport::Profile(hash);
    for (SmallVectorImpl<SourceRange>::const_iterator I =
          Ranges.begin(), E = Ranges.end(); I != E; ++I) {
      const SourceRange range = *I;
      if (!range.isValid())
        continue;
      hash.AddInteger(range.getBegin().getRawEncoding());
      hash.AddInteger(range.getEnd().getRawEncoding());
    }
  }
};

class EnhancedBugReport : public RangedBugReport {
public:
  typedef void (*VisitorCreator)(BugReporterContext &BRcC, const void *data,
                                 const ExplodedNode *N);

private:
  typedef std::vector<std::pair<VisitorCreator, const void*> > Creators;
  Creators creators;

public:
  EnhancedBugReport(BugType& D, StringRef description,
                    ExplodedNode *errornode)
   : RangedBugReport(D, description, errornode) {}

  EnhancedBugReport(BugType& D, StringRef shortDescription,
                   StringRef description, ExplodedNode *errornode)
    : RangedBugReport(D, shortDescription, description, errornode) {}

  ~EnhancedBugReport() {}

  void registerInitialVisitors(BugReporterContext& BRC, const ExplodedNode* N) {
    for (Creators::iterator I = creators.begin(), E = creators.end(); I!=E; ++I)
      I->first(BRC, I->second, N);
  }

  void addVisitorCreator(VisitorCreator creator, const void *data) {
    creators.push_back(std::make_pair(creator, data));
  }
};

//===----------------------------------------------------------------------===//
// BugReporter and friends.
//===----------------------------------------------------------------------===//

class BugReporterData {
public:
  virtual ~BugReporterData();
  virtual Diagnostic& getDiagnostic() = 0;
  virtual PathDiagnosticClient* getPathDiagnosticClient() = 0;
  virtual ASTContext& getASTContext() = 0;
  virtual SourceManager& getSourceManager() = 0;
};

class BugReporter {
public:
  enum Kind { BaseBRKind, GRBugReporterKind };

private:
  typedef llvm::ImmutableSet<BugType*> BugTypesTy;
  BugTypesTy::Factory F;
  BugTypesTy BugTypes;

  const Kind kind;
  BugReporterData& D;

  void FlushReport(BugReportEquivClass& EQ);

  llvm::FoldingSet<BugReportEquivClass> EQClasses;

protected:
  BugReporter(BugReporterData& d, Kind k) : BugTypes(F.getEmptySet()), kind(k),
                                            D(d) {}

public:
  BugReporter(BugReporterData& d) : BugTypes(F.getEmptySet()), kind(BaseBRKind),
                                    D(d) {}
  virtual ~BugReporter();

  void FlushReports();

  Kind getKind() const { return kind; }

  Diagnostic& getDiagnostic() {
    return D.getDiagnostic();
  }

  PathDiagnosticClient* getPathDiagnosticClient() {
    return D.getPathDiagnosticClient();
  }

  typedef BugTypesTy::iterator iterator;
  iterator begin() { return BugTypes.begin(); }
  iterator end() { return BugTypes.end(); }

  typedef llvm::FoldingSet<BugReportEquivClass>::iterator EQClasses_iterator;
  EQClasses_iterator EQClasses_begin() { return EQClasses.begin(); }
  EQClasses_iterator EQClasses_end() { return EQClasses.end(); }

  ASTContext& getContext() { return D.getASTContext(); }

  SourceManager& getSourceManager() { return D.getSourceManager(); }

  virtual void GeneratePathDiagnostic(PathDiagnostic& pathDiagnostic,
        SmallVectorImpl<BugReport *> &bugReports) {}

  void Register(BugType *BT);

  void EmitReport(BugReport *R);

  void EmitBasicReport(StringRef BugName, StringRef BugStr,
                       SourceLocation Loc,
                       SourceRange* RangeBeg, unsigned NumRanges);

  void EmitBasicReport(StringRef BugName, StringRef BugCategory,
                       StringRef BugStr, SourceLocation Loc,
                       SourceRange* RangeBeg, unsigned NumRanges);


  void EmitBasicReport(StringRef BugName, StringRef BugStr,
                       SourceLocation Loc) {
    EmitBasicReport(BugName, BugStr, Loc, 0, 0);
  }

  void EmitBasicReport(StringRef BugName, StringRef BugCategory,
                       StringRef BugStr, SourceLocation Loc) {
    EmitBasicReport(BugName, BugCategory, BugStr, Loc, 0, 0);
  }

  void EmitBasicReport(StringRef BugName, StringRef BugStr,
                       SourceLocation Loc, SourceRange R) {
    EmitBasicReport(BugName, BugStr, Loc, &R, 1);
  }

  void EmitBasicReport(StringRef BugName, StringRef Category,
                       StringRef BugStr, SourceLocation Loc,
                       SourceRange R) {
    EmitBasicReport(BugName, Category, BugStr, Loc, &R, 1);
  }

  static bool classof(const BugReporter* R) { return true; }

private:
  llvm::StringMap<BugType *> StrBugTypes;

  /// \brief Returns a BugType that is associated with the given name and
  /// category.
  BugType *getBugTypeForName(StringRef name, StringRef category);
};

// FIXME: Get rid of GRBugReporter.  It's the wrong abstraction.
class GRBugReporter : public BugReporter {
  ExprEngine& Eng;
  llvm::SmallSet<SymbolRef, 10> NotableSymbols;
public:
  GRBugReporter(BugReporterData& d, ExprEngine& eng)
    : BugReporter(d, GRBugReporterKind), Eng(eng) {}

  virtual ~GRBugReporter();

  /// getEngine - Return the analysis engine used to analyze a given
  ///  function or method.
  ExprEngine &getEngine() { return Eng; }

  /// getGraph - Get the exploded graph created by the analysis engine
  ///  for the analyzed method or function.
  ExplodedGraph &getGraph();

  /// getStateManager - Return the state manager used by the analysis
  ///  engine.
  GRStateManager &getStateManager();

  virtual void GeneratePathDiagnostic(PathDiagnostic &pathDiagnostic,
                     SmallVectorImpl<BugReport*> &bugReports);

  void addNotableSymbol(SymbolRef Sym) {
    NotableSymbols.insert(Sym);
  }

  bool isNotable(SymbolRef Sym) const {
    return (bool) NotableSymbols.count(Sym);
  }

  /// classof - Used by isa<>, cast<>, and dyn_cast<>.
  static bool classof(const BugReporter* R) {
    return R->getKind() == GRBugReporterKind;
  }
};

class BugReporterContext {
  GRBugReporter &BR;
  // Not the most efficient data structure, but we use an ImmutableList for the
  // Callbacks because it is safe to make additions to list during iteration.
  llvm::ImmutableList<BugReporterVisitor*>::Factory F;
  llvm::ImmutableList<BugReporterVisitor*> Callbacks;
  llvm::FoldingSet<BugReporterVisitor> CallbacksSet;
public:
  BugReporterContext(GRBugReporter& br) : BR(br), Callbacks(F.getEmptyList()) {}
  virtual ~BugReporterContext();

  void addVisitor(BugReporterVisitor* visitor);

  typedef llvm::ImmutableList<BugReporterVisitor*>::iterator visitor_iterator;
  visitor_iterator visitor_begin() { return Callbacks.begin(); }
  visitor_iterator visitor_end() { return Callbacks.end(); }

  GRBugReporter& getBugReporter() { return BR; }

  ExplodedGraph &getGraph() { return BR.getGraph(); }

  void addNotableSymbol(SymbolRef Sym) {
    // FIXME: For now forward to GRBugReporter.
    BR.addNotableSymbol(Sym);
  }

  bool isNotable(SymbolRef Sym) const {
    // FIXME: For now forward to GRBugReporter.
    return BR.isNotable(Sym);
  }

  GRStateManager& getStateManager() {
    return BR.getStateManager();
  }

  SValBuilder& getSValBuilder() {
    return getStateManager().getSValBuilder();
  }

  ASTContext& getASTContext() {
    return BR.getContext();
  }

  SourceManager& getSourceManager() {
    return BR.getSourceManager();
  }

  virtual BugReport::NodeResolver& getNodeResolver() = 0;
};

class DiagBugReport : public RangedBugReport {
  std::list<std::string> Strs;
  FullSourceLoc L;
public:
  DiagBugReport(BugType& D, StringRef desc, FullSourceLoc l) :
  RangedBugReport(D, desc, 0), L(l) {}

  virtual ~DiagBugReport() {}

  // FIXME: Move out-of-line (virtual function).
  SourceLocation getLocation() const { return L; }

  void addString(StringRef s) { Strs.push_back(s); }

  typedef std::list<std::string>::const_iterator str_iterator;
  str_iterator str_begin() const { return Strs.begin(); }
  str_iterator str_end() const { return Strs.end(); }
};

//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

namespace bugreporter {

const Stmt *GetDerefExpr(const ExplodedNode *N);
const Stmt *GetDenomExpr(const ExplodedNode *N);
const Stmt *GetCalleeExpr(const ExplodedNode *N);
const Stmt *GetRetValExpr(const ExplodedNode *N);

void registerConditionVisitor(BugReporterContext &BRC);

void registerTrackNullOrUndefValue(BugReporterContext& BRC, const void *stmt,
                                   const ExplodedNode* N);

void registerFindLastStore(BugReporterContext& BRC, const void *memregion,
                           const ExplodedNode *N);

void registerNilReceiverVisitor(BugReporterContext &BRC);  

void registerVarDeclsLastStore(BugReporterContext &BRC, const void *stmt,
                               const ExplodedNode *N);

} // end namespace clang::bugreporter

//===----------------------------------------------------------------------===//

} // end GR namespace

} // end clang namespace

#endif
