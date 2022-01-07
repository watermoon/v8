## 对象关系

* Object: 所有对象的基类, 大量的 static 方法和其它基础方法
  * Smi: [31 bit signed int] 0
  * TaggedIndex
  * HeapObject: [32 bit direct pointer] (4 字节对齐) | 01
      * JSReceiver
        * JSObject
          * JSArray
          * JSFunctionOrBoundFunction
              * JSBoundFunction
              * JSFunction
          * JSRegExp
        * JSProxy
      * FixedArrayBase
        * ByteArray
        * BytecodeArray
        * FixedArray
          * FrameArray
          * HashTable
              * Dictionary
              * StringTable
              * StringSet
              * CompilationCacheTable
          * FeedbackMetadata
      * PrimitiveHeapObject
        * BigInt
        * HeapNumber
        * Name
          * String
            * SeqString         // 堆中连续空间(数组)存储的字符串, 又分单字节和双字节(unicode)
            * SlicedString      // 采用 offset 和 lenght 切割 parent 字符串的一部分
            * ConsString        // 采用树形拼接的字符串 first + second
            * ThinString        // 引用另外一个字符串 actual
            * ExternalString    // 堆外的字符串, 又分单双字节
            * InternalizedString  // 固话到 StringTable 的字符串, 类似于字符串常量池
      * Context
      * DescriptorArray
      * PropertyArray
      * Code
      * AbstractCode, Code 或者 BytecodeArray 的 wrapper
      * Map
      * Struct
        * AccessorInfo
        * Script
        * StackFrameInfo
        * CodeCache
        * Microtask
          * CallbackTask
        * Module
          * SourceTextModule
      * FeedbackVector
      * UncompiledData

### 具体类
* Object: Object 是一个 Smi 或者一个强引用的 HeapObject, 定义了一堆的基础方法和静态方法. Object 的大小是 0 字节(没有数据)
* HeapObject: 几乎可以说是万类之父了(Object 是万类之母), 定义了字段偏移的 enum, 但是没看到定义具体字段
  * 类比较空, 方法也不多
  * 主要是了解宏 DEFINE_FIELD_OFFSET_CONSTANTS
* Code
    * 内存布局
        * header + 指令 + metadata + padding, 每一段都有对齐
    * accessor 函数命名规则
        * raw_ 开头 (raw_instruction_start) 的访问的是堆上(on-heap)代码对象
        * 驼峰命名法 (InstructionStart) 的则可能是访问对外区域(如果是嵌入 builtins)
    * 代码类型(CodeKind)
        * BYTECODE_HANDLER
        * FOR_TESTING
        * BUILTIN
        * REGEXP
        * WASM_FUNCTION
        * INTERPRETED_FUNCTION
        * TURBOPROP
        * TURBOFAN
        * ...
* String
    * StringShape: 字符串类型
    * Encoding: 单双字节
    * FlatContent
    * 最大长度: 32-bit ~ 268.4M, 64-bit ~ 536.8M
    * 内存占用公式: n 个字符的字符串消耗内存 `12 + 4 * Math.ceil(n/4)`
    * 疑惑: why?
        * 计算 hash 的最大长度才 16383, 如果字符串大于这个长度, 直接用字符串长度作为 hash 值?
        * kMaxHashCalcLength@string.h 注释如是说
* Map: 每一个堆对象都有一个 Map 描述它的结构, Map 包含 object 的 Size 信息, 如何迭代一个 object(for GC)
    * 这个类很重要, 需要搞懂