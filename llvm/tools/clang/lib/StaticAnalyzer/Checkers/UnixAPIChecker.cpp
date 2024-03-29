//= UnixAPIChecker.h - Checks preconditions for various Unix APIs --*- C++ -*-//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This defines UnixAPIChecker, which is an assortment of checks on calls
// to various, widely used UNIX/Posix functions.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSwitch.h"
#include <fcntl.h>

using namespace clang;
using namespace ento;
using llvm::Optional;

namespace {
class UnixAPIChecker : public Checker< check::PreStmt<CallExpr> > {
  mutable llvm::OwningPtr<BugType> BT_open, BT_pthreadOnce, BT_mallocZero;
  mutable Optional<uint64_t> Val_O_CREAT;

public:
  void checkPreStmt(const CallExpr *CE, CheckerContext &C) const;

  void CheckOpen(CheckerContext &C, const CallExpr *CE) const;
  void CheckPthreadOnce(CheckerContext &C, const CallExpr *CE) const;
  void CheckCallocZero(CheckerContext &C, const CallExpr *CE) const;
  void CheckMallocZero(CheckerContext &C, const CallExpr *CE) const;
  void CheckReallocZero(CheckerContext &C, const CallExpr *CE) const;

  typedef void (UnixAPIChecker::*SubChecker)(CheckerContext &,
                                             const CallExpr *) const;
private:
  bool ReportZeroByteAllocation(CheckerContext &C,
                                const ProgramState *falseState,
                                const Expr *arg,
                                const char *fn_name) const;
};
} //end anonymous namespace

//===----------------------------------------------------------------------===//
// Utility functions.
//===----------------------------------------------------------------------===//

static inline void LazyInitialize(llvm::OwningPtr<BugType> &BT,
                                  const char *name) {
  if (BT)
    return;
  BT.reset(new BugType(name, "Unix API"));
}

//===----------------------------------------------------------------------===//
// "open" (man 2 open)
//===----------------------------------------------------------------------===//

void UnixAPIChecker::CheckOpen(CheckerContext &C, const CallExpr *CE) const {
  // The definition of O_CREAT is platform specific.  We need a better way
  // of querying this information from the checking environment.
  if (!Val_O_CREAT.hasValue()) {
    if (C.getASTContext().getTargetInfo().getTriple().getVendor() 
                                                      == llvm::Triple::Apple)
      Val_O_CREAT = 0x0200;
    else {
      // FIXME: We need a more general way of getting the O_CREAT value.
      // We could possibly grovel through the preprocessor state, but
      // that would require passing the Preprocessor object to the ExprEngine.
      return;
    }
  }

  // Look at the 'oflags' argument for the O_CREAT flag.
  const ProgramState *state = C.getState();

  if (CE->getNumArgs() < 2) {
    // The frontend should issue a warning for this case, so this is a sanity
    // check.
    return;
  }

  // Now check if oflags has O_CREAT set.
  const Expr *oflagsEx = CE->getArg(1);
  const SVal V = state->getSVal(oflagsEx);
  if (!isa<NonLoc>(V)) {
    // The case where 'V' can be a location can only be due to a bad header,
    // so in this case bail out.
    return;
  }
  NonLoc oflags = cast<NonLoc>(V);
  NonLoc ocreateFlag =
    cast<NonLoc>(C.getSValBuilder().makeIntVal(Val_O_CREAT.getValue(),
                                                oflagsEx->getType()));
  SVal maskedFlagsUC = C.getSValBuilder().evalBinOpNN(state, BO_And,
                                                      oflags, ocreateFlag,
                                                      oflagsEx->getType());
  if (maskedFlagsUC.isUnknownOrUndef())
    return;
  DefinedSVal maskedFlags = cast<DefinedSVal>(maskedFlagsUC);

  // Check if maskedFlags is non-zero.
  const ProgramState *trueState, *falseState;
  llvm::tie(trueState, falseState) = state->assume(maskedFlags);

  // Only emit an error if the value of 'maskedFlags' is properly
  // constrained;
  if (!(trueState && !falseState))
    return;

  if (CE->getNumArgs() < 3) {
    ExplodedNode *N = C.generateSink(trueState);
    if (!N)
      return;

    LazyInitialize(BT_open, "Improper use of 'open'");

    BugReport *report =
      new BugReport(*BT_open,
                            "Call to 'open' requires a third argument when "
                            "the 'O_CREAT' flag is set", N);
    report->addRange(oflagsEx->getSourceRange());
    C.EmitReport(report);
  }
}

//===----------------------------------------------------------------------===//
// pthread_once
//===----------------------------------------------------------------------===//

void UnixAPIChecker::CheckPthreadOnce(CheckerContext &C,
                                      const CallExpr *CE) const {

  // This is similar to 'CheckDispatchOnce' in the MacOSXAPIChecker.
  // They can possibly be refactored.

  if (CE->getNumArgs() < 1)
    return;

  // Check if the first argument is stack allocated.  If so, issue a warning
  // because that's likely to be bad news.
  const ProgramState *state = C.getState();
  const MemRegion *R = state->getSVal(CE->getArg(0)).getAsRegion();
  if (!R || !isa<StackSpaceRegion>(R->getMemorySpace()))
    return;

  ExplodedNode *N = C.generateSink(state);
  if (!N)
    return;

  llvm::SmallString<256> S;
  llvm::raw_svector_ostream os(S);
  os << "Call to 'pthread_once' uses";
  if (const VarRegion *VR = dyn_cast<VarRegion>(R))
    os << " the local variable '" << VR->getDecl()->getName() << '\'';
  else
    os << " stack allocated memory";
  os << " for the \"control\" value.  Using such transient memory for "
  "the control value is potentially dangerous.";
  if (isa<VarRegion>(R) && isa<StackLocalsSpaceRegion>(R->getMemorySpace()))
    os << "  Perhaps you intended to declare the variable as 'static'?";

  LazyInitialize(BT_pthreadOnce, "Improper use of 'pthread_once'");

  BugReport *report = new BugReport(*BT_pthreadOnce, os.str(), N);
  report->addRange(CE->getArg(0)->getSourceRange());
  C.EmitReport(report);
}

//===----------------------------------------------------------------------===//
// "calloc",  "malloc" and "realloc" with allocation size 0
//===----------------------------------------------------------------------===//

// Returns true if we try to do a zero byte allocation, false otherwise.
// Fills in trueState and falseState.
static bool IsZeroByteAllocation(const ProgramState *state,
                                const SVal argVal,
                                const ProgramState **trueState,
                                const ProgramState **falseState) {
  llvm::tie(*trueState, *falseState) = state->assume(cast<DefinedSVal>(argVal));
  return (*falseState && !*trueState);
}

// Generates an error report, indicating that the function whose name is given
// will perform a zero byte allocation.
// Returns false if an error occured, true otherwise.
bool UnixAPIChecker::ReportZeroByteAllocation(CheckerContext &C,
                                              const ProgramState *falseState,
                                              const Expr *arg,
                                              const char *fn_name) const {
  ExplodedNode *N = C.generateSink(falseState);
  if (!N)
    return false;

  // FIXME: Add reference to CERT advisory, and/or C99 standard in bug
  // output.
  LazyInitialize(BT_mallocZero, "Undefined allocation of 0 bytes");

  llvm::SmallString<256> S;
  llvm::raw_svector_ostream os(S);    
  os << "Call to '" << fn_name << "' has an allocation size of 0 bytes";
  BugReport *report = new BugReport(*BT_mallocZero, os.str(), N);

  report->addRange(arg->getSourceRange());
  report->addVisitor(bugreporter::getTrackNullOrUndefValueVisitor(N, arg));
  C.EmitReport(report);

  return true;
}

void UnixAPIChecker::CheckCallocZero(CheckerContext &C,
                                     const CallExpr *CE) const {
  unsigned int nArgs = CE->getNumArgs();
  if (nArgs != 2)
    return;

  const ProgramState *state = C.getState();
  const ProgramState *trueState = NULL, *falseState = NULL;

  unsigned int i;
  for (i = 0; i < nArgs; i++) {
    const Expr *arg = CE->getArg(i);
    SVal argVal = state->getSVal(arg);
    if (argVal.isUnknownOrUndef()) {
      if (i == 0)
        continue;
      else
        return;
    }

    if (IsZeroByteAllocation(state, argVal, &trueState, &falseState)) {
      if (ReportZeroByteAllocation(C, falseState, arg, "calloc"))
        return;
      else if (i == 0)
        continue;
      else
        return;
    }
  }

  // Assume the the value is non-zero going forward.
  assert(trueState);
  if (trueState != state)
    C.addTransition(trueState);
}

// FIXME: Eventually this should be rolled into the MallocChecker, but this
// check is more basic and is valuable for widespread use.
void UnixAPIChecker::CheckMallocZero(CheckerContext &C,
                                     const CallExpr *CE) const {
  // Sanity check that malloc takes one argument.
  if (CE->getNumArgs() != 1)
    return;

  // Check if the allocation size is 0.
  const ProgramState *state = C.getState();
  const ProgramState *trueState = NULL, *falseState = NULL;
  const Expr *arg = CE->getArg(0);
  SVal argVal = state->getSVal(arg);

  if (argVal.isUnknownOrUndef())
    return;

  // Is the value perfectly constrained to zero?
  if (IsZeroByteAllocation(state, argVal, &trueState, &falseState)) {
    (void) ReportZeroByteAllocation(C, falseState, arg, "malloc"); 
    return;
  }
  // Assume the the value is non-zero going forward.
  assert(trueState);
  if (trueState != state)
    C.addTransition(trueState);
}

void UnixAPIChecker::CheckReallocZero(CheckerContext &C,
                                      const CallExpr *CE) const {
  if (CE->getNumArgs() != 2)
    return;

  const ProgramState *state = C.getState();
  const ProgramState *trueState = NULL, *falseState = NULL;
  const Expr *arg = CE->getArg(1);
  SVal argVal = state->getSVal(arg);

  if (argVal.isUnknownOrUndef())
    return;

  if (IsZeroByteAllocation(state, argVal, &trueState, &falseState)) {
    ReportZeroByteAllocation(C, falseState, arg, "realloc");
    return;
  }

  // Assume the the value is non-zero going forward.
  assert(trueState);
  if (trueState != state)
    C.addTransition(trueState);
}
  
//===----------------------------------------------------------------------===//
// Central dispatch function.
//===----------------------------------------------------------------------===//

void UnixAPIChecker::checkPreStmt(const CallExpr *CE, CheckerContext &C) const {
  StringRef FName = C.getCalleeName(CE);
  if (FName.empty())
    return;

  SubChecker SC =
    llvm::StringSwitch<SubChecker>(FName)
      .Case("open", &UnixAPIChecker::CheckOpen)
      .Case("pthread_once", &UnixAPIChecker::CheckPthreadOnce)
      .Case("calloc", &UnixAPIChecker::CheckCallocZero)
      .Case("malloc", &UnixAPIChecker::CheckMallocZero)
      .Case("realloc", &UnixAPIChecker::CheckReallocZero)
      .Default(NULL);

  if (SC)
    (this->*SC)(C, CE);
}

//===----------------------------------------------------------------------===//
// Registration.
//===----------------------------------------------------------------------===//

void ento::registerUnixAPIChecker(CheckerManager &mgr) {
  mgr.registerChecker<UnixAPIChecker>();
}
