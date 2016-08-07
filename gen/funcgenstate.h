//===-- gen/funcgenstate.h - Function code generation state -----*- C++ -*-===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//
//
// "Global" transitory state kept while emitting LLVM IR for the body of a
// single function, with FuncGenState being the top-level such entity.
//
//===----------------------------------------------------------------------===//

#ifndef LDC_GEN_FUNCGENSTATE_H
#define LDC_GEN_FUNCGENSTATE_H

#include "gen/irstate.h"
#include "gen/pgo.h"
#include "gen/trycatchfinally.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/CallSite.h"
#include <vector>

class Identifier;
struct IRState;
class Statement;

namespace llvm {
class AllocaInst;
class BasicBlock;
class Constant;
class MDNode;
class Value;
}

/// Stores information needed to correctly jump to a given label or loop/switch
/// statement (break/continue can be labeled, but are not necessarily).
struct JumpTarget {
  /// The basic block to ultimately branch to.
  llvm::BasicBlock *targetBlock = nullptr;

  /// The index of the target in the stack of active cleanup scopes.
  ///
  /// When generating code for a jump to this label, the cleanups between
  /// the current depth and that of the level will be emitted. Note that
  /// we need to handle only one direction (towards the root of the stack)
  /// because D forbids gotos into try or finally blocks.
  // TODO: We might not be able to detect illegal jumps across try-finally
  // blocks by only storing the index.
  CleanupCursor cleanupScope;

  /// Keeps target of the associated loop or switch statement so we can
  /// handle both unlabeled and labeled jumps.
  Statement *targetStatement = nullptr;

  JumpTarget() = default;
  JumpTarget(llvm::BasicBlock *targetBlock, CleanupCursor cleanupScope,
             Statement *targetStatement);
};

/// Keeps track of source and target label of a goto.
///
/// Used if we cannot immediately emit all the code for a jump because we have
/// not generated code for the target yet.
struct GotoJump {
  // The location of the goto instruction, for error reporting.
  Loc sourceLoc;

  /// The basic block which contains the goto as its terminator.
  llvm::BasicBlock *sourceBlock = nullptr;

  /// While we have not found the actual branch target, we might need to
  /// create a "fake" basic block in order to be able to execute the cleanups
  /// (we do not keep branching information around after leaving the scope).
  llvm::BasicBlock *tentativeTarget = nullptr;

  /// The label to target with the goto.
  Identifier *targetLabel = nullptr;

  GotoJump(Loc loc, llvm::BasicBlock *sourceBlock,
           llvm::BasicBlock *tentativeTarget, Identifier *targetLabel);
};

/// Keeps track of active (abstract) scopes in a function that influence code
/// generation of their contents. This includes cleanups (finally blocks,
/// destructors), try/catch blocks and labels for goto/break/continue.
///
/// Note that the entire code generation process, and this class in particular,
/// depends heavily on the fact that we visit the statement/expression tree in
/// its natural order, i.e. depth-first and in lexical order. In other words,
/// the code here expects that after a cleanup/catch/loop/etc. has been pushed,
/// the contents of the block are generated, and it is then popped again
/// afterwards. This is also encoded in the fact that none of the methods for
/// branching/running cleanups take a cursor for describing the "source" scope,
/// it is always assumed to be the current one.
///
/// Handling of break/continue could be moved into a separate layer that uses
/// the rest of the ScopeStack API, as it (in contrast to goto) never requires
/// resolving forward references across cleanup scopes.
class ScopeStack {
public:
  explicit ScopeStack(IRState &irs) : irs(irs), tryCatchFinallyScopes(irs) {
    unresolvedGotosPerCleanupScope.emplace_back();
  }
  ~ScopeStack();

  /// Registers a piece of cleanup code to be run.
  ///
  /// The end block is expected not to contain a terminator yet. It will be
  /// added by ScopeStack as needed, based on what follow-up blocks code from
  /// within this scope will branch to.
  void pushCleanup(llvm::BasicBlock *beginBlock, llvm::BasicBlock *endBlock) {
    tryCatchFinallyScopes.pushCleanup(beginBlock, endBlock);
    unresolvedGotosPerCleanupScope.emplace_back();
  }

  /// Terminates the current basic block with a branch to the cleanups needed
  /// for leaving the current scope and continuing execution at the target
  /// scope stack level.
  ///
  /// After running them, execution will branch to the given basic block.
  void runCleanups(CleanupCursor targetScope, llvm::BasicBlock *continueWith) {
    tryCatchFinallyScopes.runCleanups(targetScope, continueWith);
  }

  /// Pops all the cleanups between the current scope and the target cursor.
  ///
  /// This does not insert any cleanup calls, use #runCleanups() beforehand.
  void popCleanups(CleanupCursor targetScope);

  /// Returns a cursor that identifies the current cleanup scope, to be later
  /// used with #runCleanups() et al.
  ///
  /// Note that this cursor is only valid as long as the current scope is not
  /// popped.
  CleanupCursor currentCleanupScope() {
    return tryCatchFinallyScopes.currentCleanupScope();
  }

  /// Registers a try-catch scope.
  void pushTryCatch(TryCatchStatement *stmt, llvm::BasicBlock *endbb) {
    tryCatchFinallyScopes.pushTryCatch(stmt, endbb);
  }

  /// Unregisters the last registered try-catch scope.
  void popTryCatch() { tryCatchFinallyScopes.popTryCatch(); }

  /// Registers a loop statement to be used as a target for break/continue
  /// statements in the current scope.
  void pushLoopTarget(Statement *loopStatement,
                      llvm::BasicBlock *continueTarget,
                      llvm::BasicBlock *breakTarget);

  /// Pops the last pushed loop target, so it is no longer taken into
  /// consideration for resolving breaks/continues.
  void popLoopTarget();

  /// Registers a statement to be used as a target for break statements in the
  /// current scope (currently applies only to switch statements).
  void pushBreakTarget(Statement *switchStatement,
                       llvm::BasicBlock *targetBlock);

  /// Unregisters the last registered break target.
  void popBreakTarget();

  /// Adds a label to serve as a target for goto statements.
  ///
  /// Also causes in-flight forward references to this label to be resolved.
  void addLabelTarget(Identifier *labelName, llvm::BasicBlock *targetBlock);

  /// Emits a call or invoke to the given callee, depending on whether there
  /// are catches/cleanups active or not.
  template <typename T>
  llvm::CallSite callOrInvoke(llvm::Value *callee, const T &args,
                              const char *name = "", bool isNothrow = false);

  /// Terminates the current basic block with an unconditional branch to the
  /// given label, along with the cleanups to execute on the way there.
  ///
  /// Legal forward references (i.e. within the same function, and not into
  /// a cleanup scope) will be resolved.
  void jumpToLabel(Loc loc, Identifier *labelName);

  /// Terminates the current basic block with an unconditional branch to the
  /// continue target generated by the given loop statement, along with
  /// the cleanups to execute on the way there.
  void continueWithLoop(Statement *loopStatement) {
    jumpToStatement(continueTargets, loopStatement);
  }

  /// Terminates the current basic block with an unconditional branch to the
  /// closest loop continue target, along with the cleanups to execute on
  /// the way there.
  void continueWithClosest() { jumpToClosest(continueTargets); }

  /// Terminates the current basic block with an unconditional branch to the
  /// break target generated by the given loop or switch statement, along with
  /// the cleanups to execute on the way there.
  void breakToStatement(Statement *loopOrSwitchStatement) {
    jumpToStatement(breakTargets, loopOrSwitchStatement);
  }

  /// Terminates the current basic block with an unconditional branch to the
  /// closest break statement target, along with the cleanups to execute on
  /// the way there.
  void breakToClosest() { jumpToClosest(breakTargets); }

private:
  std::vector<GotoJump> &currentUnresolvedGotos();

  /// Unified implementation for labeled break/continue.
  void jumpToStatement(std::vector<JumpTarget> &targets,
                       Statement *loopOrSwitchStatement);

  /// Unified implementation for unlabeled break/continue.
  void jumpToClosest(std::vector<JumpTarget> &targets);

  /// The ambient IRState. For legacy reasons, there is currently a cyclic
  /// dependency between the two.
  IRState &irs;

  using LabelTargetMap = llvm::DenseMap<Identifier *, JumpTarget>;
  /// The labels we have encountered in this function so far, accessed by
  /// their associated identifier (i.e. the name of the label).
  LabelTargetMap labelTargets;

  ///
  std::vector<JumpTarget> breakTargets;

  ///
  std::vector<JumpTarget> continueTargets;

  ///
  TryCatchFinallyScopes tryCatchFinallyScopes;

  /// Keeps track of all the gotos originating from somewhere inside this
  /// scope for which we have not found the label yet (because it occurs
  /// lexically later in the function).
  // Note: Should also be a dense map from source block to the rest of the
  // data if we expect many gotos.
  using Gotos = std::vector<GotoJump>;
  /// The first element represents the stack of unresolved top-level gotos
  /// (no cleanups).
  std::vector<Gotos> unresolvedGotosPerCleanupScope;
};

template <typename T>
llvm::CallSite ScopeStack::callOrInvoke(llvm::Value *callee, const T &args,
                                        const char *name, bool isNothrow) {
  // If this is a direct call, we might be able to use the callee attributes
  // to our advantage.
  llvm::Function *calleeFn = llvm::dyn_cast<llvm::Function>(callee);

  // Ignore 'nothrow' if there are active catch blocks handling non-Exception
  // Throwables.
  if (isNothrow && tryCatchFinallyScopes.isCatchingNonExceptions())
    isNothrow = false;

  // Intrinsics don't support invoking and 'nounwind' functions don't need it.
  const bool doesNotThrow =
      isNothrow ||
      (calleeFn && (calleeFn->isIntrinsic() || calleeFn->doesNotThrow()));

#if LDC_LLVM_VER >= 308
  // calls inside a funclet must be annotated with its value
  llvm::SmallVector<llvm::OperandBundleDef, 2> BundleList;
#endif

  if (doesNotThrow || tryCatchFinallyScopes.empty()) {
    llvm::CallInst *call = irs.ir->CreateCall(callee, args,
#if LDC_LLVM_VER >= 308
                                              BundleList,
#endif
                                              name);
    if (calleeFn) {
      call->setAttributes(calleeFn->getAttributes());
    }
    return call;
  }

  llvm::BasicBlock *landingPad = tryCatchFinallyScopes.getLandingPad();

  llvm::BasicBlock *postinvoke = irs.insertBB("postinvoke");
  llvm::InvokeInst *invoke =
      irs.ir->CreateInvoke(callee, postinvoke, landingPad, args,
#if LDC_LLVM_VER >= 308
                           BundleList,
#endif
                           name);
  if (calleeFn) {
    invoke->setAttributes(calleeFn->getAttributes());
  }

  irs.scope() = IRScope(postinvoke);
  return invoke;
}

/// Tracks the basic blocks corresponding to the switch `case`s (and `default`s)
/// in a given function.
///
/// Since the bb for a given case must already be known when a jump to it is
/// to be emitted (at which point the former might not have been emitted yet,
/// e.g. when goto-ing forward), we lazily create them as needed.
class SwitchCaseTargets {
public:
  explicit SwitchCaseTargets(llvm::Function *llFunc) : llFunc(llFunc) {}

  /// Returns the basic block associated with the given case/default statement,
  /// asserting that it has already been created.
  llvm::BasicBlock *get(Statement *stmt);

  /// Returns the basic block associated with the given case/default statement
  /// or creates one with the given name if it does not already exist
  llvm::BasicBlock *getOrCreate(Statement *stmt, const llvm::Twine &name);

private:
  llvm::Function *const llFunc;
  llvm::DenseMap<Statement *, llvm::BasicBlock *> targetBBs;
};

/// The "global" transitory state necessary for emitting the body of a certain
/// function.
///
/// For general metadata associated with a function that persists for the entire
/// IRState lifetime (i.e. llvm::Module emission process) see IrFunction.
class FuncGenState {
public:
  explicit FuncGenState(IrFunction &irFunc, IRState &irs);

  FuncGenState(FuncGenState const &) = delete;
  FuncGenState &operator=(FuncGenState const &) = delete;

  /// Returns the stack slot that contains the exception object pointer while a
  /// landing pad is active, lazily creating it as needed.
  ///
  /// This value must dominate all uses; first storing it, and then loading it
  /// when calling _d_eh_resume_unwind. If we take a select at the end of any
  /// cleanups on the way to the latter, the value must also dominate all other
  /// predecessors of the cleanup. Thus, we just use a single alloca in the
  /// entry BB of the function.
  llvm::AllocaInst *getOrCreateEhPtrSlot();

  /// Returns the basic block with the call to the unwind resume function.
  ///
  /// Because of ehPtrSlot, we do not need more than one, so might as well
  /// save on code size and reuse it.
  llvm::BasicBlock *getOrCreateResumeUnwindBlock();

  // The function code is being generated for.
  IrFunction &irFunc;

  /// The stack of scopes inside the function.
  ScopeStack scopes;

  // PGO information
  CodeGenPGO pgo;

  /// Tracks basic blocks corresponding to switch cases.
  SwitchCaseTargets switchTargets;

  /// The marker at which to insert `alloca`s in the function entry bb.
  llvm::Instruction *allocapoint = nullptr;

  /// alloca for the nested context of this function
  llvm::Value *nestedVar = nullptr;

  /// The basic block with the return instruction.
  llvm::BasicBlock *retBlock = nullptr;

  /// A stack slot containing the return value, for functions that return by
  /// value.
  llvm::AllocaInst *retValSlot = nullptr;

  /// Similar story to ehPtrSlot, but for the selector value.
  llvm::AllocaInst *ehSelectorSlot = nullptr;

private:
  IRState &irs;
  llvm::AllocaInst *ehPtrSlot = nullptr;
  llvm::BasicBlock *resumeUnwindBlock = nullptr;
};

#endif
