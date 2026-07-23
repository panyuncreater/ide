// ============================================================
// R164 协程/生成器测试套件（四后端一致性）
// ------------------------------------------------------------
// 验证 fun* 生成器函数 + yield 表达式 + .next()/.done() 方法
// 在 Interpreter / StackVM / StackVM-IR / RegisterVM 四后端的语义一致性（重放模式）。
//
// D.7 合并：原 TempStackVMCoroutine / TempRegVMCoroutine / TempStackVMIRCoroutine
// 临时测试已合并到本套件，统一用 EXPECT_ALL_BACKENDS 宏验证四后端。
//
// 注意：MiniLang print 不自动加换行符，所有期望字符串均为连续字符。
// ============================================================

#include "common/ThreeBackends.h"
#include <gtest/gtest.h>
#include <string>

// ============================================================
// 1. 基础生成器：三个 yield 顺序返回
// ============================================================

TEST(CoroutineTest, BasicThreeYields) {
    std::string src = R"(
fun* gen() {
    yield 1;
    yield 2;
    yield 3;
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：1, 2, 3, true（print 不加换行）
    EXPECT_ALL_BACKENDS(src, "123true");
}

// ============================================================
// 2. yield 无值：等价于 yield null
// ============================================================

TEST(CoroutineTest, YieldNoValue) {
    std::string src = R"(
fun* gen() {
    yield;
    yield;
}
var g = gen();
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：null, null, true
    EXPECT_ALL_BACKENDS(src, "nullnulltrue");
}

// ============================================================
// 3. .done() 状态转换：未耗尽→已耗尽
// ============================================================

TEST(CoroutineTest, DoneStateTransition) {
    std::string src = R"(
fun* gen() {
    yield 1;
    yield 2;
}
var g = gen();
print(g.done());
print(g.next());
print(g.done());
print(g.next());
print(g.done());
)";
    // 预期：false, 1, false, 2, true
    EXPECT_ALL_BACKENDS(src, "false1false2true");
}

// ============================================================
// 4. 已耗尽后 .next() 返回最终值
// ============================================================

TEST(CoroutineTest, NextAfterDoneReturnsLastValue) {
    std::string src = R"(
fun* gen() {
    yield 1;
    yield 2;
}
var g = gen();
g.next();
g.next();
print(g.next());
print(g.next());
)";
    // 已耗尽后 .next() 返回最后一次 yield 的值（2）
    EXPECT_ALL_BACKENDS(src, "22");
}

// ============================================================
// 5. 生成器参数传递
// ============================================================

TEST(CoroutineTest, GeneratorWithParameters) {
    std::string src = R"(
fun* range(start: int, end: int) {
    var i = start;
    while (i < end) {
        yield i;
        i = i + 1;
    }
}
var g = range(10, 13);
print(g.next());
print(g.next());
print(g.next());
print(g.done());
print(g.next());
print(g.done());
)";
    // 循环内 yield → 动态 yieldCount（INT_MAX），done 仅在函数体自然结束时为 true。
    // range(10,13) yield 10,11,12（i<13）；第 3 次 .next() 后 done 仍为 false
    // （循环尚未跑到 i=13 退出）。第 4 次 .next() 重放跳过所有 yield，循环结束 → done=true，返回 null。
    // 预期：10, 11, 12, false, null, true
    EXPECT_ALL_BACKENDS(src, "101112falsenulltrue");
}

// ============================================================
// 6. 生成器闭包捕获外层变量
// ============================================================

TEST(CoroutineTest, ClosureCapture) {
    std::string src = R"(
var multiplier = 10;
fun* gen() {
    yield 1 * multiplier;
    yield 2 * multiplier;
}
var g = gen();
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：10, 20, true
    EXPECT_ALL_BACKENDS(src, "1020true");
}

// ============================================================
// 7. 生成器内含循环 + 条件 yield
// ============================================================

TEST(CoroutineTest, LoopWithConditionalYield) {
    std::string src = R"(
fun* evenNumbers(start: int, end: int) {
    var i = start;
    while (i < end) {
        if (i % 2 == 0) {
            yield i;
        }
        i = i + 1;
    }
}
var g = evenNumbers(1, 8);
print(g.next());
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // evenNumbers(1, 8) yield 2 (id=0), 4 (id=1), 6 (id=2)，yieldCount=3
    // 前 3 次 .next() 返回 2, 4, 6
    // 第 4 次 .next()：currentYieldId=3，重放跳过所有 yield，循环结束 → done=true，返回 null
    EXPECT_ALL_BACKENDS(src, "246nulltrue");
}

// ============================================================
// 8. 生成器显式 return 提前终止
// ============================================================

TEST(CoroutineTest, ExplicitReturnTerminates) {
    std::string src = R"(
fun* gen() {
    yield 1;
    return 99;
    yield 2;
}
var g = gen();
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：1, 99, true（return 后 done=true，第三个 yield 不会执行）
    EXPECT_ALL_BACKENDS(src, "199true");
}

// ============================================================
// 9. yield 表达式求值（含算术运算）
// ============================================================

TEST(CoroutineTest, YieldArithmeticExpression) {
    std::string src = R"(
fun* gen() {
    var x = 10;
    yield x + 5;
    yield x * 2;
    yield x - 3;
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：15, 20, 7, true
    EXPECT_ALL_BACKENDS(src, "15207true");
}

// ============================================================
// 10. 生成器作为闭包赋值给变量后调用
// ============================================================

TEST(CoroutineTest, GeneratorAssignedToVariable) {
    std::string src = R"(
fun* makeGen() {
    yield 100;
    yield 200;
}
var factory = makeGen;
var g = factory();
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：100, 200, true
    EXPECT_ALL_BACKENDS(src, "100200true");
}

// ============================================================
// 11. 多次调用生成器函数创建独立协程
// ============================================================

TEST(CoroutineTest, MultipleIndependentCoroutines) {
    std::string src = R"(
fun* gen() {
    yield 1;
    yield 2;
    yield 3;
}
var g1 = gen();
var g2 = gen();
print(g1.next());
print(g2.next());
print(g1.next());
print(g2.next());
print(g1.done());
print(g2.done());
)";
    // 预期：1, 1, 2, 2, false, false（两个独立协程，互不影响）
    EXPECT_ALL_BACKENDS(src, "1122falsefalse");
}

// ============================================================
// 12. 空生成器（无 yield）立即 done
// ============================================================

TEST(CoroutineTest, EmptyGeneratorImmediatelyDone) {
    std::string src = R"(
fun* empty() {
    var x = 1;
}
var g = empty();
print(g.done());
print(g.next());
)";
    // 预期：false, null（首次 .next() 执行函数体自然结束 → done=true，返回 null）
    EXPECT_ALL_BACKENDS(src, "falsenull");
}

// ============================================================
// 13. 生成器返回字符串
// ============================================================

TEST(CoroutineTest, YieldStringValues) {
    std::string src = R"(
fun* words() {
    yield "hello";
    yield "world";
}
var g = words();
print(g.next());
print(g.next());
print(g.done());
)";
    // 预期：hello, world, true
    EXPECT_ALL_BACKENDS(src, "helloworldtrue");
}

// ============================================================
// 14. 循环内条件 yield + done 中间状态（合并自 TempStackVMCoroutine/TempRegVMCoroutine）
//    覆盖动态 yieldCount 下 done=false 中间态 + 第 4 次 .next() 返回 null + done=true
// ============================================================

TEST(CoroutineTest, LoopConditionalYieldDoneState) {
    std::string src = R"(
fun* gen(start: int, end: int) {
    var i = start;
    while (i < end) {
        if (i % 2 == 1) {
            yield i;
        }
        i = i + 1;
    }
}
var g = gen(1, 6);
print(g.next());
print(g.next());
print(g.next());
print(g.done());
print(g.next());
print(g.done());
)";
    // 动态 yieldCount（INT_MAX），3 次 .next() 返回 1,3,5 后 done=false
    // 第 4 次 .next() 重放跳过所有 yield，循环结束 → done=true，返回 null
    EXPECT_ALL_BACKENDS(src, "135falsenulltrue");
}

// ============================================================
// 15. 生成器默认参数（边界条件）
// ============================================================

TEST(CoroutineTest, GeneratorWithDefaultParameters) {
    std::string src = R"(
fun* gen(start: int = 10, step: int = 2) {
    var i = start;
    while (i < start + 3 * step) {
        yield i;
        i = i + step;
    }
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
print(g.next());
print(g.done());
)";
    // 默认参数 start=10, step=2 → yield 10, 12, 14（i < 10+6=16）
    // 动态 yieldCount：3 次 .next() 返回 10,12,14 后 done=false（循环未结束），
    // 第 4 次 .next() 重放跳过所有 yield，循环结束 → done=true，返回 null
    EXPECT_ALL_BACKENDS(src, "101214falsenulltrue");
}

// ============================================================
// 16. 生成器 yield 数组（COW 类型，内存模型审计）
// ============================================================

TEST(CoroutineTest, YieldArrayValue) {
    std::string src = R"(
fun* gen() {
    yield [1, 2, 3];
    yield [4, 5];
}
var g = gen();
var a = g.next();
var b = g.next();
print(a[0]);
print(a[1]);
print(a[2]);
print(b[0]);
print(b[1]);
print(g.done());
)";
    // yield COW 数组类型，重放模式下数组值应正确返回
    EXPECT_ALL_BACKENDS(src, "12345true");
}

// ============================================================
// 17. 生成器 yield 字典（COW 类型，内存模型审计）
// ============================================================

TEST(CoroutineTest, YieldDictValue) {
    std::string src = R"(
fun* gen() {
    yield {"a": 1, "b": 2};
    yield {"c": 3};
}
var g = gen();
var d1 = g.next();
var d2 = g.next();
print(d1["a"]);
print(d1["b"]);
print(d2["c"]);
print(g.done());
)";
    // yield COW 字典类型，重放模式下字典值应正确返回
    EXPECT_ALL_BACKENDS(src, "123true");
}

// ============================================================
// 18. 生成器捕获多层 upvalue（闭包生命周期审计）
// ============================================================

TEST(CoroutineTest, NestedClosureCapture) {
    std::string src = R"(
fun outer() {
    var x = 100;
    fun middle() {
        var y = 10;
        fun* gen() {
            yield x + y;
            yield x * y;
        }
        return gen;
    }
    return middle();
}
var factory = outer();
var g = factory();
print(g.next());
print(g.next());
print(g.done());
)";
    // 生成器在两层嵌套函数内定义，捕获 x=100 和 y=10
    // 预期：110 (100+10), 1000 (100*10), true
    EXPECT_ALL_BACKENDS(src, "1101000true");
}

// ============================================================
// 19. 生成器内定义闭包捕获生成器局部变量（资源生命周期）
// ============================================================

TEST(CoroutineTest, ClosureInsideGenerator) {
    std::string src = R"(
fun* gen() {
    var base = 5;
    fun adder(n: int) {
        return base + n;
    }
    yield adder(1);
    yield adder(2);
    yield adder(3);
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // 生成器内定义闭包 adder 捕获 base=5
    // 重放模式下每次 .next() 重新执行函数体，base 重新初始化为 5
    // 预期：6, 7, 8, true
    EXPECT_ALL_BACKENDS(src, "678true");
}

// ============================================================
// 20. 生成器值作为函数参数传递（资源所有权）
// ============================================================

TEST(CoroutineTest, GeneratorAsFunctionArg) {
    std::string src = R"(
fun* gen() {
    yield 1;
    yield 2;
}
fun consume(g) {
    print(g.next());
    print(g.next());
    print(g.done());
}
var myGen = gen();
consume(myGen);
print(myGen.done());
)";
    // 生成器值作为参数传递给函数，函数内调用 .next()
    // 预期：1, 2, true（函数内耗尽）, true（外部检查仍为 true）
    EXPECT_ALL_BACKENDS(src, "12truetrue");
}

// ============================================================
// 21. 生成器内 try/catch（异常处理 + 协程交互，高频 Bug 模式）
// ============================================================

TEST(CoroutineTest, TryCatchInsideGenerator) {
    std::string src = R"(
fun* gen() {
    yield 1;
    try {
        yield 2;
        throw "error";
        yield 3;
    } catch (e) {
        yield e;
    }
    yield 4;
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.next());
print(g.done());
print(g.next());
print(g.done());
)";
    // 生成器内 try/catch：yield 1 (id=0), yield 2 (id=1), throw→catch, yield e (id=2), yield 4 (id=3)
    // 动态 yieldCount：4 次 .next() 返回 1,2,error,4 后 done=false（函数体未结束），
    // 第 5 次 .next() 重放跳过所有 yield，函数体结束 → done=true，返回 null
    EXPECT_ALL_BACKENDS(src, "12error4falsenulltrue");
}

// ============================================================
// 22. 嵌套生成器调用（生成器组合，重放模式栈帧管理）
// ============================================================

TEST(CoroutineTest, NestedGeneratorCall) {
    std::string src = R"(
fun* inner() {
    yield 10;
    yield 20;
}
fun* outer() {
    var i = inner();
    yield i.next();
    yield i.next();
    yield 30;
}
var g = outer();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // 生成器 outer 内调用另一个生成器 inner
    // 重放模式下 outer 每次重放都重新创建 inner 协程
    // 预期：10, 20, 30, true
    EXPECT_ALL_BACKENDS(src, "102030true");
}
