// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-lazy-gen.h"

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/common/globals.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/shared-function-info.h"

namespace v8 {
namespace internal {

void LazyBuiltinsAssembler::GenerateTailCallToJSCode(
    TNode<Code> code, TNode<JSFunction> function) {
  auto argc = UncheckedParameter<Int32T>(Descriptor::kActualArgumentsCount);
  auto context = Parameter<Context>(Descriptor::kContext);
  auto new_target = Parameter<Object>(Descriptor::kNewTarget);

  TailCallJSCode(code, context, function, new_target, argc);
}

void LazyBuiltinsAssembler::GenerateTailCallToReturnedCode(
    Runtime::FunctionId function_id, TNode<JSFunction> function) {
  auto context = Parameter<Context>(Descriptor::kContext);
  TNode<Code> code = CAST(CallRuntime(function_id, context, function));
  GenerateTailCallToJSCode(code, function);
}

void LazyBuiltinsAssembler::TailCallRuntimeIfMarkerEquals(
    TNode<Uint32T> marker, OptimizationMarker expected_marker,
    Runtime::FunctionId function_id, TNode<JSFunction> function) {
  Label no_match(this);
  GotoIfNot(Word32Equal(marker, Uint32Constant(expected_marker)), &no_match);
  GenerateTailCallToReturnedCode(function_id, function);
  BIND(&no_match);
}

void LazyBuiltinsAssembler::MaybeTailCallOptimizedCodeSlot(
    TNode<JSFunction> function, TNode<FeedbackVector> feedback_vector) {
  Label fallthrough(this), may_have_optimized_code(this);

  // 从反馈向量加载优化状态
  TNode<Uint32T> optimization_state =
      LoadObjectField<Uint32T>(feedback_vector, FeedbackVector::kFlagsOffset);

  // Fall through if no optimization trigger or optimized code.
  // (Fall through)落空: 如果没有优化触发器和已优化代码, 则跳过吧
  GotoIfNot(IsSetWord32(
                optimization_state,
                FeedbackVector::kHasOptimizedCodeOrCompileOptimizedMarkerMask),
            &fallthrough);

  // 可能有已优化代码
  GotoIfNot(IsSetWord32(
                optimization_state,
                FeedbackVector::kHasCompileOptimizedOrLogFirstExecutionMarker),
            &may_have_optimized_code);

  // TODO(ishell): introduce Runtime::kHandleOptimizationMarker and check
  // all these marker values there.
  // TODO: 引入运行时函数 Runtime::kHandleOptimizationMarker 来检查所有这些标记值
  // marker 相等则调用响应的函数
  // 检查顺序: 记录首次运行、非并行编译优化、并行编译优化
  // 三个标记的意义需要了解一下
  TNode<Uint32T> marker =
      DecodeWord32<FeedbackVector::OptimizationMarkerBits>(optimization_state);
  TailCallRuntimeIfMarkerEquals(marker, OptimizationMarker::kLogFirstExecution,
                                Runtime::kFunctionFirstExecution, function);
  TailCallRuntimeIfMarkerEquals(marker, OptimizationMarker::kCompileOptimized,
                                Runtime::kCompileOptimized_NotConcurrent,
                                function);
  TailCallRuntimeIfMarkerEquals(
      marker, OptimizationMarker::kCompileOptimizedConcurrent,
      Runtime::kCompileOptimized_Concurrent, function);

  Unreachable();
  BIND(&may_have_optimized_code);
  {
    Label heal_optimized_code_slot(this);
    TNode<MaybeObject> maybe_optimized_code_entry = LoadMaybeWeakObjectField(
        feedback_vector, FeedbackVector::kMaybeOptimizedCodeOffset);
    // Optimized code slot is a weak reference.
    TNode<Code> optimized_code = CAST(GetHeapObjectAssumeWeak(
        maybe_optimized_code_entry, &heal_optimized_code_slot));

    // Check if the optimized code is marked for deopt. If it is, call the
    // runtime to clear it.
    // 已优化代码是否标记了逆优化, 是的话清除标记
    TNode<CodeDataContainer> code_data_container =
        CAST(LoadObjectField(optimized_code, Code::kCodeDataContainerOffset));

    TNode<Int32T> code_kind_specific_flags = LoadObjectField<Int32T>(
        code_data_container, CodeDataContainer::kKindSpecificFlagsOffset);
    GotoIf(IsSetWord32<Code::MarkedForDeoptimizationField>(
               code_kind_specific_flags),
           &heal_optimized_code_slot);

    // Optimized code is good, get it into the closure and link the closure into
    // the optimized functions list, then tail call the optimized code.
    // 调用已优化的代码
    StoreObjectField(function, JSFunction::kCodeOffset, optimized_code);
    GenerateTailCallToJSCode(optimized_code, function);

    // Optimized code slot contains deoptimized code or code is cleared and
    // optimized code marker isn't updated. Evict the code, update the marker
    // and re-enter the closure's code.
    BIND(&heal_optimized_code_slot);
    GenerateTailCallToReturnedCode(Runtime::kHealOptimizedCodeSlot, function);
  }

  // Fall-through if the optimized code cell is clear and there is no
  // optimization marker.
  BIND(&fallthrough);
}

void LazyBuiltinsAssembler::CompileLazy(TNode<JSFunction> function) {
  // First lookup code, maybe we don't need to compile!
  Label compile_function(this, Label::kDeferred);

  // Check the code object for the SFI. If SFI's code entry points to
  // CompileLazy, then we need to lazy compile regardless of the function or
  // feedback vector marker.
  TNode<SharedFunctionInfo> shared =
      CAST(LoadObjectField(function, JSFunction::kSharedFunctionInfoOffset));
  TNode<Code> sfi_code = GetSharedFunctionInfoCode(shared, &compile_function);

  TNode<HeapObject> feedback_cell_value = LoadFeedbackCellValue(function);

  // If feedback cell isn't initialized, compile function
  // 反馈单元未初始化, 编译之
  GotoIf(IsUndefined(feedback_cell_value), &compile_function);

  Label use_sfi_code(this);
  // If there is no feedback, don't check for optimized code.
  // 如果没有反馈(单元), 那么就没必要检查已优化代码了(因为可能不存在)
  GotoIf(HasInstanceType(feedback_cell_value, CLOSURE_FEEDBACK_CELL_ARRAY_TYPE),
         &use_sfi_code);

  // If it isn't undefined or fixed array it must be a feedback vector.
  // 如果反馈安远不是 undefined 或者固定数组, 则一定是一个反馈向量
  CSA_ASSERT(this, IsFeedbackVector(feedback_cell_value));

  // Is there an optimization marker or optimized code in the feedback vector?
  // 反馈向量中存在一个优化标记📌或者已优化的代码？
  // 所以关键在于这个优化标记啥时候设置? 设置的条件是什么
  MaybeTailCallOptimizedCodeSlot(function, CAST(feedback_cell_value));
  Goto(&use_sfi_code);

  BIND(&use_sfi_code);
  // If not, install the SFI's code entry and jump to that.
  // 如果不存在, 设置/安装 SFI(shared function info)的代码入口, 然后跳转到那里
  CSA_ASSERT(this, TaggedNotEqual(sfi_code, HeapConstant(BUILTIN_CODE(
                                                isolate(), CompileLazy))));
  StoreObjectField(function, JSFunction::kCodeOffset, sfi_code);
  GenerateTailCallToJSCode(sfi_code, function);

  BIND(&compile_function);
  GenerateTailCallToReturnedCode(Runtime::kCompileLazy, function);
}

TF_BUILTIN(CompileLazy, LazyBuiltinsAssembler) {
  auto function = Parameter<JSFunction>(Descriptor::kTarget);

  CompileLazy(function);
}

TF_BUILTIN(CompileLazyDeoptimizedCode, LazyBuiltinsAssembler) {
  auto function = Parameter<JSFunction>(Descriptor::kTarget);

  // Set the code slot inside the JSFunction to CompileLazy.
  TNode<Code> code = HeapConstant(BUILTIN_CODE(isolate(), CompileLazy));
  StoreObjectField(function, JSFunction::kCodeOffset, code);
  GenerateTailCallToJSCode(code, function);
}

}  // namespace internal
}  // namespace v8
