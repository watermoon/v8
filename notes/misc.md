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