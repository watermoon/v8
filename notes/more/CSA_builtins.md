## CodeStubAssembler

### 如何写一个 CSA builtin[1]

#### Builtins
在 V8 中, builtins 可以认为是一段在运行时被 VM 执行的代码块. 通常用于实现 builtin 对象的功能(例如 RegExp 或者 Promise), 也可以用于提供其他的内部功能(例如作为 IC(Inline-Cache, 性能优化的关键) 系统的一部分).

V8 的 builtins 可以用以下几种方式来实现(各有优缺点):
* 平台相关的汇编语言(Platform-dependent assembly language): 可以非常高效, 不过需要手动移植到所有的平台, 并且难以维护
* C++: 在风格上和运行时函数比较相似, 且可以访问 V8 强大的运行时功能, 不过通常不适合与对性能敏感的领域
* JavaScript: 代码简洁且可读性好, 可以访问快速内在函数(fast intrinsics), 不过通常用于较慢的运行时调用. 由于 JS 语法的类型污染、微秒的问题, 其性能通常难以预测
* CodeStubAssembler: 性能比较接近汇编语言, 平台无关. 是对汇编的一个简单的抽象(thin abstraction)
* Torque: 用于替代 CodeStubAssembler, 详情请参考[Torque builtins](./torque.md)

#### 写一个 builtin 的步骤
* 声明: `src/builtins/builtins-definitions.h` 的宏 `BUILTIN_LIST_BASE` 中进行声明, CSA builtins 会划分为一下几类:
    * TFJ: JavaScript linkage
    * TFS: Stub linkage
    * TFC: Stub linkage, 需要一个自定义的接口描述符
    * TFH: Stub linkage, 用于 IC 处理函数
    ```cpp
    // 注意: 这里的代码和引用资料[1]的代码有一点细微出入, 这里根据 8.8 分支的代码做了调整
    // 其中第二个参数(这里是 0)在不同改的版本的 v8 代码需要不一样
    // 8.8 版本需要是参数个数减 1, 具体原因可以看文件 src/builtins/builtins-descriptors.h 中宏 DEFINE_TFJ_INTERFACE_DESCRIPTOR 的定义
    #define BUILTIN_LIST_BASE_TIER1(CPP, TFJ, TFC, TFS, TFH, ASM) \
    // [...snip...]
    TFJ(MathIs42, 1, kReceiver, kX)                               \
    // [...snip...]
    ```
* 定义(即实现): builtin 定义通常放在 `src/builtins/builtins-*-gen.cc` 文件中, 根据功能组织. 例如我们实现一个 Math builtin, 那么放在 `src/builtins/builtins-math-gen.cc`
    ```cpp
    #include "src/builtins/builtins-collections-gen.h"

    #include "src/builtins/builtins-constructor-gen.h"
    #include "src/builtins/builtins-iterator-gen.h"
    #include "src/builtins/builtins-utils-gen.h"
    #include "src/codegen/code-stub-assembler.h"

    // 注意: 这里的代码和引用资料[1]的代码有一点细微出入, 这里根据 8.8 分支的代码做了调整

    namespace v8 {
    namespace internal {
        // TF_BUILTIN 是一个用来快速创建指定 Assembler 子类的宏, 例如这里的基类是 CodeStubAssembler
        // 新创建的类是 MathIs42Assembler
        // 在 builtin 函数体内， 可以通过 "Parameter(n)" 访问参数
        // TF_BUILTIN 定义在 src/builtins/builtins-utils-gen.h
        TF_BUILTIN(MathIs42, CodeStubAssembler) {
            // 加载当前函数的上下文(作为每一个 stub 的隐式参数)和参数 X(在申明的地方 kX 指定)
            // auto context = Parameter<Context>(Descriptor::kContext);

            TVARIABLE(Object, x);
            x = Parameter<Object>(Descriptor::kX);

            // 此时的 x 可能是任意的 JS 对象, 例如 Smi, HeapNumber, 未定义等
            // 调用已有的 builtin 函数 ToNumber
            TNode<Object> number = x.value();

            // 定义几个标签用于进行 goto, 类似于 C++ 中的几个不同的 if 分支
            Label if_issmi(this), not_issmi(this), if_isheapnumber(this), return_true(this), return_false(this);

            // 判断 number 是否是 Smi, 是跳转到 if_issmi 标签, 否则跳转到 if_isheapnumber 标签
            // Branch 属于控制 CSA 的控制流函数之一, 原型如下:
            // void Branch(Node* condition, RawMachineLabel* true_val, RawMachineLabel* false_val);
            // 更多的控制流原型(如 Goto/Switch/Return/PopAndReturn 等)在 src/compiler/raw-machine-assembler.h
            Branch(TaggedIsSmi(number), &if_issmi, &not_issmi);

            // BIND 宏实际是调用 Bind 控制流函数, 用于绑定一个标签相关的代码(开始为标签生成代码)
            BIND(&if_issmi);
            {
                TNode<Smi> num_smi = CAST(number);
                // Print("debug info");
                // Print(num_smi);  // Print 打印
                Branch(SmiEqual(num_smi, SmiConstant(42)), &return_true, &return_false);
            }

            BIND(&not_issmi);
            {
                TNode<HeapObject> num_heap_object = CAST(number);
                Branch(IsHeapNumber(num_heap_object), &if_isheapnumber, &return_false);

                BIND(&if_isheapnumber);
                {
                    TNode<Float64T> value = LoadHeapNumberValue(num_heap_object);
                    Branch(Float64Equal(value, Float64Constant(42)), &return_true, &return_false);
                }
            }

            BIND(&return_true);
            Return(TrueConstant());

            BIND(&return_false);
            Return(FalseConstant());
        }
    }  // namespace internal
    }  // namespace v8

    ```

#### 将函数添加到对象 Math 中
大部分 builtins 在 `src/bootstrapper.cc` 添加(少部分在 *.js 文件中添加)
```cpp
// Existing code to set up Math, included here for clarity.
Handle<JSObject> math = factory->NewJSObject(cons, TENURED);
JSObject::AddProperty(global, name, math, DONT_ENUM);
// […snip…]
SimpleInstallFunction(math, "is42", Builtins::kMathIs42, 1, true);
```

#### 编译 & 运行
在 `BUILD.gn` 的 `v8_initializers/sources` 添加文件 `src/builtins/builtins-math-gen.cc`
```bash
tools/dev/gm.py x64.release
out/x64.release/d8
Math.is42(10086)
```

#### 定义一个 Stub linkage 的函数
同样是在 `src/builtins/builtins-definitions.h` 进行声明, 不过用 TFS 而不是 TFJ 声明
```cpp
    #define BUILTIN_LIST_BASE(CPP, TFJ, TFC, TFS, TFH, ASM)                        \
    // [...snip...]
    TFJ(MathIs42, 1, kReceiver, kX)                                              \
    TFS(MathIsHeapNumber42, kX)                                                  \
    // [...snip...]
```

在 `src/builtins/builtins-math-gen.cc` 文件添加定义
```cpp
    // [...snip...]
    TF_BUILTIN(MathIsHeapNumber42, CodeStubAssembler) {
        TVARIABLE(HeapObject, x);
        x = Parameter<HeapObject>(Descriptor::kX);
        TNode<HeapObject> number = x.value();
        CSA_ASSERT(this, IsHeapNumber(number));

        TNode<Float64T> value = LoadHeapNumberValue(number);
        auto is42 = Float64Equal(value, Float64Constant(42));
        Return(SelectBooleanConstant(is42));
    }
```
修改 `MathIs42` 函数, 改成调用我们新添加的 `MathIsHeapNumber42` builtin
```cpp
    // [...snip...]
    TF_BUILTIN(MathIs42, CodeStubAssembler) {
        // [...snip...]
        BIND(&if_isheapnumber);
        {
            auto value = CallBuiltin(Builtins::kMathIsHeapNumber42, context, num_heap_object);
            Return(value);
        }
        // [...snip...]
    }
```

* 为什么我们需要关系 TFS builtins? 为什么我们不将代码内联到函数中(或者为了更好的可读性提取出来作为一个 helper 函数)？
    * 一个重要的原因是代码空间: builtins 是在编译器生成, 并且包含在 V8 的 snapshot 中, 因此会占用每一个 isolate 的空间(还不小)
    * 将大块的公共代码提取成 TFS builtins 可以明显地以数十到数百 KB 的量级降低空间(内存)

#### 测试 stub-linkage builtins(To be continued...)
即使我们新的 builtins 使用非标准的(至少是非 C++)调用规范, 还是可以为它们写测试用例. 可以在 `test/cctest/compiler/test-run-stubs.cc` 添加如下测试用例:
```cpp
```

* 编译 & 运行
在 `BUILD.gn` 的 `cctest_sources/sources` 添加文件 `"compiler/test-run-stubs.cc"`
```bash
ninja -C out/x64.release
tools/run-tests.py --outdir out/x64.release
tools/run-tests.py --gn
```

### 参考文档
1. [CodeStubAssembler builtins](https://v8.dev/docs/csa-builtins)
2. [Taming architecture complexity in V8 - the CodeStubAssembler](https://v8.dev/blog/csa)