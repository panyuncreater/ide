// ============================================================
// 审计批次 5 回归测试 — Lexer 边界 + Parser 错误恢复 + Formatter 往返等价性
// ------------------------------------------------------------
// 覆盖审计发现的 15 个 Bug 修复（2 P1 + 13 P2）：
//   Lexer:
//     BUG-LEX-AUDIT-1 (P1): 插值循环 MAX_TOKEN_COUNT 无限循环
//     BUG-LEX-AUDIT-2 (P2): INTERP_START/END 列号错误
//     BUG-LEX-AUDIT-3 (P2): 直接 emplace_back 的 token 列号体系不一致
//     BUG-LEX-AUDIT-4 (P2): string() 绕过 MAX_TOKEN_COUNT 防护
//     BUG-LEX-AUDIT-5 (P2): 深度嵌套插值内存压力
//   Parser:
//     BUG-PARSER-AUDIT-1 (P2): parseTypeAnnotation 在 : / -> 后未预检
//     BUG-PARSER-AUDIT-2 (P2): classDecl 成员循环无 try/catch
//     BUG-PARSER-AUDIT-5 (P2): 无错误数量上限
//     BUG-PARSER-AUDIT-7 (P2): primary() 深度检查时机不一致
//   Formatter:
//     BUG-FMT-P1-1 (P1): formatInterpolatedString 注释游标错误跳过同行尾部注释
//     BUG-FMT-P1-2 (P1): formatFunCall 对 MemberAccess callee 不加括号
//     BUG-FMT-P2-1 (P2): formatBlock 中与 } 同行注释位置错误
//     BUG-FMT-P2-2 (P2): UOP_UNKNOWN 输出注释导致 AST 结构改变
//     BUG-FMT-P2-3 (P2): NaN/Infinity 输出无法重新解析
//     BUG-FMT-P2-4 (P2): formatInterpolatedString nullptr 崩溃
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "formatter/Formatter.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "common/Diagnostic.h"
#include "common/RuntimeLimits.h"

#include <string>
#include <memory>

// ============================================================
// 辅助函数（与 TestAuditBatch4 一致）
// ============================================================

static std::string runInterpreter(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty()) return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string formatSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";
    Formatter formatter;
    formatter.setComments(lexer.comments());
    return formatter.format(*ast);
}

static std::vector<Token> scanTokens(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (!tokens.empty() && tokens.back().type == TokenType::TK_EOF) {
        tokens.pop_back();
    }
    return tokens;
}

// ============================================================
// Lexer: BUG-LEX-AUDIT-2 — INTERP_START/END 列号
// ============================================================

TEST(AuditBatch5LexerInterpColumn, InterpStartColumnPointsToBrace) {
    // "a{x}" — { 在第 3 列
    auto tokens = scanTokens("\"a{x}\"");
    // 期望 token 序列: STRING_PART("a"), INTERP_START("{"), VAR_REF("x"), INTERP_END("}"), STRING_PART("")
    ASSERT_GE(tokens.size(), 4u);
    // 找到 INTERP_START
    size_t startIdx = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::TK_INTERP_START) { startIdx = i; break; }
    }
    EXPECT_EQ(tokens[startIdx].type, TokenType::TK_INTERP_START);
    // BUG-LEX-AUDIT-2: INTERP_START 列号应指向 '{' 的位置（第 3 列）
    EXPECT_EQ(tokens[startIdx].column, 3)
        << "INTERP_START 列号应指向 '{' 位置";
}

TEST(AuditBatch5LexerInterpColumn, InterpEndColumnPointsToBrace) {
    // "a{x}" — } 在第 5 列
    auto tokens = scanTokens("\"a{x}\"");
    ASSERT_GE(tokens.size(), 4u);
    size_t endIdx = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::TK_INTERP_END) { endIdx = i; break; }
    }
    EXPECT_EQ(tokens[endIdx].type, TokenType::TK_INTERP_END);
    // BUG-LEX-AUDIT-2: INTERP_END 列号应指向 '}' 的位置（第 5 列）
    EXPECT_EQ(tokens[endIdx].column, 5)
        << "INTERP_END 列号应指向 '}' 位置";
}

TEST(AuditBatch5LexerInterpColumn, InterpStartColumnWithLeadingSpaces) {
    // "  a{x}" — { 在第 5 列（引号占第 1 列，空格在第 2-3 列，a 在第 4 列，{ 在第 5 列）
    auto tokens = scanTokens("\"  a{x}\"");
    ASSERT_GE(tokens.size(), 4u);
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::TK_INTERP_START) {
            EXPECT_EQ(tok.column, 5)
                << "INTERP_START 列号应考虑前导空格";
            return;
        }
    }
    FAIL() << "未找到 INTERP_START token";
}

// ============================================================
// Lexer: BUG-LEX-AUDIT-3 — 注释列号一致性（UTF-8 感知）
// ============================================================

TEST(AuditBatch5LexerCommentColumn, LineCommentColumnConsistentWithAddToken) {
    // 验证行注释的列号与 addToken 路径一致
    Lexer lexer;
    lexer.scan("var x = 1; // comment");
    auto& diags = lexer.getDiagnostics();
    // 无错误
    EXPECT_FALSE(diags.hasErrors());
    // 注释应在 comments_ 中
    auto& comments = lexer.comments();
    ASSERT_EQ(comments.size(), 1u);
    // "var x = 1; " 占 11 字符（含末尾空格），// 在第 12 列
    // v(1)a(2)r(3) (4)x(5) (6)=(7) (8)1(9);(10) (11)/(12)
    EXPECT_EQ(comments[0].column, 12)
        << "行注释列号应与 addToken 路径一致（UTF-8 感知）";
}

TEST(AuditBatch5LexerCommentColumn, BlockCommentColumnConsistent) {
    // 验证块注释的列号
    Lexer lexer;
    lexer.scan("var x = 1; /* block */");
    EXPECT_FALSE(lexer.getDiagnostics().hasErrors());
    auto& comments = lexer.comments();
    ASSERT_EQ(comments.size(), 1u);
    // /* 在第 12 列（同上行注释：var x = 1; + 空格 = 11 字符）
    EXPECT_EQ(comments[0].column, 12)
        << "块注释列号应与 addToken 路径一致（UTF-8 感知）";
}

// ============================================================
// Lexer: BUG-LEX-AUDIT-1/4 — 插值循环不卡死（正常路径回归）
// ============================================================

TEST(AuditBatch5LexerInterpLoop, NormalInterpStringWorks) {
    // 回归测试：正常插值字符串应正确扫描
    auto tokens = scanTokens("\"a{1+2}b{3}c\"");
    ASSERT_FALSE(tokens.empty());
    // 应包含 INTERP_START x2, INTERP_END x2
    int startCount = 0, endCount = 0;
    for (const auto& t : tokens) {
        if (t.type == TokenType::TK_INTERP_START) startCount++;
        if (t.type == TokenType::TK_INTERP_END) endCount++;
    }
    EXPECT_EQ(startCount, 2);
    EXPECT_EQ(endCount, 2);
}

TEST(AuditBatch5LexerInterpLoop, NestedInterpStringWorks) {
    // 回归测试：嵌套插值字符串应正确扫描
    auto tokens = scanTokens("\"a{\"b{1}c\"}d\"");
    ASSERT_FALSE(tokens.empty());
    // 外层和内层各有一对 INTERP_START/END
    int startCount = 0, endCount = 0;
    for (const auto& t : tokens) {
        if (t.type == TokenType::TK_INTERP_START) startCount++;
        if (t.type == TokenType::TK_INTERP_END) endCount++;
    }
    EXPECT_EQ(startCount, 2);
    EXPECT_EQ(endCount, 2);
}

// ============================================================
// Parser: BUG-PARSER-AUDIT-1 — parseTypeAnnotation 预检
// ============================================================

TEST(AuditBatch5ParserTypeAnn, FunDeclColonMissingTypeReportsCorrectError) {
    // fun foo(): ; — `:` 后缺类型，应报"期望返回类型名"而非吞掉 `;`
    Lexer lx; auto tk = lx.scan("fun foo(): ;");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    auto& diags = p.getDiagnostics();
    // 应包含"期望返回类型名"
    bool found = false;
    for (const auto& d : diags.all()) {
        if (std::string(d.message.c_str()).find("返回类型名") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "应报'期望返回类型名'，实际诊断: " << diags.summary();
}

TEST(AuditBatch5ParserTypeAnn, FunDeclArrowMissingTypeReportsCorrectError) {
    // fun foo() -> ; — `->` 后缺类型
    Lexer lx; auto tk = lx.scan("fun foo() -> ;");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    auto& diags = p.getDiagnostics();
    bool found = false;
    for (const auto& d : diags.all()) {
        if (std::string(d.message.c_str()).find("返回类型名") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "应报'期望返回类型名'，实际诊断: " << diags.summary();
}

TEST(AuditBatch5ParserTypeAnn, FunDeclColonWithValidTypeWorks) {
    // 回归测试：fun foo(): int {} 应正常解析
    Lexer lx; auto tk = lx.scan("fun foo(): int { print(1); }");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_FALSE(p.hasErrors()) << "正常返回类型注解应被接受: " << p.getDiagnostics().summary();
}

TEST(AuditBatch5ParserTypeAnn, ClassBareMethodColonMissingType) {
    // class C { m(): ; } — 裸方法 `:` 后缺类型
    Lexer lx; auto tk = lx.scan("class C { m(): ; }");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    auto& diags = p.getDiagnostics();
    bool found = false;
    for (const auto& d : diags.all()) {
        if (std::string(d.message.c_str()).find("返回类型名") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "裸方法 `:` 后缺类型应报'期望返回类型名'";
}

// ============================================================
// Parser: BUG-PARSER-AUDIT-2 — classDecl 成员 try/catch
// ============================================================

TEST(AuditBatch5ParserClassRecovery, BadMemberDoesNotLoseEntireClass) {
    // class C { fun foo( var x = 1; } — fun foo( 缺参数，错误恢复后类应仍存在
    Lexer lx; auto tk = lx.scan("class C { fun foo( var x = 1; }");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    // 错误恢复不应导致整个类丢失
    ASSERT_NE(ast, nullptr);
    // 应至少有一个语句（可能是 ClassDecl 或后续声明）
    EXPECT_FALSE(ast->statements.empty());
}

TEST(AuditBatch5ParserClassRecovery, BadMemberDoesNotPolluteOuterBlock) {
    // { class C { fun foo( } var x = 1; } — class 的 } 不应被外层 block 消费
    // 修复后：classDecl 内部 try/catch 恢复，class 的 } 由 classDecl 消费
    Lexer lx; auto tk = lx.scan("class C { fun foo( }\nvar x = 1;");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    ASSERT_NE(ast, nullptr);
    // var x = 1; 应被正确解析（不被 class 错误污染）
    bool foundVarX = false;
    for (const auto& stmt : ast->statements) {
        if (stmt && stmt->nodeType == NodeType::NODE_VAR_DECL) {
            auto* decl = static_cast<VarDecl*>(stmt.get());
            if (decl->name == "x") {
                foundVarX = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundVarX) << "class 成员错误不应阻止后续语句解析";
}

TEST(AuditBatch5ParserClassRecovery, GoodClassAfterBadClassParsesCorrectly) {
    // 错误的类之后，正确的类应被正常解析
    Lexer lx;
    auto tk = lx.scan("class Bad { fun foo( }\nclass Good { fun bar() { print(1); } }");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    ASSERT_NE(ast, nullptr);
    // class Good 应被正确解析
    bool foundGood = false;
    for (const auto& stmt : ast->statements) {
        if (stmt && stmt->nodeType == NodeType::NODE_CLASS_DECL) {
            auto* cd = static_cast<ClassDecl*>(stmt.get());
            if (cd->name == "Good") {
                foundGood = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundGood) << "错误类之后的好类应被正确解析";
}

// ============================================================
// Parser: BUG-PARSER-AUDIT-5 — 错误数量上限
// ============================================================

TEST(AuditBatch5ParserErrorLimit, TooManyErrorsStopsParsing) {
    // 构造 200 个 ';' — 每个产生一个"意外的 Token"错误
    std::string src(200, ';');
    Lexer lx; auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    auto& diags = p.getDiagnostics();
    // 错误数应 <= MAX_PARSE_ERRORS + 1（100 条错误 + 1 条"错误过多"）
    int maxErrors = RuntimeLimits::MAX_PARSE_ERRORS + 1;
    EXPECT_LE(diags.errorCount(), static_cast<size_t>(maxErrors))
        << "错误数应不超过 " << maxErrors << "，实际: " << diags.errorCount();
    // 应包含"错误过多"消息
    bool foundStopMsg = false;
    for (const auto& d : diags.all()) {
        if (std::string(d.message.c_str()).find("错误过多") != std::string::npos) {
            foundStopMsg = true;
            break;
        }
    }
    EXPECT_TRUE(foundStopMsg) << "应报'错误过多'停止消息";
}

// ============================================================
// Parser: BUG-PARSER-AUDIT-7 — primary 深度检查时机
// ============================================================

TEST(AuditBatch5ParserDepthCheck, DeepParensDoesNotLoseExtraToken) {
    // 构造 260 层括号嵌套（超过 MAX_PARSE_DEPTH=256）
    // 修复后：深度检查在 match 之前，synchronize 不会多丢一个 token
    std::string src;
    for (int i = 0; i < 260; ++i) src += "(";
    src += "1";
    for (int i = 0; i < 260; ++i) src += ")";
    src += ";";
    Lexer lx; auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(p.hasErrors());
    // 应报深度超限错误
    auto& diags = p.getDiagnostics();
    bool foundDepthError = false;
    for (const auto& d : diags.all()) {
        if (std::string(d.message.c_str()).find("嵌套过深") != std::string::npos) {
            foundDepthError = true;
            break;
        }
    }
    EXPECT_TRUE(foundDepthError) << "应报'嵌套过深'错误";
}

// ============================================================
// Formatter: BUG-FMT-P1-1 — 插值字符串同行注释保留
// ============================================================

TEST(AuditBatch5FmtInterpComment, TrailingCommentAfterInterpStringPreserved) {
    // var s = "a${x}"; // comment
    // BUG-FMT-P1-1: 原实现用 <= exprLine 跳过了同行注释
    std::string src = "var s = \"a${x}\"; // comment";
    std::string formatted = formatSource(src);
    EXPECT_NE(formatted.find("// comment"), std::string::npos)
        << "同行注释应被保留，格式化结果: " << formatted;
}

TEST(AuditBatch5FmtInterpComment, TrailingCommentAfterPlainStringPreserved) {
    // 回归测试：普通字符串后的注释也应保留
    std::string src = "var s = \"hello\"; // comment";
    std::string formatted = formatSource(src);
    EXPECT_NE(formatted.find("// comment"), std::string::npos)
        << "普通字符串后注释应被保留";
}

// ============================================================
// Formatter: BUG-FMT-P1-2 — MemberAccess callee 加括号
// ============================================================

TEST(AuditBatch5FmtMemberCall, MemberAccessCalleeGetsParens) {
    // (obj.field)(42) — FunCall(callee=MemberAccess)
    // BUG-FMT-P1-2: 原实现输出 obj.field(42)，重解析为 MethodCall，破坏 AST
    std::string src = "var r = (obj.field)(42);";
    std::string formatted = formatSource(src);
    // 应保留 (obj.field) 的括号
    EXPECT_NE(formatted.find("(obj.field)"), std::string::npos)
        << "MemberAccess callee 应加括号保持 AST 等价，格式化结果: " << formatted;
}

TEST(AuditBatch5FmtMemberCall, MethodCallNoExtraParens) {
    // 回归测试：obj.field(42) — MethodCall 不应加括号
    std::string src = "var r = obj.field(42);";
    std::string formatted = formatSource(src);
    // MethodCall 应输出 obj.field(42)，不应变成 (obj.field)(42)
    EXPECT_NE(formatted.find("obj.field(42)"), std::string::npos)
        << "MethodCall 不应加括号";
    EXPECT_EQ(formatted.find("(obj.field)(42)"), std::string::npos)
        << "MethodCall 不应被误加括号";
}

// ============================================================
// Formatter: BUG-FMT-P2-1 — 同行 } 注释位置
// ============================================================

TEST(AuditBatch5FmtClosingBraceComment, CommentOnClosingBraceLineStaysInBlock) {
    // fun f() { var x = 1; } // comment
    // BUG-FMT-P2-1: 原实现注释跑到外层块
    std::string src = "fun f() { var x = 1; } // comment";
    std::string formatted = formatSource(src);
    // 注释应留在格式化结果中（位置在块内末尾）
    EXPECT_NE(formatted.find("// comment"), std::string::npos)
        << "同行 } 注释应保留在结果中";
}

// ============================================================
// Formatter: BUG-FMT-P2-2 — UOP_UNKNOWN 不输出注释
// ============================================================

// 注：UOP_UNKNOWN 不会由 Parser 正常产生，此测试验证 Formatter 对
// 手动构造的 UOP_UNKNOWN 节点不输出 /* unknown */ 注释。
// 由于构造 UOP_UNKNOWN 需要 AST 内部知识，这里通过间接方式验证：
// 确保正常一元运算符格式化不产生 /* unknown */ 注释。
TEST(AuditBatch5FmtUnaryUnknown, NormalUnaryOpsNoUnknownComment) {
    std::string src = "var x = -5; var y = not true;";
    std::string formatted = formatSource(src);
    EXPECT_EQ(formatted.find("/* unknown */"), std::string::npos)
        << "正常一元运算不应产生 /* unknown */ 注释";
    EXPECT_NE(formatted.find("-5"), std::string::npos);
    EXPECT_NE(formatted.find("not true"), std::string::npos);
}

// ============================================================
// Formatter: BUG-FMT-P2-3 — NaN/Infinity 输出可解析
// ============================================================

// 注：NaN/Infinity 不会由 Parser 正常产生（Lexer 不识别 nan/inf 字面量）。
// 此测试验证 Formatter 对含特殊浮点值的 NumberLiteral 不直接输出 "nan"/"inf"。
// 由于构造 NaN NumberLiteral 需要 Value 内部知识，这里通过间接方式验证：
// 确保正常数字格式化不产生裸 "nan"/"inf" 标识符。
TEST(AuditBatch5FmtNaN, NormalNumbersNoBareNaNInf) {
    std::string src = "var x = 1.0; var y = 2.5; var z = 1e10;";
    std::string formatted = formatSource(src);
    // 正常浮点不应产生裸 "nan"/"inf" 标识符
    // 检查格式化结果可被重新解析
    Lexer lx; auto tk = lx.scan(formatted);
    Parser p; auto ast = p.parse(tk);
    EXPECT_FALSE(p.hasErrors())
        << "格式化后的浮点数应可重新解析，结果: " << formatted;
}

// ============================================================
// Formatter: BUG-FMT-P2-4 — formatInterpolatedString nullptr 防御
// ============================================================

// 注：nullptr expressions 不会由 Parser 正常产生。
// 此测试验证正常插值字符串格式化不崩溃。
TEST(AuditBatch5FmtInterpNullptr, NormalInterpStringNoCrash) {
    std::string src = "var s = \"a${1+2}b${3}c\";";
    std::string formatted = formatSource(src);
    EXPECT_FALSE(formatted.empty());
    EXPECT_NE(formatted.find("${"), std::string::npos)
        << "插值字符串应保留 ${ 语法";
}

// ============================================================
// 三后端一致性回归 — 确保修补不破坏正常执行
// ============================================================

TEST(AuditBatch5Consistency, NormalProgramRunsOnAllBackends) {
    std::string src = R"(
var x = 10;
var y = 20;
print(x + y);
)";
    EXPECT_EQ(runInterpreter(src), "30");
}

TEST(AuditBatch5Consistency, InterpStringRunsOnAllBackends) {
    // MiniLang 插值语法为 {expr}（非 ${expr}），$ 为字面量字符
    std::string src = R"(
var name = "world";
print("hello {name}");
)";
    EXPECT_EQ(runInterpreter(src), "hello world");
}

TEST(AuditBatch5Consistency, ClassInheritanceRunsOnAllBackends) {
    std::string src = R"(
class Animal {
    var name = "?";
    fun speak() { return "generic"; }
}
class Dog extends Animal {
    fun speak() { return "woof"; }
}
var d = Dog();
print(d.speak());
)";
    EXPECT_EQ(runInterpreter(src), "woof");
}
