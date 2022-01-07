## RegExp engine
从 v8.8 开始, V8 包含了一个试验性的非回溯的正则引擎(原来的引擎是 Irregexp 引擎[2]), 新的引擎保障了执行时间与目标串的长度的线性关系
可以通过下面的配置使用新的 RegExp 引擎:
* `--enable-experimental-regexp_engine-on-excessive-backtracks`: 开启在出现过度的回溯是回退到非回溯引擎(注: 此回溯不是引擎回退)
* `--regexp-backtracks-before-fallback N (N 默认 50,000)`: 指定多少的回溯则被认为是过度
* `--enable-experimental-regexp-engine`: 为 RegExp 引擎开启非标准 flag(参数) l, 例如 `/(a*)*b/l`. 用这个标记构造的模式串会总是用新的引擎执行. 如果新的引擎无法处理 l-RegExp 模式, 会在构建的时候抛出一个异常. <font color=red>Irregexp 在大部分常用的模式下还是比新引擎存在数量级的优势, 所以新引擎还是试验性的</font>

回退机制并没有应用到所有的模式. 为了让回退机制声响, RegExp 模式全必须:
* 不包含反向引用(not contain backreferences)
* 不包含前后向预测(not contain lookahead or lookbehinds)
* 不包含大量的或者很深的嵌套重复(not contain large or deeply nested finite repetitions), 例如: `/a{200,50}/`
* 不包含 `u(Unicode)` 和 `i(case insensitive)` 标记(not have the u (Unicode) or i (case insensitive) flags set)

### 背景: 灾难性的回溯(catastrophic backtracking)
Irregexp jit-编译模式串为特殊的本地代码(native code)或者字节码, 对于大部分模式这都是非常快的(extremely fast). 但是对于一些特定的模式, Irregexp 的运行时间可能会是输入串的指数级. 例如 `/(a*)*b/.exec('a'.repeat(100))` 这个例子, 我们一辈子都不会执行完(实测 d8 会结束, 但是要很久)

这到底发生了什么事呢? Ireregexp 是一个回溯引擎. 当面对“一个匹配该如何才能继续”这样的选择时, Irregexp 会全面探索第一个候选模式(explores the first alternative in its entirety), 并在需要的时候回溯然后探索第二个候选(模式)。考虑如下模式串 `/abc|[az][by[0-9]/` 去匹配目标串 `'ab3'`
* Irregexp 首先尝试匹配 `/abc/`, 并且在第二个字符之后失败了
* 然后回溯两个字符, 并成功匹配第二个候选(模式)`/[az][by][0-9]/`
在带量词的模式中, 如 `/(abc)*xyz/`, Irregexp 找到一个匹配之后, 不得不进行选择: 是再一次匹配原模式, 还是继续匹配剩余的模式

我们来看一下对用 `/(a*)*b/` 匹配一个较小的目标串, 例如 `'aaa'` 的时候发生了什么. 这个模式包含了嵌套的量词, 所以我们是在让 Irregexp 匹配一系列由字符 `'a'` 构成的串, 在匹配一个 `'b'`. 很明显这个串不会匹配, 因为没有 `'b'`. 然而 `/(a*)*/` 能匹配, 并且它(Irregexp)会这样匹配很多次:
```
'aaa'           'aa', 'a'           'aa', ''
'a', 'aa'       'a', 'a', 'a'       'a', 'a', ''
...
```
Irregexp 由于选择了错误的方式去匹配 `/(a*)*/`, 以至于无法提前发现会最终无法匹配 `/b/`. 这个问题被认为是 "exponential" 或者 "catastrophic" 的回溯

### RegExps 作为自动机和字节码
为了了解对灾难性回溯免疫的候选宣发, 我们必须先绕道一下自动机(? we have to take a quick detour via automata). 每一个正则表达式都等效于一个自动机.
例如上文提到的正则 `/(a*)*b/` 对应下图的自动机:

<img src=./example-automaton.svg style="background-color:white;" width=600 />

注意到自动机并不是由模式唯一确定的, 上图所见的是通过一个机械的转换工程生成的, V8 的新正则引擎使用的

未标记的变是 epison transitions: 它们不消耗输入. 为了保证自动机的 size 和模式的 size 差不多, Epsilon 转移是必要. 简单地移除 epsilon 转移会导致转移数量的二次方增长.
Epsilon 转移是的可以从下面四种基本的状态来构造正则表达式对应的一个自动机:
<div align="center">
<img src=./state-types.svg style="background-color:white;" />
</div>

这里我们仅对状态移除进行分类, 而状态移入任然允许是任意的. 仅从这几类状态构建的自动机可以被表示成字节码程序, 每个状态对应一条指令. 例如, 有两个 epsilon 转移的状态表示为一个 FORK 指令.

### 回溯算法
让我们来回顾一下 Irregexp 使用的回溯算法, 然后以自动机的术语进行描述. 假设给定义个模式对应的字节码数组, 希望测试一个输入是否匹配模式. 假设代码看起来像这样:
```js
const code = [
  {opcode: 'FORK', forkPc: 4},
  {opcode: 'CONSUME', char: '1'},
  {opcode: 'CONSUME', char: '2'},
  {opcode: 'JMP', jmpPc: 6},
  {opcode: 'CONSUME', char: 'a'},
  {opcode: 'CONSUME', char: 'b'},
  {opcode: 'ACCEPT'}
];
```
这一段 bytecode 对应模式 `/12|ab/y`. FORK 指令的 forkPc 字段是我们可以继续的候选状态/指令的索引("program counter"), jmpPc 也类似. 索引已 0 开始. 回溯算法用 JavaScript 来实现的话会类似下面的样子:
```javascript
let ip = 0;  // Input posision
let pc = 0;  // Program counter: index of the next instruction.
const stack = [];  // Backtrack stack.
while (true) {
    const inst = code[pc];
    switch (inst.opcode) {
        case 'CONSUME'::
            if (ip < input.length && input[ip] === inst.char) {
                // Input matches what we expect: Continue.
                ++ip;
                ++pc;
            } else if (stack.length > 0) {
                // Wrong input character, but we can backtrack.
                const back = stack.pop();
                ip = back.ip;
                pc = back.pc;
            } else {
                // Wrong character, cannot backtrack.
                return false;
            }
            break;
        case 'FORK':
            // Save alternative for backtracking later.
            stack.push({ip: ip, pc: inst.forkPc});
            ++pc;
            break;
        case 'JMP':
            pc = inst.jmpPc;
            break;
        case 'ACCEPT':
            return true;
    }
}
```
如果字节码程序包含了不消费任何字符串的循环, 这个实现会无限循环, 例如: 如果自动机包含一个仅包含 elsilon 转换的循环. 这个问题可以通过一个提前预读一个字符解决. Irregexp 是一个更加成熟的实现, 但是最终还是基于同样的算法.

### 非回溯算法
回溯算法对应深度优先的自动机: 我们总是在一个 FORK 语句的完整空间探索它的第一个候选(匹配), 然后在必要的时候回溯第二个候选. 作为(这一算法)的替代, 非回溯算法, 很自然地就是基于广度优先的自动机. 在这一算法中, 我们会同时考虑所有的候选匹配, 与输入字符串的当前位置步调一致. 因此我们维护当前状态的一个列表, 然后通过应用每一个输入支付的对应转移状态来推进所有的状态. 至关重要地, 我们会移除当前撞他列表中的所有状态.
JavaScript 的一个简单实现大概是这个样子:
```javascript
// Input position
let ip = 0;
// List of current pc values, or `'ACCEPT'` if we've found a match. We start at
// pc 0 and follow epsilon transitions.
let pcs = followEpsilons([0]);

while (true) {
    // We're done if we've found a match...
    if (pcs === 'ACCEPT') return true;
    // ...or if we've exhausted the input string.
    if (ip >= input.length) return false;

    // Continue only with the pcs that CONSUME the correct character.
    pcs = pcs.filter(pc => code[pc].char === input[ip]);
    // Advance the remaining pcs to the next instruction.
    pcs = pcs.map(pc => pc + 1);
    // Follow epsilon transitions.
    pcs = followEpsilons(pcs);

    ++ip;
}
```
这里的 followEpsilons 是一个函数, 它的输入是 program counters 的列表, 然后计算列表中的 program counters 可以通过 epsilon 转移(例如 通过仅执行 FORK 和 JMP）)可以到达 CONSUME 指令的 program counters. 返回的列表不能包含重复项. 如果一个 ACCEPT 指令可以达到, 返回返回 'ACCEPT‘. 它的实现类似下面这样:
```javascript
function followEpsilons(pcs) {
    // Set of pcs we've seen so far.
    const visitedPcs = new Set();
    const result = [];

    while (pcs.length > 0) {
        const pc = pcs.pop();

        // We can ignore pc if we've seen it earlier.
        if (visitedPcs.has(pc)) continue;
        visitedPcs.add(pc);

        const inst = code[pc];
        switch (inst.opcode) {
            case 'CONSUME':
                result.push(pc);
                break;
            case 'FORK':
                pcs.push(pc + 1, inst.forkPc);
                break;
            case 'JMP':
                pcs.push(inst.jmpPc);
                break;
            case 'ACCEPT':
                return 'ACCEPT';
        }
    }

    return result;
}
```
由于 visitPcs 内会去重, 所以每一个 program counter 在 followEpsilons 仅会检查一次. 这保证了结果列表不会包含重复项, 并且 followEpsilons 的运行时间限制在了代码数组的 size, 例如: 模式的 size. followEsilons 最多被调用 input.length 次, 所以 RegExp 匹配总的运行时间限制在 `𝒪(pattern.length * input.length)`.
非回溯算法可以被扩展成支持 JavaScript 正则的大部分特性, 例如单词边界(word boundaries)或者计算(子)匹配边界(calculation of (sub)match boundaries). 不幸地是, backreferences, lookahead 和 lookbehind 在没有大改算法(会影响算法的渐进最差复杂度)的情况下无法支持.

新的 V8 RegExp 是基于这个新的算法, 它的实现在 `re2` 和 `Rust regex` 库中. 更多算法详情可以参考 `re2` 库的原作者 Russ Cox 的一些列博文

### 参考资料
1. [An additional non-backtracking RegExp engine](https://v8.dev/blog/non-backtracking-regexp)
2. [Irregexp, Google Chrome's New Regexp Implementation](https://blog.chromium.org/2009/02/irregexp-google-chromes-new-regexp.html)