## V8 中的字符串

### V8 中字符串表达模式
* SeqString
    * V8 堆内使用连续空间存储字符串(类似数组)
    * 实际存储是分 OneByte/TwoByte(Unicode) 两类
* ConsString(first, second)
    * 在字符串拼接时, 采用树状结构表达拼接后(first + second)的字符串
    * V8 定义最小 ConsString::kMinLength = 13 个字符(即长度小于 13 的字符串不会生成 ConsString)
* SliceString(parent, offset)
    * 在字符串切割时, 采用 offset 与 [length] 表达父字符串(parent)的一部分
    * SliceString::kMinLength = 13
* ThinString(actual)
    * 直接引用另外一个字符串对象(actual)
    * 在多数情况下可以被认为与 ConsString(actual, empty_string) 等价
    * 需要开启 flag: --thin_strings
* ExternalString
    * 代表了产生在 V8 堆外的字符串资源
    * 实际数据表达时分 OneByte/TwoByte(Unicode) 两类

### 最大字符串长度
* 32 位平台, `512M / 2 - 16 ≈ 268.4M` (v < 6.2.4, 或者 32bit 平台)
* 64 位平台, `2^31 - 1 - 24 = 1.073G` (v >= 6.2.4 且 64bit 平台)

### 参考资料
1. [奇技淫巧学 V8 之六，字符串在 V8 内的表达](https://zhuanlan.zhihu.com/p/28883711)
2. [奇技淫巧学 V8 之七，字符串的扁平化](https://zhuanlan.zhihu.com/p/28907384)
