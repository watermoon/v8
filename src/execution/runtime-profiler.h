// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_EXECUTION_RUNTIME_PROFILER_H_
#define V8_EXECUTION_RUNTIME_PROFILER_H_

#include "src/common/assert-scope.h"
#include "src/handles/handles.h"
#include "src/utils/allocation.h"

namespace v8 {
namespace internal {

class BytecodeArray;
class Isolate;
class InterpretedFrame;
class JavaScriptFrame;
class JSFunction;
enum class CodeKind;
enum class OptimizationReason : uint8_t;

class RuntimeProfiler {
 public:
  explicit RuntimeProfiler(Isolate* isolate);

  // Called from the interpreter when the bytecode interrupt has been exhausted.
  // 当字节码中断消耗完后, 解析器会调用这个函数
  void MarkCandidatesForOptimizationFromBytecode();
  // Likewise, from generated code.
  // 类似的, 从生成的代码那里调过来
  void MarkCandidatesForOptimizationFromCode();

  void NotifyICChanged() { any_ic_changed_ = true; }

  void AttemptOnStackReplacement(InterpretedFrame* frame,
                                 int nesting_levels = 1);

 private:
  // Make the decision whether to optimize the given function, and mark it for
  // optimization if the decision was 'yes'.
  // 决定是否优化给定的函数, 如果决定优化则标记它
  void MaybeOptimizeFrame(JSFunction function, JavaScriptFrame* frame,
                          CodeKind code_kind);

  // Potentially attempts OSR from and returns whether no other
  // optimization attempts should be made.
  // 潜在尝试从 OSR(on-stack-replacement)(优化)并且返回是否不应该进行其他的优化尝试
  bool MaybeOSR(JSFunction function, InterpretedFrame* frame);
  OptimizationReason ShouldOptimize(JSFunction function,
                                    BytecodeArray bytecode_array);
  void Optimize(JSFunction function, OptimizationReason reason,
                CodeKind code_kind);
  void Baseline(JSFunction function, OptimizationReason reason);

  class MarkCandidatesForOptimizationScope final {
   public:
    explicit MarkCandidatesForOptimizationScope(RuntimeProfiler* profiler);
    ~MarkCandidatesForOptimizationScope();

   private:
    HandleScope handle_scope_;
    RuntimeProfiler* const profiler_;
    DisallowHeapAllocation no_gc;
  };

  Isolate* isolate_;
  bool any_ic_changed_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_EXECUTION_RUNTIME_PROFILER_H_
