// js 会被包成一个 callalbe 对象执行
// JSEntry(42)

// 函数会触发 CompileLazy(68, 延迟编译)
function main() {
    // let a = 1+2
    /*
    let re = /[a-z]+/g;  // 执行
    MaybeHandle<JSRegExp> JSRegExp::Initialize(Handle<JSRegExp> regexp,
                                           Handle<String> source, Flags flags,
                                           uint32_t backtrack_limit) {
    */
    let re = /[a-z]+/g;  // 执行 JSRegExp::Initialize, 最快

    /*
     * Runtime_RegExpInitializeAndCompile => JSRegExp::Initialize_str => JSRegExp::Initialize_flag
     */
    // let re1 = RegExp(/[a-z]+/, "g")  // 最慢, 需要 Compile 两次 pattern

    // let re2 = RegExp("/[a-z]+/", "g")
}

main()
// console.log(re.exec("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
// console.log(re.test("abcde"));
