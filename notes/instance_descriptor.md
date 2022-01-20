## Instance descriptor
包含用于描述实例的描述符, 当前用于保存两类: 属性描述符和 map transition(即隐藏类迁移)

### 属性描述符
    * 描述属性是怎么样的, 在实例的当前 map 中是如何存储的

### Transition
    * 表示使用对象的方式是当前的 map 不支持的, 因此必须转移到一个新的 map
    * backpoint 保存 prototype 转移的旧 map

### 参考资料
1. [Instance Descriptors (DescriptorArray)](https://groups.google.com/g/v8-dev/c/7aR9_Poj0zg)