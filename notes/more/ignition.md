## Ignition
Ignition 是一个基于寄存器的解析器, 基于 TurboFan. Ignition 中的寄存器不是传统意义的机器寄存器, 而是在函数的栈帧中分配的寄存器文件(register file)中特定的 slot.

### 寄存器
* ACC: 累加寄存器, 被作为很多字节码的隐式输入输出寄存器

### 字节码处理程序
由 TurboFan 编译器生成

### 字节码
```cpp
Ldar: LoaD Accumulator from Register, 将寄存器加载到累加器, 处理函数 DoLdar
```

### 参考资料
1. [Firing up the Ignition interpreter](https://v8.dev/blog/ignition-interpreter)
2. [Ignition](https://v8.dev/docs/ignition)
3. [Ignition 设计文档(翻译版)](https://zhuanlan.zhihu.com/p/41496446)
4. [Ignition Design Doc](https://docs.google.com/document/d/11T2CRex9hXxoJwbYqVQ32yIPMh0uouUZLdyrtmMoL44/edit#)