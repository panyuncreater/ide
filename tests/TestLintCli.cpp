// ============================================================
// tests/TestLintCli.cpp - R162 静态分析 CLI 核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_lint 命名空间下的可测试函数：
//   - lintSource: 源码 → LintResult（每规则一个测试）
//   - parseArgs: 命令行参数解析
//   - processFile: 文件处理（text/json 输出）
//   - parseRuleName/formatDiagnosticText/formatResultJson/versionString
//
// 不测试 main 函数（gtest 不易测试 main），main 的退出码逻辑
// 通过 parseArgs + processFile 的组合测试间接验证。
// ============================================================
#include "cli/lint_core.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// LintOptions/LintRule/LintResult 定义在 minilang::lint 命名空间中
using namespace minilang::lint;

namespace {
/// 创建临时文件并写入内容，返回路径
std::string makeTempFile(const std::string& content, const std::string& suffix = ".ml") {
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1) + 1;
    auto path = fs::temp_directory_path() / ("minilang_lint_test_" + std::to_string(id) + suffix);
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    return path.string();
}

/// 安全删除临时文件
void removeTempFile(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

/// 在诊断集合中查找包含指定 code 的诊断
bool hasDiagnosticWithCode(const std::vector<Diagnostic>& diags, const std::string& code) {
    for (const auto& d : diags) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}
} // namespace

// ============================================================
// LintCliSource: lintSource 函数测试（每规则一个测试）
// ============================================================

TEST(LintCliSource, CleanSource_NoWarnings) {
    std::string src = "var x = 1;\nprint(x);\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.lint.warningCount, 0);
    EXPECT_TRUE(r.lint.ok);
}

TEST(LintCliSource, UnusedVariable) {
    std::string src = "var unused = 1;\nvar used = 2;\nprint(used);\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_GE(r.lint.warningCount, 1);
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-variable"));
}

TEST(LintCliSource, UnusedFunction) {
    std::string src = "fun foo() { return 1; }\nfun main() { return foo(); }\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // foo 被调用，main 跳过；应无 UnusedFunction 警告
    EXPECT_FALSE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-function"));
}

TEST(LintCliSource, UnusedFunction_Triggered) {
    std::string src = "fun foo() { return 1; }\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-function"));
}

TEST(LintCliSource, UnusedFunction_MainSkipped) {
    std::string src = "fun main() { return 1; }\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // main 是约定入口，不应报告
    EXPECT_FALSE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-function"));
}

TEST(LintCliSource, UnusedParameter) {
    std::string src = "fun foo(a) { return 1; }\nprint(foo(1));\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-parameter"));
}

TEST(LintCliSource, AssignmentInCondition) {
    // MiniLang 语法：if (x = 1) 将赋值作为条件（应触发警告）
    std::string src = "var x = 0;\nif (x = 1) { print(x); }\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-assignment-in-condition"));
}

TEST(LintCliSource, DeadCodeAfterReturn) {
    std::string src = "fun foo() { return 1; print(2); }\nprint(foo());\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-dead-code"));
}

TEST(LintCliSource, EmptyBlock) {
    std::string src = "fun foo() { }\nprint(foo());\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-empty-block"));
}

TEST(LintCliSource, CyclomaticComplexity) {
    // 构造一个圈复杂度 > 默认阈值 10 的函数
    std::string src = "fun complex(a, b, c) {\n"
                      "    if (a > 0) { return 1; }\n"
                      "    if (b > 0) { return 2; }\n"
                      "    if (c > 0) { return 3; }\n"
                      "    if (a > 1) { return 4; }\n"
                      "    if (b > 1) { return 5; }\n"
                      "    if (c > 1) { return 6; }\n"
                      "    if (a > 2) { return 7; }\n"
                      "    if (b > 2) { return 8; }\n"
                      "    if (c > 2) { return 9; }\n"
                      "    if (a > 3) { return 10; }\n"
                      "    if (b > 3) { return 11; }\n"
                      "    return 0;\n"
                      "}\n"
                      "print(complex(1, 2, 3));\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-cyclomatic-complexity"));
}

TEST(LintCliSource, CyclomaticComplexity_BelowThreshold) {
    // 圈复杂度低于阈值，不应触发
    LintOptions opts;
    opts.maxCyclomaticComplexity = 100;
    std::string src = "fun simple(a) {\n"
                      "    if (a > 0) { return 1; }\n"
                      "    return 0;\n"
                      "}\n"
                      "print(simple(1));\n";
    auto r = minilang_lint::lintSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-cyclomatic-complexity"));
}

TEST(LintCliSource, NamingConvention_Variable) {
    // 变量名不符合 camelCase（含下划线）
    std::string src = "var bad_name = 1;\nprint(bad_name);\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-naming-convention"));
}

TEST(LintCliSource, NamingConvention_Function) {
    // 函数名不符合 camelCase（含下划线）
    std::string src = "fun bad_func() { return 1; }\nprint(bad_func());\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-naming-convention"));
}

TEST(LintCliSource, NamingConvention_Class) {
    // 类名不符合 PascalCase（首字母小写）
    std::string src = "class badClass { var x = 1; }\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-naming-convention"));
}

TEST(LintCliSource, NamingConvention_AllUpperConstantSkipped) {
    // 全大写常量名应跳过（不视为命名违规）
    std::string src = "var MAX_SIZE = 100;\nprint(MAX_SIZE);\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-naming-convention"));
}

TEST(LintCliSource, DisabledRule_NoWarning) {
    // 禁用 UnusedVariable 规则
    LintOptions opts;
    opts.disabledRules.insert(LintRule::UnusedVariable);
    std::string src = "var unused = 1;\n";
    auto r = minilang_lint::lintSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(hasDiagnosticWithCode(r.lint.diagnostics.all(), "lint-unused-variable"));
}

TEST(LintCliSource, LexError) {
    // 词法错误：未闭合的字符串
    std::string src = "var x = \"unclosed;\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "lex");
}

TEST(LintCliSource, ParseError) {
    // 语法错误：缺少分号
    std::string src = "var x = 1\n";
    auto r = minilang_lint::lintSource(src, LintOptions{});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
}

TEST(LintCliSource, EmptySource) {
    // 空源码解析为空 Block，可能触发 EmptyBlock 警告；禁用该规则后应无警告
    LintOptions opts;
    opts.disabledRules.insert(LintRule::EmptyBlock);
    auto r = minilang_lint::lintSource("", opts);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.lint.warningCount, 0);
}

// ============================================================
// LintCliParseArgs: parseArgs 函数测试
// ============================================================

TEST(LintCliParseArgs, NoArgs) {
    char* argv[] = {(char*)"minilang-lint"};
    auto args = minilang_lint::parseArgs(1, argv);
    EXPECT_FALSE(args.parseError);
    EXPECT_TRUE(args.files.empty());
    EXPECT_FALSE(args.showHelp);
    EXPECT_FALSE(args.showVersion);
    EXPECT_FALSE(args.quiet);
    EXPECT_EQ(args.format, minilang_lint::OutputFormat::Text);
}

TEST(LintCliParseArgs, HelpArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--help"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
    EXPECT_FALSE(args.parseError);
}

TEST(LintCliParseArgs, HelpShortArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"-h"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(LintCliParseArgs, VersionArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--version"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(LintCliParseArgs, VersionShortArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"-V"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(LintCliParseArgs, QuietArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--quiet", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(3, argv);
    EXPECT_TRUE(args.quiet);
    ASSERT_EQ(args.files.size(), 1u);
    EXPECT_EQ(args.files[0], "foo.ml");
}

TEST(LintCliParseArgs, QuietShortArg) {
    char* argv[] = {(char*)"minilang-lint", (char*)"-q", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(3, argv);
    EXPECT_TRUE(args.quiet);
}

TEST(LintCliParseArgs, FormatText) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--format", (char*)"text", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.format, minilang_lint::OutputFormat::Text);
}

TEST(LintCliParseArgs, FormatJson) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--format", (char*)"json", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.format, minilang_lint::OutputFormat::Json);
}

TEST(LintCliParseArgs, FormatInvalid) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--format", (char*)"xml", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("xml"), std::string::npos);
}

TEST(LintCliParseArgs, FormatMissingValue) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--format"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--format"), std::string::npos);
}

TEST(LintCliParseArgs, RuleDisable) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--rule", (char*)"UnusedVariable", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.disabledRules.size(), 1u);
    EXPECT_NE(args.options.disabledRules.find(LintRule::UnusedVariable), args.options.disabledRules.end());
}

TEST(LintCliParseArgs, RuleDisableMultiple) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--rule",     (char*)"UnusedVariable",
                    (char*)"--rule",        (char*)"EmptyBlock", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(6, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.disabledRules.size(), 2u);
}

TEST(LintCliParseArgs, RuleInvalid) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--rule", (char*)"InvalidRule", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("InvalidRule"), std::string::npos);
}

TEST(LintCliParseArgs, RuleMissingValue) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--rule"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--rule"), std::string::npos);
}

TEST(LintCliParseArgs, MaxComplexity) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--max-complexity", (char*)"5", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.maxCyclomaticComplexity, 5);
}

TEST(LintCliParseArgs, MaxComplexityMissingValue) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--max-complexity"};
    auto args = minilang_lint::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--max-complexity"), std::string::npos);
}

TEST(LintCliParseArgs, MaxComplexityOutOfRange) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--max-complexity", (char*)"0", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    char* argv2[] = {(char*)"minilang-lint", (char*)"--max-complexity", (char*)"101", (char*)"foo.ml"};
    auto args2 = minilang_lint::parseArgs(4, argv2);
    EXPECT_TRUE(args2.parseError);
}

TEST(LintCliParseArgs, UnknownOption) {
    char* argv[] = {(char*)"minilang-lint", (char*)"--unknown", (char*)"foo.ml"};
    auto args = minilang_lint::parseArgs(3, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--unknown"), std::string::npos);
}

TEST(LintCliParseArgs, MultipleFiles) {
    char* argv[] = {(char*)"minilang-lint", (char*)"foo.ml", (char*)"bar.ml", (char*)"baz.ml"};
    auto args = minilang_lint::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    ASSERT_EQ(args.files.size(), 3u);
    EXPECT_EQ(args.files[0], "foo.ml");
    EXPECT_EQ(args.files[1], "bar.ml");
    EXPECT_EQ(args.files[2], "baz.ml");
}

// ============================================================
// LintCliProcessFile: processFile 函数测试
// ============================================================

TEST(LintCliProcessFile, CleanFile) {
    std::string src = "var x = 1;\nprint(x);\n";
    std::string path = makeTempFile(src);
    auto r = minilang_lint::processFile(path, LintOptions{}, minilang_lint::OutputFormat::Text, false);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.hasWarnings);
    EXPECT_EQ(r.warningCount, 0);
    removeTempFile(path);
}

TEST(LintCliProcessFile, FileWithWarning) {
    std::string src = "var unused = 1;\n";
    std::string path = makeTempFile(src);
    auto r = minilang_lint::processFile(path, LintOptions{}, minilang_lint::OutputFormat::Text, false);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.hasWarnings);
    EXPECT_GE(r.warningCount, 1);
    // 文本输出应包含 lint-unused-variable
    EXPECT_NE(r.output.find("lint-unused-variable"), std::string::npos);
    removeTempFile(path);
}

TEST(LintCliProcessFile, NonExistentFile) {
    auto r =
        minilang_lint::processFile("nonexistent_file_xyz.ml", LintOptions{}, minilang_lint::OutputFormat::Text, false);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "io");
    EXPECT_NE(r.errorMessage.find("nonexistent_file_xyz.ml"), std::string::npos);
}

TEST(LintCliProcessFile, JsonFormat) {
    std::string src = "var unused = 1;\n";
    std::string path = makeTempFile(src);
    auto r = minilang_lint::processFile(path, LintOptions{}, minilang_lint::OutputFormat::Json, false);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.hasWarnings);
    // JSON 输出应包含关键字段
    EXPECT_NE(r.output.find("\"file\""), std::string::npos);
    EXPECT_NE(r.output.find("\"warnings\""), std::string::npos);
    EXPECT_NE(r.output.find("\"diagnostics\""), std::string::npos);
    EXPECT_NE(r.output.find("lint-unused-variable"), std::string::npos);
    removeTempFile(path);
}

TEST(LintCliProcessFile, QuietModeSuppressesWarnings) {
    std::string src = "var unused = 1;\n";
    std::string path = makeTempFile(src);
    auto r = minilang_lint::processFile(path, LintOptions{}, minilang_lint::OutputFormat::Text, true);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.hasWarnings); // hasWarnings 仍为 true（仅影响输出）
    // quiet 模式下输出不应包含警告行（但摘要行可能有）
    // 注意：quiet 仅抑制警告诊断行，摘要行在 !quiet || errorCount>0 时输出
    // 这里 quiet=true 且 errorCount=0，故摘要行也不输出
    EXPECT_EQ(r.output.find("lint-unused-variable"), std::string::npos);
    removeTempFile(path);
}

TEST(LintCliProcessFile, ParseErrorFile) {
    std::string src = "var x = 1\n"; // 缺少分号
    std::string path = makeTempFile(src);
    auto r = minilang_lint::processFile(path, LintOptions{}, minilang_lint::OutputFormat::Text, false);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
    removeTempFile(path);
}

// ============================================================
// LintCliHelpers: 辅助函数测试
// ============================================================

TEST(LintCliHelpers, ParseRuleName_Valid) {
    EXPECT_EQ(minilang_lint::parseRuleName("UnusedVariable"), LintRule::UnusedVariable);
    EXPECT_EQ(minilang_lint::parseRuleName("UnusedFunction"), LintRule::UnusedFunction);
    EXPECT_EQ(minilang_lint::parseRuleName("UnusedParameter"), LintRule::UnusedParameter);
    EXPECT_EQ(minilang_lint::parseRuleName("AssignmentInCondition"), LintRule::AssignmentInCondition);
    EXPECT_EQ(minilang_lint::parseRuleName("DeadCodeAfterReturn"), LintRule::DeadCodeAfterReturn);
    EXPECT_EQ(minilang_lint::parseRuleName("EmptyBlock"), LintRule::EmptyBlock);
    EXPECT_EQ(minilang_lint::parseRuleName("CyclomaticComplexity"), LintRule::CyclomaticComplexity);
    EXPECT_EQ(minilang_lint::parseRuleName("NamingConvention"), LintRule::NamingConvention);
}

TEST(LintCliHelpers, ParseRuleName_Invalid) {
    EXPECT_EQ(minilang_lint::parseRuleName("Invalid"), LintRule::Count);
    EXPECT_EQ(minilang_lint::parseRuleName(""), LintRule::Count);
    EXPECT_EQ(minilang_lint::parseRuleName("unused-variable"), LintRule::Count); // 不接受 kebab-case
}

TEST(LintCliHelpers, FormatDiagnosticText) {
    Diagnostic diag(DiagLevel::Warning, "未使用的变量 'x'", 3, 5, DiagSource::TypeChecker, "lint-unused-variable");
    std::string text = minilang_lint::formatDiagnosticText("foo.ml", diag);
    EXPECT_NE(text.find("foo.ml"), std::string::npos);
    EXPECT_NE(text.find("3:5"), std::string::npos);
    EXPECT_NE(text.find("警告"), std::string::npos);
    EXPECT_NE(text.find("未使用的变量"), std::string::npos);
    EXPECT_NE(text.find("lint-unused-variable"), std::string::npos);
}

TEST(LintCliHelpers, FormatResultJson) {
    LintResult result;
    result.ok = true;
    result.warningCount = 1;
    result.errorCount = 0;
    result.diagnostics.addWarning("test warning", 1, 1, DiagSource::TypeChecker, "lint-test");

    std::string json = minilang_lint::formatResultJson("foo.ml", result);
    EXPECT_NE(json.find("\"file\": \"foo.ml\""), std::string::npos);
    EXPECT_NE(json.find("\"ok\": true"), std::string::npos);
    EXPECT_NE(json.find("\"warnings\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"errors\": 0"), std::string::npos);
    EXPECT_NE(json.find("lint-test"), std::string::npos);
    EXPECT_NE(json.find("test warning"), std::string::npos);
}

TEST(LintCliHelpers, VersionString) {
    std::string v = minilang_lint::versionString();
    EXPECT_NE(v.find("minilang-lint"), std::string::npos);
    EXPECT_NE(v.find("R162"), std::string::npos);
}

// ============================================================
// 拓展二期：--fix 结构化自动修复
// ============================================================

TEST(LintFix, RemovesUnusedVariable) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::applyFixes("var unused = 1;\nvar x = 2;\nprint(x);\n", opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.removedCount, 1);
    EXPECT_EQ(r.fixedSource.find("unused"), std::string::npos);
    EXPECT_NE(r.fixedSource.find("print(x)"), std::string::npos);
}

TEST(LintFix, KeepsVariableWithSideEffectInitializer) {
    minilang_lint::LintOptions opts;
    // 初始化器含函数调用：副作用保守，不删除
    auto r = minilang_lint::applyFixes("fun f() { print(1); return 2; }\n"
                                       "var unused = f();\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.removedCount, 0);
    EXPECT_EQ(r.skippedCount, 1);
    EXPECT_NE(r.fixedSource.find("unused"), std::string::npos);
}

TEST(LintFix, RemovesDeadCodeAfterReturn) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::applyFixes("fun g() {\n"
                                       "  return 1;\n"
                                       "  print(999);\n" // 不可达
                                       "}\n"
                                       "print(g());\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_GE(r.removedCount, 1);
    EXPECT_EQ(r.fixedSource.find("999"), std::string::npos);
}

TEST(LintFix, RemovesUnusedFunction) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::applyFixes("fun neverCalled() { return 1; }\n"
                                       "var x = 1;\nprint(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_GE(r.removedCount, 1);
    EXPECT_EQ(r.fixedSource.find("neverCalled"), std::string::npos);
}

TEST(LintFix, FixedSourceStaysParsableAndLintCleaner) {
    minilang_lint::LintOptions opts;
    const std::string src = "var unused = 1;\n"
                            "fun dead() { return 0; }\n"
                            "var x = 2;\nprint(x);\n";
    auto r = minilang_lint::applyFixes(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.removedCount, 2);
    // 修复后源码必须仍可解析，且相关警告消失
    auto relint = minilang_lint::lintSource(r.fixedSource, opts);
    ASSERT_TRUE(relint.ok) << relint.errorMessage;
    for (const auto& d : relint.lint.diagnostics.all()) {
        EXPECT_NE(d.code, "lint-unused-variable");
        EXPECT_NE(d.code, "lint-unused-function");
    }
}

TEST(LintFix, ParseErrorReportsPhase) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::applyFixes("var x = ;", opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
}

TEST(LintFix, ParseArgsRecognizesFixFlags) {
    const char* argv1[] = {"minilang-lint", "--fix", "a.mini"};
    auto args1 = minilang_lint::parseArgs(3, const_cast<char**>(argv1));
    EXPECT_TRUE(args1.fix);
    EXPECT_FALSE(args1.fixDryRun);
    const char* argv2[] = {"minilang-lint", "--fix-dry-run", "a.mini"};
    auto args2 = minilang_lint::parseArgs(3, const_cast<char**>(argv2));
    EXPECT_TRUE(args2.fixDryRun);
}

// ============================================================
// 拓展二期：match 穷尽性检查（NonExhaustiveMatch）
// ============================================================

namespace {
/// 检查诊断中是否含指定 code 的警告，返回首条匹配消息（无则空串）
std::string findDiagByCode(const minilang_lint::LintSourceResult& r, const std::string& code) {
    for (const auto& d : r.lint.diagnostics.all()) {
        if (d.code == code)
            return d.message;
    }
    return "";
}
} // namespace

TEST(LintExhaustiveMatch, WarnsOnMissingVariant) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::lintSource("enum Color { Red, Green, Blue }\n"
                                       "var c = Color.Red;\n"
                                       "var x = match (c) {\n"
                                       "  case Color.Red => 1\n"
                                       "  case Color.Green => 2\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    std::string msg = findDiagByCode(r, "lint-non-exhaustive-match");
    ASSERT_FALSE(msg.empty());
    EXPECT_NE(msg.find("Color.Blue"), std::string::npos); // 指名缺失 variant
}

TEST(LintExhaustiveMatch, NoWarnWhenAllVariantsCovered) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::lintSource("enum Color { Red, Green }\n"
                                       "var x = match (Color.Red) {\n"
                                       "  case Color.Red => 1\n"
                                       "  case Color.Green => 2\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
}

TEST(LintExhaustiveMatch, NoWarnWithDefault) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::lintSource("enum Color { Red, Green, Blue }\n"
                                       "var x = match (Color.Red) {\n"
                                       "  case Color.Red => 1\n"
                                       "  default => 0\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
}

TEST(LintExhaustiveMatch, GuardedCaseDoesNotCountAsCoverage) {
    minilang_lint::LintOptions opts;
    // Some 分支带 guard（可能失败）：不算覆盖 → 仍报 Some 与 None
    auto r = minilang_lint::lintSource("enum Option { Some(int), None }\n"
                                       "var x = match (Option.Some(1)) {\n"
                                       "  case Option.Some(v) if v > 0 => v\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    std::string msg = findDiagByCode(r, "lint-non-exhaustive-match");
    ASSERT_FALSE(msg.empty());
    EXPECT_NE(msg.find("Option.Some"), std::string::npos);
    EXPECT_NE(msg.find("Option.None"), std::string::npos);
}

TEST(LintExhaustiveMatch, LiteralSubPatternDoesNotCoverVariant) {
    minilang_lint::LintOptions opts;
    // Some(1) 只覆盖字面量 1，不算覆盖 Some → 报缺 Some
    auto r = minilang_lint::lintSource("enum Option { Some(int), None }\n"
                                       "var x = match (Option.Some(1)) {\n"
                                       "  case Option.Some(1) => 10\n"
                                       "  case Option.None => 0\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    std::string msg = findDiagByCode(r, "lint-non-exhaustive-match");
    ASSERT_FALSE(msg.empty());
    EXPECT_NE(msg.find("Option.Some"), std::string::npos);
}

TEST(LintExhaustiveMatch, CatchAllVariablePatternSuppresses) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::lintSource("enum Color { Red, Green, Blue }\n"
                                       "var x = match (Color.Red) {\n"
                                       "  case Color.Red => 1\n"
                                       "  case other => 0\n" // VARIABLE 兕底
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
}

TEST(LintExhaustiveMatch, OrPatternExpandsCoverage) {
    minilang_lint::LintOptions opts;
    auto r = minilang_lint::lintSource("enum Color { Red, Green, Blue }\n"
                                       "var x = match (Color.Red) {\n"
                                       "  case Color.Red or Color.Green or Color.Blue => 1\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
}

TEST(LintExhaustiveMatch, NonEnumMatchNotChecked) {
    minilang_lint::LintOptions opts;
    // 整数 match（无 VARIANT pattern）：不做穷尽性检查
    auto r = minilang_lint::lintSource("var x = match (5) {\n"
                                       "  case 1 => 10\n"
                                       "  case 2 => 20\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
}

TEST(LintExhaustiveMatch, RuleCanBeDisabled) {
    minilang_lint::LintOptions opts;
    opts.disabledRules.insert(minilang_lint::LintRule::NonExhaustiveMatch);
    auto r = minilang_lint::lintSource("enum Color { Red, Green }\n"
                                       "var x = match (Color.Red) {\n"
                                       "  case Color.Red => 1\n"
                                       "};\n"
                                       "print(x);\n",
                                       opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(findDiagByCode(r, "lint-non-exhaustive-match").empty());
    // parseRuleName 同步支持新规则名
    EXPECT_EQ(minilang_lint::parseRuleName("NonExhaustiveMatch"), minilang_lint::LintRule::NonExhaustiveMatch);
}
