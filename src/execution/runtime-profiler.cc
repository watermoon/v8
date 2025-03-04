// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/runtime-profiler.h"

#include "src/base/platform/platform.h"
#include "src/codegen/assembler.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/codegen/pending-optimization-table.h"
#include "src/diagnostics/code-tracer.h"
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/handles/global-handles.h"
#include "src/init/bootstrapper.h"
#include "src/interpreter/interpreter.h"
#include "src/tracing/trace-event.h"

namespace v8 {
namespace internal {

// Number of times a function has to be seen on the stack before it is
// optimized.
// 一个函数在被优化前, 需要在栈上被"看到(seen)"的时间
static const int kProfilerTicksBeforeOptimization = 2;

// The number of ticks required for optimizing a function increases with
// the size of the bytecode. This is in addition to the
// kProfilerTicksBeforeOptimization required for any function.
// 优化一个函数需要的 tick 数随着字节码的长度而增加. 对于任何函数, 这都是除了
// kProfilerTicksBeforeOptimization 外的额外消耗
static const int kBytecodeSizeAllowancePerTick = 1200;

// Maximum size in bytes of generate code for a function to allow OSR.
// 一个函数的生成代码允许进行 OSR(on-stack-replacement)的最大大小
static const int kOSRBytecodeSizeAllowanceBase = 180;

static const int kOSRBytecodeSizeAllowancePerTick = 48;

// Maximum size in bytes of generated code for a function to be optimized
// the very first time it is seen on the stack.
// 一个函数的生成代码第一次在栈上被"看见(seen 即执行)"即进行优化的最大大小
// 说人话: 小函数首次执行即被优化, 小的判断条件是字节码小于 90 字节
static const int kMaxBytecodeSizeForEarlyOpt = 90;

// Number of times a function has to be seen on the stack before it is
// OSRed in TurboProp
// This value is chosen so TurboProp OSRs at similar time as TurboFan. The
// current interrupt budger of TurboFan is approximately 10 times that of
// TurboProp and we wait for 3 ticks (2 for marking for optimization and an
// additional tick to mark it for OSR) and hence this is set to 3 * 10.
static const int kProfilerTicksForTurboPropOSR = 3 * 10;

#define OPTIMIZATION_REASON_LIST(V)   \
  V(DoNotOptimize, "do not optimize") \
  V(HotAndStable, "hot and stable")   \
  V(SmallFunction, "small function")

enum class OptimizationReason : uint8_t {
#define OPTIMIZATION_REASON_CONSTANTS(Constant, message) k##Constant,
  OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_CONSTANTS)
#undef OPTIMIZATION_REASON_CONSTANTS
};

char const* OptimizationReasonToString(OptimizationReason reason) {
  static char const* reasons[] = {
#define OPTIMIZATION_REASON_TEXTS(Constant, message) message,
      OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_TEXTS)
#undef OPTIMIZATION_REASON_TEXTS
  };
  size_t const index = static_cast<size_t>(reason);
  DCHECK_LT(index, arraysize(reasons));
  return reasons[index];
}

#undef OPTIMIZATION_REASON_LIST

std::ostream& operator<<(std::ostream& os, OptimizationReason reason) {
  return os << OptimizationReasonToString(reason);
}

namespace {

void TraceInOptimizationQueue(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" is already in optimization queue]\n");
  }
}

void TraceHeuristicOptimizationDisallowed(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" has been marked manually for optimization]\n");
  }
}

// TODO(jgruber): Remove this once we include this tracing with --trace-opt.
void TraceNCIRecompile(JSFunction function, OptimizationReason reason) {
  if (FLAG_trace_turbo_nci) {
    StdoutStream os;
    os << "NCI tierup mark: " << Brief(function) << ", "
       << OptimizationReasonToString(reason) << std::endl;
  }
}

void TraceRecompile(JSFunction function, OptimizationReason reason,
                    CodeKind code_kind, Isolate* isolate) {
  if (code_kind == CodeKind::NATIVE_CONTEXT_INDEPENDENT) {
    TraceNCIRecompile(function, reason);
  }
  if (FLAG_trace_opt) {
    CodeTracer::Scope scope(isolate->GetCodeTracer());
    PrintF(scope.file(), "[marking ");
    function.ShortPrint(scope.file());
    PrintF(scope.file(), " for optimized recompilation, reason: %s",
           OptimizationReasonToString(reason));
    PrintF(scope.file(), "]\n");
  }
}

}  // namespace

RuntimeProfiler::RuntimeProfiler(Isolate* isolate)
    : isolate_(isolate), any_ic_changed_(false) {}

void RuntimeProfiler::Optimize(JSFunction function, OptimizationReason reason,
                               CodeKind code_kind) {
  DCHECK_NE(reason, OptimizationReason::kDoNotOptimize);
  TraceRecompile(function, reason, code_kind, isolate_);
  function.MarkForOptimization(ConcurrencyMode::kConcurrent);
}

void RuntimeProfiler::AttemptOnStackReplacement(InterpretedFrame* frame,
                                                int loop_nesting_levels) {
  JSFunction function = frame->function();
  SharedFunctionInfo shared = function.shared();
  if (!FLAG_use_osr || !shared.IsUserJavaScript()) {
    return;
  }

  // If the code is not optimizable, don't try OSR.
  if (shared.optimization_disabled()) return;

  // We're using on-stack replacement: Store new loop nesting level in
  // BytecodeArray header so that certain back edges in any interpreter frame
  // for this bytecode will trigger on-stack replacement for that frame.
  if (FLAG_trace_osr) {
    CodeTracer::Scope scope(isolate_->GetCodeTracer());
    PrintF(scope.file(), "[OSR - arming back edges in ");
    function.PrintName(scope.file());
    PrintF(scope.file(), "]\n");
  }

  DCHECK_EQ(StackFrame::INTERPRETED, frame->type());
  int level = frame->GetBytecodeArray().osr_loop_nesting_level();
  frame->GetBytecodeArray().set_osr_loop_nesting_level(
      Min(level + loop_nesting_levels, AbstractCode::kMaxLoopNestingMarker));
}

void RuntimeProfiler::MaybeOptimizeFrame(JSFunction function,
                                         JavaScriptFrame* frame,
                                         CodeKind code_kind) {
  DCHECK(CodeKindCanTierUp(code_kind));
  if (function.IsInOptimizationQueue()) {
    TraceInOptimizationQueue(function);
    return;
  }

  if (FLAG_testing_d8_test_runner &&
      !PendingOptimizationTable::IsHeuristicOptimizationAllowed(isolate_,
                                                                function)) {
    TraceHeuristicOptimizationDisallowed(function);
    return;
  }

  if (function.shared().optimization_disabled()) return;

  // Note: We currently do not trigger OSR compilation from NCI or TP code.
  // TODO(jgruber,v8:8888): But we should.
  if (frame->is_interpreted()) {
    DCHECK_EQ(code_kind, CodeKind::INTERPRETED_FUNCTION);
    if (FLAG_always_osr) {
      AttemptOnStackReplacement(InterpretedFrame::cast(frame),
                                AbstractCode::kMaxLoopNestingMarker);
      // Fall through and do a normal optimized compile as well.
    } else if (MaybeOSR(function, InterpretedFrame::cast(frame))) {
      return;
    }
  }

  OptimizationReason reason =
      ShouldOptimize(function, function.shared().GetBytecodeArray());

  if (reason != OptimizationReason::kDoNotOptimize) {
    Optimize(function, reason, code_kind);
  }
}

bool RuntimeProfiler::MaybeOSR(JSFunction function, InterpretedFrame* frame) {
  int ticks = function.feedback_vector().profiler_ticks();
  // TODO(rmcilroy): Also ensure we only OSR top-level code if it is smaller
  // than kMaxToplevelSourceSize.

  // Turboprop optimizes quite early. So don't attempt to OSR if the loop isn't
  // hot enough.
  if (FLAG_turboprop && ticks < kProfilerTicksForTurboPropOSR) {
    return false;
  }

  if (function.IsMarkedForOptimization() ||
      function.IsMarkedForConcurrentOptimization() ||
      function.HasAvailableOptimizedCode()) {
    // Attempt OSR if we are still running interpreted code even though the
    // the function has long been marked or even already been optimized.
    // TODO(turboprop, mythria): Currently we don't tier up from Turboprop code
    // to Turbofan OSR code. When we start supporting this, the ticks have to be
    // scaled accordingly
    int64_t allowance =
        kOSRBytecodeSizeAllowanceBase +
        static_cast<int64_t>(ticks) * kOSRBytecodeSizeAllowancePerTick;
    if (function.shared().GetBytecodeArray().length() <= allowance) {
      AttemptOnStackReplacement(frame);
    }
    return true;
  }
  return false;
}

OptimizationReason RuntimeProfiler::ShouldOptimize(JSFunction function,
                                                   BytecodeArray bytecode) {
  PrintF("ShouldOptimize...\n");
  if (function.ActiveTierIsTurbofan()) {  // 函数当前等级已经是 Turbofan, 无需优化了
    return OptimizationReason::kDoNotOptimize;
  }
  // 开启了 turboprop 编译器, 且函数的最高层级优化是 Turboprop, 则也无需优化
  if (V8_UNLIKELY(FLAG_turboprop) && function.ActiveTierIsToptierTurboprop()) {
    return OptimizationReason::kDoNotOptimize;
  }
  int ticks = function.feedback_vector().profiler_ticks();
  int scale_factor = function.ActiveTierIsMidtierTurboprop()
                         ? FLAG_ticks_scale_factor_for_top_tier  // 默认 10
                         : 1;
  int ticks_for_optimization =
      kProfilerTicksBeforeOptimization +  // 2
      (bytecode.length() / kBytecodeSizeAllowancePerTick);  // 1200
  ticks_for_optimization *= scale_factor;  // 乘以 scale
  PrintF("ShouldOptimize| ticks=%d bytecode_size=%d scale_factor=%d ticks_for_optimization=%d\n",
         ticks, bytecode.length(), scale_factor, ticks_for_optimization);
  if (ticks >= ticks_for_optimization) {  // 如果 profiler 的 tick 数达到阈值, 表明函数是热函数且稳定
    return OptimizationReason::kHotAndStable;
  } else if (!any_ic_changed_ &&
             bytecode.length() < kMaxBytecodeSizeForEarlyOpt) {
    // TODO(turboprop, mythria): Do we need to support small function
    // optimization for TP->TF tier up. If so, do we want to scale the bytecode
    // size?
    // If no IC was patched since the last tick and this function is very
    // small, optimistically optimize it now.
    return OptimizationReason::kSmallFunction;
  } else if (FLAG_trace_opt_verbose) {
    PrintF("[not yet optimizing ");
    function.PrintName();
    PrintF(", not enough ticks: %d/%d and ", ticks, ticks_for_optimization);
    if (any_ic_changed_) {
      PrintF("ICs changed]\n");
    } else {
      PrintF(" too large for small function optimization: %d/%d]\n",
             bytecode.length(), kMaxBytecodeSizeForEarlyOpt);
    }
  }
  return OptimizationReason::kDoNotOptimize;
}

RuntimeProfiler::MarkCandidatesForOptimizationScope::
    MarkCandidatesForOptimizationScope(RuntimeProfiler* profiler)
    : handle_scope_(profiler->isolate_), profiler_(profiler) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.MarkCandidatesForOptimization");
}

RuntimeProfiler::MarkCandidatesForOptimizationScope::
    ~MarkCandidatesForOptimizationScope() {
  profiler_->any_ic_changed_ = false;
}

void RuntimeProfiler::MarkCandidatesForOptimizationFromBytecode() {
  if (!isolate_->use_optimizer()) return;
  MarkCandidatesForOptimizationScope scope(this);
  int i = 0;
  for (JavaScriptFrameIterator it(isolate_); i < FLAG_frame_count && !it.done();
       i++, it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (!frame->is_interpreted()) continue;

    JSFunction function = frame->function();
    DCHECK(function.shared().is_compiled());
    if (!function.shared().IsInterpreted()) continue;

    if (!function.has_feedback_vector()) continue;

    MaybeOptimizeFrame(function, frame, CodeKind::INTERPRETED_FUNCTION);

    // TODO(leszeks): Move this increment to before the maybe optimize checks,
    // and update the tests to assume the increment has already happened.
    function.feedback_vector().SaturatingIncrementProfilerTicks();
  }
}

void RuntimeProfiler::MarkCandidatesForOptimizationFromCode() {
  if (!isolate_->use_optimizer()) return;
  MarkCandidatesForOptimizationScope scope(this);
  int i = 0;
  for (JavaScriptFrameIterator it(isolate_); i < FLAG_frame_count && !it.done();
       i++, it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (!frame->is_optimized()) continue;

    JSFunction function = frame->function();
    auto code_kind = function.code().kind();
    if (!CodeKindIsOptimizedAndCanTierUp(code_kind)) {
      continue;
    }

    DCHECK(function.shared().is_compiled());
    DCHECK(function.has_feedback_vector());

    function.feedback_vector().SaturatingIncrementProfilerTicks();

    MaybeOptimizeFrame(function, frame, code_kind);
  }
}

}  // namespace internal
}  // namespace v8
