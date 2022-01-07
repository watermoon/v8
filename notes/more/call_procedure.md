## JavaScript 调用链追踪

整个流程以 d8 执行一个 javascript 文件, javascript 文件中执行一个通过 CSA 添加的 builtin 函数

* Shell::Main@d8.cc - 入口
    * V8 引擎初始化
    * 创建 Isolate
    * Builtins::built_handle id=61 (InterpreterEntryTrampoline)
    * 执行脚本文件
* Shell::RunMain: 创建 N 个线程, 每个一个 Isolate
    * Shell::RunMain: 执行执行上下文
* SourceGroup::Execute| 执行脚本文件
    * SourceGroup::Execuate 调用 Shell::ExecuteString
* Shell::ExecuteString: 解析脚本
    * Parser::ParseProgram => 得到 Script, 后面会编译 Script
        * Parser::DoParseProgram
    * ScriptCompiler::Compile => CompileUnboundInternal
                                => GetSharedFunctionInfoForScript
                                    => CompileScriptOnMainThread
                                        => CompileToplevel
                                            => IterativelyExecuteAndFinalizeUnoptimizedCompilationJobs
                                                => Compiler::GetSharedFunctionInfo # 用于检查是否已经编译过, 编译过后跳过后面的编译逻辑
                                                => ExecuteSingleUnoptimizedCompilationJob
                                                    => UnoptimizedCompilationJob 构建编译 job, Ignition 解析器相关
                                                    => job->ExecuteJob
                                                        => GenerateBytecode
                                                => UpdateSharedFunctionFlagsAfterCompilation
                                                => FinalizeSingleUnoptimizedCompilationJob
                                            => FinalizeUnoptimizedScriptCompilation
    * Builtins::builtin_handle id=71 CompileLazy(编译 test.js 脚本外层)
    * Shell::ExecuteString
    * Script::Run
* Script::Run 将脚本转成一个 JSFunction
    * Script::Run: 调用 InterpreterEntryTrampoline
    * Execution::Call => 进入 bytecode
    * Execution::Invoke
    * Builtins::builtin_handle id=45 JSEntry
    * GeneratedCode::Call 调用函数指针
    * Builtins::builtin_handle id=71 CompileLazy(编译 test.js 中的 main 函数)
    * 
