// ============================================================
// TestJITCoverageGaps — testing.md「后续覆盖率提升方向 #1」JIT 缺口补齐
// ------------------------------------------------------------
// 覆盖此前点名的四个 JIT 未覆盖区域：
//   1. 字符串方法（len/upper/lower/substr/indexOf/split/join/replace/trim）
//   2. enum + match 表达式
//   3. 并发原语（channel/mutex/spawn）
//   4. 模块系统（无 moduleLoader 场景）
//
// 实测现状（2026-07-28 探测）：
//   - 字符串拼接/比较/索引：JIT 已支持，与 StackVM 一致
//   - 字符串方法调用：JIT 报 <jit-runtime:类型 string 不支持方法 X>（优雅降级）
//   - enum variant：JIT 报 <jit-compile:JIT 不支持的 OpCode: OP_BUILD_ENUM_VARIANT>
//   - channel/mutex/spawn：JIT 报 <jit-compile:JIT 不支持的函数调用（未注册）: X>
//
// 测试策略：
//   - 已支持特性：JIT 与 StackVM 输出严格一致（四后端一致性扩展）
//   - 未支持特性：锁定「StackVM 输出正确值 + JIT 优雅降级（编译/运行期
//     报错标记，不崩溃、不静默输出错值）」。若未来 JIT 补全实现
//     （jitOut == expected），断言同样通过，无需改测试。
// ============================================================

#include "common/ThreeBackends.h"

#include <gtest/gtest.h>
#include <string>

#ifdef MINILANG_USE_JIT

using minilang_test::runJIT;
using minilang_test::runStackVM;

namespace {

// JIT 与 StackVM 输出严格一致（已支持特性）
void expectJitMatchesStackVM(const std::string& src) {
    std::string vmOut = runStackVM(src);
    std::string jitOut = runJIT(src);
    EXPECT_EQ(jitOut, vmOut) << "JIT 与 StackVM 输出不一致\nJIT: " << jitOut << "\nVM:  " << vmOut;
}

// 未支持特性的优雅降级锁：StackVM 输出正确值；JIT 要么输出同样的正确值
// （未来补全实现），要么报 <jit-compile:/<jit-runtime: 标记——绝不崩溃、
// 绝不静默输出错误值。
void expectVmOkJitGraceful(const std::string& src, const std::string& expected) {
    EXPECT_EQ(runStackVM(src), expected) << "StackVM 基准输出错误";
    std::string jitOut = runJIT(src);
    bool graceful = jitOut.find("<jit-compile:") != std::string::npos ||
                    jitOut.find("<jit-runtime:") != std::string::npos;
    EXPECT_TRUE(graceful || jitOut == expected) << "JIT 输出既非正确值也非优雅降级标记: " << jitOut;
}

} // namespace

// ============================================================
// 第一组：字符串（拼接/比较/索引已支持；方法调用锁定降级）
// ============================================================

TEST(JITCoverageGaps, StringConcatAndCompare) {
    expectJitMatchesStackVM("var a = \"foo\" + \"bar\";"
                            "print(a);"
                            "print(a == \"foobar\");"
                            "print(a != \"foo\");");
}

TEST(JITCoverageGaps, StringIndexAccess) {
    expectJitMatchesStackVM("var s = \"abc\";print(s[0]);print(s[2]);");
}

TEST(JITCoverageGaps, StringLen) {
    expectVmOkJitGraceful("print(\"hello\".len());", "5");
}

TEST(JITCoverageGaps, StringUpperLower) {
    expectVmOkJitGraceful("print(\"AbC\".upper());print(\"AbC\".lower());", "ABCabc");
}

TEST(JITCoverageGaps, StringSubstrIndexOf) {
    expectVmOkJitGraceful("var s = \"hello world\";"
                          "print(s.substr(6));"
                          "print(s.substr(0, 5));"
                          "print(s.indexOf(\"world\"));"
                          "print(s.indexOf(\"xyz\"));",
                          "worldhello6-1");
}

TEST(JITCoverageGaps, StringSplitJoinReplaceTrim) {
    expectVmOkJitGraceful("var parts = \"a,b,c\".split(\",\");"
                          "print(parts.len());"
                          "print(parts.join(\"-\"));"
                          "print(\"  pad  \".trim());"
                          "print(\"aXbXc\".replace(\"X\", \"_\"));",
                          "3a-b-cpada_b_c");
}

// ============================================================
// 第二组：enum + match 表达式
// ============================================================

TEST(JITCoverageGaps, EnumVariantMatch) {
    expectVmOkJitGraceful("enum Color { Red, Green, Blue }"
                          "var c = Color.Green;"
                          "var r = match (c) {"
                          "    case Color.Red => \"r\";"
                          "    case Color.Green => \"g\";"
                          "    case Color.Blue => \"b\";"
                          "};"
                          "print(r);",
                          "g");
}

TEST(JITCoverageGaps, EnumMatchDefault) {
    expectVmOkJitGraceful("enum Color { Red, Green, Blue }"
                          "var c = Color.Blue;"
                          "var r = match (c) {"
                          "    case Color.Red => \"r\";"
                          "    default => \"other\";"
                          "};"
                          "print(r);",
                          "other");
}

TEST(JITCoverageGaps, MatchIntLiteral) {
    expectVmOkJitGraceful("var x = 7;"
                          "var r = match (x) {"
                          "    case 7 => \"seven\";"
                          "    default => \"other\";"
                          "};"
                          "print(r);",
                          "seven");
}

// ============================================================
// 第三组：并发原语
// ============================================================

TEST(JITCoverageGaps, ChannelSendRecvTryRecv) {
    expectVmOkJitGraceful("var ch = channel();"
                          "ch.send(1);"
                          "ch.send(2);"
                          "print(ch.recv());"
                          "print(ch.tryRecv());"
                          "print(ch.tryRecv());", // 第三次空通道 → null
                          "12null");
}

TEST(JITCoverageGaps, ChannelRecvTimeout) {
    expectVmOkJitGraceful("var ch = channel();"
                          "ch.send(42);"
                          "print(ch.recvTimeout(1000));"
                          "print(ch.recvTimeout(10));", // 空通道超时 → null
                          "42null");
}

TEST(JITCoverageGaps, MutexLockTryLockUnlock) {
    expectVmOkJitGraceful("var m = mutex();"
                          "m.lock();"
                          "print(m.tryLock());" // 已持有 → false
                          "m.unlock();"
                          "print(m.tryLock());" // 可获取 → true
                          "m.unlock();",
                          "falsetrue");
}

TEST(JITCoverageGaps, SpawnJoinRunsClosure) {
    // spawn 延迟执行模式：join() 时在主线程同步执行闭包（副作用验证，
    // join 返回值在 VM 路径为 null，不作断言）
    expectVmOkJitGraceful("fun work() { print(6 * 7); }"
                          "var t = spawn(work);"
                          "t.join();",
                          "42");
}

// ============================================================
// 第五组：enum/并发原语 JIT 原生化后的严格一致性（补全后 JIT==StackVM）
// ------------------------------------------------------------
// 上述 expectVmOkJitGraceful 锁允许「优雅降级 或 正确值」；JIT 补全实现后
// 以下用 expectJitMatchesStackVM 锁定 JIT 与 StackVM 输出严格一致。
// ============================================================

TEST(JITCoverageGaps, EnumVariantMatchStrict) {
    expectJitMatchesStackVM("enum Color { Red, Green, Blue }"
                            "var c = Color.Green;"
                            "var r = match (c) {"
                            "    case Color.Red => \"r\";"
                            "    case Color.Green => \"g\";"
                            "    case Color.Blue => \"b\";"
                            "};"
                            "print(r);");
}

TEST(JITCoverageGaps, EnumVariantWithPayloadFieldExtract) {
    // 带字段 variant 构造 + match 绑定 + 字段提取（OP_BUILD_ENUM_VARIANT +
    // OP_ENUM_VARIANT_NAME + OP_ENUM_VARIANT_FIELD 全链路）
    expectJitMatchesStackVM("enum Shape { Circle(int), Rect(int, int) }"
                            "var s = Shape.Rect(3, 4);"
                            "var area = match (s) {"
                            "    case Shape.Circle(r) => r * r * 3;"
                            "    case Shape.Rect(w, h) => w * h;"
                            "};"
                            "print(area);");
}

TEST(JITCoverageGaps, EnumArityMismatchGraceful) {
    // arity 不一致：JIT 与 StackVM 均应在运行期报错（不静默构造），且不执行后续 print。
    // JIT jitBuildEnumVariant 的校验与 VM executeContainerBuildOps 对齐（错误文本一致）。
    const std::string src = "enum E { V(int) }"
                            "var x = E.V();"
                            "print(1);";
    std::string vmOut = runStackVM(src);
    std::string jitOut = runJIT(src);
    // 错误发生在 `var x = E.V()`（print 之前），故输出以错误标记开头（print 未执行，
    // 无先导输出）。不用 find("1") 判定——错误文本“期望 1 个参数”本身含 "1"。
    EXPECT_EQ(vmOut.rfind("<", 0), 0u) << "StackVM 应在 print 前报 arity 错误: " << vmOut;
    EXPECT_EQ(jitOut.rfind("<", 0), 0u) << "JIT 应在 print 前报 arity 错误或优雅降级: " << jitOut;
}

TEST(JITCoverageGaps, ChannelSendRecvStrict) {
    expectJitMatchesStackVM("var ch = channel();"
                            "ch.send(1);"
                            "ch.send(2);"
                            "print(ch.recv());"
                            "print(ch.tryRecv());"
                            "print(ch.tryRecv());");
}

TEST(JITCoverageGaps, MutexLockTryLockUnlockStrict) {
    expectJitMatchesStackVM("var m = mutex();"
                            "m.lock();"
                            "print(m.tryLock());"
                            "m.unlock();"
                            "print(m.tryLock());"
                            "m.unlock();");
}

TEST(JITCoverageGaps, RwLockReadWriteStrict) {
    expectJitMatchesStackVM("var rw = rwlock();"
                            "print(rw.tryReadLock());"
                            "rw.readUnlock();"
                            "print(rw.tryWriteLock());"
                            "rw.writeUnlock();");
}

TEST(JITCoverageGaps, SpawnJoinRunsClosureStrict) {
    // spawn 延迟执行：join() 在主线程同步执行闭包，副作用（print）应与 StackVM 一致
    expectJitMatchesStackVM("fun work() { print(6 * 7); }"
                            "var t = spawn(work);"
                            "t.join();");
}

TEST(JITCoverageGaps, SpawnClosureWithArgsStrict) {
    // spawn 传参 + 闭包内多语句，验证跳板参数传递与返回
    expectJitMatchesStackVM("fun add(a, b) { print(a + b); }"
                            "var t = spawn(add, 10, 20);"
                            "t.join();");
}

TEST(JITCoverageGaps, ChannelCloseThenRecvStrict) {
    expectJitMatchesStackVM("var ch = channel();"
                            "ch.send(7);"
                            "ch.close();"
                            "print(ch.recv());"
                            "print(ch.recv());"); // 关闭且空 → null
}

// ============================================================
// 第四组：模块系统（测试 helper 未注入 moduleLoader）
// ============================================================

// 两后端均应在编译期报错（moduleLoader 未设置/模块不存在），
// JIT 不崩溃、不静默执行后续语句。
TEST(JITCoverageGaps, ImportWithoutLoaderGracefulError) {
    const std::string src = "import \"nonexistent.mini\";\nprint(99);";
    std::string vmOut = runStackVM(src);
    std::string jitOut = runJIT(src);
    EXPECT_NE(vmOut.find("<compile:"), std::string::npos) << "StackVM: " << vmOut;
    EXPECT_NE(jitOut.find("<"), std::string::npos) << "JIT: " << jitOut;
    // 均不应执行到 print(99)
    EXPECT_EQ(vmOut.find("99"), std::string::npos) << "StackVM 不应执行 print: " << vmOut;
    EXPECT_EQ(jitOut.find("99"), std::string::npos) << "JIT 不应执行 print: " << jitOut;
}

#else // !MINILANG_USE_JIT

TEST(JITCoverageGaps, SkippedWithoutJIT) {
    GTEST_SKIP() << "MINILANG_USE_JIT=OFF，跳过 JIT 覆盖缺口测试";
}

#endif // MINILANG_USE_JIT
