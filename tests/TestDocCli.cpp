// ============================================================
// tests/TestDocCli.cpp - R162 B2 文档生成器 CLI 核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_doc 命名空间下的可测试函数：
//   - generateDoc: 源码 → DocResult
//   - processFile: 文件处理（markdown/html/json 输出）
//   - parseArgs: 命令行参数解析
//   - parseFormatName/versionString
// ============================================================
#include "cli/doc_core.h"
#include "doc/DocGenerator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace minilang::doc;

namespace {
/// 创建临时文件并写入内容，返回路径
std::string makeTempFile(const std::string& content, const std::string& suffix = ".ml") {
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1) + 1;
    auto path = fs::temp_directory_path() / ("minilang_doc_test_" + std::to_string(id) + suffix);
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
// DocCliSource: generateDoc 函数测试
// ============================================================

TEST(DocCliSource, EmptySource) {
    auto r = minilang_doc::generateDoc("", DocFormat::Markdown);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.doc.entries.size(), 0u);
}

TEST(DocCliSource, FunctionWithoutDoc) {
    std::string src = "fun foo(a) { return a; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].kind, DocEntryKind::Function);
    EXPECT_EQ(r.doc.entries[0].name, "foo");
    EXPECT_TRUE(r.doc.entries[0].doc.summary.empty());
}

TEST(DocCliSource, FunctionWithLineDocComment) {
    std::string src = "/// 计算两数之和\n"
                      "/// 这是多行摘要\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].name, "add");
    EXPECT_FALSE(r.doc.entries[0].doc.summary.empty());
    EXPECT_NE(r.doc.entries[0].doc.summary.find("计算两数之和"), std::string::npos);
}

TEST(DocCliSource, FunctionWithBlockDocComment) {
    std::string src = "/** 计算两数之和 */\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].name, "add");
    EXPECT_NE(r.doc.entries[0].doc.summary.find("计算两数之和"), std::string::npos);
}

TEST(DocCliSource, FunctionWithParamTag) {
    std::string src = "/// 计算两数之和\n"
                      "/// @param a 第一个数\n"
                      "/// @param b 第二个数\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    auto params = r.doc.entries[0].doc.params();
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].value, "a");
    EXPECT_NE(params[0].body.find("第一个数"), std::string::npos);
    EXPECT_EQ(params[1].value, "b");
}

TEST(DocCliSource, FunctionWithReturnTag) {
    std::string src = "/// 计算两数之和\n"
                      "/// @return 两数之和\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    auto* ret = r.doc.entries[0].doc.returnTag();
    ASSERT_NE(ret, nullptr);
    EXPECT_NE(ret->body.find("两数之和"), std::string::npos);
}

TEST(DocCliSource, FunctionWithExampleTag) {
    std::string src = "/// 示例函数\n"
                      "/// @example add(1, 2)\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    bool hasExample = false;
    for (const auto& t : r.doc.entries[0].doc.tags) {
        if (t.kind == DocTagKind::Example) {
            hasExample = true;
            EXPECT_NE(t.body.find("add(1, 2)"), std::string::npos);
        }
    }
    EXPECT_TRUE(hasExample);
}

TEST(DocCliSource, FunctionWithDeprecatedTag) {
    std::string src = "/// 旧函数\n"
                      "/// @deprecated 请使用 newAdd\n"
                      "fun add(a, b) { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(r.doc.entries[0].doc.isDeprecated());
}

TEST(DocCliSource, FunctionWithSinceTag) {
    std::string src = "/// 函数\n"
                      "/// @since 1.0.0\n"
                      "fun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_TRUE(r.doc.entries[0].doc.hasSince());
    EXPECT_EQ(r.doc.entries[0].doc.sinceVersion(), "1.0.0");
}

TEST(DocCliSource, ClassDecl) {
    std::string src = "/// 一个类\n"
                      "class Foo {\n"
                      "    var x = 1;\n"
                      "    fun bar() { return x; }\n"
                      "}\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].kind, DocEntryKind::Class);
    EXPECT_EQ(r.doc.entries[0].name, "Foo");
    EXPECT_NE(r.doc.entries[0].doc.summary.find("一个类"), std::string::npos);
    EXPECT_GE(r.doc.entries[0].members.size(), 2u);
}

TEST(DocCliSource, ClassWithSuperClass) {
    std::string src = "/// 子类\n"
                      "class Bar : Foo {\n"
                      "    var y = 2;\n"
                      "}\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].superClass, "Foo");
    EXPECT_NE(r.doc.entries[0].signature.find("Foo"), std::string::npos);
}

TEST(DocCliSource, EnumDecl) {
    std::string src = "/// 颜色枚举\n"
                      "enum Color {\n"
                      "    RED,\n"
                      "    GREEN,\n"
                      "    BLUE\n"
                      "}\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].kind, DocEntryKind::Enum);
    EXPECT_EQ(r.doc.entries[0].name, "Color");
    EXPECT_EQ(r.doc.entries[0].variants.size(), 3u);
}

TEST(DocCliSource, GenericEnumDecl) {
    std::string src = "/// 结果类型\n"
                      "enum Result<T, E> {\n"
                      "    Ok(T),\n"
                      "    Err(E)\n"
                      "}\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].typeParams.size(), 2u);
    EXPECT_NE(r.doc.entries[0].signature.find("T"), std::string::npos);
}

TEST(DocCliSource, VariableDecl) {
    std::string src = "/// 全局常量\n"
                      "var PI = 3.14;\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_EQ(r.doc.entries[0].kind, DocEntryKind::Variable);
    EXPECT_EQ(r.doc.entries[0].name, "PI");
}

TEST(DocCliSource, MultipleEntries) {
    std::string src = "/// 函数1\n"
                      "fun foo() { return 1; }\n"
                      "/// 函数2\n"
                      "fun bar() { return 2; }\n"
                      "/// 类1\n"
                      "class Baz { var x = 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.doc.entries.size(), 3u);
    EXPECT_EQ(r.doc.entries[0].name, "foo");
    EXPECT_EQ(r.doc.entries[1].name, "bar");
    EXPECT_EQ(r.doc.entries[2].name, "Baz");
}

TEST(DocCliSource, NonDocCommentIgnored) {
    std::string src = "// 普通注释，不应作为文档注释\n"
                      "fun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_TRUE(r.doc.entries[0].doc.summary.empty());
}

TEST(DocCliSource, DocCommentNotAdjacent) {
    // 文档注释与声明之间有空行（行号不连续）——不关联
    std::string src = "/// 文档注释\n"
                      "\n"
                      "fun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    // 空行导致行号不连续，文档注释不应关联（endLine=1, declLine=3, 不匹配）
    EXPECT_TRUE(r.doc.entries[0].doc.summary.empty());
}

TEST(DocCliSource, LexError) {
    std::string src = "var x = \"unclosed;\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "lex");
}

TEST(DocCliSource, ParseError) {
    std::string src = "var x = 1\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
}

TEST(DocCliSource, FunctionSignature) {
    std::string src = "fun add(a: int, b: int): int { return a + b; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    ASSERT_EQ(r.doc.entries.size(), 1u);
    EXPECT_NE(r.doc.entries[0].signature.find("add"), std::string::npos);
    EXPECT_NE(r.doc.entries[0].signature.find("int"), std::string::npos);
}

TEST(DocCliSource, MarkdownFormat) {
    std::string src = "/// 函数\nfun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Markdown);
    ASSERT_TRUE(r.ok);
    DocGenerator gen(DocFormat::Markdown);
    std::string md = gen.formatOutput(r.doc);
    EXPECT_NE(md.find("# API 文档"), std::string::npos);
    EXPECT_NE(md.find("## 函数"), std::string::npos);
    EXPECT_NE(md.find("foo"), std::string::npos);
}

TEST(DocCliSource, HtmlFormat) {
    std::string src = "/// 函数\nfun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Html);
    ASSERT_TRUE(r.ok);
    DocGenerator gen(DocFormat::Html);
    std::string html = gen.formatOutput(r.doc);
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("foo"), std::string::npos);
}

TEST(DocCliSource, JsonFormat) {
    std::string src = "/// 函数\nfun foo() { return 1; }\n";
    auto r = minilang_doc::generateDoc(src, DocFormat::Json);
    ASSERT_TRUE(r.ok);
    DocGenerator gen(DocFormat::Json);
    std::string json = gen.formatOutput(r.doc);
    EXPECT_NE(json.find("\"entries\""), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"foo\""), std::string::npos);
}

// ============================================================
// DocCliProcessFile: processFile 函数测试
// ============================================================

TEST(DocCliProcessFile, CleanFile) {
    std::string src = "/// 函数\nfun foo() { return 1; }\n";
    std::string path = makeTempFile(src);
    auto r = minilang_doc::processFile(path, DocFormat::Markdown);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.entryCount, 1);
    EXPECT_NE(r.output.find("foo"), std::string::npos);
    removeTempFile(path);
}

TEST(DocCliProcessFile, NonExistentFile) {
    auto r = minilang_doc::processFile("nonexistent_file_xyz.ml", DocFormat::Markdown);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "io");
}

TEST(DocCliProcessFile, ParseErrorFile) {
    std::string src = "var x = 1\n"; // 缺少分号
    std::string path = makeTempFile(src);
    auto r = minilang_doc::processFile(path, DocFormat::Markdown);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorPhase, "parse");
    removeTempFile(path);
}

TEST(DocCliProcessFile, JsonOutput) {
    std::string src = "/// 函数\nfun foo() { return 1; }\n";
    std::string path = makeTempFile(src);
    auto r = minilang_doc::processFile(path, DocFormat::Json);
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.output.find("\"entries\""), std::string::npos);
    removeTempFile(path);
}

// ============================================================
// DocCliParseArgs: parseArgs 函数测试
// ============================================================

TEST(DocCliParseArgs, NoArgs) {
    char* argv[] = {(char*)"minilang-doc"};
    auto args = minilang_doc::parseArgs(1, argv);
    EXPECT_FALSE(args.parseError);
    EXPECT_TRUE(args.files.empty());
    EXPECT_EQ(args.format, DocFormat::Markdown);
    EXPECT_TRUE(args.outputFile.empty());
}

TEST(DocCliParseArgs, HelpArg) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--help"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(DocCliParseArgs, HelpShortArg) {
    char* argv[] = {(char*)"minilang-doc", (char*)"-h"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(DocCliParseArgs, VersionArg) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--version"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(DocCliParseArgs, VersionShortArg) {
    char* argv[] = {(char*)"minilang-doc", (char*)"-V"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(DocCliParseArgs, FormatMarkdown) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--format", (char*)"markdown", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.format, DocFormat::Markdown);
}

TEST(DocCliParseArgs, FormatMarkdownShort) {
    char* argv[] = {(char*)"minilang-doc", (char*)"-f", (char*)"md", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.format, DocFormat::Markdown);
}

TEST(DocCliParseArgs, FormatHtml) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--format", (char*)"html", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.format, DocFormat::Html);
}

TEST(DocCliParseArgs, FormatJson) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--format", (char*)"json", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.format, DocFormat::Json);
}

TEST(DocCliParseArgs, FormatInvalid) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--format", (char*)"xml", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("xml"), std::string::npos);
}

TEST(DocCliParseArgs, FormatMissingValue) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--format"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--format"), std::string::npos);
}

TEST(DocCliParseArgs, OutputFile) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--output", (char*)"api.md", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.outputFile, "api.md");
}

TEST(DocCliParseArgs, OutputShortArg) {
    char* argv[] = {(char*)"minilang-doc", (char*)"-o", (char*)"api.md", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.outputFile, "api.md");
}

TEST(DocCliParseArgs, OutputMissingValue) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--output"};
    auto args = minilang_doc::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--output"), std::string::npos);
}

TEST(DocCliParseArgs, UnknownOption) {
    char* argv[] = {(char*)"minilang-doc", (char*)"--unknown", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(3, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--unknown"), std::string::npos);
}

TEST(DocCliParseArgs, MultipleFiles) {
    char* argv[] = {(char*)"minilang-doc", (char*)"foo.ml", (char*)"bar.ml", (char*)"baz.ml"};
    auto args = minilang_doc::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    ASSERT_EQ(args.files.size(), 3u);
    EXPECT_EQ(args.files[0], "foo.ml");
    EXPECT_EQ(args.files[1], "bar.ml");
    EXPECT_EQ(args.files[2], "baz.ml");
}

TEST(DocCliParseArgs, CombinedOptions) {
    char* argv[] = {(char*)"minilang-doc", (char*)"-f", (char*)"html", (char*)"-o", (char*)"out.html", (char*)"foo.ml"};
    auto args = minilang_doc::parseArgs(6, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.format, DocFormat::Html);
    EXPECT_EQ(args.outputFile, "out.html");
    ASSERT_EQ(args.files.size(), 1u);
    EXPECT_EQ(args.files[0], "foo.ml");
}

// ============================================================
// DocCliHelpers: 辅助函数测试
// ============================================================

TEST(DocCliHelpers, ParseFormatName) {
    EXPECT_EQ(minilang_doc::parseFormatName("markdown"), DocFormat::Markdown);
    EXPECT_EQ(minilang_doc::parseFormatName("md"), DocFormat::Markdown);
    EXPECT_EQ(minilang_doc::parseFormatName("html"), DocFormat::Html);
    EXPECT_EQ(minilang_doc::parseFormatName("json"), DocFormat::Json);
    // 无效名返回默认 Markdown
    EXPECT_EQ(minilang_doc::parseFormatName("xml"), DocFormat::Markdown);
    EXPECT_EQ(minilang_doc::parseFormatName(""), DocFormat::Markdown);
}

TEST(DocCliHelpers, VersionString) {
    std::string v = minilang_doc::versionString();
    EXPECT_NE(v.find("minilang-doc"), std::string::npos);
    EXPECT_NE(v.find("R162"), std::string::npos);
}
