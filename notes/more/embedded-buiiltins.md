## Embedded builtins

V8 built-in 函数(builtins) 需要占用每一个 V8 实例的内存. 而每一个 chrome tab 中的 V8 实例数量在快速增长. 本文描述了 google 如何降低每一个 website 的 V8 堆内存, 中位数减少了 19% 的内存.

### 背景
* 2015 年, builtins 还大部分都是用 self-hosted js、原生汇编或者 C++ 实现的。它们都很小, 所以每个 isolate 一份倒也没什么问题
* 2016 年, V8 开始试验使用 CSA 来实现 builtins. 这很方便(因为跨平台), 而且很高效(接近汇编), 所以 CSA 变得到处都是
    * 由于一些列的原因, CSA 倾向于产生更大的代码(相比之前, CSA 实现的 builtins 代码增长到原来的三倍)
* 2017 年中, 每个 isolate 的消耗已经急剧增长了
* 2017 后半期, 我们初步通过实现延迟 builtins(lazy builtin) 和字节码处理器(bytecode handler)反序列化
    * 这一方案是我们的初步分析发现: 大部分的网站都仅使用一般左右的 builtins. Chrome 64 发布, 并取得预期的效果
    * 不过 builtin 的内存消耗依然与 isolate 的数量线性相关
* Spectre[2] 漏洞被揭露, Chrome 最终开启网站隔离(site isolate)来降低它的影响. 而网站隔离会导致更多的 V8 isolate 被创建

### Embedded builtins
* 思想
    * 概念上, builtins 在不同 isolate 是完全一样的, 而会和 isolate 绑定只是因为实现细节
    * 那么只要能将 builtins 做成真正的 isolate 无关的话, 就可以在进程间通过内存共享了
* 现状
    * 生成的 builtin 代码既不是 isolate 也不是进程无关, 主要是因为内嵌的指针指向 isolate 或者进程的特定数据
    * V8 对执行不在托管堆内的生成代码毫无概念

#### isolate 和进程无关代码
Builtins 是由  V8 编译器的内部流水线生成, 其中在代码中直接嵌入了堆常量(heap constants, 在 isolate 的托管堆内存)、调用目标(call targets, 代码对象, 也在托管堆中)、以及 isolate 和进程的特定地址(例如 C 运行时函数, 或者 isolate 自身的指针, 也称为'外部引用(external references)‘ 等等引用.
在 X64 的汇编中, 加载这样一个对象的代码可能像这样:
```asm
// Load an embedded address into register rbx.
REX.W movq rbx,0x56526afd0f70
```
V8 有一个运行中的 GC, 目标对象的位置可能会随着时间修改, GC 会更新生成的代码来指向新的位置.

在 x64 和其它大部分架构中, 调用其他的代码对象是通过一个高效的调用指令, 这个指令通过指定相对于当前 PC(program counter)的偏移来指明目标代码对象.
一个有意思的细节是:
V8 在托管堆的开始出预留了整个 CODE_SPACE, 以保证所有可能的代码对象保持在一个互相可以寻址的偏移处. 相关的调用可能像这个样子:
```asm
// Call instruction located at [pc + <offset>].
call <offset>
```

<img src="./pc-relative-call.png" style="background-color: white" />

为了可以在进程间共享, 生成的代码必须不可修改(immutable), 同时保持 isolate 和进程无关. 上面的指令序列都不满足要求: 他们直接将地址嵌入到代码中, 并且在运行时通过 GC 打补丁(即 GC 运行时更新和修改)

为了解决这个问题, google 引入了一个间接的专用的, 所谓的根寄存器, 其中保存了一个指向当前 isolate 已知位置的指针.

<img src="./isolate-layout.png" style="background-color: white" />

V8 的 isolate 类包含了根表(roots table), 表里包含了指向所有在托管堆上的根对象的指针. 而根寄存器永远保存这根表的地址. 因此以新的, isolate 和进程无关的方式加载一个根对象变成:
```asm
// Load the constant address located at the given
// offset from roots.
REX.W movq rax,[kRootRegister + <offset>]
```
根堆常量可以像之前一样直接加载, 其它的堆常量则需要使用间接的方式: 先加载全局的 builtin 常量池, 然后再到目标堆常量:
```asm
// Load the builtins constant pool, then the
// desired constant.
REX.W movq rax,[kRootRegister + <offset>]
REX.W movq rax,[rax + 0x1d7]
```
对于代码对象, google 最初切换成一个更加复杂的方式: 从全局 builtins 常量池加载目标代码对象, 将目标地址加载仅寄存器, 最后执行间接调用

以上, 生成代码变成了 isolate 和进程无关的了, 我们可以在进程间共享它(builtins)了.

### Sharing across processes
一开始两种方案:
* 通过 mmap 的数据块来进行共享
* 直接嵌到二进制中(不需要用户, 即 embedded 来做额外的修改)

一个可执行文件划分成几个部分. 例如 ELF 二进制包含了:
* .data(intialized data)
* .ro_data(initialized read-only data)
* .bss(uninitialized data)
* .text(native executable code)

目标是将 builtins 代码打包进 .text 区域

<img src=./binary-format.png style="background-color:white;"/>

这些是通过在 V8 的内部编译器流水线中增加一个新的编译步骤来为所有的 builtins 生成原生代码(native code), 然后输出到 embedded.cc 的文件中.

<img src=./build-process.png style="background-color:white;"/>

embedded.cc 文件包含元数据和生成的 builtins 机器码(以 .byte 指令的形式)来指引 C++ 编译器(我们这里是 gcc/clang) 来将指定的字节序列直接放到 object 文件中(然后是可执行文件)
```asm
// Information about embedded builtins are included in
// a metadata table.
V8_EMBEDDED_TEXT_HEADER(v8_Default_embedded_blob_)
__asm__(".byte 0x65,0x6d,0xcd,0x37,0xa8,0x1b,0x25,0x7e\n"
[snip metadata]

// Followed by the generated machine code.
__asm__(V8_ASM_LABEL("Builtins_RecordWrite"));
__asm__(".byte 0x55,0x48,0x89,0xe5,0x6a,0x18,0x48,0x83\n"
[snip builtins code]
```
.text 段的内容会在运行时映射到只读的可执行内存. 如果一段内存只包含位置无关的代码而没有可重定位的符号, 操作系统会在运行时在不同的进程间共享这部分内存.

不过 V8 的代码对象(Code objects)不仅包含指令流, 还包含一些元数据(有些时候还是 isolate 相关的). 普通的代码对象打包操作会将元数据和指令流一起打包进托管堆的一个变长代码对象中.

<img src=./code-on-heap.png style="background-color:white;"/>

每一个 builtin 在托管堆中有一个关联的代码对象, 成为 `off-heap trampoline`(离堆蹦蹦床, 即胶水函数). 元数据想标准的代码对象一样保存在 trampoline 中, 但是内联的指令流仅仅包含一个很短的指令: 加载 builtin 真正的地址, 然后跳转过去.

<img src=./code-off-heap.png style="background-color: white" />

### 性能优化
上一节提到的解决方案基本是特性完整(feature-complete), 不过 benchmark 显示有比较大的性能损失. Speedometer 2.0 显示有 5% 的倒退.

性能损失主要在于频繁的间接访问:
* 从根列表加载根常量(1 次间接访问)
* 其它堆常量从全局 builtins 常量池访问(2 次)
* 在一个堆对象中解包外部引用(3 次)

最大的问题是我们新的调用序列, 加载 trampoline 代码对象(托管堆中), 调用它(指 trampoline), 仅仅为了跳到目标地址. 最终, 显现出来的就是在托管堆和嵌入二进制的代码间的调用天生地更慢, 可能是长距离的跳转(long jump distance)干扰了 CPU 的分支预测

因此我们的工作集中在:
1. 减少间接访问
    * 修改 isolate 的对象布局, 将大部分的对象加载改进成相对于 root 的一次加载(turn most object loads into a single root-relative)
    * 全局的 builtins 常量池依然存在, 单仅包含不常访问的对象
2. 提高 builtin 调用序列
    * builtin 到 builtin 的调用转换成一个相对于 pc 的调用指令. 这个对于运行时生成的 JIT 代码无法实现, 因此此时相对与 pc 的偏移可能超过了最大的 32-bit 值
    * 内联了离堆(off-heap) trampoline 到所有的调用点(call sites), 将调用序列从 6 个指令减少到 2 个指令

这些优化昨晚后, 性能损失被限制到了 0.5% 左右

<img src=./isolate-layout-optimized.png style="background-color:white" />

### 结果
<img src=./result.png style="background-color:white" />

消耗从 `c * (1 + n)` (其中 c 是所有 builtins 的内存消耗, n 是 isolate 的数量) 降低到了 `c * 1`
注: 实际上由于离堆 trampoline 的存在, 还是有一些每个 isolate 相关的消耗.

中位数来看, V8 堆内存消耗下降了 19%. 绝对值, 50 分位节省改了 1.9M, 30 分位节省 3.4M, 10 分位节省了 6.5M

### 原文链接
1. [embedded-builtins](https://v8.dev/blog/embedded-builtins)
2. [About Spectre](https://googleprojectzero.blogspot.com/2018/01/reading-privileged-memory-with-side.html)