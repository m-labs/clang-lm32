//===--- Tranforms.cpp - Tranformations to ARC mode -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Transforms.h"
#include "Internals.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/ParentMap.h"
#include "clang/Analysis/DomainSpecific/CocoaConventions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/DenseSet.h"
#include <map>

using namespace clang;
using namespace arcmt;
using namespace trans;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// Helpers.
//===----------------------------------------------------------------------===//

/// \brief 'Loc' is the end of a statement range. This returns the location
/// immediately after the semicolon following the statement.
/// If no semicolon is found or the location is inside a macro, the returned
/// source location will be invalid.
SourceLocation trans::findLocationAfterSemi(SourceLocation loc,
                                            ASTContext &Ctx) {
  SourceManager &SM = Ctx.getSourceManager();
  if (loc.isMacroID()) {
    if (!SM.isAtEndOfMacroInstantiation(loc))
      return SourceLocation();
    loc = SM.getInstantiationRange(loc).second;
  }
  loc = Lexer::getLocForEndOfToken(loc, /*Offset=*/0, SM, Ctx.getLangOptions());

  // Break down the source location.
  std::pair<FileID, unsigned> locInfo = SM.getDecomposedLoc(loc);

  // Try to load the file buffer.
  bool invalidTemp = false;
  llvm::StringRef file = SM.getBufferData(locInfo.first, &invalidTemp);
  if (invalidTemp)
    return SourceLocation();

  const char *tokenBegin = file.data() + locInfo.second;

  // Lex from the start of the given location.
  Lexer lexer(SM.getLocForStartOfFile(locInfo.first),
              Ctx.getLangOptions(),
              file.begin(), tokenBegin, file.end());
  Token tok;
  lexer.LexFromRawLexer(tok);
  if (tok.isNot(tok::semi))
    return SourceLocation();

  return tok.getLocation().getFileLocWithOffset(1);
}

bool trans::hasSideEffects(Expr *E, ASTContext &Ctx) {
  if (!E || !E->HasSideEffects(Ctx))
    return false;

  E = E->IgnoreParenCasts();
  ObjCMessageExpr *ME = dyn_cast<ObjCMessageExpr>(E);
  if (!ME)
    return true;
  switch (ME->getMethodFamily()) {
  case OMF_autorelease:
  case OMF_dealloc:
  case OMF_release:
  case OMF_retain:
    switch (ME->getReceiverKind()) {
    case ObjCMessageExpr::SuperInstance:
      return false;
    case ObjCMessageExpr::Instance:
      return hasSideEffects(ME->getInstanceReceiver(), Ctx);
    default:
      break;
    }
    break;
  default:
    break;
  }

  return true;
}

namespace {

class ReferenceClear : public RecursiveASTVisitor<ReferenceClear> {
  ExprSet &Refs;
public:
  ReferenceClear(ExprSet &refs) : Refs(refs) { }
  bool VisitDeclRefExpr(DeclRefExpr *E) { Refs.erase(E); return true; }
  bool VisitBlockDeclRefExpr(BlockDeclRefExpr *E) { Refs.erase(E); return true; }
};

class ReferenceCollector : public RecursiveASTVisitor<ReferenceCollector> {
  ValueDecl *Dcl;
  ExprSet &Refs;

public:
  ReferenceCollector(ValueDecl *D, ExprSet &refs)
    : Dcl(D), Refs(refs) { }

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    if (E->getDecl() == Dcl)
      Refs.insert(E);
    return true;
  }

  bool VisitBlockDeclRefExpr(BlockDeclRefExpr *E) {
    if (E->getDecl() == Dcl)
      Refs.insert(E);
    return true;
  }
};

class RemovablesCollector : public RecursiveASTVisitor<RemovablesCollector> {
  ExprSet &Removables;

public:
  RemovablesCollector(ExprSet &removables)
  : Removables(removables) { }
  
  bool shouldWalkTypesOfTypeLocs() const { return false; }
  
  bool TraverseStmtExpr(StmtExpr *E) {
    CompoundStmt *S = E->getSubStmt();
    for (CompoundStmt::body_iterator
        I = S->body_begin(), E = S->body_end(); I != E; ++I) {
      if (I != E - 1)
        mark(*I);
      TraverseStmt(*I);
    }
    return true;
  }
  
  bool VisitCompoundStmt(CompoundStmt *S) {
    for (CompoundStmt::body_iterator
        I = S->body_begin(), E = S->body_end(); I != E; ++I)
      mark(*I);
    return true;
  }
  
  bool VisitIfStmt(IfStmt *S) {
    mark(S->getThen());
    mark(S->getElse());
    return true;
  }
  
  bool VisitWhileStmt(WhileStmt *S) {
    mark(S->getBody());
    return true;
  }
  
  bool VisitDoStmt(DoStmt *S) {
    mark(S->getBody());
    return true;
  }
  
  bool VisitForStmt(ForStmt *S) {
    mark(S->getInit());
    mark(S->getInc());
    mark(S->getBody());
    return true;
  }
  
private:
  void mark(Stmt *S) {
    if (!S) return;
    
    if (LabelStmt *Label = dyn_cast<LabelStmt>(S))
      return mark(Label->getSubStmt());
    if (ImplicitCastExpr *CE = dyn_cast<ImplicitCastExpr>(S))
      return mark(CE->getSubExpr());
    if (ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(S))
      return mark(EWC->getSubExpr());
    if (Expr *E = dyn_cast<Expr>(S))
      Removables.insert(E);
  }
};

} // end anonymous namespace

void trans::clearRefsIn(Stmt *S, ExprSet &refs) {
  ReferenceClear(refs).TraverseStmt(S);
}

void trans::collectRefs(ValueDecl *D, Stmt *S, ExprSet &refs) {
  ReferenceCollector(D, refs).TraverseStmt(S);
}

void trans::collectRemovables(Stmt *S, ExprSet &exprs) {
  RemovablesCollector(exprs).TraverseStmt(S);
}

//===----------------------------------------------------------------------===//
// getAllTransformations.
//===----------------------------------------------------------------------===//

static void independentTransforms(MigrationPass &pass) {
  rewriteAutoreleasePool(pass);
  changeIvarsOfAssignProperties(pass);
  removeRetainReleaseDealloc(pass);
  rewriteUnusedInitDelegate(pass);
  removeZeroOutPropsInDealloc(pass);
  makeAssignARCSafe(pass);
  rewriteUnbridgedCasts(pass);
  rewriteBlockObjCVariable(pass);
}

std::vector<TransformFn> arcmt::getAllTransformations() {
  std::vector<TransformFn> transforms;

  transforms.push_back(independentTransforms);
  // This depends on previous transformations removing various expressions.
  transforms.push_back(removeEmptyStatementsAndDealloc);

  return transforms;
}
