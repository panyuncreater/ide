// ============================================================
// 嵌套 try-finally + break/continue 续跳链测试
// ------------------------------------------------------------
// 验证 emitFinallyJump / emitFinallyJumpIR 在多层 try-finally 嵌套场景下
// 的正确性。已知 bug：finallyIndices[i-2] 索引计算错误，导致 2+ 层嵌套
// try-finally 内的 break/continue 续跳链被破坏（部分 finally 块被跳过）。
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>

static std::string runInterpreter(const std::string& src) {
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
        if (out.empty())
            return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

// ============================================================
// 单层 try-finally + break（baseline，应通过）
// ============================================================
TEST(NestedTryFinallyBreak, SingleLayerBreak) {
    std::string src = "var log = \"\";"
                      "for (var i = 0; i < 3; i = i + 1) {"
                      "  try {"
                      "    if (i == 1) { break; }"
                      "    log = log + \"try;\";"
                      "  } finally {"
                      "    log = log + \"finally;\";"
                      "  }"
                      "}"
                      "log = log + \"done;\";"
                      "print(log);";
    // i=0: try + finally
    // i=1: break (finally runs)
    // 退出循环 + done
    std::string expected = "try;finally;finally;done;";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 两层嵌套 try-finally + break（BUG 复现）
// ============================================================
// 预期执行顺序：
//   i=0: inner-try → inner-finally → outer-finally
//   i=1: break → inner-finally → outer-finally → 跳出循环
//   done
// 期望输出: "inner-try;inner-finally;outer-finally;inner-finally;outer-finally;done;"
TEST(NestedTryFinallyBreak, TwoLayerBreak) {
    std::string src = "var log = \"\";"
                      "for (var i = 0; i < 3; i = i + 1) {"
                      "  try {"
                      "    try {"
                      "      if (i == 1) { break; }"
                      "      log = log + \"inner-try;\";"
                      "    } finally {"
                      "      log = log + \"inner-finally;\";"
                      "    }"
                      "  } finally {"
                      "    log = log + \"outer-finally;\";"
                      "  }"
                      "}"
                      "log = log + \"done;\";"
                      "print(log);";
    std::string expected = "inner-try;inner-finally;outer-finally;inner-finally;outer-finally;done;";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 两层嵌套 try-finally + continue（BUG 复现）
// ============================================================
// 预期执行顺序：
//   i=0: inner-try → inner-finally → outer-finally
//   i=1: continue → inner-finally → outer-finally → 进入下一轮
//   i=2: inner-try → inner-finally → outer-finally
//   done
TEST(NestedTryFinallyBreak, TwoLayerContinue) {
    std::string src = "var log = \"\";"
                      "for (var i = 0; i < 3; i = i + 1) {"
                      "  try {"
                      "    try {"
                      "      if (i == 1) { continue; }"
                      "      log = log + \"inner-try;\";"
                      "    } finally {"
                      "      log = log + \"inner-finally;\";"
                      "    }"
                      "  } finally {"
                      "    log = log + \"outer-finally;\";"
                      "  }"
                      "}"
                      "log = log + \"done;\";"
                      "print(log);";
    std::string expected = "inner-try;inner-finally;outer-finally;inner-finally;outer-finally;inner-try;inner-finally;"
                           "outer-finally;done;";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 三层嵌套 try-finally + break（更复杂的 BUG 复现）
// ============================================================
TEST(NestedTryFinallyBreak, ThreeLayerBreak) {
    std::string src = "var log = \"\";"
                      "for (var i = 0; i < 3; i = i + 1) {"
                      "  try {"
                      "    try {"
                      "      try {"
                      "        if (i == 1) { break; }"
                      "        log = log + \"a-try;\";"
                      "      } finally {"
                      "        log = log + \"a-finally;\";"
                      "      }"
                      "    } finally {"
                      "      log = log + \"b-finally;\";"
                      "    }"
                      "  } finally {"
                      "    log = log + \"c-finally;\";"
                      "  }"
                      "}"
                      "log = log + \"done;\";"
                      "print(log);";
    std::string expected = "a-try;a-finally;b-finally;c-finally;a-finally;b-finally;c-finally;done;";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}
