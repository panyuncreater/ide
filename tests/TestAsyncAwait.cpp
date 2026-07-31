// ============================================================
// TestAsyncAwait.cpp — 七特性 MVP 阶段 4：async/await + 协作式调度器
// ------------------------------------------------------------
// 语法：
//   async fun f() {...}    —— async 函数声明，调用返回 Task/协程
//   await expr             —— 驱动协程到完成并取最终值（仅 async fun 体内）
// 语义（同步 drain 模型，复用 R164 协程重放模式，四后端一致）：
//   - async fun 复用生成器机制：调用返回协程值（.next()/.done() 可用）
//   - await t（t 为协程）→ 循环 t.next() 直到 done，结果为最终值
//   - await v（v 非协程）→ 恒等返回 v
//   - std/async 调度器 runAll/runTask 提供协作式交错执行
// ============================================================

#include "common/BuiltinModules.h"
#include "common/ThreeBackends.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

namespace {
// std/async 调度器测试需 moduleLoader（拦截 std/ 内建模块）。
// ThreeBackends.h 的 run* 不设 moduleLoader，此处定义带 loader 的四后端辅助。
std::string builtinLoader(const std::string& modulePath) {
    if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
        return BuiltinModuleRegistry::getSource(modulePath);
    }
    return "";
}

std::string asyncRunInterp(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    interp.setModuleLoader(builtinLoader);
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const std::exception& e) {
        return out + "<runtime:" + e.what() + ">";
    }
    return out;
}

std::string asyncRunStackVM(const std::string& src, bool useIR) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(useIR);
    c.setModuleLoader(builtinLoader);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

std::string asyncRunRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.setModuleLoader(builtinLoader);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}
} // namespace

// 带 moduleLoader 的四后端一致性断言（供 std/async 调度器测试）
#define EXPECT_ALL_BACKENDS_WITH_MODULES(src, expected)                                                                \
    EXPECT_EQ(asyncRunInterp(src), expected);                                                                          \
    EXPECT_EQ(asyncRunStackVM(src, false), expected);                                                                  \
    EXPECT_EQ(asyncRunStackVM(src, true), expected);                                                                   \
    EXPECT_EQ(asyncRunRegVM(src), expected);

// ============================================================
// 1. async fun 调用返回可迭代 Task（协程）
// ============================================================

TEST(AsyncAwait, AsyncFunReturnsTask) {
    // async fun 内含 yield → 调用返回协程，.next() 逐步驱动
    std::string src = R"(
async fun counter() {
    yield 1;
    yield 2;
    yield 3;
}
var t = counter();
print(t.next());
print(t.next());
print(t.done());
)";
    EXPECT_ALL_BACKENDS(src, "12false");
}

// ============================================================
// 2. await 已完成任务 / await 驱动到完成
// ============================================================

TEST(AsyncAwait, AwaitDrainsGeneratorToFinalValue) {
    // await 驱动协程直到 done，取最后一次 next() 的值（自然结束的 return 值）
    std::string src = R"(
async fun compute() {
    yield 10;
    yield 20;
    return 99;
}
async fun main() {
    var r = await compute();
    return r;
}
var result = main();
print(result.next());
)";
    // main 是 async fun → 调用返回协程；main body 内 await compute() 驱动 compute
    // 到 done（return 99），r=99，main return 99；main.next() 跑完 main → 99
    EXPECT_ALL_BACKENDS(src, "99");
}

TEST(AsyncAwait, AwaitSynchronousValueIsIdentity) {
    // await 非协程值 → 恒等返回
    std::string src = R"(
async fun f() {
    var x = await 42;
    return x + 1;
}
var t = f();
print(t.next());
)";
    EXPECT_ALL_BACKENDS(src, "43");
}

// ============================================================
// 3. await 链（一个 async fun await 另一个）
// ============================================================

TEST(AsyncAwait, AwaitChain) {
    std::string src = R"(
async fun inner() {
    yield 1;
    return 5;
}
async fun middle() {
    var v = await inner();
    return v * 2;
}
async fun outer() {
    var w = await middle();
    return w + 1;
}
var t = outer();
print(t.next());
)";
    // inner drain → 5; middle → 10; outer → 11
    EXPECT_ALL_BACKENDS(src, "11");
}

// ============================================================
// 4. 两个 task 经 std/async 调度器交错执行
// ============================================================

TEST(AsyncAwait, SchedulerRunAllInterleaves) {
    std::string src = R"(
import { runAll } from "std/async";
async fun taskA() {
    yield 1;
    yield 2;
    return 100;
}
async fun taskB() {
    yield 1;
    return 200;
}
var results = runAll([taskA(), taskB()]);
print(results[0]);
print(results[1]);
)";
    EXPECT_ALL_BACKENDS_WITH_MODULES(src, "100200");
}

TEST(AsyncAwait, SchedulerRunTaskDrivesToCompletion) {
    std::string src = R"(
import { runTask } from "std/async";
async fun job() {
    yield 1;
    yield 2;
    return 42;
}
print(runTask(job()));
)";
    EXPECT_ALL_BACKENDS_WITH_MODULES(src, "42");
}

// ============================================================
// 5. await 非法上下文报错（parse 阶段）
// ============================================================

namespace {
bool asyncParseHasErrors(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    return p.hasErrors() || !ast;
}
} // namespace

TEST(AsyncAwait, AwaitOutsideAsyncFunIsParseError) {
    // 普通函数体内 await 非法
    EXPECT_TRUE(asyncParseHasErrors("fun f() { var x = await 1; return x; }"));
}

TEST(AsyncAwait, AwaitAtTopLevelIsParseError) {
    EXPECT_TRUE(asyncParseHasErrors("var x = await 1;"));
}

TEST(AsyncAwait, AwaitInNestedNonAsyncFunIsParseError) {
    // async fun 内的普通嵌套 fun 体内 await 非法（上下文独立判定）
    EXPECT_TRUE(asyncParseHasErrors("async fun outer() {"
                                    "  fun inner() { return await 1; }"
                                    "  return inner();"
                                    "}"));
}

TEST(AsyncAwait, AsyncOnNonFunIsParseError) {
    EXPECT_TRUE(asyncParseHasErrors("async var x = 1;"));
}

// ============================================================
// 6. async fun 内 await + yield 混合（协作式让出 + 等待结果）
// ============================================================

TEST(AsyncAwait, MixedAwaitAndYield) {
    std::string src = R"(
async fun subtask() {
    return 7;
}
async fun worker() {
    yield 1;
    var v = await subtask();
    yield v;
    return v + 1;
}
var t = worker();
print(t.next());
print(t.next());
print(t.next());
print(t.done());
)";
    // next1→yield 1; next2→await subtask()=7, yield 7; next3→return 8; done
    EXPECT_ALL_BACKENDS(src, "178true");
}
