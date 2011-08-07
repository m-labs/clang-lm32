//== CheckerContext.h - Context info for path-sensitive checkers--*- C++ -*--=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines CheckerContext that provides contextual info for
// path-sensitive checkers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SA_CORE_PATHSENSITIVE_CHECKERCONTEXT
#define LLVM_CLANG_SA_CORE_PATHSENSITIVE_CHECKERCONTEXT

#include "clang/Analysis/Support/SaveAndRestore.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"

namespace clang {

namespace ento {

class CheckerContext {
  ExplodedNodeSet &Dst;
  StmtNodeBuilder &B;
  ExprEngine &Eng;
  ExplodedNode *Pred;
  SaveAndRestore<bool> OldSink;
  const void *checkerTag;
  SaveAndRestore<ProgramPoint::Kind> OldPointKind;
  SaveOr OldHasGen;
  const GRState *ST;
  const Stmt *statement;
  const unsigned size;
public:
  bool *respondsToCallback;
public:
  CheckerContext(ExplodedNodeSet &dst, StmtNodeBuilder &builder,
                 ExprEngine &eng, ExplodedNode *pred,
                 const void *tag, ProgramPoint::Kind K,
                 bool *respondsToCB = 0,
                 const Stmt *stmt = 0, const GRState *st = 0)
    : Dst(dst), B(builder), Eng(eng), Pred(pred),
      OldSink(B.BuildSinks),
      checkerTag(tag),
      OldPointKind(B.PointKind, K),
      OldHasGen(B.hasGeneratedNode),
      ST(st), statement(stmt), size(Dst.size()),
      respondsToCallback(respondsToCB) {}

  ~CheckerContext();

  ExprEngine &getEngine() {
    return Eng;
  }

  AnalysisManager &getAnalysisManager() {
    return Eng.getAnalysisManager();
  }

  ConstraintManager &getConstraintManager() {
    return Eng.getConstraintManager();
  }

  StoreManager &getStoreManager() {
    return Eng.getStoreManager();
  }

  ExplodedNodeSet &getNodeSet() { return Dst; }
  StmtNodeBuilder &getNodeBuilder() { return B; }
  ExplodedNode *&getPredecessor() { return Pred; }
  const GRState *getState() { return ST ? ST : B.GetState(Pred); }
  const Stmt *getStmt() const { return statement; }

  ASTContext &getASTContext() {
    return Eng.getContext();
  }
  
  BugReporter &getBugReporter() {
    return Eng.getBugReporter();
  }
  
  SourceManager &getSourceManager() {
    return getBugReporter().getSourceManager();
  }

  SValBuilder &getSValBuilder() {
    return Eng.getSValBuilder();
  }

  SymbolManager &getSymbolManager() {
    return getSValBuilder().getSymbolManager();
  }

  ExplodedNode *generateNode(bool autoTransition = true) {
    assert(statement && "Only transitions with statements currently supported");
    ExplodedNode *N = generateNodeImpl(statement, getState(), false,
                                       checkerTag);
    if (N && autoTransition)
      Dst.Add(N);
    return N;
  }
  
  ExplodedNode *generateNode(const Stmt *stmt, const GRState *state,
                             bool autoTransition = true, const void *tag = 0) {
    assert(state);
    ExplodedNode *N = generateNodeImpl(stmt, state, false,
                                       tag ? tag : checkerTag);
    if (N && autoTransition)
      addTransition(N);
    return N;
  }

  ExplodedNode *generateNode(const GRState *state, ExplodedNode *pred,
                             bool autoTransition = true) {
   assert(statement && "Only transitions with statements currently supported");
    ExplodedNode *N = generateNodeImpl(statement, state, pred, false);
    if (N && autoTransition)
      addTransition(N);
    return N;
  }

  ExplodedNode *generateNode(const GRState *state, bool autoTransition = true,
                             const void *tag = 0) {
    assert(statement && "Only transitions with statements currently supported");
    ExplodedNode *N = generateNodeImpl(statement, state, false,
                                       tag ? tag : checkerTag);
    if (N && autoTransition)
      addTransition(N);
    return N;
  }

  ExplodedNode *generateSink(const Stmt *stmt, const GRState *state = 0) {
    return generateNodeImpl(stmt, state ? state : getState(), true,
                            checkerTag);
  }
  
  ExplodedNode *generateSink(const GRState *state = 0) {
    assert(statement && "Only transitions with statements currently supported");
    return generateNodeImpl(statement, state ? state : getState(), true,
                            checkerTag);
  }

  void addTransition(ExplodedNode *node) {
    Dst.Add(node);
  }
  
  void addTransition(const GRState *state, const void *tag = 0) {
    assert(state);
    // If the 'state' is not new, we need to check if the cached state 'ST'
    // is new.
    if (state != getState() || (ST && ST != B.GetState(Pred)))
      // state is new or equals to ST.
      generateNode(state, true, tag);
    else
      Dst.Add(Pred);
  }

  void EmitReport(BugReport *R) {
    Eng.getBugReporter().EmitReport(R);
  }

  AnalysisContext *getCurrentAnalysisContext() const {
    return Pred->getLocationContext()->getAnalysisContext();
  }

private:
  ExplodedNode *generateNodeImpl(const Stmt* stmt, const GRState *state,
                             bool markAsSink, const void *tag) {
    ExplodedNode *node = B.generateNode(stmt, state, Pred, tag);
    if (markAsSink && node)
      node->markAsSink();
    return node;
  }

  ExplodedNode *generateNodeImpl(const Stmt* stmt, const GRState *state,
                                 ExplodedNode *pred, bool markAsSink) {
   ExplodedNode *node = B.generateNode(stmt, state, pred, checkerTag);
    if (markAsSink && node)
      node->markAsSink();
    return node;
  }
};

} // end GR namespace

} // end clang namespace

#endif
