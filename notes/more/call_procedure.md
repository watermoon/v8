## JavaScript 调用链追踪
* 代码版本: v8.8(不同版本有差异)

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
    * Parser::ParseProgram => 得到 Script, 后面会编译 Script  # FLAG_parse_only 才进来
        * Parser::DoParseProgram
    * ScriptCompiler::Compile # 大部分都进这里吧
      => CompileUnboundInternal
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
                  => InstallUnoptimizedCode
                    => 创建 feedback 元数据
              => FinalizeUnoptimizedScriptCompilation
    * ScriptCompiler::CreateCodeCache && StoreInCodeCache # 按需缓存代码
    * Script::Run # 执行脚本
    * ScriptCompiler::CreateCodeCache && StoreInCodeCache # 按需缓存执行后代码
* Script::Run 将脚本转成一个 JSFunction
    * Script::Run: 调用 InterpreterEntryTrampoline
    * Execution::Call
    * Execution::Invoke
    * v8::internal::(anonymous-namespace)::Invoke
      => JSEntry 拿到 JS 入口函数的 Code 对象(会用到其首个指令)
        => 根据 target 不同获取不同 builtin 的代码对象
          * 构造函数?: JSConstructEntry
          * Callable: JSEntry
          * Microtask: JSRunMicrotasksEntry
      => 得到对应的入口函数的 GeneratedCode 对象 stub_entry
        => stub_entry.Call 调用具体的 JS 函数, 即调用函数指针
        => 进入汇编了, 线索断了. 最后的线索在 Generate_JSEntry@builtins-x64.cc
