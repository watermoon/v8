## Ignition(点火)
Ignition 是一个基于寄存器的解析器, 基于 TurboFan. Ignition 中的寄存器不是传统意义的机器寄存器, 而是在函数的栈帧中分配的寄存器文件(register file)中特定的 slot.

### Ignition bytecode 流水线
```
                    | => Register Optimizer => Peephole Optimizer => Dead-code Elimination
Parser => AST => Bytecode                                                 | => Bytecode Array Writer => Bytecode
                                                                                                           ||
                                                                                                           \/
                                                                                                       Interpreter(Ignition)
```

<img src=../img/Ignition_bytecode_pipeline.png width=600 height=400 />

### TurboFan 流水线
```
                            WebAssembly ---
                                          ||
                                          \/
JavaScript Source => JavaScript => Simple => Machine => Scheduler => CodeGen => Machine Code
                                                                  /\
                                                                  ||
                                                          Interpreter Assembler
```

<img src=../img/TurboFan_pipeline.png width=600 height=400 />

### 关键类
* BytecodeGenerator : public AstVisitor<BytecodeGenerator>
* Scope 域: 很关键的一个东西, 还有一个就是 Context 上下文
    * 表示不同类型的作用域, 例如 script/module/declaration/class 等
* Register: 用于在栈帧中索引参数和本地变量, 通过索引(index)
    *  < 0: 参数
    * >= 0: 本地变量
* 字节码处理程序
  * 用 IGNITION_HANDLER 进行定义, @interpret-generator.cc
  * 基类: InterpreterAssembler => CodeStubAssembler => compiler::CodeAssembler, 类名 BytecodeName ## InterpreterAssembler
  * 例如 IGNITION_HANDLER(LdaNamedProperty, InterpreterAssembler)

### 类关系
* InterpreterCompilationJob : UnoptimizedCompilationJob
[源文件](./class.dio)
<img src=../img/ignition_classes.svg style="background-color:white" />

### 入口 & 流程
#### 非优化编译
* ExecuteSingleUnoptimizedCompilationJob
  => interpreter::Interpreter::NewCompilationJob
  => job->ExecuteJob // UnoptimizedCompilationJob::ExecuteJob@compiler.cc
    => InterpreterCompilationJob::ExecuteJobImpl()
      => generator()->GenerateBytecode(stack_limit());

* Interpreter::NewCompilationJob(构造 InterpreterCompilationJob)
  * InterpreterCompilationJob(ParseInfo* parse_info, FunctionLiteral* literal, AccountingAllocator* allocator, std::vector<FunctionLiteral*>* eager_inner_literals)
    * 基类(UnoptimizedCompilationJob) 构造
    * 成员变量初始化
      * zone_(allocator, ZONE_NAME)
      * compilation_info_(&zone_, parseinfo, literal)
      * generator_(&zone, &compilation_info_, parse_info->ast_string_constants(), eager_inner_literals) // 类 BytecodeGenerator
        * info_ = compilator_info
        * closure_scope_/current_scope_ = literal->scope() // 通过 compilation_info_ 间接引用
#### 优化编译
* LazyBuiltinsAssembler::CompileLazy @builtins-lazy-gen.cc
  => MaybeTailCallOptimizedCodeSlot: 存在优化向量或者优化标记才调用
    => 调用已优化代码或啥都不做
    => 或根据标记调用, 会调用下一个实际编译优化代码的 Runtime 函数
      * OptimizationMarker::kLogFirstExecution => Runtime::kFunctionFirstExecution
      * OptimizationMarker::kCompileOptimized => Runtime::kCompileOptimized_NotConcurrent
      * OptimizationMarker::kCompileOptimizedConcurrent => Runtime::kCompileOptimized_Concurrent
      * 关键点: 这个标记是在哪里设置的, 设置的条件是什么?
        * 设置函数 SetOptimizationMarker
* Runtime::kCompileOptimized_Concurrent/Runtime::kCompileOptimized_NotConcurrent 两个 runtime 函数
  => CompileOptimized @runtime-compiler.cc
    => Compiler::CompileOptimized @compiler.cc
      => v8::internal::GetOptimizedCode @compiler.cc
        => Pipeline::NewCompilationJob
* Runtime_CompileForOnStackReplacement @runtime-compiler.cc 设置 OptimizationMarker::kCompileOptimized 标记
  => Compiler::GetOptimizedCodeForOSR (会先判断是否适合做 OSR - on stack replacement)
    => GetOptimizedCode
* SetOptimizationMarker 设置优化标记, 总共两个地方有这个函数, JSFunction 的调用 feedback_vector 的接口
  * v8::internal::JSFunction::SetOptimizationMarker @src/objects/js-function.h 判断条件在此
  * v8::internal::FeedbackVector::SetOptimizationMarker
* JSFunction::MarkForOptimization @js-function.h 将函数标记为优化编译
  * 触发 (<= 符号表示调用来源)
    * Compiler::PostInstantiation
      <= Factory::JSFunctionBuilder::JSFunctionBuilder
    * RuntimeProfiler::Optimize (Cautions!!! 热函数判断而来优化?)
      <= RuntimeProfiler::ShouldOptimize
        <= RuntimeProfiler::MaybeOptimizeFrame
          * MaybeOSR: 拦截 ShouldOptimize 调用
          <= MarkCandidatesForOptimizationFromBytecode <font color=red>调用 SaturatingIncrementProfilerTicks 增加 profiler_tick</font>
            <= Runtime_BytecodeBudgetInterruptFromBytecode
              <= InterpreterAssembler::UpdateInterruptBudget -> 解析器执行
                <= BuildUpdateInterruptBudget
                  <= BytecodeGraphBuilder 类中, 构建跳转和函数返回的字节码执行图时会调用此函数
                    * BuildReturn
                    * BuildJump
                    * BuildJumpIf
                    * BuildJumpIfNot
                    * BuildJumpIfFalse
                    * BuildJumpIfTrue
                    * 为什么都是在这些返回或者跳转的地方, 因为计算 ticker 的 budget 是以执行了多少字节的指令来判断的, 所以在返回/跳转地地方统计就能统计完整
          <= MarkCandidatesForOptimizationFromCode 调用 2
            <= Runtime_BytecodeBudgetInterruptFromCode
              <= EffectControlLinearizer::LowerUpdateInterruptBudget
    * Runtime_OptimizeFunctionOnNextCall
    * Runtime_OptimizeOsr
* RuntimerProfiler::ShouldOptimize @src/execution/runtime-profiler.cc 函数是否可优化的条件
  * 判断是否不需要优化
    * TurboFan
    * Turboprop
  * 根据 profiler 收集的数据判断是否需要优化
    ```cpp
    // 注: 通过指令 %OptimizeFunctionOnNextCall() 强行优化后, 再多次执行函数, 被 profiler 判断为热
    // 函数后依然会进行优化, 原因不明. 测试脚本: test_opt.js
    // %OptimizeFunctionOnNextCall(add): TurboFan
    // profiler 热函数: TurboFan-OSR

    // 这个 profiler_ticks 的增加通过 SaturatingIncrementProfilerTicks 接口进行
    // 执行路径同 ShouldOptimize, 即增加一次执行一次
    int ticks = function.feedback_vector().profiler_ticks();  // 执行时收集的 tick
    int scale_factor = function.ActiveTierIsMidtierTurboprop()  // 中层的 Turboprop 优化
                            ? FLAG_ticks_scale_factor_for_top_tier  // 默认 10
                            : 1;
    int ticks_for_optimization =  // 进行优化需要达到的 tick (即条件)
        kProfilerTicksBeforeOptimization +  // 2
        (bytecode.length() / kBytecodeSizeAllowancePerTick);  // 1200
    ticks_for_optimization *= scale_factor;  // 乘以 scale
    if (ticks >= ticks_for_optimization) {   // 如果 profiler 的 tick 数达到阈值, 表明函数是热函数且稳定
        return OptimizationReason::kHotAndStable;
    } else if (!any_ic_changed_ &&  // 小函数优化
                bytecode.length() < kMaxBytecodeSizeForEarlyOpt) {  // 90
        // TODO(turboprop, mythria): Do we need to support small function
        // optimization for TP->TF tier up. If so, do we want to scale the bytecode
        // size?
        // If no IC was patched since the last tick and this function is very
        // small, optimistically optimize it now.
        return OptimizationReason::kSmallFunction;
    } else if (FLAG_trace_opt_verbose) {
        // 打印优化跟踪信息, 指定 flag: --trace_opt
    }
    ```
* 完整的热点函数优化逻辑
* 
### Ignition 总体设计
* 字节码处理程序是用高级的机器架构相关的汇编代码(CSA)写的, 被 TurboFan 编译
* 为了被解析器执行, 函数会在初次的未优化编译步骤中被 BytecodeGenerator 翻译成字节码
* BytecodeGenerator 是一个 AstVisitor, 会遍历函数的 AST 并生成合适的字节码
* 字节码会通过 SharedFunctionInfo 对象的一个字段和函数关联起来, 函数的代码入口地址会设置成 InterpreterEntryTrampoline builtin stub(桩, 用于构架解析器的堆栈)

#### 字节码处理器(Bytecode Handler)生成
* 由 TurboFan 编译器生成
* 每个处理器都是自身的代码对象, 并且独立生成
* 处理器通过 InterpreterAssembler 以 TurboFan 操作图的方式编写, InterpreterAssembler 是 CodeStubAssembler 的子类

#### 字节码生成
* AST => 字节码
* InterpreterCompilationJob::ExecuteJobImp()
  * BytecodeGenerator::GenerateBytecode(uintptr_t stack_limit)
    * InitializeAstVisitor(stack_limit);
    * AllocateTopLevelRegisters // 分配顶层寄存器 incoming_new_target_or_generator_
    * GenerateBytecodeBody()
      * ... 参数、本地变量
      * 类型分析统计, 统计参数的类型信息? 为优化做准备吗?
      * BuildIncrementBlockCoverageCounterIfEnabled: 函数 body 代码块覆盖统计 + 1
      * VisitStatements // 递归生成 statement 的字节码
      * BuildReturn()
* InterpreterCompilationJob::FinalizeJobImpl()
  * bytecodes = generator()->FinalizeBytecode(...)
  * compilation_info_->SetBytecodeArray(bytecodes)

##### 解析器寄存器分配
* 在生成字节码的过程中, BytecodeGenerator 需要在函数的寄存器文件中为本地变量、下下文对象指针(用于维护跨函数闭包状态)、表达式计算的临时值分配寄存器.
* 在函数执行的过程中, 寄存器文件会在函数的开场白/序幕(prologue)中作为函数栈帧的一部分进行分配. 字节码通过指定寄存器索引来操作这些寄存器
* 由于寄存器索引与函数栈帧槽位直接映射, 解析器也可以像范围寄存器那样直接访问栈上的其它操作.
* 得益于这种寄存器文件的方式, Ignition 不用像 full-codegen 编译器那样动态地 push/pop 值到栈上.
(注意: 函数调用的参数是通过另外的 builtin 进行, 而不是解析器自身进行)
* 损益
    * 好处: 栈帧可以在函数的序幕分配一次即可, 并且保持了和架构需求的对齐(例如 Arm64 架构要求 16 字节的栈对齐)
    * 不足: 要求 BytecodeGenerator 在代码生成的时候就计算好最大的栈帧 size

* 函数内的本地变量被解析器(parser)提升(hoisted)到函数的顶部. 这样就可以提前知道本地变量的个数, 然后在 AST 遍历的初始阶段分配寄存器索引
* 内部上下文的情况也是类似

* 表达式计算需要的临时变量, 需要在消费具体的 AST 节点的时候才知道. BytecodeGenerator 使用一个域内(scoped)的 BytecodeRegisterAllocator 来为它们分配寄存器. 对于每一个语句, 创建一个新的分配器, 并且在整个函数的代码生成过程中, 记录最大的临时寄存器数量. 这样就能为函数在寄存器文件分配足够的额外槽位.
* 寄存器文件的总大小会被保存在 BytecodeArray 对象中

* 在函数入口, 解析器序幕会为寄存器文件增加需要的空间. 所有的槽位都被初始化为 undefined_value, 这样 GC 就可以一致地(以 tagged 指针的方式)访问所有寄存器文件

##### 上下文链
* 进入新的上下文(创建了 ContextScope 对象), 会生成一个 ContextPush 字节码. 当前的上下文对象会保存到寄存器中
* 退出上下文, 生成 ContextPop 字节码
* 好处: 避免了解析器跟踪当前上下文

##### 常量池条目
* 存储生成的字节码会引用到的常量堆对象(heap object)
* 每个 BytecodeArray 对象都可以有自己的常量池(FixexArray), 通过索引访问(如 LdaConstant 2)
* 字符串和堆数字总是放到常量池
* Smi(Small Integer)和古怪的类型则拥有直接加载他们的字节码, 不放在常量池中
* 常量池也可以存放前向跳转的偏移
* 常量池是在代码生成的时候由 ConstantArrayBuilder 负责构建
* 复杂逻辑: 前向跳转(因为跳转字节码生成的时候还不知道跳转的偏移是多少)
  * 所以在字节码流中预留空间, 然后 BytecodeArrayBuilder 跟踪这个位置, 并在知道了偏移之后填上偏移值
  * 字节码生成器生成一个前向跳转字节码和保留有一个字节作为操作数(即跳转偏移)
  * 当偏移得到之后, 如果偏移可以放到一个字节里, 则直接 patch 更新到预留操作数里
  * 如果跳转偏移较大, 那么偏移会保存到常量池中, 并且跳转字节码(因为跳转偏移在常量池)和操作数都被会更新

##### 局部控制流

#### 解析器代码执行
Ignition 解析器是一个基于寄存器的、间接线性解析器(ITC, 对应于 DTC). 从 JavaScript 到解析器的入口点是 `InterpreterEntryTampoline` builtin.

<img src=../img/ignition_InterpreterEntryTrampoline_frame.png style="background-color:white" />

解析器栈帧是 `InterpreterEntryTampoline` 构建的, 它会依次压入固定的值 (Caller PC, Frame Pointer, JSFunction, Context, BytecodeArray, Bytecode Offset), 然后在栈上分配寄存器文件空间(BytecodeArray 对象包含一个字段说明需要多大的空间, 详情请看字节码生成部分), 然后用 `undefined` 填充所有的寄存器.

`InterpreterEntryTampoline` 会初始化一些会被解析器用到的固定的机器寄存器:
* `kInterpreterAccumulatorRegister`: 隐式累加寄存器
* `kInterpreterBytecodeArrayRegister`: 指向 `BytecodeArray` 对象的开始位置
* `kInterpreterBytecodeOffsetRegister`: 当前执行的偏移, 本质上是解析器的字节码 PC
* `kInterpreterDispatchTableRegister`: 指向解析器的调度表
* `kInterpreterRegisterFileRegister`: 指向寄存器文件的开始问题. 由于 TurboFan 可以直接访问父栈指针, 所以很快会去掉
* `kContextRegister`: 指向当前上下文对象, 很快会去掉, 而改用 `Register::current_context()`

(构建完解析器栈帧后), 接着它(InterpreterEntryTampoline)会调用字节码数组流中的第一个字节码的字节码处理程序

如果字节码处理程序比较简单, 不调用任何其它函数(除了分发操作的尾调用), 这样 TurboFan 可以省去为字节码处理程序创建栈帧.
然而, 对于更复杂的字节码处理程序, TurboFan 会在执行进入到字节码处理程序的时候创建一个新的 STUB 帧. 相比于必须的固定帧, STUB 帧仅存储被 TurboFan 溢出(spilled by TurboFan)的机器寄存器值(例如跨调用时, 被调者保存的寄存器). 所有与被解析器函数相关的状态都存在于寄存器帧上.

##### 解析器寄存器访问
* 通过索引访问, 索引是相对于寄存器文件开始位置, 基于字(word-based)的偏移, 可正可负
* 正: 函数参数; 负: 局部变量, `kInterpreterRegisterFileRegister + offset`

##### 宽操作数(没看懂)
Ignition 支持不同宽度的操作数. Ignition 通过特定前缀的字节码来支持更宽的操作数.

##### JS 调用
* 调用其他的 JS 函数是通过 `Call` 字节码处理的. `BytecodeGenerator` 保证需要传递给被调函数的参数在一个连续的寄存器集合中. 接着生成一个 `Call` 字节码和保存了被调函数的寄存器操作数, 第二个寄存器操作数保存了参数的开始位置, 第三个寄存器操作数则指明了传递的参数个数.
* 解析器的 `Call` 字节码处理器在被解析的帧中用当前的字节码 offset 更新栈槽上的字节码偏移值. (这个 Call 是字节码)
* 接着调用 `InterpreterPushArgsAndCall` builtin, 传递被调函数、第一个参数的内存地址、参数数量
* `InterpreterPushArgsAndCall` 会将参数拷贝并压入到栈中, 接着调用 `Call` builtins (这个是字节码处理程序里调用的 Call builtin)
* 调用其他被解析的函数(interpreted function)和调用 JIT 函数是一样的. `Call` builtin 会加载 JS 函数的代码如何域, 这又是 `InterpreterEntryTrampoline` builtin stub(所有从 JS 进入解析器都是这个函数), 即会重入解析器
* 一个被解析的函数返回时, 解析器会尾调用 `InterpreterExitTrampoline` builtin stub, 以销毁解析器栈帧并将控制权交换给调用函数(Caller PC), 并将累计寄存器的值作为返回值

##### 属性加载和存储
* 字节码通过 inline caches(ICs)加载哈存储 JS 对象上的属性. 字节码处理程序和 JIT 的代码(例如 full-codegen)都是调用 `LoadIC/StoreIC` 代码 stub.
* 字节码处理程序将函数的 `TypeFeedbackVector` 和操作的 AST 节点上关联的类型反馈槽位一起传给合适的 IC stub. IC 可以和由 full-codegen 编译器生成的代码一样更新 `TypeFeedbackVector`, 使得学习后面优化编译时类型反馈成为可能

##### 二元操作(Binary ops)
* 对于二元操作和其它的一元操作, full-codegen 当前使用 ICs 对机器码打补丁. 但是对于 Ignition, 则不能这么用因为这会对字节码处理器打补丁, 因此对于二元操作和一元操作, 我们并没有收集任何类型反馈
* 未来(相对于设计文档)优化, 可能会做一些反向打补丁

#### TurboFan 字节码图构建器
BytecodeGenerator 生成的 BytecodeArray 包含了喂到 TurboFan 里生成一个 TurboFan 编译图的所有必要信息. 这一切是由 BytecodeGraphBuilder 来完成的.
* BytecodeGraphBuilder 通过遍历 BytecodeArray 来执行以下基本的分支分析, 找到前继、后续字节码. 这主要用于建立在构建 TurboFan 图时的合适循环头部环境(appropriate loop header environments)
* BytecodeGraphBuilder 再次遍历 BytecodeArray, 然后调用每一个字节码的特定访问器(visitor). 访问器将操作添加到 TurboFan 图来执行字节码操作. 大部分的字节码都有一个对应的 JSOperator 在 TurboFan 中.
* BytecodeGraphBuilder 维护了一个环境, 这个环境会跟踪 BytecodeArray 中的寄存器文件中每一个寄存器中的值是由哪个节点(TurboFan 图中的节点)来保存. 环境同时还会跟踪当前上下文对象(会被 Push/PopContext 字节码更新).
  * 当字节码访问器访问一个从寄存器读值的字节码时, 将会查询环境中与寄存器关联的节点, 将其作为当前 JSOperator 的输入节点
  * 与输入类型, 输出值到寄存器时, 访问器会更新环境中对应的节点

##### 逆优化(Deoptimization)
BytecodeGraphBuilder 需要跟踪解析器的栈帧状态, 以在逆优化时重建解析器栈帧, 然后重新进入函数.
环境已经跟踪了这些, 在跟踪节点和寄存器文件的映射关系时.
* BytecodeGraphBuilder 会在 FrameState 节点检查这些信息(eager - 节点执行前检查, lazy - 节点执行后检查)
* JSOperator 的 FrameState 节点可能会触发逆优化
* 由于一个字节码对应到一个 JSOperator 节点, 我们只会在一个字节码的前或后进行逆优化
* 因此我们可以使用字节码偏移作为逆优化点的 BailoutId
* 当 TurboFan 生成代码来处理潜在的逆优化时, 它会序列化一个 TranslatedState 记录来描述如何为这个逆优化点重建一个解析器帧
* 运行时通过 InterpreterNotifyDeoptimized builtin 重新进入解析器

### 字节码 handler 走读
以 `IGNITION_HANDLER(LdaNamedProperty, InterpreterAssembler) @interpreter-generator.cc` 为例, 会生成类 LdaNamedPropertyAssembler 类, 其中接口 `GenerateImpl` 实现如下:
* GenerateImpl()
  => LoadFeedbackVector()
  => LoadRegisterAtOperandIndex(0)  // 加载第一个参数 receiver
  => 剩下的两个参数都是通过 lambda 函数来获取, 不太明白为什么要 lambda, 直接获取不好吗? 难为是为了节省获取的成本, 判断不通过即不用调用此函数?
  => LoadIC_BytecodeHandler         // 真正的 handler
  => SetAccumulator                 // 返回结果(即设置 acc 寄存器)
  => Dispatch()                     // 下一个字节码
* AccessorAssembler::LoadIC_BytecodeHandler(LazyLoadICParameters*, ExitPoint*)  // accessor-assember.cc
  * 第一个参数即前面获取的几个参数, 构造的一个结构体
  => 没有 feedback, no_feedback 返回
  => Map 已废弃, miss 返回
  => 单态: TryMonomorphicCase
    => 找到 handler: HandleLoadICHandlerCase, 大量默认参数(ICMode 和之后的均默认)
  => 非单态: HandlePolymorphicCase
    => 找到 handler: HandleLoadICHandlerCase
  => 非内联: ReturnCallStub
* HandleLoadICHandlerCase()
  * 默认参数 ICMode::kNonGlobalIC, OnNonExistent::kReturnUndefined, kOnlyProperties, LoadAccessMode::kLoad
  => SmiHandler(|handler| 是一个 Smi 对象, 不知道啥意思): HandleLoadICSmiHandlerCase
    => 各种类型的加载, 需要是 SmiHandler: HandleLoadICSmiHandlerLoadNamedCase
      => HandleLoadField(以 field 为例) // 没看到如何更新 IC, o(╥﹏╥)o
        => InObject
        => OutOfObject
  => ProtoHandler: HandleLoadICProtoHandler
  => 回退 exit_point->ReturnCallStub

### 参考资料
1. [Firing up the Ignition interpreter](https://v8.dev/blog/ignition-interpreter)
2. [Ignition](https://v8.dev/docs/ignition)
3. [Ignition 设计文档(翻译版)](https://zhuanlan.zhihu.com/p/41496446)
4. [Ignition Design Doc](https://docs.google.com/document/d/11T2CRex9hXxoJwbYqVQ32yIPMh0uouUZLdyrtmMoL44/edit#)
5. [Sea of Nodes](https://darksi.de/d.sea-of-nodes/)

### 附录
* 字节码列表在 bytecodes.h, 用 BYTECODE_LIST 定义. V8 很喜欢用 XXX_LIST 来精简代码, 例如 Builtins
