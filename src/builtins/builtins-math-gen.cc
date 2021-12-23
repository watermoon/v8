#include "src/builtins/builtins-collections-gen.h"

#include "src/builtins/builtins-constructor-gen.h"
#include "src/builtins/builtins-iterator-gen.h"
#include "src/builtins/builtins-utils-gen.h"
#include "src/codegen/code-stub-assembler.h"

// 注意: 这里的代码和引用资料[1]的代码有一点细微出入, 这里根据 8.8 分支的代码做了调整

namespace v8 {
namespace internal {
    void demo_callback(int a) {
        PrintF("### demo_callback| a=%d", a);
    }

    int demo_cppfunc(const char* msg, int id, int* res) {
        PrintF("### demo_cppfunc| msg=%s id=%d", msg, id);
        for (int i = 0; i < 3; ++i) {
            demo_callback(id * 10 + i);
        }
        *res = 3322;
        return id * 10;
    }

    // TF_BUILTIN 是一个用来快速创建指定 Assembler 子类的宏, 例如这里的基类是 CodeStubAssembler
    // 新创建的类是 MathIs42Assembler
    // 在 builtin 函数体内， 可以通过 "Parameter(n)" 访问参数
    // TF_BUILTIN 定义在 src/builtins/builtins-utils-gen.h
    TF_BUILTIN(MathIs42, CodeStubAssembler) {
        // 加载当前函数的上下文(作为每一个 stub 的隐式参数)和参数 X(在申明的地方 kX 指定)
        auto context = Parameter<Context>(Descriptor::kContext);

        TVARIABLE(Object, x);
        x = Parameter<Object>(Descriptor::kX);
        Print(StringConstant("### MathIs42| x="));
        // Print(x);

        // 此时的 x 可能是任意的 JS 对象, 例如 Smi, HeapNumber, 未定义等
        // 调用已有的 builtin 函数 ToNumber
        TNode<Object> number = x.value();
        Print(StringConstant("### MathIs42| x.value="));
        Print(number);

        int res = 0;
        int ret = demo_cppfunc("test_call_cpp_function", 10, &res);
        Print(SmiConstant(res));
        Print(SmiConstant(ret));

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
                auto value = CallBuiltin(Builtins::kMathIsHeapNumber42, context, num_heap_object);
                // Branch(value.value(), &return_true, &return_false);
                Return(value);
                // TNode<Float64T> value = LoadHeapNumberValue(num_heap_object);
                // Branch(Float64Equal(value, Float64Constant(42)), &return_true, &return_false);
            }
        }

        BIND(&return_true);
        Return(TrueConstant());

        BIND(&return_false);
        Return(FalseConstant());
    }

    TF_BUILTIN(MathIsHeapNumber42, CodeStubAssembler) {
        TVARIABLE(HeapObject, x);
        x = Parameter<HeapObject>(Descriptor::kX);
        TNode<HeapObject> number = x.value();
        CSA_ASSERT(this, IsHeapNumber(number));

        TNode<Float64T> value = LoadHeapNumberValue(number);
        auto is42 = Float64Equal(value, Float64Constant(42));
        Return(SelectBooleanConstant(is42));
    }
}  // namespace internal
}  // namespace v8
