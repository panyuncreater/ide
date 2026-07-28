// ============================================================
// tests/TestCoverageCli.cpp - R110 行级覆盖率 CLI 核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_coverage 命名空间下的可测试函数：
//   - analyzeSource: 源码 → FileCoverage
//   - analyzeFile:   文件路径 → FileCoverage
//   - analyzeFiles:  多文件 → CoverageReport
//   - formatFileText / formatReportText: 文本格式化
//   - formatFileLcov / formatReportLcov: LCOV 格式化
//   - parseArgs:     命令行参数解析
//   - versionString
//
// 不测试 main 函数（gtest 不易测试 main），main 的退出码逻辑
// 通过 parseArgs + analyzeFiles + formatReport 的组合测试间接验证。
// ============================================================
#include "cli/coverage_core.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
/// 创建临时文件并写入内容，返回路径（仿 TestFmtCli.cpp 的原子计数器模式）
std::string makeTempFile(const std::string& content, const std::string& suffix = ".ml") {
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1) + 1;
    auto path = fs::temp_directory_path() / ("minilang_coverage_test_" + std::to_string(id) + suffix);
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
// CoverageCliAnalyzeSource: analyzeSource 函数测试（StackVM 后端）
// ============================================================

TEST(CoverageCliAnalyzeSource, BasicPrintStackVM) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    opts.backend = minilang_coverage::Backend::StackVM;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_EQ(fc.sourceName, "<stdin>");
    EXPECT_EQ(fc.backendUsed, "StackVM");
    EXPECT_GE(fc.totalExecutable, 1);
    EXPECT_GE(fc.totalCovered, 1);
    EXPECT_LE(fc.totalCovered, fc.totalExecutable);
    EXPECT_GT(fc.ratio, 0.0);
    EXPECT_LE(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, EmptySource) {
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource("", "<stdin>", opts);
    // 空源码：词法/语法通过，但无可执行行
    EXPECT_TRUE(fc.ok);
    EXPECT_EQ(fc.totalExecutable, 0);
    EXPECT_EQ(fc.totalCovered, 0);
    EXPECT_DOUBLE_EQ(fc.ratio, 0.0);
}

TEST(CoverageCliAnalyzeSource, CommentOnlySource) {
    std::string src = "// just a comment\n// another comment\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    EXPECT_TRUE(fc.ok);
    EXPECT_EQ(fc.totalExecutable, 0);
}

TEST(CoverageCliAnalyzeSource, BranchNotTaken) {
    // 注意：MiniLang 编译器对 if (true/false) 常量条件做死代码消除（C17 fix），
    // 故必须用变量条件（编译期无法求值）才能保留 then 分支的字节码。
    // var x = 5; if (x > 10) 中 x > 10 在编译期为非常量，会保留 then 分支。
    std::string src = "var x = 5;\n"
                      "if (x > 10) {\n"
                      "    print(\"big\");\n"
                      "}\n"
                      "print(\"done\");\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    // 至少应有 1 个未覆盖行（print("big") 在 false 分支中）
    int uncoveredCount = 0;
    for (const auto& lc : fc.lines) {
        if (lc.executable && !lc.covered) {
            ++uncoveredCount;
        }
    }
    EXPECT_GE(uncoveredCount, 1);
    // 不应 100% 覆盖
    EXPECT_LT(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, AllLinesCovered) {
    std::string src = "var x = 5;\n"
                      "print(x);\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_EQ(fc.totalCovered, fc.totalExecutable);
    EXPECT_DOUBLE_EQ(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, ForLoopAllCovered) {
    std::string src = "var sum = 0;\n"
                      "for (var i = 0; i < 3; i = i + 1) {\n"
                      "    sum = sum + i;\n"
                      "}\n"
                      "print(sum);\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_DOUBLE_EQ(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, LexError) {
    // 未闭合的块注释触发词法错误
    std::string src = "var x = 5; /* unterminated";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    EXPECT_FALSE(fc.ok);
    EXPECT_EQ(fc.errorPhase, "lex");
}

TEST(CoverageCliAnalyzeSource, ParseError) {
    // 缺少标识符触发语法错误
    std::string src = "var = 5;";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    EXPECT_FALSE(fc.ok);
    EXPECT_EQ(fc.errorPhase, "parse");
}

TEST(CoverageCliAnalyzeSource, FunctionCallCovered) {
    std::string src = "fun add(a, b) {\n"
                      "    return a + b;\n"
                      "}\n"
                      "print(add(2, 3));\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    // 函数被调用，内部所有可执行行应被覆盖
    EXPECT_DOUBLE_EQ(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, FunctionDefinedButNotCalled) {
    std::string src = "fun unused() {\n"
                      "    print(\"not called\");\n"
                      "}\n"
                      "print(\"main\");\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    // 函数未调用，内部 print 行应未覆盖
    int uncoveredCount = 0;
    for (const auto& lc : fc.lines) {
        if (lc.executable && !lc.covered) {
            ++uncoveredCount;
        }
    }
    EXPECT_GE(uncoveredCount, 1);
    EXPECT_LT(fc.ratio, 100.0);
}

TEST(CoverageCliAnalyzeSource, LineCoverageConsistency) {
    // 所有 covered 行必须是 executable
    std::string src = "var x = 5;\nif (x > 10) {\n    print(\"big\");\n}\nprint(x);\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok);
    for (const auto& lc : fc.lines) {
        if (lc.covered) {
            EXPECT_TRUE(lc.executable) << "Line " << lc.line << " is covered but not executable";
        }
        if (lc.executable && lc.covered) {
            EXPECT_GT(lc.count, 0u) << "Line " << lc.line << " covered but count=0";
        }
    }
}

// ============================================================
// CoverageCliRegisterVM: RegisterVM 后端测试
// ============================================================

TEST(CoverageCliRegisterVM, BasicPrint) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    opts.backend = minilang_coverage::Backend::RegisterVM;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_EQ(fc.backendUsed, "RegisterVM");
    EXPECT_GE(fc.totalExecutable, 1);
    EXPECT_GE(fc.totalCovered, 1);
    EXPECT_GT(fc.ratio, 0.0);
}

TEST(CoverageCliRegisterVM, BranchNotTaken) {
    // 注意：MiniLang 编译器对 if (true/false) 常量条件做死代码消除（C17 fix），
    // 故必须用变量条件（编译期无法求值）才能保留 then 分支的字节码。
    std::string src = "var x = 5;\n"
                      "if (x > 10) {\n"
                      "    print(\"no\");\n"
                      "}\n"
                      "print(\"yes\");\n";
    minilang_coverage::CoverageOptions opts;
    opts.backend = minilang_coverage::Backend::RegisterVM;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    int uncoveredCount = 0;
    for (const auto& lc : fc.lines) {
        if (lc.executable && !lc.covered) {
            ++uncoveredCount;
        }
    }
    EXPECT_GE(uncoveredCount, 1);
}

TEST(CoverageCliRegisterVM, LexError) {
    std::string src = "var x = 5; /* unterminated";
    minilang_coverage::CoverageOptions opts;
    opts.backend = minilang_coverage::Backend::RegisterVM;
    auto fc = minilang_coverage::analyzeSource(src, "<stdin>", opts);
    EXPECT_FALSE(fc.ok);
    EXPECT_EQ(fc.errorPhase, "lex");
}

// ============================================================
// CoverageCliAnalyzeFile: analyzeFile 函数测试
// ============================================================

TEST(CoverageCliAnalyzeFile, ValidFile) {
    std::string path = makeTempFile("print(\"hello\");\n");
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeFile(path, opts);
    EXPECT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_NE(fc.sourceName, path); // 应是文件名而非全路径
    EXPECT_GE(fc.totalExecutable, 1);
    removeTempFile(path);
}

TEST(CoverageCliAnalyzeFile, NonExistentFile) {
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeFile("nonexistent_file_xyz.ml", opts);
    EXPECT_FALSE(fc.ok);
    EXPECT_EQ(fc.errorPhase, "io");
}

TEST(CoverageCliAnalyzeFile, LexErrorFile) {
    std::string path = makeTempFile("var = 5;");
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeFile(path, opts);
    EXPECT_FALSE(fc.ok);
    EXPECT_EQ(fc.errorPhase, "parse");
    removeTempFile(path);
}

// ============================================================
// CoverageCliAnalyzeFiles: analyzeFiles 多文件聚合测试
// ============================================================

TEST(CoverageCliAnalyzeFiles, MultipleFiles) {
    std::string path1 = makeTempFile("print(\"file1\");\n");
    std::string path2 = makeTempFile("var x = 5;\nprint(x);\n");
    minilang_coverage::CoverageOptions opts;
    auto report = minilang_coverage::analyzeFiles({path1, path2}, opts);
    EXPECT_TRUE(report.ok);
    EXPECT_EQ(report.files.size(), 2u);
    EXPECT_GE(report.totalExecutable, 2);
    EXPECT_GE(report.totalCovered, 2);
    EXPECT_DOUBLE_EQ(report.ratio, 100.0);
    removeTempFile(path1);
    removeTempFile(path2);
}

TEST(CoverageCliAnalyzeFiles, MixedSuccessFailure) {
    std::string path1 = makeTempFile("print(\"ok\");\n");
    std::string path2 = "nonexistent_xyz.ml";
    minilang_coverage::CoverageOptions opts;
    auto report = minilang_coverage::analyzeFiles({path1, path2}, opts);
    EXPECT_FALSE(report.ok); // 一个失败 → ok=false
    EXPECT_EQ(report.files.size(), 2u);
    EXPECT_TRUE(report.files[0].ok);
    EXPECT_FALSE(report.files[1].ok);
    removeTempFile(path1);
}

TEST(CoverageCliAnalyzeFiles, BothBackendProducesTwoEntries) {
    std::string path = makeTempFile("print(\"hello\");\n");
    minilang_coverage::CoverageOptions opts;
    opts.backend = minilang_coverage::Backend::Both;
    auto report = minilang_coverage::analyzeFiles({path}, opts);
    EXPECT_EQ(report.files.size(), 2u); // StackVM + RegisterVM
    EXPECT_TRUE(report.files[0].ok);
    EXPECT_TRUE(report.files[1].ok);
    removeTempFile(path);
}

// ============================================================
// CoverageCliFormatText: 文本格式化测试
// ============================================================

TEST(CoverageCliFormatText, BasicFormat) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_NE(text.find("test.ml"), std::string::npos);
    EXPECT_NE(text.find("StackVM"), std::string::npos);
    EXPECT_NE(text.find("%"), std::string::npos);
}

TEST(CoverageCliFormatText, ShowSource) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    opts.showSource = true;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_NE(text.find("源码"), std::string::npos);
    EXPECT_NE(text.find("print"), std::string::npos);
}

TEST(CoverageCliFormatText, ShowUncovered) {
    // 注意：MiniLang 编译器对 if (true/false) 常量条件做死代码消除（C17 fix），
    // 故必须用变量条件（编译期无法求值）才能保留 then 分支的字节码。
    std::string src = "var x = 5;\n"
                      "if (x > 10) {\n"
                      "    print(\"no\");\n"
                      "}\n"
                      "print(\"yes\");\n";
    minilang_coverage::CoverageOptions opts;
    opts.showUncovered = true;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_NE(text.find("未覆盖"), std::string::npos);
}

TEST(CoverageCliFormatText, NoUncoveredHidesList) {
    // 即使有未覆盖行，showUncovered=false 时不应在文本中出现"未覆盖"列表
    std::string src = "var x = 5;\n"
                      "if (x > 10) {\n"
                      "    print(\"no\");\n"
                      "}\n";
    minilang_coverage::CoverageOptions opts;
    opts.showUncovered = false;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_EQ(text.find("未覆盖"), std::string::npos);
}

TEST(CoverageCliFormatText, AllCoveredMessage) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    opts.showUncovered = true;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_NE(text.find("所有可执行行均已覆盖"), std::string::npos);
}

TEST(CoverageCliFormatText, ErrorReport) {
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource("var = 5;", "test.ml", opts);
    ASSERT_FALSE(fc.ok);
    std::string text = minilang_coverage::formatFileText(fc, opts);
    EXPECT_NE(text.find("错误"), std::string::npos);
    EXPECT_NE(text.find("parse"), std::string::npos);
}

TEST(CoverageCliFormatText, ReportSummary) {
    std::string path1 = makeTempFile("print(\"a\");\n");
    std::string path2 = makeTempFile("print(\"b\");\n");
    minilang_coverage::CoverageOptions opts;
    auto report = minilang_coverage::analyzeFiles({path1, path2}, opts);
    ASSERT_TRUE(report.ok);
    std::string text = minilang_coverage::formatReportText(report, opts);
    EXPECT_NE(text.find("汇总"), std::string::npos);
    EXPECT_NE(text.find("文件数"), std::string::npos);
    removeTempFile(path1);
    removeTempFile(path2);
}

// ============================================================
// CoverageCliFormatLcov: LCOV 格式化测试
// ============================================================

TEST(CoverageCliFormatLcov, BasicLcov) {
    std::string src = "print(\"hello\");";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    EXPECT_NE(lcov.find("SF:test.ml"), std::string::npos);
    EXPECT_NE(lcov.find("LF:"), std::string::npos);
    EXPECT_NE(lcov.find("LH:"), std::string::npos);
    EXPECT_NE(lcov.find("end_of_record"), std::string::npos);
    // DA 行格式：DA:<line>,<count>
    EXPECT_NE(lcov.find("DA:"), std::string::npos);
}

TEST(CoverageCliFormatLcov, LcovFormatCorrectness) {
    // 验证 LF == 可执行行数，LH == 已覆盖行数
    // 注意：MiniLang 编译器对 if (true/false) 常量条件做死代码消除（C17 fix），
    // 故必须用变量条件（编译期无法求值）才能保留 then 分支的字节码。
    std::string src = "var x = 5;\n"
                      "if (x > 10) {\n"
                      "    print(\"no\");\n"
                      "}\n"
                      "print(\"yes\");\n";
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource(src, "test.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    // 提取 LF 和 LH 值
    auto lfPos = lcov.find("LF:");
    auto lhPos = lcov.find("LH:");
    ASSERT_NE(lfPos, std::string::npos);
    ASSERT_NE(lhPos, std::string::npos);
    int lf = std::stoi(lcov.substr(lfPos + 3));
    int lh = std::stoi(lcov.substr(lhPos + 3));
    EXPECT_EQ(lf, fc.totalExecutable);
    EXPECT_EQ(lh, fc.totalCovered);
    EXPECT_LE(lh, lf);
}

TEST(CoverageCliFormatLcov, EmptySourceLcov) {
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource("", "empty.ml", opts);
    ASSERT_TRUE(fc.ok);
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    EXPECT_NE(lcov.find("SF:empty.ml"), std::string::npos);
    EXPECT_NE(lcov.find("LF:0"), std::string::npos);
    EXPECT_NE(lcov.find("LH:0"), std::string::npos);
}

TEST(CoverageCliFormatLcov, ErrorLcov) {
    minilang_coverage::CoverageOptions opts;
    auto fc = minilang_coverage::analyzeSource("var = 5;", "err.ml", opts);
    ASSERT_FALSE(fc.ok);
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    EXPECT_NE(lcov.find("ERROR"), std::string::npos);
}

TEST(CoverageCliFormatLcov, ReportLcovHasTestName) {
    std::string path = makeTempFile("print(\"hello\");\n");
    minilang_coverage::CoverageOptions opts;
    auto report = minilang_coverage::analyzeFiles({path}, opts);
    ASSERT_TRUE(report.ok);
    std::string lcov = minilang_coverage::formatReportLcov(report);
    EXPECT_NE(lcov.find("TN:minilang-coverage"), std::string::npos);
    removeTempFile(path);
}

// ============================================================
// CoverageCliParseArgs: 命令行参数解析测试
// ============================================================

TEST(CoverageCliParseArgs, NoArgs) {
    char* argv[] = {const_cast<char*>("minilang-coverage")};
    auto args = minilang_coverage::parseArgs(1, argv);
    EXPECT_FALSE(args.showHelp);
    EXPECT_FALSE(args.showVersion);
    EXPECT_FALSE(args.parseError);
    EXPECT_TRUE(args.files.empty());
    // 默认值
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::StackVM);
    EXPECT_EQ(args.options.format, minilang_coverage::OutputFormat::Text);
}

TEST(CoverageCliParseArgs, HelpArg) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--help")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(CoverageCliParseArgs, HelpShortArg) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("-h")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(CoverageCliParseArgs, VersionArg) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--version")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(CoverageCliParseArgs, VersionShortArg) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("-V")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(CoverageCliParseArgs, BackendStack) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--backend"), const_cast<char*>("stack"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::StackVM);
    EXPECT_EQ(args.files.size(), 1u);
}

TEST(CoverageCliParseArgs, BackendRegister) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--backend"),
                    const_cast<char*>("register"), const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::RegisterVM);
}

TEST(CoverageCliParseArgs, BackendBoth) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--backend"), const_cast<char*>("both"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::Both);
}

TEST(CoverageCliParseArgs, BackendInvalid) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--backend"),
                    const_cast<char*>("invalid"), const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("未知后端"), std::string::npos);
}

TEST(CoverageCliParseArgs, BackendMissingValue) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--backend")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("--backend 需要一个参数"), std::string::npos);
}

TEST(CoverageCliParseArgs, FormatText) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--format"), const_cast<char*>("text"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.format, minilang_coverage::OutputFormat::Text);
}

TEST(CoverageCliParseArgs, FormatLcov) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--format"), const_cast<char*>("lcov"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.format, minilang_coverage::OutputFormat::Lcov);
}

TEST(CoverageCliParseArgs, FormatInvalid) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--format"), const_cast<char*>("json"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("未知格式"), std::string::npos);
}

TEST(CoverageCliParseArgs, FormatMissingValue) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--format")};
    auto args = minilang_coverage::parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
}

TEST(CoverageCliParseArgs, ShowCountsFlag) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--show-counts"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(3, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_TRUE(args.options.showCounts);
}

TEST(CoverageCliParseArgs, ShowSourceFlag) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--show-source"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(3, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_TRUE(args.options.showSource);
}

TEST(CoverageCliParseArgs, NoUncoveredFlag) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--no-uncovered"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(3, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_FALSE(args.options.showUncovered);
}

TEST(CoverageCliParseArgs, FailUnderValid) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--fail-under"), const_cast<char*>("80"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_DOUBLE_EQ(args.options.failUnder, 80.0);
}

TEST(CoverageCliParseArgs, FailUnderZero) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--fail-under"), const_cast<char*>("0"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_DOUBLE_EQ(args.options.failUnder, 0.0);
}

TEST(CoverageCliParseArgs, FailUnderInvalid) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--fail-under"),
                    const_cast<char*>("not-a-number"), const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
}

TEST(CoverageCliParseArgs, FailUnderOutOfRange) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--fail-under"), const_cast<char*>("150"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
}

TEST(CoverageCliParseArgs, FailUnderNegative) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--fail-under"), const_cast<char*>("-10"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    EXPECT_TRUE(args.parseError);
}

TEST(CoverageCliParseArgs, MultipleFiles) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("foo.ml"), const_cast<char*>("bar.ml"),
                    const_cast<char*>("baz.ml")};
    auto args = minilang_coverage::parseArgs(4, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.files.size(), 3u);
    EXPECT_EQ(args.files[0], "foo.ml");
    EXPECT_EQ(args.files[1], "bar.ml");
    EXPECT_EQ(args.files[2], "baz.ml");
}

TEST(CoverageCliParseArgs, UnknownOption) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("--unknown-option"),
                    const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(3, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("未知选项"), std::string::npos);
}

TEST(CoverageCliParseArgs, MixedOptionsAndFiles) {
    char* argv[] = {const_cast<char*>("minilang-coverage"),
                    const_cast<char*>("--backend"),
                    const_cast<char*>("register"),
                    const_cast<char*>("foo.ml"),
                    const_cast<char*>("--format"),
                    const_cast<char*>("lcov"),
                    const_cast<char*>("bar.ml")};
    auto args = minilang_coverage::parseArgs(7, argv);
    ASSERT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::RegisterVM);
    EXPECT_EQ(args.options.format, minilang_coverage::OutputFormat::Lcov);
    EXPECT_EQ(args.files.size(), 2u);
    EXPECT_EQ(args.files[0], "foo.ml");
    EXPECT_EQ(args.files[1], "bar.ml");
}

TEST(CoverageCliParseArgs, DefaultValues) {
    char* argv[] = {const_cast<char*>("minilang-coverage"), const_cast<char*>("foo.ml")};
    auto args = minilang_coverage::parseArgs(2, argv);
    ASSERT_FALSE(args.parseError);
    EXPECT_EQ(args.options.backend, minilang_coverage::Backend::StackVM);
    EXPECT_EQ(args.options.format, minilang_coverage::OutputFormat::Text);
    EXPECT_TRUE(args.options.showUncovered);
    EXPECT_FALSE(args.options.showCounts);
    EXPECT_FALSE(args.options.showSource);
    EXPECT_DOUBLE_EQ(args.options.failUnder, -1.0);
}

// ============================================================
// CoverageCliVersion: 版本字符串测试
// ============================================================

TEST(CoverageCliVersion, VersionString) {
    std::string v = minilang_coverage::versionString();
    EXPECT_NE(v.find("minilang-coverage"), std::string::npos);
    EXPECT_NE(v.find("1.0.0"), std::string::npos);
}

// ============================================================
// 拓展二期：分支覆盖率（--branch）
// ============================================================

namespace {
/// 分支测试共用源码：if/else 两向 + 仅真向的 if
const char* kBranchSource = "var i = 0;\n"
                            "while (i < 4) {\n"        // 分支：真 4 次 + 假 1 次（退出）
                            "  if (i < 2) {\n"          // 分支：真 2 次 + 假 2 次
                            "    print(\"lo\");\n"
                            "  } else {\n"
                            "    print(\"hi\");\n"
                            "  }\n"
                            "  i = i + 1;\n"
                            "}\n";
} // namespace

TEST(CoverageBranch, StackVmBothDirectionsCovered) {
    minilang_coverage::CoverageOptions opts;
    opts.branch = true;
    auto fc = minilang_coverage::analyzeSource(kBranchSource, "<test>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    ASSERT_FALSE(fc.branches.empty());
    EXPECT_EQ(fc.totalBranchOutcomes, static_cast<int>(fc.branches.size()) * 2);
    // 两个条件（while + if）均双向覆盖 → 全部 outcome 命中
    EXPECT_EQ(fc.coveredBranchOutcomes, fc.totalBranchOutcomes);
    // 验证计数语义：存在真/假均 >0 的分支
    bool bothSeen = false;
    for (const auto& bc : fc.branches) {
        if (bc.trueCount > 0 && bc.falseCount > 0)
            bothSeen = true;
    }
    EXPECT_TRUE(bothSeen);
}

TEST(CoverageBranch, OneSidedBranchReportedUncovered) {
    minilang_coverage::CoverageOptions opts;
    opts.branch = true;
    // 条件恒真：假分支永不执行 → 至少一个 outcome 未覆盖
    auto fc = minilang_coverage::analyzeSource("var x = 1;\nif (x < 10) {\n  print(x);\n}\n", "<test>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    ASSERT_FALSE(fc.branches.empty());
    EXPECT_LT(fc.coveredBranchOutcomes, fc.totalBranchOutcomes);
    // 找到该分支：真>0，假==0
    bool found = false;
    for (const auto& bc : fc.branches) {
        if (bc.trueCount > 0 && bc.falseCount == 0)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(CoverageBranch, RegisterVmMatchesStackVm) {
    minilang_coverage::CoverageOptions stackOpts;
    stackOpts.branch = true;
    stackOpts.backend = minilang_coverage::Backend::StackVM;
    auto stackFc = minilang_coverage::analyzeSource(kBranchSource, "<test>", stackOpts);
    ASSERT_TRUE(stackFc.ok) << stackFc.errorMessage;

    minilang_coverage::CoverageOptions regOpts;
    regOpts.branch = true;
    regOpts.backend = minilang_coverage::Backend::RegisterVM;
    auto regFc = minilang_coverage::analyzeSource(kBranchSource, "<test>", regOpts);
    ASSERT_TRUE(regFc.ok) << regFc.errorMessage;

    // 三后端一致性：两后端分支覆盖结论一致（全覆盖）
    EXPECT_EQ(stackFc.coveredBranchOutcomes, stackFc.totalBranchOutcomes);
    EXPECT_EQ(regFc.coveredBranchOutcomes, regFc.totalBranchOutcomes);
    EXPECT_FALSE(regFc.branches.empty());
}

TEST(CoverageBranch, LcovContainsBrdaRecords) {
    minilang_coverage::CoverageOptions opts;
    opts.branch = true;
    auto fc = minilang_coverage::analyzeSource(kBranchSource, "test.ml", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    EXPECT_NE(lcov.find("BRDA:"), std::string::npos);
    EXPECT_NE(lcov.find("BRF:"), std::string::npos);
    EXPECT_NE(lcov.find("BRH:"), std::string::npos);
}

TEST(CoverageBranch, DisabledByDefaultNoBranchData) {
    minilang_coverage::CoverageOptions opts; // branch 默认 false
    auto fc = minilang_coverage::analyzeSource(kBranchSource, "<test>", opts);
    ASSERT_TRUE(fc.ok) << fc.errorMessage;
    EXPECT_TRUE(fc.branches.empty());
    EXPECT_EQ(fc.totalBranchOutcomes, 0);
    std::string lcov = minilang_coverage::formatFileLcov(fc);
    EXPECT_EQ(lcov.find("BRDA:"), std::string::npos);
}

TEST(CoverageBranch, ParseArgsRecognizesBranchFlag) {
    const char* argv[] = {"minilang-coverage", "--branch", "a.ml"};
    auto args = minilang_coverage::parseArgs(3, const_cast<char**>(argv));
    EXPECT_TRUE(args.options.branch);
}
