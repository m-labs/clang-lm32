//= ProgramState.cpp - Path-Sensitive "State" for tracking values --*- C++ -*--=
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements ProgramState and ProgramStateManager.
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/CFG.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SubEngine.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/TaintManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;

// Give the vtable for ConstraintManager somewhere to live.
// FIXME: Move this elsewhere.
ConstraintManager::~ConstraintManager() {}

ProgramState::ProgramState(ProgramStateManager *mgr, const Environment& env,
                 StoreRef st, GenericDataMap gdm)
  : stateMgr(mgr),
    Env(env),
    store(st.getStore()),
    GDM(gdm),
    refCount(0) {
  stateMgr->getStoreManager().incrementReferenceCount(store);
}

ProgramState::ProgramState(const ProgramState &RHS)
    : llvm::FoldingSetNode(),
      stateMgr(RHS.stateMgr),
      Env(RHS.Env),
      store(RHS.store),
      GDM(RHS.GDM),
      refCount(0) {
  stateMgr->getStoreManager().incrementReferenceCount(store);
}

ProgramState::~ProgramState() {
  if (store)
    stateMgr->getStoreManager().decrementReferenceCount(store);
}

ProgramStateManager::~ProgramStateManager() {
  for (GDMContextsTy::iterator I=GDMContexts.begin(), E=GDMContexts.end();
       I!=E; ++I)
    I->second.second(I->second.first);
}

const ProgramState*
ProgramStateManager::removeDeadBindings(const ProgramState *state,
                                   const StackFrameContext *LCtx,
                                   SymbolReaper& SymReaper) {

  // This code essentially performs a "mark-and-sweep" of the VariableBindings.
  // The roots are any Block-level exprs and Decls that our liveness algorithm
  // tells us are live.  We then see what Decls they may reference, and keep
  // those around.  This code more than likely can be made faster, and the
  // frequency of which this method is called should be experimented with
  // for optimum performance.
  ProgramState NewState = *state;

  NewState.Env = EnvMgr.removeDeadBindings(NewState.Env, SymReaper, state);

  // Clean up the store.
  StoreRef newStore = StoreMgr->removeDeadBindings(NewState.getStore(), LCtx,
                                                   SymReaper);
  NewState.setStore(newStore);
  SymReaper.setReapedStore(newStore);
  
  return getPersistentState(NewState);
}

const ProgramState *ProgramStateManager::MarshalState(const ProgramState *state,
                                            const StackFrameContext *InitLoc) {
  // make up an empty state for now.
  ProgramState State(this,
                EnvMgr.getInitialEnvironment(),
                StoreMgr->getInitialStore(InitLoc),
                GDMFactory.getEmptyMap());

  return getPersistentState(State);
}

const ProgramState *ProgramState::bindCompoundLiteral(const CompoundLiteralExpr *CL,
                                            const LocationContext *LC,
                                            SVal V) const {
  const StoreRef &newStore = 
    getStateManager().StoreMgr->BindCompoundLiteral(getStore(), CL, LC, V);
  return makeWithStore(newStore);
}

const ProgramState *ProgramState::bindDecl(const VarRegion* VR, SVal IVal) const {
  const StoreRef &newStore =
    getStateManager().StoreMgr->BindDecl(getStore(), VR, IVal);
  return makeWithStore(newStore);
}

const ProgramState *ProgramState::bindDeclWithNoInit(const VarRegion* VR) const {
  const StoreRef &newStore =
    getStateManager().StoreMgr->BindDeclWithNoInit(getStore(), VR);
  return makeWithStore(newStore);
}

const ProgramState *ProgramState::bindLoc(Loc LV, SVal V) const {
  ProgramStateManager &Mgr = getStateManager();
  const ProgramState *newState = makeWithStore(Mgr.StoreMgr->Bind(getStore(), 
                                                             LV, V));
  const MemRegion *MR = LV.getAsRegion();
  if (MR && Mgr.getOwningEngine())
    return Mgr.getOwningEngine()->processRegionChange(newState, MR);

  return newState;
}

const ProgramState *ProgramState::bindDefault(SVal loc, SVal V) const {
  ProgramStateManager &Mgr = getStateManager();
  const MemRegion *R = cast<loc::MemRegionVal>(loc).getRegion();
  const StoreRef &newStore = Mgr.StoreMgr->BindDefault(getStore(), R, V);
  const ProgramState *new_state = makeWithStore(newStore);
  return Mgr.getOwningEngine() ? 
           Mgr.getOwningEngine()->processRegionChange(new_state, R) : 
           new_state;
}

const ProgramState *
ProgramState::invalidateRegions(ArrayRef<const MemRegion *> Regions,
                                const Expr *E, unsigned Count,
                                StoreManager::InvalidatedSymbols *IS,
                                bool invalidateGlobals) const {
  if (!IS) {
    StoreManager::InvalidatedSymbols invalidated;
    return invalidateRegionsImpl(Regions, E, Count,
                                 invalidated, invalidateGlobals);
  }
  return invalidateRegionsImpl(Regions, E, Count, *IS, invalidateGlobals);
}

const ProgramState *
ProgramState::invalidateRegionsImpl(ArrayRef<const MemRegion *> Regions,
                                    const Expr *E, unsigned Count,
                                    StoreManager::InvalidatedSymbols &IS,
                                    bool invalidateGlobals) const {
  ProgramStateManager &Mgr = getStateManager();
  SubEngine* Eng = Mgr.getOwningEngine();
 
  if (Eng && Eng->wantsRegionChangeUpdate(this)) {
    StoreManager::InvalidatedRegions Invalidated;
    const StoreRef &newStore
      = Mgr.StoreMgr->invalidateRegions(getStore(), Regions, E, Count, IS,
                                        invalidateGlobals, &Invalidated);
    const ProgramState *newState = makeWithStore(newStore);
    return Eng->processRegionChanges(newState, &IS, Regions, Invalidated);
  }

  const StoreRef &newStore =
    Mgr.StoreMgr->invalidateRegions(getStore(), Regions, E, Count, IS,
                                    invalidateGlobals, NULL);
  return makeWithStore(newStore);
}

const ProgramState *ProgramState::unbindLoc(Loc LV) const {
  assert(!isa<loc::MemRegionVal>(LV) && "Use invalidateRegion instead.");

  Store OldStore = getStore();
  const StoreRef &newStore = getStateManager().StoreMgr->Remove(OldStore, LV);

  if (newStore.getStore() == OldStore)
    return this;

  return makeWithStore(newStore);
}

const ProgramState *ProgramState::enterStackFrame(const StackFrameContext *frame) const {
  const StoreRef &new_store =
    getStateManager().StoreMgr->enterStackFrame(this, frame);
  return makeWithStore(new_store);
}

SVal ProgramState::getSValAsScalarOrLoc(const MemRegion *R) const {
  // We only want to do fetches from regions that we can actually bind
  // values.  For example, SymbolicRegions of type 'id<...>' cannot
  // have direct bindings (but their can be bindings on their subregions).
  if (!R->isBoundable())
    return UnknownVal();

  if (const TypedValueRegion *TR = dyn_cast<TypedValueRegion>(R)) {
    QualType T = TR->getValueType();
    if (Loc::isLocType(T) || T->isIntegerType())
      return getSVal(R);
  }

  return UnknownVal();
}

SVal ProgramState::getSVal(Loc location, QualType T) const {
  SVal V = getRawSVal(cast<Loc>(location), T);

  // If 'V' is a symbolic value that is *perfectly* constrained to
  // be a constant value, use that value instead to lessen the burden
  // on later analysis stages (so we have less symbolic values to reason
  // about).
  if (!T.isNull()) {
    if (SymbolRef sym = V.getAsSymbol()) {
      if (const llvm::APSInt *Int = getSymVal(sym)) {
        // FIXME: Because we don't correctly model (yet) sign-extension
        // and truncation of symbolic values, we need to convert
        // the integer value to the correct signedness and bitwidth.
        //
        // This shows up in the following:
        //
        //   char foo();
        //   unsigned x = foo();
        //   if (x == 54)
        //     ...
        //
        //  The symbolic value stored to 'x' is actually the conjured
        //  symbol for the call to foo(); the type of that symbol is 'char',
        //  not unsigned.
        const llvm::APSInt &NewV = getBasicVals().Convert(T, *Int);
        
        if (isa<Loc>(V))
          return loc::ConcreteInt(NewV);
        else
          return nonloc::ConcreteInt(NewV);
      }
    }
  }
  
  return V;
}

const ProgramState *ProgramState::BindExpr(const Stmt *S, SVal V, bool Invalidate) const{
  Environment NewEnv = getStateManager().EnvMgr.bindExpr(Env, S, V,
                                                         Invalidate);
  if (NewEnv == Env)
    return this;

  ProgramState NewSt = *this;
  NewSt.Env = NewEnv;
  return getStateManager().getPersistentState(NewSt);
}

const ProgramState *ProgramState::bindExprAndLocation(const Stmt *S, SVal location,
                                            SVal V) const {
  Environment NewEnv =
    getStateManager().EnvMgr.bindExprAndLocation(Env, S, location, V);

  if (NewEnv == Env)
    return this;
  
  ProgramState NewSt = *this;
  NewSt.Env = NewEnv;
  return getStateManager().getPersistentState(NewSt);
}

const ProgramState *ProgramState::assumeInBound(DefinedOrUnknownSVal Idx,
                                      DefinedOrUnknownSVal UpperBound,
                                      bool Assumption) const {
  if (Idx.isUnknown() || UpperBound.isUnknown())
    return this;

  // Build an expression for 0 <= Idx < UpperBound.
  // This is the same as Idx + MIN < UpperBound + MIN, if overflow is allowed.
  // FIXME: This should probably be part of SValBuilder.
  ProgramStateManager &SM = getStateManager();
  SValBuilder &svalBuilder = SM.getSValBuilder();
  ASTContext &Ctx = svalBuilder.getContext();

  // Get the offset: the minimum value of the array index type.
  BasicValueFactory &BVF = svalBuilder.getBasicValueFactory();
  // FIXME: This should be using ValueManager::ArrayindexTy...somehow.
  QualType indexTy = Ctx.IntTy;
  nonloc::ConcreteInt Min(BVF.getMinValue(indexTy));

  // Adjust the index.
  SVal newIdx = svalBuilder.evalBinOpNN(this, BO_Add,
                                        cast<NonLoc>(Idx), Min, indexTy);
  if (newIdx.isUnknownOrUndef())
    return this;

  // Adjust the upper bound.
  SVal newBound =
    svalBuilder.evalBinOpNN(this, BO_Add, cast<NonLoc>(UpperBound),
                            Min, indexTy);

  if (newBound.isUnknownOrUndef())
    return this;

  // Build the actual comparison.
  SVal inBound = svalBuilder.evalBinOpNN(this, BO_LT,
                                cast<NonLoc>(newIdx), cast<NonLoc>(newBound),
                                Ctx.IntTy);
  if (inBound.isUnknownOrUndef())
    return this;

  // Finally, let the constraint manager take care of it.
  ConstraintManager &CM = SM.getConstraintManager();
  return CM.assume(this, cast<DefinedSVal>(inBound), Assumption);
}

const ProgramState *ProgramStateManager::getInitialState(const LocationContext *InitLoc) {
  ProgramState State(this,
                EnvMgr.getInitialEnvironment(),
                StoreMgr->getInitialStore(InitLoc),
                GDMFactory.getEmptyMap());

  return getPersistentState(State);
}

void ProgramStateManager::recycleUnusedStates() {
  for (std::vector<ProgramState*>::iterator i = recentlyAllocatedStates.begin(),
       e = recentlyAllocatedStates.end(); i != e; ++i) {
    ProgramState *state = *i;
    if (state->referencedByExplodedNode())
      continue;
    StateSet.RemoveNode(state);
    freeStates.push_back(state);
    state->~ProgramState();
  }
  recentlyAllocatedStates.clear();
}

const ProgramState *ProgramStateManager::getPersistentStateWithGDM(
                                                     const ProgramState *FromState,
                                                     const ProgramState *GDMState) {
  ProgramState NewState = *FromState;
  NewState.GDM = GDMState->GDM;
  return getPersistentState(NewState);
}

const ProgramState *ProgramStateManager::getPersistentState(ProgramState &State) {

  llvm::FoldingSetNodeID ID;
  State.Profile(ID);
  void *InsertPos;

  if (ProgramState *I = StateSet.FindNodeOrInsertPos(ID, InsertPos))
    return I;

  ProgramState *newState = 0;
  if (!freeStates.empty()) {
    newState = freeStates.back();
    freeStates.pop_back();    
  }
  else {
    newState = (ProgramState*) Alloc.Allocate<ProgramState>();
  }
  new (newState) ProgramState(State);
  StateSet.InsertNode(newState, InsertPos);
  recentlyAllocatedStates.push_back(newState);
  return newState;
}

const ProgramState *ProgramState::makeWithStore(const StoreRef &store) const {
  ProgramState NewSt = *this;
  NewSt.setStore(store);
  return getStateManager().getPersistentState(NewSt);
}

void ProgramState::setStore(const StoreRef &newStore) {
  Store newStoreStore = newStore.getStore();
  if (newStoreStore)
    stateMgr->getStoreManager().incrementReferenceCount(newStoreStore);
  if (store)
    stateMgr->getStoreManager().decrementReferenceCount(store);
  store = newStoreStore;
}

//===----------------------------------------------------------------------===//
//  State pretty-printing.
//===----------------------------------------------------------------------===//

static bool IsEnvLoc(const Stmt *S) {
  // FIXME: This is a layering violation.  Should be in environment.
  return (bool) (((uintptr_t) S) & 0x1);
}

void ProgramState::print(raw_ostream &Out, CFG *C,
                         const char *NL, const char *Sep) const {
  // Print the store.
  ProgramStateManager &Mgr = getStateManager();
  Mgr.getStoreManager().print(getStore(), Out, NL, Sep);
  bool isFirst = true;

  // FIXME: All environment printing should be moved inside Environment.
  if (C) {
    // Print Subexpression bindings.
    for (Environment::iterator I = Env.begin(), E = Env.end(); I != E; ++I) {
      if (C->isBlkExpr(I.getKey()) || IsEnvLoc(I.getKey()))
        continue;

      if (isFirst) {
        Out << NL << NL << "Sub-Expressions:" << NL;
        isFirst = false;
      } else {
        Out << NL;
      }

      Out << " (" << (void*) I.getKey() << ") ";
      LangOptions LO; // FIXME.
      I.getKey()->printPretty(Out, 0, PrintingPolicy(LO));
      Out << " : " << I.getData();
    }

    // Print block-expression bindings.
    isFirst = true;
    for (Environment::iterator I = Env.begin(), E = Env.end(); I != E; ++I) {
      if (!C->isBlkExpr(I.getKey()))
        continue;

      if (isFirst) {
        Out << NL << NL << "Block-level Expressions:" << NL;
        isFirst = false;
      } else {
        Out << NL;
      }

      Out << " (" << (void*) I.getKey() << ") ";
      LangOptions LO; // FIXME.
      I.getKey()->printPretty(Out, 0, PrintingPolicy(LO));
      Out << " : " << I.getData();
    }
  } else {
    // Print All bindings - no info to differentiate block from subexpressions.
    for (Environment::iterator I = Env.begin(), E = Env.end(); I != E; ++I) {
      if (IsEnvLoc(I.getKey()))
        continue;

      if (isFirst) {
        Out << NL << NL << "Expressions:" << NL;
        isFirst = false;
      } else {
        Out << NL;
      }

      Out << " (" << (void*) I.getKey() << ") ";
      LangOptions LO; // FIXME.
      I.getKey()->printPretty(Out, 0, PrintingPolicy(LO));
      Out << " : " << I.getData();
    }
  }

  // Print locations.
  isFirst = true;
  
  for (Environment::iterator I = Env.begin(), E = Env.end(); I != E; ++I) {
    if (!IsEnvLoc(I.getKey()))
      continue;
    
    if (isFirst) {
      Out << NL << NL << "Load/store locations:" << NL;
      isFirst = false;
    } else {
      Out << NL;
    }

    const Stmt *S = (Stmt*) (((uintptr_t) I.getKey()) & ((uintptr_t) ~0x1));
    
    Out << " (" << (void*) S << ") ";
    LangOptions LO; // FIXME.
    S->printPretty(Out, 0, PrintingPolicy(LO));
    Out << " : " << I.getData();
  }

  Mgr.getConstraintManager().print(this, Out, NL, Sep);

  // Print checker-specific data.
  Mgr.getOwningEngine()->printState(Out, this, NL, Sep);
}

void ProgramState::printDOT(raw_ostream &Out, CFG &C) const {
  print(Out, &C, "\\l", "\\|");
}

void ProgramState::dump(CFG &C) const {
  print(llvm::errs(), &C);
}

void ProgramState::dump() const {
  print(llvm::errs(), 0);
}

//===----------------------------------------------------------------------===//
// Generic Data Map.
//===----------------------------------------------------------------------===//

void *const* ProgramState::FindGDM(void *K) const {
  return GDM.lookup(K);
}

void*
ProgramStateManager::FindGDMContext(void *K,
                               void *(*CreateContext)(llvm::BumpPtrAllocator&),
                               void (*DeleteContext)(void*)) {

  std::pair<void*, void (*)(void*)>& p = GDMContexts[K];
  if (!p.first) {
    p.first = CreateContext(Alloc);
    p.second = DeleteContext;
  }

  return p.first;
}

const ProgramState *ProgramStateManager::addGDM(const ProgramState *St, void *Key, void *Data){
  ProgramState::GenericDataMap M1 = St->getGDM();
  ProgramState::GenericDataMap M2 = GDMFactory.add(M1, Key, Data);

  if (M1 == M2)
    return St;

  ProgramState NewSt = *St;
  NewSt.GDM = M2;
  return getPersistentState(NewSt);
}

const ProgramState *ProgramStateManager::removeGDM(const ProgramState *state, void *Key) {
  ProgramState::GenericDataMap OldM = state->getGDM();
  ProgramState::GenericDataMap NewM = GDMFactory.remove(OldM, Key);

  if (NewM == OldM)
    return state;

  ProgramState NewState = *state;
  NewState.GDM = NewM;
  return getPersistentState(NewState);
}

bool ScanReachableSymbols::scan(nonloc::CompoundVal val) {
  for (nonloc::CompoundVal::iterator I=val.begin(), E=val.end(); I!=E; ++I)
    if (!scan(*I))
      return false;

  return true;
}

bool ScanReachableSymbols::scan(const SymExpr *sym) {
  unsigned &isVisited = visited[sym];
  if (isVisited)
    return true;
  isVisited = 1;
  
  if (!visitor.VisitSymbol(sym))
    return false;
  
  // TODO: should be rewritten using SymExpr::symbol_iterator.
  switch (sym->getKind()) {
    case SymExpr::RegionValueKind:
    case SymExpr::ConjuredKind:
    case SymExpr::DerivedKind:
    case SymExpr::ExtentKind:
    case SymExpr::MetadataKind:
      break;
    case SymExpr::CastSymbolKind:
      return scan(cast<SymbolCast>(sym)->getOperand());
    case SymExpr::SymIntKind:
      return scan(cast<SymIntExpr>(sym)->getLHS());
    case SymExpr::SymSymKind: {
      const SymSymExpr *x = cast<SymSymExpr>(sym);
      return scan(x->getLHS()) && scan(x->getRHS());
    }
  }
  return true;
}

bool ScanReachableSymbols::scan(SVal val) {
  if (loc::MemRegionVal *X = dyn_cast<loc::MemRegionVal>(&val))
    return scan(X->getRegion());

  if (nonloc::LocAsInteger *X = dyn_cast<nonloc::LocAsInteger>(&val))
    return scan(X->getLoc());

  if (SymbolRef Sym = val.getAsSymbol())
    return scan(Sym);

  if (const SymExpr *Sym = val.getAsSymbolicExpression())
    return scan(Sym);

  if (nonloc::CompoundVal *X = dyn_cast<nonloc::CompoundVal>(&val))
    return scan(*X);

  return true;
}

bool ScanReachableSymbols::scan(const MemRegion *R) {
  if (isa<MemSpaceRegion>(R))
    return true;
  
  unsigned &isVisited = visited[R];
  if (isVisited)
    return true;
  isVisited = 1;

  // If this is a symbolic region, visit the symbol for the region.
  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(R))
    if (!visitor.VisitSymbol(SR->getSymbol()))
      return false;

  // If this is a subregion, also visit the parent regions.
  if (const SubRegion *SR = dyn_cast<SubRegion>(R))
    if (!scan(SR->getSuperRegion()))
      return false;

  // Now look at the binding to this region (if any).
  if (!scan(state->getSValAsScalarOrLoc(R)))
    return false;

  // Now look at the subregions.
  if (!SRM.get())
    SRM.reset(state->getStateManager().getStoreManager().
                                           getSubRegionMap(state->getStore()));

  return SRM->iterSubRegions(R, *this);
}

bool ProgramState::scanReachableSymbols(SVal val, SymbolVisitor& visitor) const {
  ScanReachableSymbols S(this, visitor);
  return S.scan(val);
}

bool ProgramState::scanReachableSymbols(const SVal *I, const SVal *E,
                                   SymbolVisitor &visitor) const {
  ScanReachableSymbols S(this, visitor);
  for ( ; I != E; ++I) {
    if (!S.scan(*I))
      return false;
  }
  return true;
}

bool ProgramState::scanReachableSymbols(const MemRegion * const *I,
                                   const MemRegion * const *E,
                                   SymbolVisitor &visitor) const {
  ScanReachableSymbols S(this, visitor);
  for ( ; I != E; ++I) {
    if (!S.scan(*I))
      return false;
  }
  return true;
}

const ProgramState* ProgramState::addTaint(const Stmt *S,
                                           TaintTagType Kind) const {
  SymbolRef Sym = getSVal(S).getAsSymbol();
  assert(Sym && "Cannot add taint to statements whose value is not a symbol");
  return addTaint(Sym, Kind);
}

const ProgramState* ProgramState::addTaint(SymbolRef Sym,
                                           TaintTagType Kind) const {
  const ProgramState *NewState = set<TaintMap>(Sym, Kind);
  assert(NewState);
  return NewState;
}

bool ProgramState::isTainted(const Stmt *S, TaintTagType Kind) const {
  SVal val = getSVal(S);
  return isTainted(val, Kind);
}

bool ProgramState::isTainted(SVal V, TaintTagType Kind) const {
  if (const SymExpr *Sym = V.getAsSymExpr())
    return isTainted(Sym, Kind);
  if (loc::MemRegionVal *RegVal = dyn_cast<loc::MemRegionVal>(&V))
    return isTainted(RegVal->getRegion(), Kind);
  return false;
}

bool ProgramState::isTainted(const MemRegion *Reg, TaintTagType K) const {
  if (!Reg)
    return false;

  // Element region (array element) is tainted if either the base or the offset
  // are tainted.
  if (const ElementRegion *ER = dyn_cast<ElementRegion>(Reg))
    return isTainted(ER->getSuperRegion(), K) || isTainted(ER->getIndex(), K);

  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(Reg))
    return isTainted(SR->getSymbol(), K);

  if (const SubRegion *ER = dyn_cast<SubRegion>(Reg))
    return isTainted(ER->getSuperRegion(), K);

  return false;
}

bool ProgramState::isTainted(const SymExpr* Sym, TaintTagType Kind) const {
  if (!Sym)
    return false;
  
  // Traverse all the symbols this symbol depends on to see if any are tainted.
  bool Tainted = false;
  for (SymExpr::symbol_iterator SI = Sym->symbol_begin(), SE =Sym->symbol_end();
       SI != SE; ++SI) {
    assert(isa<SymbolData>(*SI));
    const TaintTagType *Tag = get<TaintMap>(*SI);
    Tainted = (Tag && *Tag == Kind);

    // If this is a SymbolDerived with a tainted parent, it's also tainted.
    if (const SymbolDerived *SD = dyn_cast<SymbolDerived>(*SI))
      Tainted = Tainted || isTainted(SD->getParentSymbol(), Kind);
    if (Tainted)
      return true;
  }
  
  return Tainted;
}
