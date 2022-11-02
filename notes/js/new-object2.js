obj = {};
console.log("obj = {}");
obj.a = 'a';
obj.b = 2;
%DebugPrint(obj);

function Point(a, b) {
    this.a = a
    this.b = b
}
// obj0 与 obj 的 map 不同
console.log("obj0 通过函数创建");
obj0 = new Point(1, 2);
%DebugPrint(obj0);

// obj 与 obj1 的 map 不同`
obj1 = { "a": 1, "b": 2};
console.log("obj1 一开始就两个属性 a&b");
%DebugPrint(obj1);

obj2 = { "a": "a", "b": "b"};
console.log("obj2一开始就两个属性 a&b");
%DebugPrint(obj1);
// obj1 与 obj2 的 map 相同

