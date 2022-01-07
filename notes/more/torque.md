## Torque 用户手册

### 零散知识
* BUILTIN@builtins-utils.h
    * 定义 C++ 的 builtin 函数

* BUILTIN_LIST
* setup-builtins-internal.cc: 建立 builtin

### 参考例子
* class Uri @uri.h
* Json parser @builtins-json.cc => json-parser.h

### 试验
* risk, 参考 Uri?
* HiRisk, 参考 GlobalDecodeURI 实现
    * builtins-global.cc: 函数实现
* 定义相关枚举值? builtins-definitions.h

<br />

### Torque 如何生成代码
* `gn` 构建先调用 Torque 编译器处理 `*.tq` 文件, 在 `gen/torque-generated` 生成对应的 `*-tq-csa.cc` 和 `-tq-csa.h` 文件, 也会生成一下在编译 V8 时需要用到的 `.h` 头文件(通常在包含那些在 `.tq` 文件中定义的类)
* `gn` 接着编译上一步生成的 `.cc` 文件, 生成 `mksnapshot` 可执行文件
* 当 `mksnapshot` 运行的时候, 所有 V8 的 builtins 都会生成和打包到快照文件(snapshot file)中. 其中包含了所有在 Torque 中定义以及用到 Torque 定义的功能的 builtins
* 编译 V8 剩下的部分. 所有的 Torque-authored 的 builtins 都可以通过 snapshot 访问到, 而这个 snapshot 会被链接到 V8 中.

生成过程如下图所示:
<img src=./build-process.svg style="background-color:white;"/>

### 调试
运行的时候加上参数 `--gdb-jit-full`

### `constexpr`
类似 C++ 中的 `constexpr` 表达式, Torque 也借来了这个语法, 但是和 C++ 还是有一些区别:
* C++ 是完全在编译器执行
* Torque 不是完全在编译器(Torque 编译器)执行的, 部分是在 `mksnapshot` 运行阶段执行, 然后生成到 snapshot 文件中
* 和泛化结合, `constexpr` 是可以自动地生成多个非常有效的特例化(specialized) builtins

### 文件
* 后缀 `.tq`
* 语法
    ```tq
    Declaration :
        AbstractTypeDeclaration
        ClassDeclaration
        TypeAliasDeclaration
        EnumDeclaration
        CallableDeclaration
        ConstDeclaration
        GenericSpecialization

    NamespaceDeclaration :
        namespace IdentifierName { Declaration* }

    FileDeclaration :
        NamespaceDeclaration
        Declaration
    ```

### 命名空间
和 C++ 类似, 没什么好说

### 声明
#### 类型
* Torque 是强类型, 但是有个问题是: Torque 并不天生地知道写 Torque 时的核心类型
* Torque 的类型系统严格指定了 Torque 类型之间的关系, 以和手写的 `CodeStubAssembler` 更好地交互
* 但是 Torque 类型与 `CodeStubAssembler` 和 C++ 类型之间却是一个松耦合, 没有一个显式的映射关系. 而是依赖于 C++ 编译器来保证严格的映射
* 抽象类型(Abstract types): Torque 的抽象类型直接映射到 C++ **编译期**和 CodeStubAssembler **运行期**的值. 声明指定了一个名字和 C++ 类型的关系
    * 语法
        ```tq
        AbstractTypeDeclaration :
            type IdentifierName ExtendsDeclaration opt GeneratesDeclaration opt ConstexprDeclaration opt

        ExtendsDeclaration :
            extends IdentifierName ;

        GeneratesDeclaration :
            generates StringLiteral ;

        ConstexprDeclaration :
            constexpr StringLiteral ;
        ```
    * `IdentifierName`: 抽象类型的名字
    * `ExtendsDeclaration`: 指明这类型是从哪个类型继承而来(optional)
    * `GeneratesDeclaration`: 指明一个字符串来对应在 `CodeStubAssembler` 代码中使用的 C++ `TNode` 类型, 来包含它的类型的运行期的值
    * `ConstexprDeclaration`: 用来在构建时间(`mksnapshot`-时间)计算的, 一个字符串指明 C++ 类型来对应 `constexpr` 版本的 Torque 类型
    * `base.tq` 中 Torque 的 31/32 bit 有符号整数类型
        ```tq
        type int32 generates 'TNode<Int32T>' constexpr 'int32_t';
        type int31 extends int32 generates 'TNode<Int32T>' constexpr 'int31_t';
        ```
    可以看到, 上面的几个字段都只是为了告诉 `CodeStubAssembler` 和 `mksnapshot` 在运行期或者编辑期字段的类型是什么

* 联合类型(Union types): 表明一个类型属于几个可能的类型. 只允许 tagged 值是联合类型, 因为它们可以在运行期通过 map 指针区分. (非 tagged 类型无法在运行期区分)
    * 联合类型需要满足以下条件
        * `A | B = B | A` - 交换律
        * `A | (B | C) = (A | B) | C` - 结合律
        * `A | B = A` - 当 `B` 是 `A` 的子类型

* 类类型(Class types): 
    * 每一个 Torque 类的类型必须对应到一个 C++ 代码中的 一个 HeapObject 的子类
    * 为了最小化维护 V8 中 C++ 和 Torque 实现中的对象访问代码(object-accessing code)的消耗, Torque 类定义会在可能并且合适的时候被用来生成 C++ 对象访问代码, 以此来减少手动保证 C++ 和 Torque 同步的麻烦
    * 语法
        ```tq
        ClassDeclaration :
            ClassAnnotation* extern opt transient opt class IdentifierName ExtendsDeclaration opt GeneratesDeclaration opt {
                ClassMethodDeclaration*
                ClassFieldDeclaration*
            }

        ClassAnnotation :
            @doNotGenerateCppClass
            @generateBodyDescriptor
            @generatePrint
            @abstract
            @export
            @noVerifier
            @hasSameInstanceTypeAsParent
            @highestInstanceTypeWithinParentClassRange
            @lowestInstanceTypeWithinParentClassRange
            @reserveBitsInInstanceType ( NumericLiteral )
            @apiExposedInstanceTypeValue ( NumericLiteral )

        ClassMethodDeclaration :
            transitioning opt IdentifierName ImplicitParameters opt ExplicitParameters ReturnType opt LabelsDeclaration opt StatementBlock

        ClassFieldDeclaration :
            ClassFieldAnnotation* weak opt const opt FieldDeclaration;

        ClassFieldAnnotation :
            @noVerifier
            @if ( Identifier )
            @ifnot ( Identifier )

        FieldDeclaration :
            Identifier ArraySpecifier opt : Type ;

        ArraySpecifier :
            [ Expression ]
        ```
    * 例子
        ```tq
        extern class JSProxy extends JSReceiver {
            target: JSReceiver|Null;
            handler: JSReceiver|Null;
        }
        ```
        `extern` 标志着这个类定义在 C++, 而不是只在 Torque
    * 类成员定义会隐式地生成可以在 CodeStubAssembler 中使用的 getters 和 setters
    * C++ 手写的类必须继承自 Torque 生成的类模板, 按照上面的类似可能是
        ```c++
        // In js-proxy.h
        class JSProxy : public TorqueGeneratedJSProxy<JSProxy, JSReceiver> {

            // Whatever the class needs beyond Torque-generated stuff goes here...

            // At the end, because it messes with public/private:
            TQ_OBJECT_CONSTRUCTORS(JSProxy)
        }

        // In js-proxy-inl.h:
        TQ_OBJECT_CONSTRUCTORS_IMPL(JSProxy)
        ```
    * 类的类型注解(Class type annotations)
        * `@doNotGenerateCppClass` - 可用于不能使用上面的模式继承的时候, 直接继承自它的超类, 然后包含 Torque 生成的宏用于访问字段偏移常量
            ```cpp
            class JSProxy : public JSReceiver {
             public:
              DEFINE_FIELD_OFFSET_CONSTANTS(
                  JSReceiver::kHeaderSize, TORQUE_GENERATED_JS_PROXY_FIELDS)
              // Reset of class omitted...
            };
            ```
        * `@generateBodyDescriptor` - 会使得 Torque 在类内生成一个 `BodyDescriptor` 类, 用于说明 GC 应该如何访问对象. 否则, C++ 代码必须定义自己的对象访问方式, 或者使用现在的模式之一(例如继承 `Struct` 且在 `STRUCT_LIST` 添加类, 表示这个类期望仅包含 tagged 值)
        * `@generatePrint` - 生成器会实现一个 C++ 函数来按照 Torque 布局定义的字段的值. 例如 `void TorqueGeneratedJSProxy<JSProxy, JSReceiver>::JSProxyPrint(std::ostream& os)`
        * `@noVerifier` - Torque 编译器默会为所有 `extern` 类生成验证代码, 除非设置了 `noVerifier` 注解. 例如上面的例子会生成 `void TorqueGeneratedClassVerifiers::JSProxyVerify(JSProxy o, Isolate* isolate)` 函数类验证字段的有效性. 同时也会在生成的类 `TorqueGeneratedJSProxy<JSProxy, JSReceiver>::JSProxyVerify` 中生成对应的函数, 此对应函数会调用 `TorqueGeneratedClassVerifiers` 中的静态方法.
            * 如果你想对垒增加额外的验证, 可以通过在 C++ 类中添加 `DECL_VERIFER(JSProxy)`(这会掩盖(hide)被继承的类 `JSProxyVerify`), 并在 `src/objects-debug.cc` 实现它.
            * 这种自定义的校验器(verifier)的第一步需要调用生成的校验器, 例如 `TorqueGeneratedClassVerifiers::JSProxyVerify(*this, isolate);`(类似调用基类构造函数)
            * 如果希望在每次 GC 前后调用这些校验器, 添加编译选项 `v8_enable_verify_heap = true`, 且运行时使用参数 `--verify-heap`
        * `@abstract` - 表示类不会被实例化(类似纯虚类), 没有自己的实例类型. 逻辑上属于这个类的实例类型是其派生类的实例类型
        * `@export` - Torque 编译机会为类生成一个具体的(concrete) C++ 类(例如上面例子中的 `JSProxy`). 当你不希望在编译器提供的 Torque 生成类上增加任何功能时, 这很明显非常有用. **不能和 `extern` 一起使用**, 对于一个仅在 Torque 内使用的类, 既不用 `extern` 也不用 `@export` 是最合适的
        * `@hasSameInstanceTypeAsParent` - 表示与分类拥有同样的实例类型, 不过可能重命名了一些字段或者有一个不同的映射(map). 这种情况下, 其分类不是抽象的(abstract)
        * `@highestInstanceTypeWithinParentClassRange/lowestInstanceTypeWithinParentClassRange/reserveBitsInInstanceType/apiExposedInstanceTypeValue` - 这几个注解都会影响实例类型的生成. 通常情况下忽略这些也没事. Torque 负责在枚举 `v8::internal::InstanceType` 中为每一个类附一个唯一的值. 这样 V8 可以在运行时确定 JS 堆(heap)中的任意对象类型
            * Torque 的实例类型赋值对于大部分情况下都是足够的, 但是在极少数情况下, 我们可能希望为特定的类设置我们制定的实例类型, 例如对于不同的构建(across builds)实例联系性保持一致; 又或者希望在实例类型范围的开头或者结束赋值给它的弗雷; 或者预留一部分实例类型给 Torque 外面来定义
    * 类字段(Class fields)
        * 和普通值(plain value, 不知道哪个词合适)一样, class 字段可以包含索引的数据, 例如:
            ```tq
            extern class CoverageInfo extends HeapObject {
              const slot_count: int32;
              slots[slot_count]: CoverageInfoSlot;
            }
            ```
            表示 `CoverageInfo` 的大小是可变的(基于 `slot_count`)
        * 和 C++ 不同, Torque 不会隐式地在字段之间进行填充(add padding). 相反, 如果字段没有正确对齐会失败并跑出错误.
        * 另外, Torque 要求强字段(strong fields), 弱字段(weak fields) 和标量字段(scalar fields)这些字段根据类别(category)放到一起
        * `const` 表示一个字段不能在运行期修改(或至少不能轻易修改), Torque 会然编译失败, 如果你尝试设置它
        * `weak` 一个字段的前面声明 `weak` 表示这个字段应该和其它的 `weak` 字段放到一起. `weak` 还会影响例如常量 `kEndOfStrongFieldsOffset` 和 `kStartOfWeakFieldsOffset` 的生成(这两个常量会在自定义的 `BodyDescriptors` 中使用. 我们希望在 Torque 能完全生成全部的 `BodyDescriptors` 时移除这个关键字)
            * 当一个对象存储在一个可能是弱引用的字段(第二位是 1)中, `Weak<T>` 应该在类型中使用.
        * `@if/ifnot` 标记着字段应该包含在一些构建配置中, 而不是其它(配置). 他们接受文件 `src/torque/torque-parser.cc` 中的 `BuildFlags` 列表中的值
    * 完全在 Torque 之外定义的类(Classes defined entirely outside Torque)
        * 有些类不在 Torque 中定义, 但是 Torque 必须知道每一个类, 因为它需要为实例类型复制.
        * 对于这些类, 可以声明成一个没有 body 的类, Torque 就不会为它声明出实例类型意外的任何东西, 例如:
            ```tq
            extern class OrderedHashMap extends HashTable;
            ```

* 形状(Shapes)
    * 类似于定义一个类, 除了使用的关键字 `shape` 而不是 `class`
    * 一个 `shape` 是 `JSObject` 的子类型, 表示对象内属性(特别是"数据属性"而不是"内部槽位(internal slots)")在一个时间点(point-in-time)排列
    * 一个 `shape` 没有自己的实例类型(instance type)
    * 一个拥有特定形状(shape)的对象可能会在任意时间改变或者失去这个形状(shape, 翻译成特征好点?), 因为对象可能会进入到字典模式, 并将它所有属性移出到一个单独的辅助存储器(backing store)

* 结构体(Structs)
    * 数据的集合, 可以包含操作这些数据的 macros
    * 语法和 class 类似
    * 结构体注解(struct annotations)
        * 任何标记了 `@export` 的结构体都会被包含在生成的文件 `gen/torque-generated/csa-types-tq.h` 中, 且名字会增加前缀 `TorqueStruct`
        * 结构体字段可以被标记为 `const`, 表示不能被写入(written to). 但是整个结构体可以被覆写(overwrittern)
    * 结构体作为类字段(structs as fields)
        * 作为类字段， 表示类内的字段挤压(packed)和排序(ordered)(structs 本身没有对齐的要求), 这对于类内字段的索引比较有用
    * 引用和分片(References and Slices)
        * `Reference<T>` 和 `Slice<T>` 是特殊的结构体, 表示堆对象内数据的指针. 两者都包含一个对象和一个 offset. `Slice<T>` 还包含一个长度
        * 除了直接构造结构体, 可以通过特殊语法 `&o.x` 创建一个对象 `o` 的 `x` 字段的引用, 如果 `x` 是一个索引字段(indexed field), 则会创建一个 `Slice`
        * `Reference<T>` 可以通过 `*` 或 `->` 进行解引用, 与 C++ 语法一样
        * `Reference<T>` 不应该直接使用, 而是使用它的两个子类型 `MutableReference<T>` 和 `ConstReference<T>`, 通过语法糖 `&T` 和 `const &T` 创建

* 位字段结构体(Bitfield structs)
一个 `bitfield struct` 表示一系列的数字型的数据打包成一个单一的数字. 其语法与 `struct` 类似, 除了每个字段数有额外的数字表明占用多少位, 例如:
    ```tq
    bitfield struct DebuggerHints extends uint31 {
    side_effect_state: int32: 2 bit;
    debug_is_blackboxed: bool: 1 bit;
    computed_debug_is_blackboxed: bool: 1 bit;
    debugging_id: int32: 20 bit;
    }
    ```
如果一个 bitfield 结构体或者任何其它数字型数据保存在一个 Smi 中, 它可以使用类型 `SimTagged<T>` 表示

* 函数指针类型(Function pointer types): 函数指针类型仅可以指向在 Torque 定义的 builtins, 因为这保正了默认的 ABI, 可以像一样对一个匿名函数进行 `typedef`
    ```tq
    type CompareBuiltinFn = builtin(implicit context: Context)(Object, Object, Object) => Number;
    ```

* 特殊类型(Special types): 两个关键字表明的特殊类型 `void` 和 `never`. 一个是不会返回值, 另外一个是永远不会实际返回(例如, 仅通过退出处理异常路径)

* 瞬时类型(Transient types)(理解还有些障碍)
由于 V8 中的堆对象布局可以在运行期改变, 为了表示一个对象的这种性质, 可以在声明一个类型时加上 `transient` 关键字
`transitioning` 关键字则用于标记可调用的类型(即函数)

* 枚举(Enum)
    * 作用与 C++ 类似, 但是语法有一些区别
        ```tq
        EnumDeclaration :
          extern enum IdentifierName ExtendsDeclaration opt ConstexprDeclaration opt { IdentifierName list+ (, ...) opt }
        ```
        简单的例子如下:
        ```tq
        extern enum LanguageMode extends Smi {
          kStrict,
          kSloppy
        }
        ```
        * `extends` 表明底层的类型是 `Smi` 生成的 `TNode<Smi>`
        * 由于没有指定 `constexpr` 语句来指明枚举的名字, 编译器会生成一个 `constexpr LanguageMode`(来指定枚举的默认名字)
        * 如果没有 `extends` 语句, Torque 会生成仅有 `constexpr` 版本的枚举类型
        * `extern` 告诉 Torque 编译器, 有一个这个枚举的 C++ 定义(目前仅支持 `extern` 类型的枚举)
        * 如果 C++ 中定义的枚举值比 `.tq` 文件中多, 需要通过在 `.tq` 定义的枚举最后一个值之后添加 `...` 告诉 Torque. 例如:
            ```tq
            enum ExtractFixedArrayFlag constexpr 'CodeStubAssembler::ExtractFixedArrayFlag' {
              kFixedDoubleArrays,
              kAllFixedArrays,
              kFixedArrays,
              ...
            }
            ```

#### 可调用(Callables)
概念上类似于 Javascript 或者 C++ 中的函数, Torque 提供了几种类型的可调用性: `macro`s, `builtin`s, `runtime`s, `intrinsic`s

* `macro` callabales: 对应一块(chunk) 生成的 CSA-producing C++
    * 可以完全在 Torque 中定义, 此时 Torque 编译器负责生成 CSA 代码
    * 也可以用 `extern` 标记, 此时必须提供实现在 CodeStubAssembler 类的 CSA 代码
    * 概念上, 可以认为 `macro`s 是一块内联的 CSA 代码
    * 语法
        ```txt
        MacroDeclaration :
            transitioning opt macro IdentifierName ImplicitParameters opt ExplicitParameters ReturnType opt LabelsDeclaration opt StatementBlock
            extern transitioning opt macro IdentifierName ImplicitParameters opt ExplicitTypes ReturnType opt LabelsDeclaration opt ;
        ```
    * 非 `extern` 的 Torque `macro` 用其主体的 `StatementBlock` 来在它的命名空间的生成的 `Assembler` 类中生成 CSA 函数. 这些代码比较像 `code-stub-assembler.cc` 中的代码(可读性会差一些)
    * 可以有显式和隐式的参数, 以及可选的返回类型、可选的标签(label, 给直接 jump to 使用, 通常用于异常退出)
    ```tq
    extern macro BranchIfFastJSArrayForCopy(Object, Context): never
        labels Taken, NotTaken;
    macro BranchIfNotFastJSArrayForCopy(implicit context: Context)(o: Object):
        never
        labels Taken, NotTaken {
      BranchIfFastJSArrayForCopy(o, context) otherwise NotTaken, Taken;
    }
    ```

* `builtin` callables: 与 `macro` 类似, 区别在于: 从 Torque 代码调用的时候, CSA 代码不会在调用端内联(inlined)
    * 语法
        ```tq
        MacroDeclaration :
            transitioning opt javascript opt builtin IdentifierName ImplicitParameters opt ExplicitParametersOrVarArgs ReturnType opt StatementBlock
            extern transitioning opt javascript opt builtin IdentifierName ImplicitParameters opt ExplicitTypesOrVarArgs ReturnType opt ;
        ```
    * `builtin` 也不能有 labels(`macro` 可以有)
    * 可以为 builtin 精心设计一个尾调用(tailcall), 或者一个运行时函数当且仅当它是 builtin 内的最后一个调用。这样编译器可能可以避免创建一个新的栈帧(stack frame)
        * 例如简单的在调用前加上关键字 `tail`, 如 `tail MyBuiltin(foo, bar);`

* `runtime` callbales: 与 `builtin`s 类似, 可以暴露一个接口给外部的 Torque, 也不能有标签(labels)
    * 必须以 V8 标准运行时回调实现(啥事标准运行时回调呢?), 而不是在 CSA 中实现
    * 语法
        ```tq
        MacroDeclaration :
            extern transitioning opt runtime IdentifierName ImplicitParameters opt ExplicitTypesOrVarArgs ReturnType opt ;
        ```
    * 可以在合适的时候作为尾调用, 调用前增加 `tail` 关键字

* `intrinsic` callables: 提供无法在 Torque 中以其它方式实现的接口来访问内部功能(有点拗口, 因为也理解不够透彻). 它们在 Torque 中声明但没有实现, 因为实现是由 Toruqe 编译器提供的
    * 语法
        ```tq
        IntrinsicDeclaration :
            intrinsic % IdentifierName ImplicitParameters opt ExplicitParameters ReturnType opt ;
        ```
    * 大部分情况下, "用户"的 Torque 代码应该很少会直接用到 `intrinsic`s, 当前支持的 `intrinsic`是有:
        ```tq
        // %RawObjectCast downcasts from Object to a subtype of Object without
        // rigorous testing if the object is actually the destination type.
        // RawObjectCasts should *never* (well, almost never) be used anywhere in
        // Torque code except for in Torque-based UnsafeCast operators preceeded by an
        // appropriate type assert()
        intrinsic %RawObjectCast<A: type>(o: Object): A;

        // %RawPointerCast downcasts from RawPtr to a subtype of RawPtr without
        // rigorous testing if the object is actually the destination type.
        intrinsic %RawPointerCast<A: type>(p: RawPtr): A;

        // %RawConstexprCast converts one compile-time constant value to another.
        // Both the source and destination types should be 'constexpr'.
        // %RawConstexprCast translate to static_casts in the generated C++ code.
        intrinsic %RawConstexprCast<To: type, From: type>(f: From): To;

        // %FromConstexpr converts a constexpr value into into a non-constexpr
        // value. Currently, only conversion to the following non-constexpr types
        // are supported: Smi, Number, String, uintptr, intptr, and int32
        intrinsic %FromConstexpr<To: type, From: type>(b: From): To;

        // %Allocate allocates an unitialized object of size 'size' from V8's
        // GC heap and "reinterpret casts" the resulting object pointer to the
        // specified Torque class, allowing constructors to subsequently use
        // standard field access operators to initialize the object.
        // This intrinsic should never be called from Torque code. It's used
        // internally when desugaring the 'new' operator.
        intrinsic %Allocate<Class: type>(size: intptr): Class;
        ```
    * 与 `builtin`s 和 `runtime`s 一样, `intrinsic`s 也不能有标签

#### 显式参数(Explict parameters)
    * 参数列表和 TypeScript 函数的类似, 但是不支持 **可选参数** 和 **默认参数**
    * 如果使用内部的 JavaScript 调用规范(builtin 前加上前缀 `javascript`), 还可以支持剩余参数(rest parameters, 即 "..." 形式)
    * 语法
        ```tq
        ExplicitParameters :
            ( ( IdentifierName : TypeIdentifierName ) list* )
            ( ( IdentifierName : TypeIdentifierName ) list+ (, ... IdentifierName ) opt )
        ```
        例如:
        ```tq
        javascript builtin ArraySlice(
            (implicit context: Context)(receiver: Object, ...arguments): Object {
            // …
        }
        ```

#### 隐式参数(Implicit parameters)
    * 可以类似 `Scala 隐式参数`一样指定隐式参数
    * 语法
        ```tq
        ImplicitParameters :
            ( implicit ( IdentifierName : TypeIdentifierName ) list* )
        ```
    * 具体地, 一个 `macro` 可以在显示参数之外再申明隐式参数, 例如:
        ```tq
        macro Foo(implicit context: Context)(x: Smi, y: Smi)
        ```
    * 与 Scala 不同, V8 要求隐式参数的名字必须一样
    * 隐式参数不会影响重载(overload resolution), 即在比较重载集合的候选(函数)时, 不会在调用侧考虑可用的隐式绑定. 只有在选择了一个最佳重载时, 才会考虑隐式绑定参数是否合适
    * `js-implicit`: 如果 Torque 中的函数是 JavaScript 链接的, 应该是用 `js-implicit` 替代 `implicit`, 此时参数类型限制在以下几类:
        * context: `NativeContext`
        * receiver: `JSAny`(JavaScript 中的 `this`)
        * target: `JSFunction`(JavaScript 中的 `arguments.callee`)
        * newTarget: `JSAny`(JavaScript 中的 `new.target`)
        * 上面的四个参数类型按需声明即可, 不需要每个都声明

#### 重载 resolution(overload resolution)
    * `macro`s 和操作符(operators, 其实也是 `macro`s 的别称) 允许参数类型的重载
    * 重载规则受 C++ 启发: 严格优于其他首选才会被选择, 意味着至少一个参数是严格比其它的好, 且其它参数不差于其它的候选

#### 延迟块(Deferred blocks)
一段语句可以被标记为 `deferred`, 用于告诉编译这段代码会比较少执行到(有点像 C++ 的 unlikely). 这样编译器就可以考虑将相关代码放到最后, 从而提升缓存局部性(类似 CPU 的分支预测)

### 移植 CSA 代码到 Torque
[Array.of 接口从 CSA 移植到 Torque](https://chromium-review.googlesource.com/c/v8/v8/+/1296464)

### 参考资料
1. [V8 Torque user manual](https://v8.dev/docs/torque)
2. [learning-v8](https://github.com/danbev/learning-v8#torque)

### 杂七杂八资料
* [V8 构建系统](https://www.bilibili.com/video/BV1K7411a7tQ/?spm_id_from=333.788.recommend_more_video.-1)
    * 中科院软件所的分享, 比较硬核, 对应的 [github](https://github.com/plctlab/v8-internals)
* [a deep dive into v8](https://blog.appsignal.com/2020/07/01/a-deep-dive-into-v8.html)
* [Chrome V8 源码](https://www.zhihu.com/people/v8blink)
* [奇技淫巧学 V8 系列](https://www.zhihu.com/people/superzheng.com/posts)
* [V8 测试, 编译测试用例](https://v8.dev/docs/test)
* [V8 官方 blog](https://v8.dev/docs)
