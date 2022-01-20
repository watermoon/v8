## Miscellaneous notes

* ver8.9 之后, v8 彻底采用新的 callstack(放弃原来的 adapter stack - 主要解决调用参数不匹配问题?)
    * 据官方给得数据, 字节码执行速度提升 11%, TF 优化后的代码速度最高提升有 40%
* Isolate/Context 类值得细看
* InterpreterEntryTrampoline 是字节码的入口(Ignition), 负责构建函数调用的堆栈(不同类型的函数有不同的堆栈), 源码在 Generate_InterpreterEntryTrampoline(不同架构不同)
* Ignition
    * bytecode handler: 每个字节码对应一个 handler
    * bytecode array: 字节码列表, js 代码编译后生成. 执行之前需要预先做好好准备, 包括构建堆栈、参数入栈等, 具体由 InterpreterEntryTrampoline 负责
    * Dispatch: 每执行一条字节码后都调用这个函数
    * Ignition 是一个基于寄存器的解析器, 寄存器由 v8 用栈实现, 不是物理寄存器, 但有个例外, 即累加寄存器, 这是物理寄存器(有了这个, 每个字节码的指令少了一个操作数, 节省了指令长度)
    * dispatch table: 字节码分发表
* builtin 编译后会生成 snapshot_blob.bin 文件
* bytecode handler 只是一种 Builtin，还有其它的 Builtin，byteocde 是 Builtin，Builtin 并不都是 bytecode
* 寄器存访问公式：r[i]=FP-2-kFixedFrameHeaderSize-i
* 参数访问公式：a[i]=FP+2+parameter_count-i-1，parameter_count表示参数的数量
* IGNITION_HANDLER: 生成 bytecode handler 的宏
* 生成 ide 编译工程
    ```bash
    # xcode
    gn gen --ide=xcode ../proj --args="is_component_build = true is_debug = true v8_optimized_debug = false"
    # or
    gn gen --ide=xcode ../proj --args="is_component_build = true symbol_level = 2 is_debug = true v8_optimized_debug = false"
    # visual studio
    gn gen --ide=vs ../proj --args="is_component_build = true is_debug = true v8_optimized_debug = false"
    ```
  * args.gn
      ```bash
      is_component_build = true
      is_debug = true
      symbol_level = 2
      target_cpu = "x64"
      use_goma = false
      goma_dir = "None"
      v8_enable_backtrace = true
      v8_enable_fast_mksnapshot = true
      v8_enable_slow_dchecks = true
      v8_optimized_debug = false
      ```
  * api.h 这个文件的接口值得好好看一下
* gdb -q ./d8
    ```bash
    r --allow_natives_syntax ./test.js
    a = [12, "ab"]
    %DebugPrint(a)
    disas main # 反汇编
    x/gx addr # 打印地址
    # 加断点
    b v8::base::ieee754::cosh
    # 在 GDB 显示 %DebugPrint(在设置了断点的情况下)
    r --allow_natives_syntax ./test.js > ~/Desktop/out.log # %DebugPrint 重定向到文件 out.log
    # v8 对象的内存结构
    ```
* gdb 使用 100 个技巧

https://tech-blog.cymetrics.io/posts/maxchiu/turbofan/