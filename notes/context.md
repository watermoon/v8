## 上下文(Context)
src/objects/contexts.h

* JS 函数是一对 (上下文, 函数代码)
* 在运行时, contexts 会构造一个与执行栈并行的栈, 最顶的 context 作为当前 context
* 所有的 contexts 都具有如下槽位(slots):
    * scope_info: 描述当前 context 的域信息. 它包含静态分配的 context slots, 以及在栈上分配的本地变量
        * 名字在遇到 'with' 或者 'eval' 而进行动态查找时会用到, 并且在调试时也需要
    * previous: 指向上一个 context 的指针
    * extension: 额外的数据, 这个 slot 只有在 extension_bit 时才有效.
        * 对于 native contexts, 包含全局对象
        * 对于 module contexts, 包含模块对象
        * 对于 await contexs, 包含生成器对象
        * 对于 var block contexts, 可能包含一个 "扩展对象(extension object)"
        * 对于 with contexts, 包含一个 "扩展对象"
            * "扩展对象"即用额外的变量进行动态扩展的 context
