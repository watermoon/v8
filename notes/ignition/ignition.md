## 解析器 Ignition(点火)

### Ignition bytecode pipeline
                    | => Register Optimizer => Peephole Optimizer => Dead-code Elimination
Parser => AST => Bytecode                                                 | => Bytecode Array Writer => Bytecode
                                                                                                           ||
                                                                                                           \/
                                                                                                       Interpreter(Ignition)
* Backend of TurboFan Compiler

### TurboFan pipeline
                            WebAssembly ---
                                          ||
                                          \/
JavaScript Source => JavaScript => Simple => Machine => Scheduler => CodeGen => Machine Code
                                                                  /\
                                                                  ||
                                                          Interpreter Assembler

### Building an Interpreter using TurboFan

### 字节码生成
* InterpreterCompilationJob::ExecuteJobImp()
  * BytecodeGenerator::GenerateBytecode(uintptr_t stack_limit)
    * InitializeAstVisitor(stack_limit);
    * AllocateTopLevelRegisters // 分配顶层寄存器 incoming_new_target_or_generator_
    * GenerateBytecodeBody()
* InterpreterCompilationJob::FinalizeJobImpl()
  * bytecodes = generator()->FinalizeBytecode(...)
  * compilation_info_->SetBytecodeArray(bytecodes)

#### 关键类
* BytecodeGenerator : public AstVisitor<BytecodeGenerator>
* Scope 域: 很关键的一个东西, 还有一个就是 Context 上下文
    * 表示不同类型的作用域, 例如 script/module/declaration/class 等
* Register: 用于在栈帧中索引参数和本地变量, 通过索引(index)
    *  < 0: 参数
    * >= 0: 本地变量

#### 类关系
* InterpreterCompilationJob : UnoptimizedCompilationJob
<img src=./ignition_classes.svg style="background-color:white" />

#### 入口 & 流程
* ExecuteSingleUnoptimizedCompilationJob
  => interpreter::Interpreter::NewCompilationJob
  => job->ExecuteJob
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


### 字节码执行
