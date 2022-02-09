## IC

### FeedbackVector
* 固定的 header
    * shared function 信息(包含 feedback 元数据)
    * 调用次数
    * 运行时 profiler ticks
    * 优化有的代码单元
* header 之后是 feedback slot 的数组, 长度由 feedback 元数据决定

### FeedbackSlot

### IC 种类

### 调试信息
* 宏 `V8_TRACE_FEEDBACK_UPDATES`

### Talk[1]


### 参考资料
1. [V8 and How It Listens to You - michael Stanton](https://www.youtube.com/watch?v=u7zRSm8jzvA)
   * presentator: V8 编译器团队的经理
   * about ICs and Feedback Vector