// ============================================================
// TestMultilineString — 多行字符串字面量回归锁
// ------------------------------------------------------------
// 背景（faq.md Q6 澄清）：MiniLang 普通双引号字符串字面量允许包含
// 字面换行（Lexer string() 主循环经 advance() 消费 \r/\n 并将
// \r/\r\n 规范化为 \n），无需专门的三引号语法。本文件将该行为
// 固化为回归锁：
//   1. Lexer 层：跨行字面量扫描为单个 TK_STRING_LIT，值含 \n，
//      CRLF 规范化为 \n，行号跟踪正确
//   2. 执行层：四后端（Interpreter/StackVM/StackVM-IR/RegisterVM）
//      输出一致
//   3. 插值：跨行字符串中 {expr} 插值正常
//   4. Formatter：跨行字面量往返语义等价（format→执行 输出不变，
//      二次格式化稳定）
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "formatter/Formatter.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>
#include <vector>

namespace {

// ============================================================
// 辅助函数：四后端执行器
// ============================================================

std::string runInterpreter(const std::string& src) {
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

std::string runStackVM(const std::string& src) {
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

std::string runStackVM_IR(const std::string& src) {
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

std::string runRegVM(const std::string& src) {
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

// 四后端输出一致性断言
void expectAllBackends(const std::string& src, const std::string& expected) {
    EXPECT_EQ(runInterpreter(src), expected) << "Interpreter 路径";
    EXPECT_EQ(runStackVM(src), expected) << "StackVM 路径";
    EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM-IR 路径";
    EXPECT_EQ(runRegVM(src), expected) << "RegisterVM 路径";
}

std::string formatSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";
    Formatter formatter;
    formatter.setComments(lexer.comments());
    return formatter.format(*ast);
}

// 过滤 EOF，返回有效 Token
std::vector<Token> scanTokens(const std::string& src) {
    Lexer lexer;
    auto tokens = lexer.scan(src);
    std::vector<Token> result;
    for (const auto& t : tokens) {
        if (t.type != TokenType::TK_EOF)
            result.push_back(t);
    }
    return result;
}

} // namespace

// ============================================================
// 第一组：Lexer 层扫描行为
// ============================================================

// 跨行字面量扫描为单个 TK_STRING_LIT，值保留字面换行
TEST(MultilineString, LexerSingleTokenWithNewline) {
    auto tokens = scanTokens("\"line1\nline2\nline3\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    ASSERT_TRUE(tokens[0].literalIsString());
    EXPECT_EQ(tokens[0].literalString(), "line1\nline2\nline3");
}

// CRLF 规范化为 \n（advance() 统一规范化，跨平台源码一致）
TEST(MultilineString, LexerCrlfNormalizedToLf) {
    auto tokens = scanTokens("\"a\r\nb\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    ASSERT_TRUE(tokens[0].literalIsString());
    EXPECT_EQ(tokens[0].literalString(), "a\nb");
}

// 跨行字符串后的 Token 行号正确（行号跟踪未被破坏）
TEST(MultilineString, LexerLineTrackingAfterMultilineString) {
    auto tokens = scanTokens("\"a\nb\"\nvar");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    EXPECT_EQ(tokens[0].line, 1); // 起始行
    EXPECT_EQ(tokens[1].type, TokenType::TK_VAR);
    EXPECT_EQ(tokens[1].line, 3); // 字符串占 1-2 行，var 在第 3 行
}

// 未闭合的跨行字符串仍报「未终止的字符串」（错误路径不回退）
TEST(MultilineString, LexerUnterminatedAcrossLines) {
    Lexer lexer;
    auto tokens = lexer.scan("\"line1\nline2");
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// ============================================================
// 第二组：四后端执行一致性
// ============================================================

TEST(MultilineString, FourBackendsPrintMultiline) {
    const std::string src = "var s = \"line1\nline2\nline3\";\nprint(s);";
    expectAllBackends(src, "line1\nline2\nline3");
}

// 跨行字面量与 \n 转义等价
TEST(MultilineString, LiteralNewlineEqualsEscape) {
    const std::string srcLiteral = "print(\"a\nb\");";
    const std::string srcEscape = "print(\"a\\nb\");";
    EXPECT_EQ(runInterpreter(srcLiteral), runInterpreter(srcEscape));
    EXPECT_EQ(runStackVM(srcLiteral), runStackVM(srcEscape));
}

// 跨行字符串拼接与方法调用正常
TEST(MultilineString, ConcatAndLength) {
    const std::string src = "var s = \"ab\ncd\";\nprint(s.len());\nprint(s + \"!\");";
    expectAllBackends(src, "5ab\ncd!");
}

// ============================================================
// 第三组：跨行字符串中的插值
// ============================================================

TEST(MultilineString, InterpolationInsideMultiline) {
    const std::string src = "var x = 42;\nvar s = \"head\n{x}\ntail\";\nprint(s);";
    expectAllBackends(src, "head\n42\ntail");
}

// ============================================================
// 第四组：Formatter 往返语义等价
// ============================================================

// format 后再执行，输出与原源码一致（语义保持）
TEST(MultilineString, FormatterRoundtripSemantics) {
    const std::string src = "var s = \"line1\nline2\";\nprint(s);";
    std::string formatted = formatSource(src);
    ASSERT_FALSE(formatted.empty());
    EXPECT_EQ(runInterpreter(formatted), runInterpreter(src)) << "格式化后语义变化，formatted:\n" << formatted;
    // 二次格式化稳定（幂等）
    EXPECT_EQ(formatSource(formatted), formatted);
}
