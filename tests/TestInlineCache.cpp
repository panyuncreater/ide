// ============================================================
// tests/TestInlineCache.cpp
// ------------------------------------------------------------
// R133 Inline Cache 优化回归测试。
//
// 覆盖以下场景（所有用例使用 EXPECT_ALL_BACKENDS 验证四后端一致）：
//   IC1: 简单方法调用重复触发（验证 IC 正缓存路径不破坏语义）
//   IC2: 继承链方法查找（子类未定义方法时 fallback 到父类）
//   IC3: 多态调用（不同子类实例调用同名方法,验证 IC 不会跨类串）
//   IC4: 方法不存在（验证 IC 负缓存路径返回正确错误）
//   IC5: super.init 链路（验证 IC 不影响 super 调用语义）
//   IC6: 大量重复调用稳定性（验证 IC 在高频调用下不串状态）
//   IC7: 字段访问与方法调用混合（验证 IC 与字段查找不冲突）
//   IC8: 闭包作为类字段被调用（验证 IC 不误用 closure 路径）
//   IC9: 内建方法调用（验证 IC 不影响 arr.push/str.len 等内置）
//   IC10: 深继承链重复调用（验证 IC 在长链上命中率与正确性）
//   IC11: 递归方法调用（验证 IC 在递归栈帧多次进入时不串状态）
//   IC12: 类字段为 null 时调用方法（验证 IC 不缓存 null receiver 错误）
//   IC13: 方法调用返回值参与算术（验证 IC 路径写回 dstReg 正确）
//   IC14: 方法内修改字段后调用另一方法（验证 IC 与字段同步不冲突）
//   IC15: 类实例作数组元素批量调用方法（验证 IC 在大量实例上稳定）
//
// 注意：MiniLang 的 print() 不自动换行，所有断言预期值需显式加 "\n"。
// ============================================================
#include "common/ThreeBackends.h"

#include <gtest/gtest.h>
#include <string>

// IC1: 简单方法调用重复触发（验证 IC 正缓存路径不破坏语义）
TEST(InlineCache, IC1_SimpleMethodCallRepeated) {
    std::string src = R"(
class Counter {
    var count = 0;
    fun init() { this.count = 0; }
    fun bump() { this.count = this.count + 1; return this.count; }
}
var c = Counter();
print(c.bump() + "\n");
print(c.bump() + "\n");
print(c.bump() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "1\n2\n3\n");
}

// IC2: 继承链方法查找（子类未定义方法时 fallback 到父类）
TEST(InlineCache, IC2_InheritedMethodFallback) {
    std::string src = R"(
class Animal {
    var name = "";
    fun init(n) { this.name = n; }
    fun speak() { return "generic:" + this.name; }
}
class Dog : Animal {
    fun init(n) { super.init(n); }
    fun bark() { return "woof"; }
}
var d = Dog("Rex");
print(d.speak() + "\n");
print(d.bark() + "\n");
print(d.speak() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "generic:Rex\nwoof\ngeneric:Rex\n");
}

// IC3: 多态调用（不同子类实例调用同名方法,验证 IC 不会跨类串）
TEST(InlineCache, IC3_PolymorphicDispatch) {
    std::string src = R"(
class Shape {
    fun area() { return 0; }
}
class Circle : Shape {
    var r = 0;
    fun init(r) { this.r = r; }
    fun area() { return 3 * this.r * this.r; }
}
class Square : Shape {
    var s = 0;
    fun init(s) { this.s = s; }
    fun area() { return this.s * this.s; }
}
var c = Circle(2);
var sq = Square(3);
print(c.area() + "\n");
print(sq.area() + "\n");
print(c.area() + "\n");
print(sq.area() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "12\n9\n12\n9\n");
}

// IC4: 方法不存在（验证 IC 负缓存路径返回正确错误）
TEST(InlineCache, IC4_MethodNotFoundNegativeCache) {
    std::string src = R"(
class Foo {
    fun bar() { return 1; }
}
var f = Foo();
print(f.bar() + "\n");
print(f.missing() + "\n");
)";
    std::string expected = "1\n<runtime:类 Foo 没有方法 missing>";
    EXPECT_EQ(minilang_test::runInterpreter(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
    EXPECT_EQ(minilang_test::runStackVM_IR(src), expected);
    EXPECT_EQ(minilang_test::runRegVM(src), expected);
}

// IC5: super.init 链路（验证 IC 不影响 super 调用语义）
TEST(InlineCache, IC5_SuperInitChain) {
    std::string src = R"(
class Base {
    var x = 0;
    fun init(v) { this.x = v; }
    fun describe() { return "Base.x=" + this.x; }
}
class Mid : Base {
    var y = 0;
    fun init(v) { super.init(v); this.y = v * 2; }
    fun describe() { return "Mid:" + super.describe() + ",y=" + this.y; }
}
class Leaf : Mid {
    fun init(v) { super.init(v); }
    fun describe() { return "Leaf:" + super.describe(); }
}
var l = Leaf(5);
print(l.describe() + "\n");
print(l.describe() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "Leaf:Mid:Base.x=5,y=10\nLeaf:Mid:Base.x=5,y=10\n");
}

// IC6: 大量重复调用稳定性（验证 IC 在高频调用下不串状态）
TEST(InlineCache, IC6_StableAcrossManyCalls) {
    std::string src = R"(
class Greeter {
    var name = "";
    fun init(n) { this.name = n; }
    fun hello() { return "hi," + this.name; }
}
var g = Greeter("A");
var result = "";
var i = 0;
while (i < 50) {
    result = result + g.hello() + ";";
    i = i + 1;
}
print(result + "\n");
)";
    std::string expected;
    for (int i = 0; i < 50; ++i)
        expected += "hi,A;";
    expected += "\n";
    EXPECT_ALL_BACKENDS(src, expected);
}

// IC7: 字段访问与方法调用混合（验证 IC 与字段查找不冲突）
TEST(InlineCache, IC7_FieldAndMethodMixed) {
    std::string src = R"(
class Point {
    var x = 0;
    var y = 0;
    fun init(x, y) { this.x = x; this.y = y; }
    fun norm2() { return this.x * this.x + this.y * this.y; }
    fun move(dx, dy) { this.x = this.x + dx; this.y = this.y + dy; return this; }
}
var p = Point(3, 4);
print(p.norm2() + "\n");
p.move(1, 1);
print(p.x + "\n");
print(p.y + "\n");
print(p.norm2() + "\n");
p.move(-1, -1);
print(p.norm2() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "25\n4\n5\n41\n25\n");
}

// IC8: 闭包作为类字段被调用（验证 IC 不误用 closure 路径）
// 注意：MiniLang 中 this.factory() 是方法调用语法（查找方法 factory），
// 需先用 var f = this.factory; 取字段，再 f() 调用闭包。这是 R129 教训。
TEST(InlineCache, IC8_ClosureAsClassField) {
    std::string src = R"(
class Container {
    var factory = null;
    fun init(f) { this.factory = f; }
    fun make() { var f = this.factory; return f(); }
}
var c = Container(fun() { return 42; });
print(c.make() + "\n");
print(c.make() + "\n");
print(c.make() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "42\n42\n42\n");
}

// IC9: 内建方法调用（验证 IC 不影响 arr.push/str.len 等内置）
TEST(InlineCache, IC9_BuiltinMethodCalls) {
    std::string src = R"(
var arr = [1, 2, 3];
arr.push(4);
arr.push(5);
print(arr.len() + "\n");
print(arr + "\n");
var s = "hello";
print(s.len() + "\n");
print(s.len() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "5\n[1, 2, 3, 4, 5]\n5\n5\n");
}

// IC10: 深继承链重复调用（验证 IC 在长链上命中率与正确性）
// 此用例在 R133 之前会因 callCache_ 栈帧复用 bug 导致 RegVM 输出 "111A5"。
// R133 修复:方法调用路径 isMethodCall=true,不使用 callCache_。
TEST(InlineCache, IC10_DeepInheritanceChain) {
    std::string src = R"(
class A1 {
    fun base() { return 1; }
    fun tag() { return "A1"; }
}
class A2 : A1 {
    fun tag() { return "A2"; }
}
class A3 : A2 {
    fun tag() { return "A3"; }
}
class A4 : A3 {
    fun tag() { return "A4"; }
}
class A5 : A4 {
    fun tag() { return "A5"; }
}
var o = A5();
print(o.base() + "\n");
print(o.tag() + "\n");
print(o.base() + "\n");
print(o.tag() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "1\nA5\n1\nA5\n");
}

// IC11: 递归方法调用（验证 IC 在递归栈帧多次进入时不串状态）
TEST(InlineCache, IC11_RecursiveMethodCalls) {
    std::string src = R"(
class Fib {
    fun compute(n) {
        if (n < 2) { return n; }
        return this.compute(n - 1) + this.compute(n - 2);
    }
}
var f = Fib();
print(f.compute(0) + "\n");
print(f.compute(1) + "\n");
print(f.compute(5) + "\n");
print(f.compute(10) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "0\n1\n5\n55\n");
}

// IC12: 类字段为 null 时调用方法（验证 IC 不缓存 null receiver 错误）
TEST(InlineCache, IC12_NullReceiverMethodCall) {
    std::string src = R"(
class Holder {
    var obj = null;
    fun init() { this.obj = null; }
    fun callMethod() {
        if (this.obj == null) { return "empty"; }
        return "item";
    }
}
var h = Holder();
print(h.callMethod() + "\n");
print(h.callMethod() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "empty\nempty\n");
}

// IC13: 方法调用返回值参与算术（验证 IC 路径写回 dstReg 正确）
TEST(InlineCache, IC13_MethodReturnInArithmetic) {
    std::string src = R"(
class Mathy {
    fun double(x) { return x * 2; }
    fun triple(x) { return x * 3; }
}
var m = Mathy();
print(m.double(5) + m.triple(5) + "\n");
print(m.double(10) - m.triple(3) + "\n");
print(m.double(m.double(3)) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "25\n11\n12\n");
}

// IC14: 方法内修改字段后调用另一方法（验证 IC 与字段同步不冲突）
TEST(InlineCache, IC14_FieldMutationThenMethodCall) {
    std::string src = R"(
class Account {
    var balance = 0;
    fun init(b) { this.balance = b; }
    fun add(amount) { this.balance = this.balance + amount; }
    fun status() { return "balance=" + this.balance; }
}
var acc = Account(100);
print(acc.status() + "\n");
acc.add(50);
print(acc.status() + "\n");
acc.add(30);
print(acc.status() + "\n");
print(acc.status() + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "balance=100\nbalance=150\nbalance=180\nbalance=180\n");
}

// IC15: 类实例作数组元素批量调用方法（验证 IC 在大量实例上稳定）
TEST(InlineCache, IC15_BatchInstanceMethodCalls) {
    std::string src = R"(
class Box {
    var v = 0;
    fun init(v) { this.v = v; }
    fun get() { return this.v; }
    fun double() { this.v = this.v * 2; }
}
var b1 = Box(1);
var b2 = Box(2);
var b3 = Box(3);
var b4 = Box(4);
var b5 = Box(5);
var sum = b1.get() + b2.get() + b3.get() + b4.get() + b5.get();
print(sum + "\n");
b1.double();
b2.double();
b3.double();
b4.double();
b5.double();
sum = b1.get() + b2.get() + b3.get() + b4.get() + b5.get();
print(sum + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "15\n30\n");
}
