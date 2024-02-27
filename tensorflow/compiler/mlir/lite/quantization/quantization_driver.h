/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_QUANTIZATION_DRIVER_H_
#define TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_QUANTIZATION_DRIVER_H_

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantTypes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/quantization/quantization_utils.h"

namespace mlir {
namespace quant {

static bool HasQuantParams(QuantParams p) {
  return p == quant::QuantizedType();
}

// The state for each op result during the quantization parameters propagation.
struct QuantState {
  // Quantization parameters propagated to an op result.
  QuantParams params;
  // A flag indicates this state (the params) shouldn't be changed after it is
  // initialized. This flag will be set to true if the quantization parameters
  // are from the quantization-aware training.
  const bool immutable;

  bool IsEmpty() { return HasQuantParams(params); }
};

// The state for rescaling the propagated quantization parameters. This can be
// on the input side to satisfy the constraint of previous operation, or on the
// output side to satisfy the constraint of the next operation.
struct RequantizeState {
  // Sometimes, we have to "requantize" the quantization result to satisfy all
  // the constraints. The "requantize" can happen either on the input or output
  // of the quantization result.
  enum RequantizePosition {
    NO_REQUANTIZE,
    ON_INPUT,
    ON_OUTPUT
  } pos = NO_REQUANTIZE;

  // Quantization parameters will be used to add the requantize ops.
  QuantParams params;

  // Avoid clobbering all uses of the value, limit to just these ops.
  SmallVector<std::pair<Operation*, int>> users;
};

using RequantizeStates = SmallVector<RequantizeState>;

// This is a worklist-driven driver for propagating quantization parameters
// across operations.
//
// The initial quantization parameters are extracted from the quantized type
// between adjacent `quantfork::QuantizeCastOp` and
// `quantfork::DequantizeCastOp`s. All these initial parameters are marked as
// immutable because they are from quantization-aware training.
//
// The algorithm traverses each op and sets the quantization parameters of its
// operands and results, according to its quantization specification, and then
// adds the operands and results to the worklist. If there are any conflicts
// (for example, there are quantization parameters propagated from the previous
// iteration), this process stops if the existing parameters are the immutable,
// or adding `requantize` op to resolve the conflicts.
//
// After the algorithm is converged, pairs of `quantfork::QuantizeCastOp` and
// `quantfork::DequantizeCastOp` are inserted to the right position to
// materialize the propagation and requantize results.
//
class QuantizationDriver {
 public:
  explicit QuantizationDriver(func::FuncOp fn, bool is_signed, int bit_width,
                              bool disable_per_channel,
                              OpQuantSpecGetter op_quant_spec_getter,
                              OpQuantScaleSpecGetter op_quant_scale_spec_getter,
                              bool infer_tensor_range,
                              bool legacy_float_scale = false,
                              bool is_qdq_conversion = false)
      : fn_(fn),
        builder_(fn.getBody()),
        is_signed_(is_signed),
        bit_width_(bit_width),
        disable_per_channel_(disable_per_channel),
        op_quant_spec_getter_(op_quant_spec_getter),
        op_quant_scale_spec_getter_(op_quant_scale_spec_getter),
        infer_tensor_range_(infer_tensor_range),
        legacy_float_scale_(legacy_float_scale),
        is_qdq_conversion_(is_qdq_conversion) {}

  // The entry point of the quantization parameters propagation.
  void Run();

  // Sets up the states for all the op results in the function.
  void Initialize();

  // Propagates the quantization parameters across all the ops.
  bool PropagateParamsAndReturnIfChanged();

  // Inserts the Quantize and Dequantize ops according to the propagation
  // result.
  void Finalize();

  llvm::SmallVector<BlockArgument, 4> GetArgs() { return args_; }

  // Returns the state of the block argument.
  QuantState& GetArgQuantState(BlockArgument arg) {
    return states_[arg_states_[arg]];
  }

 private:
  // This is used to identify an operand or result of an op. The second element
  // of this pair is the index of the operand or result.
  using OpValue = std::pair<mlir::Operation*, int>;

  // Duplicates the constant op if it has multiple uses, and replaces
  // target_op->operand[operand_index] with the newly created op. This also
  // replaces corresponsing quantization states.
  arith::ConstantOp DuplicateConstantOpIfNeeded(arith::ConstantOp op,
                                                Operation* target_op,
                                                int operand_index);

  // Adjusts bias scale that is derived from other scales (fc, conv ops) to
  // prevent overflow of quantized bias values. This also changes quantization
  // state of other inputs when needed.
  bool SetBiasParamsWithAdjustments(Operation* op, int bias_index,
                                    const std::vector<int>& input_indices,
                                    QuantParams params);

  // Checks preconditions to adjust bias scale.
  bool ShouldCheckBiasScale(Operation* op, int bias_index,
                            const std::vector<int>& input_indices,
                            QuantParams params, int& input_index,
                            int& filter_index);

  // Preprocesses the constants by doing the following:
  //   - Duplicates constants if it is used by multiple ops. For example, if a
  //     constant is used by multiple ops as a bias, duplicate constants and
  //     let each op assign its own quantization parameter for bias.
  //   - Adds all the non-bias constants (weights) to a set for looking up
  //     later.
  //   - Adds all per-channel weights to a set for looking up later.
  void PreprocessConstantOps();

  // Sets up all the data structures for quantization propagation.
  void SetupAllStates();

  // Returns Whether the constant is a weight, which shouldn't be shared by
  // different ops.
  bool IsWeight(Operation* cst) { return llvm::is_contained(weights_, cst); }

  // Returns all the related quantization constraints of the op.
  std::unique_ptr<OpQuantSpec> GetQuantSpec(Operation* op);
  std::unique_ptr<OpQuantScaleSpec> GetQuantScaleSpec(Operation* op);

  // Returns whether quantization parameters have been propagated to the results
  // of this op.
  bool IsQuantized(Operation* op);

  // Adds all the users of index-th result of op to the work list.
  void AddUserToList(Operation* op, int index) {
    for (Operation* user : op->getResult(index).getUsers()) {
      work_list_.push_back(user);
    }
  }

  // Adds the defining op of index-th operand of op to the work list.
  void AddOperandToList(Operation* op, int index) {
    if (Operation* inst = op->getOperand(index).getDefiningOp()) {
      work_list_.push_back(inst);
    }
  }

  // Returns the quantization params for the bias input from the non-bias
  // operands which have their indexes in the `non_biases` vector. The returned
  // parameters are calculated by `func`.
  QuantParams GetBiasParams(Operation* op, int bias_index,
                            const std::vector<int>& non_biases,
                            AccumulatorScaleFunc func);

  // Sets the quantization parameters of the result to a fixed value. If any
  // quantization parameters have been propagated, a `requantize` will happen on
  // the input of propagated quantization.
  bool SetResultParams(Operation* op, int index, QuantParams params);

  // Sets the quantization parameters of the operand to a fixed value. If any
  // quantization parameters have been propagated, a `requantize` will happen on
  // the output of propagated quantization. When `override` is set, quantization
  // state of the value is replaced instead of adding requantization.
  bool SetOperandParams(Operation* op, int index, QuantParams params,
                        bool override = false);

  // Sets the quantization parameters of the constant result according to its
  // content.
  bool SetConstantResultParams(Operation* op);

  // Inserts the Quantize and Dequantize ops for quantizing the index-th result
  // of the op.
  void QuantizeOpResult(Operation* op, int index, QuantParams params);

  void QuantizeArg(BlockArgument arg, QuantParams params);

  // Inserts the Quantize and Dequantize ops to quantize the value and returns
  // the Quantize op.
  void QuantizeValue(Value value, QuantParams params, Location loc);

  // Inserts the Quantize ops for requantizing the index-th result of the op.
  void RequantizeOpResult(Operation* op, int index, RequantizeStates* states);

  // Inserts the Quantize ops for requantizing a block argument.
  void RequantizeArg(BlockArgument arg, RequantizeStates* states);

  // Inserts the Quantize and Dequantize ops to quantize the value and returns
  // the Quantize op.
  void RequantizeValue(Value value, RequantizeStates* states, Location loc);

  // Returns the quantization parameter satisfies the same scale
  // constraints for the op. Returns an empty option if this quantization
  // parameter doesn't exist.
  QuantParams GetQuantParamsForSameScaleConstraint(Operation* op);

  // Returns the state of the index-th operand of the op.
  QuantState& GetOperandQuantState(Operation* op, int index) {
    return states_[operand_states_[{op, index}]];
  }

  // Returns the state of the index-th result of the op.
  QuantState& GetResultQuantState(Operation* op, int index) {
    return states_[result_states_[{op, index}]];
  }

  // Returns the states of the index-th operand of the op.
  RequantizeStates& GetOperandRequantizeStates(Operation* op, int index) {
    return rescale_states_[operand_states_[{op, index}]];
  }

  // Returns the states of the index-th result of the op.
  RequantizeStates& GetResultRequantizeStates(Operation* op, int index) {
    return rescale_states_[result_states_[{op, index}]];
  }

  // Returns the states of the arg.
  RequantizeStates& GetArgRequantizeStates(BlockArgument arg) {
    return rescale_states_[arg_states_[arg]];
  }

  // Uses the type of `val` to set the initial state of the index-th result if
  // `as_result` is true or index-th operand if `as_result` is false. The state
  // is immutable if the type is a quantized type. Returns the index of this
  // new state in the state vector.
  int InitializeState(Operation* op, int index, Value val, bool as_result);

  // Sets the state of an argument. If this value is cached, uses the cached
  // result without creating new entry in the state vector. Otherwise, allocate
  // a new entry in the state vector.
  void InitializeArgState(BlockArgument arg, Value arg_value);

  // Sets the state of the index-th operand of the op. If this operand is
  // cached, uses the cached result without creating new entry in the state
  // vector. Otherwise, allocate a new entry in the state vector.
  void InitializeOperandState(Operation* op, int index, Value in);

  // Sets the state of the index-th result of the op. If this result is cached,
  // uses the cached result without creating new entry in the state vector.
  // Otherwise, allocate a new entry in the state vector.
  void InitializeResultState(Operation* op, int index, Value res);

  // Helps debug output for requantize states.
  void DumpRequantizeStates(const RequantizeStates& requantize_states);

  void DumpStates(Operation* current_op);

  func::FuncOp fn_;
  OpBuilder builder_;
  const bool is_signed_;
  const int bit_width_;
  const bool disable_per_channel_;

  // We should distinguish weights and bias constants. Biases are specified by
  // the quantization spec or are the operands of ops with same scale spec. The
  // rest are weights.
  llvm::DenseSet<Operation*> weights_;

  // The weights require narrow_range quantization. This map collects all the
  // weight operands defined by the op quant spec. If the value of the entry is
  // positive, per-channel quantization is required.
  llvm::DenseMap<Operation*, int> optimized_weights_;

  // All the ops needs to propagate the quantization parameters to.
  std::vector<Operation*> work_list_;
  absl::flat_hash_set<Operation*> quantized_;

  // The vector contains all the quantization parameters propagated from the
  // defining operations of the value, or from the quantization aware training.
  std::vector<QuantState> states_;

  // The map contains all the quantization parameters which are required to
  // satisfy the same operands and results constraint. The keys of this map are
  // the values from `operand_states_` and `result_state_`.
  std::unordered_map<int, RequantizeStates> rescale_states_;

  // Maps of indexes to the propagation state vector from the ops operands,
  // results and arguments.
  llvm::DenseMap<OpValue, int> operand_states_;
  llvm::DenseMap<OpValue, int> result_states_;
  llvm::DenseMap<BlockArgument, int> arg_states_;
  llvm::DenseMap<Value, int> value_to_state_;

  // This vector is to preserve the arguments order, so the newly inserted
  // quantized ops for the arguments are deterministically ordered.
  llvm::SmallVector<BlockArgument, 4> args_;

  OpQuantSpecGetter op_quant_spec_getter_;
  OpQuantScaleSpecGetter op_quant_scale_spec_getter_;

  // Infer output ranges for activation ops and constants. This is usually
  // required for post-training quantization.
  const bool infer_tensor_range_;

  // Calculate scales in float instead of double, so that the scales and
  // quantized values are exactly the same with the TOCO quantizer.
  const bool legacy_float_scale_;

  // If true, the model is a floating point graph with QDQ ops to be eliminated
  // and fused into quantized kernels.
  const bool is_qdq_conversion_;
};

// Propagates quantization parameters across ops in this function and satisfies
// the quantization specification of the ops. This methods assumes the initial
// quantization parameters are stored as adjacent quantize and dequantize ops
// and the propagation results are materialized by inserting pairs of quantize
// and dequantize ops to this function. Set `disable_per_channel` to true to not
// use per channel quantization even the op supports it.
// Setting `infer_tensor_range` to true, to infer quantization parameters from
// the activation ops and weight constants. This is only used for post-training
// quantization.
void ApplyQuantizationParamsPropagation(mlir::func::FuncOp func, bool is_signed,
                                        int bit_width, bool disable_per_channel,
                                        OpQuantSpecGetter op_quant_spec_getter,
                                        bool infer_tensor_ranges,
                                        bool legacy_float_scale,
                                        bool is_qdq_conversion);

void ApplyQuantizationParamsPropagation(
    mlir::func::FuncOp func, bool is_signed, int bit_width,
    bool disable_per_channel, OpQuantSpecGetter op_quant_spec_getter,
    OpQuantScaleSpecGetter op_quant_scale_spec_getter, bool infer_tensor_ranges,
    bool legacy_float_scale, bool is_qdq_conversion);

}  // namespace quant
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_QUANTIZATION_DRIVER_H_
