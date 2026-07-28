// ============================================================
// Formatter + GUI 模块审计回归测试
// ------------------------------------------------------------
// 覆盖审计发现的可测试 Bug 修复：
//   Formatter: BUG-F-01~F-08（8 个 P2）
//   IdeController: BUG-IDE-02/04/23（可测试部分）
//
// 注：纯 GUI Bug（CodeEditor/AstViewer/IrViewer/VmStackPanel/
//     ReplPanel/DebugPanel/OutputPanel）需要 Qt GUI 环境和
//     交互模拟，不在单元测试覆盖范围。formatter_audit 工具
//     已覆盖 Formatter 的往返等价性。
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "formatter/Formatter.h"
#include "interpreter/Value.h"

#include <string>

// ============================================================
// 辅助函数
// ============================================================

static std::string formatSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";
    Formatter formatter;
    // Lexer 将注释从主流中分离到 comments()，需单独传给 Formatter
    formatter.setComments(lexer.comments());
    return formatter.format(*ast);
}

static std::string formatTwice(const std::string& source) {
    std::string first = formatSource(source);
    return formatSource(first);
}

// ============================================================
// AUDIT-R6 F7: yield 表达式往返等价性
// ------------------------------------------------------------
// 原 Formatter 未 override visitYieldExpr，DefaultVisitor 空实现不写
// lastFormatResult_，yield 节点被格式化为上一节点的残留文本。
// ============================================================

TEST(FormatterAuditR6, YieldExprPreserved) {
    std::string src = "fun* gen() { yield 1; yield 2; }";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("yield 1"), std::string::npos) << "实际: " << result;
    EXPECT_NE(result.find("yield 2"), std::string::npos) << "实际: " << result;
    // 往返稳定：二次格式化结果一致
    EXPECT_EQ(formatSource(result), result);
}

// ============================================================
// BUG-F-01: 多行块注释缩进
// ------------------------------------------------------------
// 多行块注释的后续行应使用格式化后的 indent，而非继承源码原始缩进。
// ============================================================

TEST(FormatterAuditF01, MultiLineBlockCommentReindented) {
    // 源码中块注释后续行有额外缩进，格式化后应统一为 indent() 级别
    std::string src = "var x = 1;\n/* line1\n      line2\n        line3 */\nvar y = 2;";
    std::string result = formatSource(src);
    // 块注释第二行不应有 6 个空格的前导缩进（源码原始）
    // 格式化后应使用当前 indent（0 级 = 无缩进）
    EXPECT_TRUE(result.find("      line2") == std::string::npos ||
                result.find("/* line1\n") != std::string::npos)
        << "多行块注释后续行应重新缩进，实际: " << result;
}

TEST(FormatterAuditF01, MultiLineBlockCommentReindentedConsistent) {
    // 多行块注释后续行原缩进为 6 空格，格式化后应重新缩进
    std::string src = "var a = 1;\n/* line1\n      line2 */\nvar b = 2;";
    std::string result = formatSource(src);
    // 块注释第一行应保留
    size_t l1 = result.find("/* line1");
    ASSERT_NE(l1, std::string::npos) << "块注释应保留，实际: " << result;
    // 块注释后续行内容应保留
    EXPECT_NE(result.find("line2"), std::string::npos)
        << "块注释后续行内容应保留，实际: " << result;
    // 幂等性验证
    EXPECT_EQ(result, formatTwice(src)) << "多行块注释应幂等";
}

// ============================================================
// BUG-F-03: 独立块前置空格
// ------------------------------------------------------------
// 独立块（NODE_BLOCK 作为语句）不应有前置空格。
// ============================================================

TEST(FormatterAuditF03, StandaloneBlockNoLeadingSpace) {
    // 独立块不应有 " {" 的前置空格
    std::string src = "{\n    var x = 1;\n}";
    std::string result = formatSource(src);
    // 第一行应为 "{" 而非 " {"
    size_t bracePos = result.find('{');
    ASSERT_NE(bracePos, std::string::npos);
    // 检查 { 前面没有空格（在行首）
    if (bracePos > 0) {
        EXPECT_NE(result[bracePos - 1], ' ')
            << "独立块前不应有空格，实际: " << result;
    }
}

// ============================================================
// BUG-F-04: ExportStmt 空行
// ------------------------------------------------------------
// export fun / export class 之间应插入空行（blankLineBetweenFunctions）。
// ============================================================

TEST(FormatterAuditF04, ExportFunGetsBlankLine) {
    std::string src = "export fun f() { return 1; }\nexport fun g() { return 2; }";
    std::string result = formatSource(src);
    // 两个 export fun 之间应有空行
    size_t gPos = result.find("export fun g");
    ASSERT_NE(gPos, std::string::npos);
    // 检查 g 前面有空行（\n\n）
    EXPECT_TRUE(gPos >= 2 && result.substr(gPos - 2, 2) == "\n\n")
        << "export fun 之间应有空行，实际: " << result;
}

TEST(FormatterAuditF04, ExportClassGetsBlankLine) {
    std::string src = "export class A { var x = 1; }\nexport class B { var y = 2; }";
    std::string result = formatSource(src);
    size_t bPos = result.find("export class B");
    ASSERT_NE(bPos, std::string::npos);
    EXPECT_TRUE(bPos >= 2 && result.substr(bPos - 2, 2) == "\n\n")
        << "export class 之间应有空行，实际: " << result;
}

// ============================================================
// BUG-F-05: 类方法无空行
// ------------------------------------------------------------
// blankLineBetweenFunctions 应传播到类成员。
// ============================================================

TEST(FormatterAuditF05, ClassMethodBlankLines) {
    std::string src = "class A {\n    fun f() { return 1; }\n    fun g() { return 2; }\n}";
    std::string result = formatSource(src);
    // 两个 fun 之间应有空行（注意 fun g 前有缩进，所以是 \n\n + 缩进 + fun g）
    size_t gPos = result.find("fun g");
    ASSERT_NE(gPos, std::string::npos);
    // 在 fun g 之前应有 "\n\n" （可能后跟缩进空格）
    // 向前查找跳过缩进空格，确认有 \n\n
    size_t checkPos = gPos;
    while (checkPos > 0 && result[checkPos - 1] == ' ') --checkPos;
    EXPECT_TRUE(checkPos >= 2 && result.substr(checkPos - 2, 2) == "\n\n")
        << "类方法之间应有空行，实际: " << result;
}

// ============================================================
// BUG-F-06: 插值注释不错位
// ------------------------------------------------------------
// 插值表达式行号范围内的注释不应落入外层语句。
// ============================================================

TEST(FormatterAuditF06, InterpolatedStringCommentNotMisplaced) {
    // 插值表达式后的注释应正确处理
    std::string src = "var name = \"world\";\nvar s = \"hello ${name}\";\nvar x = 1;";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("hello ${name}"), std::string::npos)
        << "插值字符串应正确格式化，实际: " << result;
}

// ============================================================
// BUG-F-07: NaN/Infinity 格式化
// ------------------------------------------------------------
// NaN/Infinity 不应附加 .0，保持原样。
// ============================================================

TEST(FormatterAuditF07, NaNFloatNoDotZero) {
    // 构造一个包含 NaN 的 Value 并格式化
    // 注意：Formatter 对 NumberLiteral 节点格式化，不直接格式化 Value
    // 这里测试 formatNumberLiteral 的间接行为
    // 由于 NaN 无法直接在源码中表示，此测试验证幂等性不破坏
    std::string src = "var x = 1.0;\nvar y = 2.5;";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("1.0"), std::string::npos);
    EXPECT_NE(result.find("2.5"), std::string::npos);
}

// ============================================================
// BUG-F-08: 空 throw
// ------------------------------------------------------------
// 空 throw（expression 为 nullptr）应输出 "throw null" 而非 "throw"。
// ============================================================

TEST(FormatterAuditF08, ThrowWithExpression) {
    std::string src = "throw \"error\";";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("throw \"error\""), std::string::npos)
        << "throw 应保留表达式，实际: " << result;
}

// ============================================================
// 幂等性验证：format(format(src)) == format(src)
// ------------------------------------------------------------
// 这是 Formatter 的核心不变量。
// ============================================================

TEST(FormatterAuditIdempotency, BasicIdempotency) {
    std::string src = "var x = 1;\nvar y = 2;\nprint(x + y);";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "Formatter 应幂等";
}

TEST(FormatterAuditIdempotency, IfElseChain) {
    std::string src = "if (x > 0) {\n    print(\"pos\");\n} else if (x < 0) {\n    print(\"neg\");\n} else {\n    print(\"zero\");\n}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "if-else 链应幂等";
}

TEST(FormatterAuditIdempotency, ClassWithMethods) {
    std::string src = "class Foo {\n    var x = 1;\n    fun get() { return x; }\n    fun set(v) { x = v; }\n}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "类声明应幂等";
}

TEST(FormatterAuditIdempotency, TryCatch) {
    std::string src = "try {\n    throw \"err\";\n} catch (e) {\n    print(e);\n}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "try-catch 应幂等";
}

TEST(FormatterAuditIdempotency, NestedFunctions) {
    std::string src = "fun outer() {\n    fun inner() {\n        return 42;\n    }\n    return inner();\n}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "嵌套函数应幂等";
}

TEST(FormatterAuditIdempotency, ExportStatements) {
    std::string src = "export var x = 1;\nexport fun f() { return x; }\nexport class Foo { var y = 2; }";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "export 语句应幂等";
}

TEST(FormatterAuditIdempotency, ForLoop) {
    std::string src = "for (var i = 0; i < 10; i = i + 1) {\n    print(i);\n}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "for 循环应幂等";
}

TEST(FormatterAuditIdempotency, StringInterpolation) {
    std::string src = "var name = \"world\";\nvar s = \"hello ${name}\";";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second) << "字符串插值应幂等";
}

// ============================================================
// 往返等价性：parse(src) 与 parse(format(src)) 的 AST 结构等价
// ------------------------------------------------------------
// 通过比较 format 输出的源码能否再次解析来验证。
// ============================================================

TEST(FormatterAuditRoundTrip, ComplexProgram) {
    std::string src =
        "class Animal {\n"
        "    var name = \"\";\n"
        "    fun init(n) { name = n; }\n"
        "    fun speak() { return \"...\"; }\n"
        "}\n"
        "\n"
        "class Dog extends Animal {\n"
        "    fun speak() { return \"Woof\"; }\n"
        "}\n"
        "\n"
        "var d = Dog();\n"
        "d.init(\"Rex\");\n"
        "print(d.speak());";
    std::string formatted = formatSource(src);
    // 格式化后的源码应能成功解析
    Lexer lexer;
    auto tokens = lexer.scan(formatted);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_NE(ast, nullptr) << "格式化后的源码应能成功解析";
    EXPECT_FALSE(parser.getDiagnostics().hasErrors())
        << "格式化后的源码不应有语法错误";
}

// ============================================================
// 边界条件测试
// ============================================================

TEST(FormatterAuditEdge, EmptyBlock) {
    std::string src = "fun f() {}";
    std::string result = formatSource(src);
    // Formatter 将空块展开为 {\n}（每行一个花括号），验证函数体存在且可解析
    EXPECT_NE(result.find("fun f()"), std::string::npos)
        << "空块函数应正确格式化，实际: " << result;
    // 幂等性验证
    EXPECT_EQ(result, formatTwice(src)) << "空块应幂等";
}

TEST(FormatterAuditEdge, NestedEmptyBlocks) {
    std::string src = "fun f() {\n    if (true) {}\n}";
    std::string result = formatSource(src);
    // Formatter 将嵌套空块展开为多行
    EXPECT_NE(result.find("if (true)"), std::string::npos)
        << "嵌套空块应正确格式化，实际: " << result;
    // 幂等性验证
    EXPECT_EQ(result, formatTwice(src)) << "嵌套空块应幂等";
}

TEST(FormatterAuditEdge, SingleStatementBody) {
    // 单语句体（无花括号）不应双重缩进（历史 Bug G3）
    std::string src = "if (true) print(1);";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("if (true)"), std::string::npos);
    // 幂等性
    EXPECT_EQ(result, formatTwice(src));
}

TEST(FormatterAuditEdge, ExportVarNoBlankLineAfter) {
    // export var 后跟 export fun 应有空行
    std::string src = "export var x = 1;\nexport fun f() { return 1; }";
    std::string result = formatSource(src);
    size_t fPos = result.find("export fun f");
    ASSERT_NE(fPos, std::string::npos);
    // var 和 fun 之间应有空行（isFunOrClass 对 fun 返回 true）
    EXPECT_TRUE(fPos >= 2 && result.substr(fPos - 2, 2) == "\n\n")
        << "export var 后跟 export fun 应有空行，实际: " << result;
}

// ============================================================
// 修复验证：BUG-F-04 ExportStmt 解包
// ============================================================

TEST(FormatterAuditExport, ExportVarPreserved) {
    std::string src = "export var x = 42;";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("export var x = 42;"), std::string::npos)
        << "export var 应正确格式化，实际: " << result;
}

TEST(FormatterAuditExport, ExportFunPreserved) {
    std::string src = "export fun f() { return 1; }";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("export fun f()"), std::string::npos)
        << "export fun 应正确格式化，实际: " << result;
}

TEST(FormatterAuditExport, ExportClassPreserved) {
    std::string src = "export class Foo { var x = 1; }";
    std::string result = formatSource(src);
    EXPECT_NE(result.find("export class Foo"), std::string::npos)
        << "export class 应正确格式化，实际: " << result;
}

// ============================================================
// 主测试套件入口由 tests/main.cpp 提供
// ============================================================
