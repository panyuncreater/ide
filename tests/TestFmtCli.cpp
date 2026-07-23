// ============================================================
// tests/TestFmtCli.cpp - R109 代码格式化 CLI 核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_fmt 命名空间下的可测试函数：
//   - formatSource: 源码 → 格式化后字符串
//   - parseArgs: 命令行参数解析
//   - processFile: 文件处理（Check/Write/DryRun）
//   - versionString
//
// 不测试 main 函数（gtest 不易测试 main），main 的退出码逻辑
// 通过 parseArgs + processFile 的组合测试间接验证。
// ============================================================
#include "cli/fmt_core.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
/// 创建临时文件并写入内容，返回路径
/// 使用静态原子计数器生成唯一文件名，避免 std::tmpnam 的不安全性
std::string makeTempFile(const std::string& content, const std::string& suffix = ".ml") {
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1) + 1;
    auto path = fs::temp_directory_path() / ("minilang_fmt_test_" + std::to_string(id) + suffix);
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
} // namespace

// ============================================================
// FmtCliFormatSource: formatSource 函数测试
// ============================================================
TEST(FmtCliFormatSource, BasicFormat) {
    std::string src = "var  x=1;";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // 格式化后应有 "var x = 1;"
    EXPECT_NE(r.output.find("var x = 1;"), std::string::npos);
}

TEST(FmtCliFormatSource, EmptySource) {
    auto r = minilang_fmt::formatSource("", FormatOptions());
    EXPECT_TRUE(r.ok);
    // 空源码格式化后应为空字符串或仅空白
    EXPECT_TRUE(r.output.empty() || r.output.find_first_not_of(" \t\n") == std::string::npos);
}

TEST(FmtCliFormatSource, PreserveComments) {
    std::string src = "// 注释\nvar x = 1;";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_NE(r.output.find("注释"), std::string::npos);
}

TEST(FmtCliFormatSource, Idempotent) {
    std::string src = "var  x=1;var   y=2;";
    auto r1 = minilang_fmt::formatSource(src, FormatOptions());
    ASSERT_TRUE(r1.ok);
    auto r2 = minilang_fmt::formatSource(r1.output, FormatOptions());
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r1.output, r2.output);
}

TEST(FmtCliFormatSource, StyleKr) {
    FormatOptions opts; // 默认 K&R
    std::string src = "if (x) { print(x); }";
    auto r = minilang_fmt::formatSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // K&R 风格：if (x) {
    EXPECT_NE(r.output.find("if (x) {"), std::string::npos);
}

TEST(FmtCliFormatSource, StyleAllman) {
    FormatOptions opts = FormatOptions::allman();
    std::string src = "if (x) { print(x); }";
    auto r = minilang_fmt::formatSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // Allman 风格：if (x)\n{
    EXPECT_NE(r.output.find("if (x)\n{"), std::string::npos);
}

TEST(FmtCliFormatSource, StyleCompact) {
    FormatOptions opts = FormatOptions::compact();
    std::string src = "fun f() { return 1; }";
    auto r = minilang_fmt::formatSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // 紧凑样式：indentSize=2
    EXPECT_EQ(opts.indentSize, 2);
    EXPECT_FALSE(opts.blankLineBetweenFunctions);
}

TEST(FmtCliFormatSource, StyleTabbed) {
    FormatOptions opts = FormatOptions::tabbed();
    std::string src = "fun f() {\nreturn 1;\n}";
    auto r = minilang_fmt::formatSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(opts.useTabs);
}

TEST(FmtCliFormatSource, CustomIndent) {
    FormatOptions opts;
    opts.indentSize = 2;
    std::string src = "fun f() {\nreturn 1;\n}";
    auto r = minilang_fmt::formatSource(src, opts);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // 缩进 2 空格
    EXPECT_NE(r.output.find("\n  return"), std::string::npos);
}

TEST(FmtCliFormatSource, LexError) {
    // 非法字符（@ 不在 MiniLang 词法中）
    std::string src = "var x = @;";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "lex");
}

TEST(FmtCliFormatSource, ParseError) {
    // 缺少分号
    std::string src = "var x = 1";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
}

TEST(FmtCliFormatSource, LambdaExpression) {
    // R98 W3: lambda 表达式应被正确格式化
    std::string src = "var f=fun(x){return x*2;};";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    ASSERT_TRUE(r.ok) << r.errorMessage;
    // 应包含 "fun(x)"（匿名 lambda）和 "*"
    EXPECT_NE(r.output.find("fun(x)"), std::string::npos);
}

TEST(FmtCliFormatSource, ClassDecl) {
    std::string src = "class Foo{var x=1;fun bar(){return x;}}";
    auto r = minilang_fmt::formatSource(src, FormatOptions());
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_NE(r.output.find("class Foo"), std::string::npos);
    EXPECT_NE(r.output.find("fun bar()"), std::string::npos);
}

// ============================================================
// FmtCliParseArgs: parseArgs 函数测试
// ============================================================
TEST(FmtCliParseArgs, NoArgs) {
    char* argv[] = {(char*)"minilang-fmt"};
    auto args = minilang_fmt::parseArgs(1, argv);
    EXPECT_FALSE(args.parseError);
    EXPECT_TRUE(args.files.empty());
    EXPECT_FALSE(args.showHelp);
    EXPECT_FALSE(args.showVersion);
    EXPECT_EQ(args.mode, minilang_fmt::ProcessMode::Write); // 默认
}

TEST(FmtCliParseArgs, HelpArg) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--help"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
    EXPECT_FALSE(args.parseError);
}

TEST(FmtCliParseArgs, HelpShortArg) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"-h"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(FmtCliParseArgs, VersionArg) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--version"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(FmtCliParseArgs, VersionShortArg) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"-V"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(FmtCliParseArgs, CheckMode) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--check", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(3, argv);
    EXPECT_EQ(args.mode, minilang_fmt::ProcessMode::Check);
    ASSERT_EQ(args.files.size(), 1u);
    EXPECT_EQ(args.files[0], "foo.ml");
}

TEST(FmtCliParseArgs, WriteMode) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--write", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(3, argv);
    EXPECT_EQ(args.mode, minilang_fmt::ProcessMode::Write);
}

TEST(FmtCliParseArgs, DryRunMode) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--dry-run", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(3, argv);
    EXPECT_EQ(args.mode, minilang_fmt::ProcessMode::DryRun);
}

TEST(FmtCliParseArgs, IndentOption) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--indent", (char*)"2", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.indentSize, 2);
}

TEST(FmtCliParseArgs, IndentMissingValue) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--indent"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--indent"), std::string::npos);
}

TEST(FmtCliParseArgs, IndentOutOfRange) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--indent", (char*)"0", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    char* argv2[] = {(char*)"minilang-fmt", (char*)"--indent", (char*)"17", (char*)"foo.ml"};
    auto args2 = minilang_fmt::parseArgs(4, argv2);
    EXPECT_TRUE(args2.parseError);
}

TEST(FmtCliParseArgs, StyleKr) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style", (char*)"kr", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.braceStyle, BraceStyle::SAME_LINE);
}

TEST(FmtCliParseArgs, StyleAllman) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style", (char*)"allman", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.braceStyle, BraceStyle::NEXT_LINE);
}

TEST(FmtCliParseArgs, StyleCompact) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style", (char*)"compact", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.indentSize, 2);
    EXPECT_FALSE(args.options.blankLineBetweenFunctions);
}

TEST(FmtCliParseArgs, StyleTabbed) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style", (char*)"tabbed", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_TRUE(args.options.useTabs);
}

TEST(FmtCliParseArgs, StyleInvalid) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style", (char*)"unknown", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("unknown"), std::string::npos);
}

TEST(FmtCliParseArgs, StyleMissingValue) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--style"};
    auto args = minilang_fmt::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
}

TEST(FmtCliParseArgs, TabsOption) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--tabs", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(3, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_TRUE(args.options.useTabs);
}

TEST(FmtCliParseArgs, MultipleFiles) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"a.ml", (char*)"b.ml", (char*)"c.ml"};
    auto args = minilang_fmt::parseArgs(4, argv);
    ASSERT_EQ(args.files.size(), 3u);
    EXPECT_EQ(args.files[0], "a.ml");
    EXPECT_EQ(args.files[1], "b.ml");
    EXPECT_EQ(args.files[2], "c.ml");
}

TEST(FmtCliParseArgs, UnknownOption) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--unknown-option", (char*)"foo.ml"};
    auto args = minilang_fmt::parseArgs(3, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--unknown-option"), std::string::npos);
}

TEST(FmtCliParseArgs, MixedOptionsAndFiles) {
    char* argv[] = {(char*)"minilang-fmt", (char*)"--check", (char*)"--indent", (char*)"2",
                    (char*)"foo.ml",       (char*)"bar.ml"};
    auto args = minilang_fmt::parseArgs(6, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.mode, minilang_fmt::ProcessMode::Check);
    EXPECT_EQ(args.options.indentSize, 2);
    ASSERT_EQ(args.files.size(), 2u);
}

// ============================================================
// FmtCliProcessFile: processFile 函数测试
// ============================================================
TEST(FmtCliProcessFile, CheckFormattedFile) {
    std::string content = "var x = 1;\n";
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::Check, FormatOptions());
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.needsFormat); // 已格式化
    removeTempFile(path);
}

TEST(FmtCliProcessFile, CheckNeedsFormatFile) {
    std::string content = "var  x=1;\n"; // 需要格式化
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::Check, FormatOptions());
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(r.needsFormat);
    removeTempFile(path);
}

TEST(FmtCliProcessFile, WriteMode) {
    std::string content = "var  x=1;\n";
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::Write, FormatOptions());
    EXPECT_TRUE(r.ok) << r.errorMessage;
    // 读回文件验证
    std::ifstream ifs(path);
    std::string formatted((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(formatted.find("var x = 1;"), std::string::npos);
    removeTempFile(path);
}

TEST(FmtCliProcessFile, WriteModeNoChangeOnFormattedFile) {
    std::string content = "var x = 1;\n";
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::Write, FormatOptions());
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.needsFormat);
    removeTempFile(path);
}

TEST(FmtCliProcessFile, DryRunMode) {
    std::string content = "var  x=1;\n";
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::DryRun, FormatOptions());
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.output.empty());
    EXPECT_NE(r.output.find("var x = 1;"), std::string::npos);
    // 文件未被修改
    std::ifstream ifs(path);
    std::string current((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(current, content);
    removeTempFile(path);
}

TEST(FmtCliProcessFile, NonExistentFile) {
    auto r = minilang_fmt::processFile("/nonexistent/path/file.ml", minilang_fmt::ProcessMode::Check, FormatOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "io");
    EXPECT_NE(r.errorMessage.find("无法打开文件"), std::string::npos);
}

TEST(FmtCliProcessFile, ParseErrorFile) {
    std::string content = "var x = 1"; // 缺少分号
    auto path = makeTempFile(content);
    auto r = minilang_fmt::processFile(path, minilang_fmt::ProcessMode::Check, FormatOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
    removeTempFile(path);
}

// ============================================================
// FmtCliVersion: 版本信息测试
// ============================================================
TEST(FmtCliVersion, VersionString) {
    auto v = minilang_fmt::versionString();
    EXPECT_NE(v.find("minilang-fmt"), std::string::npos);
    EXPECT_NE(v.find("1.0.0"), std::string::npos);
}
