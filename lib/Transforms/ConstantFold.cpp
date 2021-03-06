//===- ConstantFold.cpp - Pass that does constant folding -----------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "mlir/AffineOps/AffineOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/Pass/Pass.h"
#include "mlir/StandardOps/Ops.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"

using namespace mlir;

namespace {
/// Simple constant folding pass.
struct ConstantFold : public FunctionPass<ConstantFold> {
  // All constants in the function post folding.
  SmallVector<Value *, 8> existingConstants;
  // Operations that were folded and that need to be erased.
  std::vector<Operation *> opInstsToErase;

  void foldOperation(Operation *op);
  void runOnFunction() override;
};
} // end anonymous namespace

/// Attempt to fold the specified operation, updating the IR to match.  If
/// constants are found, we keep track of them in the existingConstants list.
///
void ConstantFold::foldOperation(Operation *op) {
  // If this operation is already a constant, just remember it for cleanup
  // later, and don't try to fold it.
  if (auto constant = op->dyn_cast<ConstantOp>()) {
    existingConstants.push_back(constant);
    return;
  }

  // Get values for operands that are trivial constants. nullptr is used as
  // placeholder for non-constant operands.
  SmallVector<Attribute, 8> operandConstants;
  for (auto *operand : op->getOperands()) {
    Attribute operandCst = nullptr;
    if (auto *operandOp = operand->getDefiningOp()) {
      if (auto operandConstantOp = operandOp->dyn_cast<ConstantOp>())
        operandCst = operandConstantOp.getValue();
    }
    operandConstants.push_back(operandCst);
  }

  // Attempt to constant fold the operation.
  SmallVector<Attribute, 8> resultConstants;
  if (failed(op->constantFold(operandConstants, resultConstants)))
    return;

  // Ok, if everything succeeded, then we can create constants corresponding
  // to the result of the call.
  // TODO: We can try to reuse existing constants if we see them laying
  // around.
  assert(resultConstants.size() == op->getNumResults() &&
         "constant folding produced the wrong number of results");

  FuncBuilder builder(op);
  for (unsigned i = 0, e = op->getNumResults(); i != e; ++i) {
    auto *res = op->getResult(i);
    if (res->use_empty()) // ignore dead uses.
      continue;

    auto cst = builder.create<ConstantOp>(op->getLoc(), res->getType(),
                                          resultConstants[i]);
    existingConstants.push_back(cst);
    res->replaceAllUsesWith(cst);
  }

  // At this point the operation is dead, so we can remove it.  We add it to
  // a vector to avoid invalidating our walker.
  opInstsToErase.push_back(op);
}

// For now, we do a simple top-down pass over a function folding constants.  We
// don't handle conditional control flow, block arguments, folding
// conditional branches, or anything else fancy.
void ConstantFold::runOnFunction() {
  existingConstants.clear();
  opInstsToErase.clear();

  getFunction().walk([&](Operation *op) { foldOperation(op); });

  // At this point, these operations are dead, remove them.
  // TODO: This is assuming that all constant foldable operations have no
  // side effects.  When we have side effect modeling, we should verify that
  // the operation is effect-free before we remove it.  Until then this is
  // close enough.
  for (auto *op : opInstsToErase) {
    op->erase();
  }

  // By the time we are done, we may have simplified a bunch of code, leaving
  // around dead constants.  Check for them now and remove them.
  for (auto *cst : existingConstants) {
    if (cst->use_empty())
      cst->getDefiningOp()->erase();
  }
}

/// Creates a constant folding pass.
FunctionPassBase *mlir::createConstantFoldPass() { return new ConstantFold(); }

static PassRegistration<ConstantFold>
    pass("constant-fold", "Constant fold operations in functions");
