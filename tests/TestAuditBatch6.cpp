// ============================================================
// 审计批次 6 回归测试（2026-08-03 审计报告剩余项复核后补强）
// ------------------------------------------------------------
// 覆盖点：
//   D7  闭包经数组/字典索引调用的 capturedVars 写回持久性
//       （TestThreeEnginesConsistency R2 既有覆盖为"关闭后读取"路径，
//         写回路径此前无回归锁）
//   D8  catch 变量在 catch 块内 var 重声明的三后端语义
//       （顶层遮蔽路径的 DELETE_VAR 清理与重声明变量的交互此前无测试）
// 全部用例走四后端差分（Interpreter / StackVM / StackVM-IR / RegVM），
// 任何一后端偏离即失败——语义锁，先探测后固化。
// ============================================================

#include <gtest/gtest.h>

#include "compiler/core/Compiler.h"
#include "compiler/backend-reg/RegisterVM.h"
#include "compiler/backend-stack/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <string>

namespace {

static std::string runInterp(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
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

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
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

// RegisterVM 路径（RegisterBytecodeBackend 本身基于 IR，与
// TestThreeEnginesConsistency::runRegVM_IR 的编译方式一致）
static std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
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

// 五后端一致性断言
static void expectAllBackends(const std::string& src, const std::string& expected, int line) {
    EXPECT_EQ(runInterp(src), expected) << "Interpreter @line " << line;
    EXPECT_EQ(runStackVM(src), expected) << "StackVM @line " << line;
    EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM-IR @line " << line;
    EXPECT_EQ(runRegVM(src), expected) << "RegVM @line " << line;
}

} // namespace

// ============================================================
// D7: 闭包经数组索引调用——capturedVars 写回持久性
// ------------------------------------------------------------
// 闭包存入数组后经 fs[0]() 调用，每次调用 count = count + 1。
// capturedVars 写回机制（writeBackCapturedVars / OP_CLOSE_UPVALUE）
// 必须使变异跨调用持久：连续三次调用输出 123。
// 若某后端输出 111，说明该后端的索引调用路径未写回捕获变量
// （与直接变量调用路径不一致），属三后端语义分歧。
// ============================================================
TEST(AuditBatch6ClosureWriteback, ArrayIndexCallMutationPersists) {
    std::string src =
        "fun makeCounters() {"
        "  var count = 0;"
        "  var fs = [];"
        "  fs.push(fun() { count = count + 1; return count; });"
        "  return fs;"
        "}"
        "var fs = makeCounters();"
        "print(fs[0]());"
        "print(fs[0]());"
        "print(fs[0]());";
    expectAllBackends(src, "123", __LINE__);
}

// 字典存储变体：m["c"]() 调用路径的写回持久性
TEST(AuditBatch6ClosureWriteback, DictIndexCallMutationPersists) {
    std::string src =
        "fun makeCounters() {"
        "  var count = 0;"
        "  var m = {};"
        "  m[\"c\"] = fun() { count = count + 1; return count; };"
        "  return m;"
        "}"
        "var m = makeCounters();"
        "print(m[\"c\"]());"
        "print(m[\"c\"]());"
        "print(m[\"c\"]());";
    expectAllBackends(src, "123", __LINE__);
}

// 两个闭包共享同一捕获变量的索引调用——快照语义锁定。
// 与 TestThreeEnginesConsistency::R2_MultiClosureSharedCounterAfterClose 一致：
// 本语言的捕获语义是"快照"而非 JS/Lua 的共享单元格——makePair 返回后
// count 关闭为终值 0，两个闭包各自持有独立快照，互不可见对方变异：
// fs[0]→1, fs[1]→10, fs[0]→2（自身快照 1 再 +1）。四后端一致即语义锁成立。
TEST(AuditBatch6ClosureWriteback, SharedCaptureViaIndexCallsSnapshotSemantics) {
    std::string src =
        "fun makePair() {"
        "  var count = 0;"
        "  var fs = [];"
        "  fs.push(fun() { count = count + 1; return count; });"
        "  fs.push(fun() { count = count + 10; return count; });"
        "  return fs;"
        "}"
        "var fs = makePair();"
        "print(fs[0]());"
        "print(fs[1]());"
        "print(fs[0]());";
    expectAllBackends(src, "1102", __LINE__);
}

// ============================================================
// D8: catch 变量在 catch 块内 var 重声明——三后端语义锁
// ------------------------------------------------------------
// 顶层遮蔽路径的 cleanup（DELETE_VAR 按名擦除 + 全局原值恢复）与
// "catch 块内 var 重声明同名变量"的交互此前无测试。语义预期：
// 重声明只影响 catch 块内可见性，全局原值不受污染。
// ============================================================
TEST(AuditBatch6CatchRedecl, CatchVarRedeclaredInBlock) {
    std::string src =
        "try { throw \"x\"; } catch (err) {"
        "  var err = \"redeclared\";"
        "  print(err);"
        "}"
        "print(\"done\");";
    expectAllBackends(src, "redeclareddone", __LINE__);
}

TEST(AuditBatch6CatchRedecl, CatchVarRedeclaredWithGlobalShadow) {
    std::string src =
        "var err = \"outer\";"
        "try { throw \"x\"; } catch (err) {"
        "  var err = \"inner\";"
        "  print(err);"
        "}"
        "print(err);";
    expectAllBackends(src, "innerouter", __LINE__);
}

// catch 块内重声明后仍可抛出新值（重声明不破坏异常传播）
TEST(AuditBatch6CatchRedecl, RedeclaredThenRethrow) {
    std::string src =
        "var log = \"\";"
        "try {"
        "  try { throw \"first\"; } catch (err) {"
        "    var err = \"second\";"
        "    throw err;"
        "  }"
        "} catch (e) { log = log + e; }"
        "print(log);";
    expectAllBackends(src, "second", __LINE__);
}
