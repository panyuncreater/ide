// ============================================================
// TestMacro.cpp — 七特性 MVP 阶段 2：宏系统（parse 期展开）
// ------------------------------------------------------------
// 语法：
//   macro name(p1, p2) { <expr> }   —— 表达式模板宏声明
//   name!(arg1, arg2)               —— 宏调用（parse 期立即展开）
// 语义：
//   - 展开 = 克隆 body 模板 + 参数名 VarRef 替换为实参 AST（AST 级替换）
//   - 同一形参多次出现时共享同一实参子树（同 C 宏：实参副作用会重复执行）
//   - 四后端对 MacroCallExpr 一律求值/编译 expanded 子树，语义天然一致
//   - 未定义宏 / arity 不符 / 递归自引用 / body 含不支持构造 → ParseError
//   - Formatter 打印原始声明与 name!(args) 调用形式（往返等价）
// ============================================================

#include "common/ThreeBackends.h"
#include "formatter/Formatter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

// ============================================================
// 1. 基础展开
// ============================================================

TEST(MacroSystem, BasicExpansion) {
    std::string src = R"(
macro square(x) { x * x }
print(square!(5));
)";
    EXPECT_ALL_BACKENDS(src, "25");
}

TEST(MacroSystem, ZeroParamMacro) {
    std::string src = R"(
macro answer() { 42 }
print(answer!());
)";
    EXPECT_ALL_BACKENDS(src, "42");
}

TEST(MacroSystem, MultiParamExpansion) {
    std::string src = R"(
macro clamp(v, lo, hi) { min(max(v, lo), hi) }
print(clamp!(15, 0, 10));
print(clamp!(-3, 0, 10));
print(clamp!(7, 0, 10));
)";
    EXPECT_ALL_BACKENDS(src, "1007");
}

// ============================================================
// 2. 实参为复杂表达式 + 多次代入（C 宏语义：副作用重复执行）
// ============================================================

TEST(MacroSystem, ComplexArgSubstitutedMultipleTimes) {
    // square!(a + 1) 展开为 (a+1)*(a+1)——实参表达式在两个替换点各求值一次
    std::string src = R"(
macro square(x) { x * x }
var a = 3;
print(square!(a + 1));
)";
    EXPECT_ALL_BACKENDS(src, "16");
}

TEST(MacroSystem, SideEffectArgEvaluatedTwice) {
    // 教学对比点：宏展开（f() 执行两次）vs 函数调用（实参只求值一次）
    std::string src = R"(
macro double_it(x) { x + x }
var count = 0;
fun bump() {
    count = count + 1;
    return 10;
}
print(double_it!(bump()));
print(count);
)";
    EXPECT_ALL_BACKENDS(src, "202");
}

// ============================================================
// 3. 宏调用宏（嵌套展开）
// ============================================================

TEST(MacroSystem, MacroCallingMacro) {
    std::string src = R"(
macro square(x) { x * x }
macro quad(x) { square!(x) * square!(x) }
print(quad!(2));
)";
    EXPECT_ALL_BACKENDS(src, "16");
}

TEST(MacroSystem, MacroArgIsMacroCall) {
    std::string src = R"(
macro inc(x) { x + 1 }
print(inc!(inc!(inc!(0))));
)";
    EXPECT_ALL_BACKENDS(src, "3");
}

// ============================================================
// 4. 宏在函数体/闭包内使用（自由变量分析经 children() 正确穿透）
// ============================================================

TEST(MacroSystem, MacroInsideFunctionAndClosure) {
    std::string src = R"(
macro triple(x) { x * 3 }
fun outer() {
    var base = 7;
    fun inner() {
        return triple!(base);
    }
    return inner();
}
print(outer());
)";
    EXPECT_ALL_BACKENDS(src, "21");
}

// ============================================================
// 5. 错误路径（ParseError）
// ============================================================

namespace {
// 返回 parse 是否产生错误（宏错误均为 ParseError，四后端 parse 阶段一致）
bool parseHasErrors(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    return p.hasErrors() || !ast;
}
} // namespace

TEST(MacroSystem, UndefinedMacroIsParseError) {
    EXPECT_TRUE(parseHasErrors("print(nosuch!(1));"));
}

TEST(MacroSystem, ArityMismatchIsParseError) {
    EXPECT_TRUE(parseHasErrors("macro pair(a, b) { a + b } print(pair!(1));"));
}

TEST(MacroSystem, RecursiveMacroIsParseError) {
    // 自引用：宏在 body 解析完成后才登记 → body 内 self!(...) 报"未定义的宏"
    EXPECT_TRUE(parseHasErrors("macro looper(x) { looper!(x) } print(looper!(1));"));
}

TEST(MacroSystem, DuplicateMacroDefinitionIsParseError) {
    EXPECT_TRUE(parseHasErrors("macro m(x) { x } macro m(y) { y + 1 } print(m!(1));"));
}

TEST(MacroSystem, DuplicateParamNameIsParseError) {
    EXPECT_TRUE(parseHasErrors("macro bad(a, a) { a } print(bad!(1));"));
}

TEST(MacroSystem, UnsupportedBodyConstructIsParseError) {
    // lambda（FunDecl）不在表达式模板宏支持的节点子集内
    EXPECT_TRUE(parseHasErrors("macro bad(x) { fun(y) { return y; } } print(bad!(1));"));
}

// ============================================================
// 6. Formatter 往返等价
// ============================================================

namespace {
std::string formatMacroSource(const std::string& source) {
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
} // namespace

TEST(MacroSystem, FormatterPreservesMacroSyntax) {
    std::string src = "macro square(x) { x * x }\nprint(square!(5));";
    std::string result = formatMacroSource(src);
    EXPECT_NE(result.find("macro square(x) { x * x }"), std::string::npos) << "实际: " << result;
    EXPECT_NE(result.find("square!(5)"), std::string::npos) << "实际: " << result;
    // 往返稳定：二次格式化结果一致
    EXPECT_EQ(formatMacroSource(result), result);
}

TEST(MacroSystem, FormattedSourceStillExecutes) {
    // format 后的源码重新执行语义不变（宏表在重新 parse 时重建）
    std::string src = "macro cube(x) { x * x * x }\nprint(cube!(3));";
    std::string formatted = formatMacroSource(src);
    ASSERT_FALSE(formatted.empty());
    EXPECT_ALL_BACKENDS(formatted, "27");
}
