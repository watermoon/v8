## 对象内存布局
```bash
# 每个元素的实际地址是对象地址: addr + (obj & 0xFFFFFFFF00000000)
--------------  <= 0x22c708148aa0 对象首地址(0x22c708148aa1 - 1)
| 0x0830394d |  <= Map 的地址
|------------|
| 0x08042229 |  <= properties
|------------|
| 0x082d2611 |  <= elements
|------------|
| 0x00000006 |  <= ???
|------------|
```


### 试验方法
  ```bash
    tools/dev/gm.py x64.debug
    cd out/x64.debug
    gdb ./d8
    (gdb) b v8::base::ieee754::cosh  # 断点到 Math.cosh 函数, 为了停止 d8 运行, 以检查内存
    (gdb) r --allow_natives_syntax object.js
    [New Thread 0x7fffedfc4700 (LWP 10539)]
    DebugPrint: 0x22c708148aa1: [JSArray]
    - map: 0x22c70830394d <Map(PACKED_ELEMENTS)> [FastProperties]
    - prototype: 0x22c7082cb615 <JSArray[0]>
    - elements: 0x22c7082d2611 <FixedArray[3]> [PACKED_ELEMENTS (COW)]
    - length: 3
    - properties: 0x22c708042229 <FixedArray[0]>
    - All own properties (excluding elements): {
        0x22c708044699: [String] in ReadOnlySpace: #length: 0x22c708242159 <AccessorInfo> (const accessor descriptor), location: descriptor
    }
    - elements: 0x22c7082d2611 <FixedArray[3]> {
              0: 123
              1: 0x22c7082d2625 <HeapNumber 1.23>
              2: 0x22c7082d2591 <String[3]: #123>
    }
    0x22c70830394d: [Map]
    - type: JS_ARRAY_TYPE
    - instance size: 16
    - inobject properties: 0
    - elements kind: PACKED_ELEMENTS
    - unused property fields: 0
    - enum length: invalid
    - back pointer: 0x22c708303925 <Map(HOLEY_DOUBLE_ELEMENTS)>
    - prototype_validity cell: 0x22c708242445 <Cell value= 1>
    - instance descriptors #1: 0x22c7082cbac9 <DescriptorArray[1]>
    - transitions #1: 0x22c7082cbb45 <TransitionArray[4]>Transition array #1:
        0x22c708044f9d <Symbol: (elements_transition_symbol)>: (transition to HOLEY_ELEMENTS) -> 0x22c708303975 <Map(HOLEY_ELEMENTS)>

    - prototype: 0x22c7082cb615 <JSArray[0]>
    - constructor: 0x22c7082cb3b1 <JSFunction Array (sfi = 0x22c70824f825)>
    - dependent code: 0x22c7080421b5 <Other heap object (WEAK_FIXED_ARRAY_TYPE)>
    - construction counter: 0


    Breakpoint 1, v8::base::ieee754::cosh(double) (x=1) at ../../src/base/ieee754.cc:2551
    2551	  int32_t ix
    (gdb) # 查看对象布局
    (gdb) x/8xw 0x22c708148aa1 - 1
    0x22c708148aa0:	0x0830394d	0x08042229	0x082d2611	0x00000006
    0x22c708148ab0:	0xbeadbeef	0xbeadbeef	0xbeadbeef	0xbeadbeef
    # 通过输出可以看到: object 布局是 Map | properties | elements
    (gdb) # 查看 map 的第一个 int field
    (gdb) x/4ub 0x22c70830394d + 3 # 减一后跳过指向 MetaMap(即这个 Map 的 Map, 因为当前 Map 也是一个 heapobject) 的指针(4bytes)
    0x22c708303950:	4	4	4	23
    # instance size: 4 * words = 16 bytes
    # inobject properties start offset in words: 4 * words = 16 bytes
    # used_or_unused_instance_size_in_words: 4
    # inobject properties: instnace_size_in_word - inobject_properties_start_in_word = 4 - 4 = 0
    # unused property fields: if used_or_unused_instance_size_in_words >= 3: return instance_size_in_word - used_or_unused_instance_size_in_words
    #                         else: return used_or_unused_instance_size_in_words;
    #   即: 如果 instance 中使用或没使用的大小(in word)大于等于 3(JSObject::kFieldsAdded), 则用 instance_size 减去已使用值(前面的使用或没使用可以明确为已使用); 否则即没使用的大小
    #
    (gdb) # 查看 map 的第二个 int field
    (gdb) x/4uh 0x22c70830394d + 7
    0x22c708303954:	1059	4352	2047	2560
    # instance type: 1059 即 JS_ARRAY_TYPE
    (gdb) x/4xw 0x22c70830394d + 7
    0x22c708303954:	0x11000423	0x0a0007ff	0x082cb615	0x08303925
    # elements kind: 0x11000423 => 最后一个字节的 0x11(0b0001,0001)[3..7] bit, 即 0b0000,10 = 2 = PACKED_ELEMENTS
    (gdb) bit fields 之后的字段
    # prototype: 082cb615
    # constructor_or_backpointer_or_native_context: 08303925
    # instance_descriptors: 082cbac9
    # dependent_code: 082cbac9
    # prototype_validity_cell: 080421b5
    # raw_transitions: 08242445
    (gdb) x/8xw 0x22c70830394d + 15
    0x22c70830395c:	0x082cb615	0x08303925	0x082cbac9	0x080421b5
    0x22c70830396c:	0x08242445	0x082cbb45	0x08042115	0x17040404
    (gdb) # 查看 elements
    (gdb) x/8xw 0x22c7082d2611 - 1
    0x22c7082d2610:	0x0804252d	0x00000006	0x000000f6	0x082d2625
    0x22c7082d2620:	0x082d2591	0x080423cd	0x7ae147ae	0x3ff3ae14
    # 偏移 4(HeapObject::kHeaderSize) 是元素个数, 即 0x00000006, 而长度是一个 Smi 对象, 所以值是 3, 实际长度为 3
    # FixedArrayBase 头 8 字节(HeapObject 4 + length 4)
    # FixedArray 的结构是 FixedArrayBase + object 元素
    # 即 0x000000f6、0x082d2625、0x082d2591 三个元素
    (gdb) # 查看元素三(String 类型), 元素二是 HeapNumber 暂时跳过
    (gdb) x/8xw 0x22c7082d2591 - 1
    0x22c7082d2590:	0x0804224d	0x0c0001ec	0x00000003	0xbe333231
    0x22c7082d25a0:	0x0804224d	0x22577fc2	0x0000000a	0x75626544
    # String 类型的继承链是: String => Name => PrimitiveHeapObject => HeapObject
    # PrimitiveHeapObject 没加任何字段, kHeaderSize = HeapObject::kHeaderSize = 4
    # Name: hash(4)
    # String: length(4), 这里的 length 返回的是 int32_t, 不想 FixedArray 一样是 Smi。是因为基类是 PrimitiveHeapObject 的缘故吗？
    # 由此可以看到 hash 是 0x0c0001ec, 长度是 3
    (gdb) # 查看其 Map 的 instance_size 和 inobject properties start offset in word
    (gdb) x/4ub 0x22c70804224d + 3
    0x22c708042250:	0	185	0	61
    # instance_size = 0, 表示长度可变
    (gdb) # 查看其 Map 第二个 int field 中的 elment kind
    (gdb) x/4uh 0x22c70804224d + 7
    0x22c708042254:	8	6400	1023	2112
    # 8 查表(tools/v8heapconst.py) 可以知道 8 表示的是 ONE_BYTE_INTERNALIZED_STRING_TYPE
    (gdb) bit 后的字段 instance descriptors 和 dependent code
    # 需要重新阅读 https://v8.dev/blog/fast-properties, 学习其中的 descriptor array
    # ### 明天继续吧, 好累呀……
  ```

  ### 附录一: object.js
  ```js
  arr = [123, 1.23, "123"];
  %DebugPrint(arr);
  Math.cosh(1);  // for breakpoint to stop d8 from running
  ```