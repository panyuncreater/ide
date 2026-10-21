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

#include "interpreter/CoroutineFiber.h"

// 平台覆盖锁定（ADR-007 协程真挂起）：有 fiber 平台层的平台上宏必须启用。
// Windows = fiber API；Linux = POSIX ucontext + ASan fiber 标注
// （interpreter/InterpreterCoroutine.cpp 平台层）。macOS/其他平台保留重放。
#if defined(_WIN32) || defined(__linux__)
static_assert(MINILANG_CORO_FIBER == 1, "fiber platform must enable coroutine true suspension (ADR-007)");
#endif
#if !defined(_WIN32) && !defined(__linux__)
static_assert(MINILANG_CORO_FIBER == 0, "platform without fiber layer must stay in replay mode (ADR-002)");
#endif

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

// ============================================================
// 真挂起模式语义锁定（第四轮架构优化，MINILANG_CORO_FIBER=1 平台）
// ------------------------------------------------------------
// 重放模式下生成器体前缀副作用在每次 .next() 重复执行（O(n²)）；真挂起
// 模式下副作用只执行一次（O(n)）。以下测试锁定挂起模式的可观察语义，
// 并要求四后端一致（同一宏门控下三执行后端同步启用挂起）。
// ============================================================
#if MINILANG_CORO_FIBER
#include "interpreter/CoroutineFiber.h"

// 副作用只执行一次：循环体含 log 追加，n 次 .next() 恰好 n 次进入循环体
TEST(CoroutineSuspension, SideEffectsExecuteOnce) {
    std::string src = R"(
fun* gen() {
    var i = 0;
    while (i < 3) {
        log = log + "b";
        yield i;
        i = i + 1;
    }
}
var log = "";
var g = gen();
g.next();
g.next();
g.next();
print(log);
print(g.done());
)";
    // 挂起：每次 .next() 恰好经过循环体一次（重放模式会得到 "bbbbbb"）；
    // 动态 yieldCount（循环内 yield）→ done 仅在循环退出后为 true
    EXPECT_ALL_BACKENDS(src, "bbbfalse");
}

// yield 表达式值语义：恢复后 yield 表达式求值为其自身 yield 值（对齐重放 skip）
TEST(CoroutineSuspension, YieldExpressionValueOnResume) {
    std::string src = R"(
fun* gen() {
    var y = yield 5;
    yield y;
}
var g = gen();
print(g.next());
print(g.next());
print(g.done());
)";
    // 恢复后 y = 5（yield 表达式值 = 自身 yield 值），第二个 yield 返回 5
    EXPECT_ALL_BACKENDS(src, "55true");
}

// 跨挂起点的局部变量状态连续性
TEST(CoroutineSuspension, LocalStatePersistsAcrossSuspensions) {
    std::string src = R"(
fun* gen() {
    var a = 1;
    yield a;
    a = a + 10;
    yield a;
    a = a + 100;
    yield a;
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    EXPECT_ALL_BACKENDS(src, "111111true");
}

// try/catch 内 yield 跨挂起点（异常处理状态随快照恢复）
TEST(CoroutineSuspension, TryCatchAcrossSuspension) {
    std::string src = R"(
fun* gen() {
    try {
        yield 1;
        throw "e";
    } catch (e) {
        yield 2;
    }
    yield 3;
}
var g = gen();
print(g.next());
print(g.next());
print(g.next());
print(g.done());
)";
    // yield 1 → throw → catch → yield 2 → yield 3 → done
    EXPECT_ALL_BACKENDS(src, "123true");
}

// 跨调用方深度恢复：两次 .next() 的调用方帧深不同（backend-consistency §9 重定基
// 不变量——恢复后循环边界/挂起基线必须以**本次**入口深度为准）。
// P1 fix 前（2026-10-22）：StackVM/RegisterVM（关 IR 内联实测）恢复后把循环边界回拨
// 到首次挂起入口——深恢复越界执行调用方代码（实测 133true），浅恢复短路消耗快照
// （后续 .next() 全新重建，报"yield 只能在生成器函数体内出现"）。
TEST(CoroutineSuspension, ResumeAtDeeperCallSite) {
    std::string src = R"(
fun* g() { yield 1; yield 2; yield 3; }
var x = g();
print(x.next());
fun h() { return x.next(); }
print(h());
print(x.next());
print(x.done());
)";
    EXPECT_ALL_BACKENDS(src, "123true");
}

TEST(CoroutineSuspension, ResumeAtShallowerCallSite) {
    std::string src = R"(
fun* g() { yield 1; yield 2; yield 3; }
var x = g();
fun h() { return x.next(); }
print(h());
print(x.next());
print(x.next());
print(x.done());
)";
    EXPECT_ALL_BACKENDS(src, "123true");
}

// O(n) 行为代理锁定：300 次 .next() 序列正确（重放模式下此程序为 O(n²) 步数，
// 挂起模式下线性；值序列一致即锁挂起语义不回退）
TEST(CoroutineSuspension, LongSequenceValues) {
    std::string src = R"(
fun* gen() {
    var i = 0;
    while (i < 300) {
        yield i;
        i = i + 1;
    }
}
var g = gen();
var sum = 0;
var k = 0;
while (k < 300) {
    sum = sum + g.next();
    k = k + 1;
}
print(sum);
print(g.done());
)";
    // 0+1+...+299 = 44850；300 次 yield 后循环未退出，done 仍 false
    EXPECT_ALL_BACKENDS(src, "44850false");
}
#endif // MINILANG_CORO_FIBER

// ============================================================
// 2026-10-21 审计修复回归（真挂起）
// ------------------------------------------------------------
// 修复 1（P0）：挂起协程快照（suspendedEnv/funEnv/suspendedCallStack、VM 侧
//   stackSuffix/寄存器窗口）不在任何 GC 根集——挂起窗口的增量 GC 把仅被快照
//   持有的容器清空元素，恢复后数据损坏。修复 = CoroutineData::gcRootContributor
//   贡献回调（mark 到达 VAL_COROUTINE 时追加快照根）。
// 修复 2（P1）：失败协程弃用重建导致 yield 计数错位——值序列错乱 + done 提前
//   置位，与重放"同点再抛"分歧。修复 = 保留失败 fiber 持续重抛。
// ============================================================

TEST(CoroutineSuspension, FailedGeneratorRethrowsSameErrorOnEveryNext) {
    // P1 fix（2026-10-22）：错误逃逸后各后端语义对齐——解释器 fiber 保留失败态
    // 同点重抛（上轮修复）；VM 侧原以全新调用重入生成器体（重挂首 yield → 计数器
    // 伪递增 → 提前置 done + 返回首 yield 值，探针实测 1E1false1true）——现经
    // CoroutineData::vmPendingErrorBox 保留异常值 + 入口前置重抛，四后端一致：
    // 每次 .next() 同点重抛同一错误值，done 保持 false。
    // 注意：① throw 必须位于非最终 yield 之后（经辅助函数在 yield 之间逃逸）——
    // 生成器最终 yield 之后的尾部语句不执行（既有设计），直接写在末尾是死代码；
    // ② 重放平台（=0）VM 全新调用重执行+重抛，与解释器重放一致（无失败保留）。
    std::string src = R"(
fun boom() { throw "boom"; }
fun* g() {
    yield 1;
    boom();
    yield 2;
}
var x = g();
print(x.next());
try { x.next(); } catch (e) { print(e); }
try { x.next(); } catch (e) { print(e); }
print(x.done());
)";
    EXPECT_ALL_BACKENDS(src, "1boomboomfalse");
}

TEST(CoroutineSuspension, FailedGeneratorErrorEscapePrefix) {
    // 四后端一致前缀：生成器体内未捕获错误穿透到调用方 catch（异常值 = throw 值），
    // 错误后 done 保持 false
    std::string src = R"(
fun boom() { throw "boom"; }
fun* g() {
    yield 1;
    boom();
    yield 2;
}
var x = g();
print(x.next());
try { x.next(); } catch (e) { print(e); }
print(x.done());
)";
    EXPECT_ALL_BACKENDS(src, "1boomfalse");
}

TEST(CoroutineSuspension, TcoContextRestoredAcrossGeneratorTermination) {
    // P1 fix（2026-10-22，F3/W2）：fiber 体守卫跨挂起边界滞留——生成器在异于首次
    // .next() 的函数上下文终结（return 提前耗尽）时，守卫析构写回首次进入时的陈旧
    // 蹦床快照（tcoDecl_=&a, tcoName_="a"），终结上下文 c 的尾调用 return a(k+1)
    // 被误分类为 SelfFunction 无 target 信号，被 c 自己的蹦床循环捕获携错误实参重跑
    // （缺陷输出 57：c 体以 k=6 重跑后 a(7)；a 的函数体从未执行）。修复 = TCO 五成员
    // 纳入挂起/恢复协议（coroutineSuspend 对称保存/恢复 + CoroutineContextGuard
    // 退出恢复调用方真值）。期望：a(5)→5，c(5)→a(6)→6。
    std::string src = R"(
fun* gen() {
    yield 1;
    if (true) { return "early"; }
    yield 2;
}
fun a(n) { var x = g.next(); return n; }
fun c(k) { var y = g.next(); return a(k + 1); }
var g = gen();
print(a(5));
print(c(5));
)";
    EXPECT_EQ(minilang_test::runInterpreter(src), "56");
}

TEST(CoroutineSuspension, SuspendedGeneratorLocalsSurviveIncrementalGc) {
    // P0 回归（Interpreter 路径，阈值 8192）：挂起后 3 万次分配确保多次触发
    // 增量 GC；修复前仅被生成器局部环境持有的数组 a 被清空元素，恢复后索引报错。
    // VM 路径执行期由 CallbackSuppressor 抑制增量 GC（快照根集由 gcRootContributor
    // 覆盖跨 execute 边界的挂起窗口），本用例不触发；重放平台（=0）每次 .next()
    // 重建局部环境，本用例退化为通过（不影响判定）。
    std::string src = R"(
fun* gen() {
    var a = [10, 20, 30];
    yield a[0];
    yield a[1];
}
var g = gen();
print(g.next());
var i = 0;
while (i < 30000) {
    var junk = [i, i + 1];
    i = i + 1;
}
print(g.next());
)";
    EXPECT_EQ(minilang_test::runInterpreter(src), "1020");
}
