// ============================================================
// tests/TestFuzzCli.cpp - R164 模糊测试 CLI 核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_fuzz 命名空间下的可测试函数：
//   - fuzzSource: 单源码单后端执行（Interpreter/StackVM/RegisterVM）
//   - fuzzThreeAgree: 三后端差分（合法程序应一致）
//   - runFuzzBatch: 批量生成 + 执行（固定种子 + 小迭代数）
//   - processFile: 文件输入模式
//   - parseArgs: 命令行参数解析
//   - parseBackendName/parseModeName/parseCategoryName/versionString
//     /formatSummaryText/formatSummaryJson
//
// 不测试 main 函数（gtest 不易测试），main 的退出码逻辑
// 通过 parseArgs + runFuzzBatch/processFile 的组合测试间接验证。
// ============================================================
#include "cli/fuzz_core.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace minilang_fuzz;

namespace {
/// 创建临时文件并写入内容，返回路径
std::string makeTempFile(const std::string& content, const std::string& suffix = ".ml") {
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1) + 1;
    auto path = fs::temp_directory_path() / ("minilang_fuzz_test_" + std::to_string(id) + suffix);
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
// FuzzCliSource: fuzzSource 单源码单后端执行测试
// ============================================================

TEST(FuzzCliSource, SimplePrint_Interpreter) {
    FuzzResult r = fuzzSource("print(1 + 2);\n", FuzzBackend::Interpreter);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.crashDetected);
    EXPECT_FALSE(r.disagreement);
    EXPECT_EQ(r.interpOutput, "3");
}

TEST(FuzzCliSource, SimplePrint_StackVM) {
    FuzzResult r = fuzzSource("print(1 + 2);\n", FuzzBackend::StackVM);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.crashDetected);
    EXPECT_EQ(r.stackvmOutput, "3");
}

TEST(FuzzCliSource, SimplePrint_RegisterVM) {
    FuzzResult r = fuzzSource("print(1 + 2);\n", FuzzBackend::RegisterVM);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.crashDetected);
    EXPECT_EQ(r.regvmOutput, "3");
}

TEST(FuzzCliSource, DivisionByZero_RuntimeError) {
    FuzzResult r = fuzzSource("var z = 0;\nprint(10 / z);\n", FuzzBackend::Interpreter);
    // 运行时错误不是崩溃
    EXPECT_FALSE(r.crashDetected);
    EXPECT_FALSE(r.disagreement);
    EXPECT_EQ(r.errorPhase, "runtime");
    EXPECT_NE(r.interpOutput.find("<runtime:"), std::string::npos);
}

TEST(FuzzCliSource, ParseFailure) {
    FuzzResult r = fuzzSource("var @#$;\n", FuzzBackend::Interpreter);
    EXPECT_FALSE(r.crashDetected);
    EXPECT_EQ(r.errorPhase, "parse");
    EXPECT_EQ(r.interpOutput, "<parse-fail>");
}

TEST(FuzzCliSource, UndefinedVariable_RuntimeError) {
    FuzzResult r = fuzzSource("print(undefined_var);\n", FuzzBackend::Interpreter);
    EXPECT_FALSE(r.crashDetected);
    EXPECT_EQ(r.errorPhase, "runtime");
}

TEST(FuzzCliSource, Recursion_Interpreter) {
    FuzzResult r = fuzzSource("fun fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }\nprint(fib(10));\n",
                              FuzzBackend::Interpreter);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "55");
}

// ============================================================
// FuzzCliThreeAgree: 三后端差分测试
// ============================================================

TEST(FuzzCliThreeAgree, SimpleArithmetic) {
    FuzzResult r = fuzzThreeAgree("print(1 + 2 * 3);\n");
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.disagreement);
    EXPECT_EQ(r.interpOutput, r.stackvmOutput);
}

TEST(FuzzCliThreeAgree, FloatArithmetic) {
    FuzzResult r = fuzzThreeAgree("print(1.5 + 2.5);\n");
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "4");
}

TEST(FuzzCliThreeAgree, ControlFlow) {
    std::string src = "var i = 0;\nvar s = 0;\nwhile (i < 5) { s = s + i; i = i + 1; }\nprint(s);\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "10");
}

TEST(FuzzCliThreeAgree, FunctionCall) {
    std::string src = "fun add(a, b) { return a + b; }\nprint(add(3, 4));\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "7");
}

TEST(FuzzCliThreeAgree, RuntimeErrorAgreement) {
    // 三后端应产生相同的运行时错误消息
    std::string src = "var z = 0;\nif (z == 0) { print(10 / z); } else { print(z); }\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_FALSE(r.crashDetected);
    EXPECT_FALSE(r.disagreement) << "Interpreter=" << r.interpOutput << " StackVM=" << r.stackvmOutput
                                 << " RegVM=" << r.regvmOutput;
}

TEST(FuzzCliThreeAgree, StringConcat) {
    std::string src = "var s = \"a\" + \"b\" + \"c\";\nprint(s);\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "abc");
}

TEST(FuzzCliThreeAgree, ArrayOperations) {
    // MiniLang print 不输出换行符（三后端设计约定，见 Interpreter::visitPrintStmt），
    // 两次 print 的输出直接拼接为 "32"。
    std::string src = "var arr = [1, 2, 3];\nprint(arr.len());\nprint(arr[1]);\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_FALSE(r.disagreement) << "Interpreter=" << r.interpOutput << " StackVM=" << r.stackvmOutput
                                 << " RegVM=" << r.regvmOutput;
    EXPECT_EQ(r.interpOutput, "32");
}

TEST(FuzzCliThreeAgree, ClassMethod) {
    std::string src = "class Point { var x; var y; fun init(x, y) { this.x = x; this.y = y; } fun sum() { return "
                      "this.x + this.y; } }\nvar p = Point(3, 4);\nprint(p.sum());\n";
    FuzzResult r = fuzzThreeAgree(src);
    EXPECT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.interpOutput, "7");
}

// ============================================================
// FuzzCliRunBatch: 批量生成 + 执行测试
// ============================================================

TEST(FuzzCliRunBatch, GenerateMode_NoCrashes) {
    FuzzOptions opts;
    opts.seed = 42;
    opts.iterations = 20;
    opts.mode = FuzzMode::Generate;
    opts.quiet = true;
    opts.dumpCrashes = false;
    FuzzSummary s = runFuzzBatch(opts);
    EXPECT_EQ(s.totalRuns, 20);
    EXPECT_EQ(s.crashes, 0) << "生成模式不应有崩溃";
    // 三后端差分可能有 RegisterVM 编译失败（已知 IR lowering 限制），但不应有分歧
    EXPECT_EQ(s.disagreements, 0) << "生成模式不应有三后端分歧";
}

TEST(FuzzCliRunBatch, GenerateMode_SpecificCategory) {
    FuzzOptions opts;
    opts.seed = 100;
    opts.iterations = 10;
    opts.mode = FuzzMode::Generate;
    opts.categoryMask = static_cast<uint32_t>(FuzzCategory::Arithmetic);
    opts.quiet = true;
    opts.dumpCrashes = false;
    FuzzSummary s = runFuzzBatch(opts);
    EXPECT_EQ(s.totalRuns, 10);
    EXPECT_EQ(s.crashes, 0);
}

TEST(FuzzCliRunBatch, MutateMode_NoCrashes) {
    FuzzOptions opts;
    opts.seed = 200;
    opts.iterations = 15;
    opts.mode = FuzzMode::Mutate;
    opts.quiet = true;
    opts.dumpCrashes = false;
    FuzzSummary s = runFuzzBatch(opts);
    EXPECT_EQ(s.totalRuns, 15);
    // 变异模式可能产生大量 parse-fail，但不应崩溃
    EXPECT_EQ(s.crashes, 0) << "变异模式不应有崩溃（仅 parse 失败）";
}

TEST(FuzzCliRunBatch, SeedReproducibility) {
    FuzzOptions opts1;
    opts1.seed = 999;
    opts1.iterations = 5;
    opts1.quiet = true;
    opts1.dumpCrashes = false;
    FuzzSummary s1 = runFuzzBatch(opts1);

    FuzzOptions opts2;
    opts2.seed = 999;
    opts2.iterations = 5;
    opts2.quiet = true;
    opts2.dumpCrashes = false;
    FuzzSummary s2 = runFuzzBatch(opts2);

    EXPECT_EQ(s1.totalRuns, s2.totalRuns);
    EXPECT_EQ(s1.crashes, s2.crashes);
    EXPECT_EQ(s1.disagreements, s2.disagreements);
}

TEST(FuzzCliRunBatch, SummarySeedSet) {
    FuzzOptions opts;
    opts.seed = 12345;
    opts.seedSpecified = true;  // Bug #67 fix: 必须显式标记种子已指定
    opts.iterations = 3;
    opts.quiet = true;
    opts.dumpCrashes = false;
    FuzzSummary s = runFuzzBatch(opts);
    EXPECT_EQ(s.seed, 12345u);
}

// ============================================================
// FuzzCliProcessFile: 文件输入模式测试
// ============================================================

TEST(FuzzCliProcessFile, ValidFile) {
    std::string path = makeTempFile("print(1 + 2);\n");
    FuzzOptions opts;
    opts.dumpCrashes = false;
    FuzzSummary s = processFile(path, opts);
    EXPECT_EQ(s.totalRuns, 1);
    EXPECT_EQ(s.crashes, 0);
    EXPECT_EQ(s.disagreements, 0);
    EXPECT_EQ(s.agreements, 1);
    removeTempFile(path);
}

TEST(FuzzCliProcessFile, NonExistentFile) {
    FuzzOptions opts;
    FuzzSummary s = processFile("nonexistent_file_xyz.ml", opts);
    // BUG-93 fix: 文件错误改用 ok/errorMessage 字段（替代 crashes=-1 哨兵值）
    EXPECT_FALSE(s.ok);
    EXPECT_FALSE(s.errorMessage.empty());
    EXPECT_EQ(s.crashes, 0); // crashes 不再被置为负数
}

TEST(FuzzCliProcessFile, RuntimeErrorFile) {
    std::string path = makeTempFile("var z = 0;\nprint(10 / z);\n");
    FuzzOptions opts;
    opts.dumpCrashes = false;
    FuzzSummary s = processFile(path, opts);
    EXPECT_EQ(s.totalRuns, 1);
    EXPECT_EQ(s.crashes, 0);
    EXPECT_EQ(s.disagreements, 0);
    EXPECT_EQ(s.runtimeErrors, 1);
    removeTempFile(path);
}

// ============================================================
// FuzzCliParseArgs: 参数解析测试
// ============================================================

TEST(FuzzCliParseArgs, NoArgs) {
    CliArgs args = parseArgs(1, const_cast<char**>(std::array<const char*, 1>{"minilang-fuzz"}.data()));
    EXPECT_FALSE(args.parseError);
    EXPECT_FALSE(args.showHelp);
    EXPECT_EQ(args.options.mode, FuzzMode::Generate);
    EXPECT_EQ(args.options.iterations, 100);
}

TEST(FuzzCliParseArgs, HelpFlag) {
    const char* argv[] = {"minilang-fuzz", "--help"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_TRUE(args.showHelp);
}

TEST(FuzzCliParseArgs, VersionFlag) {
    const char* argv[] = {"minilang-fuzz", "--version"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_TRUE(args.showVersion);
}

TEST(FuzzCliParseArgs, SeedOption) {
    const char* argv[] = {"minilang-fuzz", "--seed", "42"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.seed, 42u);
}

TEST(FuzzCliParseArgs, IterationsOption) {
    const char* argv[] = {"minilang-fuzz", "--iterations", "500"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.iterations, 500);
}

TEST(FuzzCliParseArgs, ShortIterationsOption) {
    const char* argv[] = {"minilang-fuzz", "-n", "50"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.iterations, 50);
}

TEST(FuzzCliParseArgs, ModeOption) {
    const char* argv[] = {"minilang-fuzz", "--mode", "mutate"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.mode, FuzzMode::Mutate);
}

TEST(FuzzCliParseArgs, BackendOption) {
    const char* argv[] = {"minilang-fuzz", "--backend", "interp"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.backend, FuzzBackend::Interpreter);
}

TEST(FuzzCliParseArgs, CategoryOption) {
    const char* argv[] = {"minilang-fuzz", "--category", "recursion"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.categoryMask, static_cast<uint32_t>(FuzzCategory::Recursion));
}

TEST(FuzzCliParseArgs, FormatOption) {
    const char* argv[] = {"minilang-fuzz", "--format", "json"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.format, OutputFormat::Json);
}

TEST(FuzzCliParseArgs, QuietFlag) {
    const char* argv[] = {"minilang-fuzz", "--quiet"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_TRUE(args.options.quiet);
}

TEST(FuzzCliParseArgs, NoDumpFlag) {
    const char* argv[] = {"minilang-fuzz", "--no-dump"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_FALSE(args.options.dumpCrashes);
}

TEST(FuzzCliParseArgs, FileArgument_SetsFileMode) {
    const char* argv[] = {"minilang-fuzz", "test.ml"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.mode, FuzzMode::File);
    EXPECT_EQ(args.files.size(), 1u);
    EXPECT_EQ(args.files[0], "test.ml");
}

TEST(FuzzCliParseArgs, MultipleFiles) {
    const char* argv[] = {"minilang-fuzz", "a.ml", "b.ml", "c.ml"};
    CliArgs args = parseArgs(4, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError);
    EXPECT_EQ(args.options.mode, FuzzMode::File);
    EXPECT_EQ(args.files.size(), 3u);
}

TEST(FuzzCliParseArgs, UnknownOption_Error) {
    const char* argv[] = {"minilang-fuzz", "--unknown-option"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("未知的选项"), std::string::npos);
}

TEST(FuzzCliParseArgs, MissingSeedValue_Error) {
    const char* argv[] = {"minilang-fuzz", "--seed"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("缺少参数"), std::string::npos);
}

TEST(FuzzCliParseArgs, InvalidSeedValue_Error) {
    const char* argv[] = {"minilang-fuzz", "--seed", "not-a-number"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("无效的种子值"), std::string::npos);
}

TEST(FuzzCliParseArgs, InvalidIterationsValue_Error) {
    const char* argv[] = {"minilang-fuzz", "--iterations", "0"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_TRUE(args.parseError);
}

TEST(FuzzCliParseArgs, CombinedOptions) {
    const char* argv[] = {"minilang-fuzz", "--seed",   "1",       "--iterations", "10",
                          "--mode",        "generate", "--quiet", "--no-dump"};
    CliArgs args = parseArgs(9, const_cast<char**>(argv));
    EXPECT_FALSE(args.parseError) << args.errorMessage;
    EXPECT_EQ(args.options.seed, 1u);
    EXPECT_EQ(args.options.iterations, 10);
    EXPECT_EQ(args.options.mode, FuzzMode::Generate);
    EXPECT_TRUE(args.options.quiet);
    EXPECT_FALSE(args.options.dumpCrashes);
}

// ============================================================
// FuzzCliHelpers: 辅助函数测试
// ============================================================

TEST(FuzzCliHelpers, VersionString) {
    std::string v = versionString();
    EXPECT_NE(v.find("minilang-fuzz"), std::string::npos);
    EXPECT_NE(v.find("1.0.0"), std::string::npos);
}

TEST(FuzzCliHelpers, ParseBackendName_Valid) {
    EXPECT_EQ(parseBackendName("interp"), FuzzBackend::Interpreter);
    EXPECT_EQ(parseBackendName("stackvm"), FuzzBackend::StackVM);
    EXPECT_EQ(parseBackendName("regvm"), FuzzBackend::RegisterVM);
    EXPECT_EQ(parseBackendName("all"), FuzzBackend::All);
}

TEST(FuzzCliHelpers, ParseBackendName_Invalid) {
    EXPECT_EQ(parseBackendName("invalid"), FuzzBackend::All);
    EXPECT_EQ(parseBackendName(""), FuzzBackend::All);
}

TEST(FuzzCliHelpers, ParseModeName_Valid) {
    EXPECT_EQ(parseModeName("generate"), FuzzMode::Generate);
    EXPECT_EQ(parseModeName("mutate"), FuzzMode::Mutate);
    EXPECT_EQ(parseModeName("file"), FuzzMode::File);
}

TEST(FuzzCliHelpers, ParseModeName_Invalid) {
    EXPECT_EQ(parseModeName("invalid"), FuzzMode::Generate);
}

TEST(FuzzCliHelpers, ParseCategoryName_Valid) {
    EXPECT_EQ(parseCategoryName("arithmetic"), static_cast<uint32_t>(FuzzCategory::Arithmetic));
    EXPECT_EQ(parseCategoryName("controlflow"), static_cast<uint32_t>(FuzzCategory::ControlFlow));
    EXPECT_EQ(parseCategoryName("function"), static_cast<uint32_t>(FuzzCategory::Function));
    EXPECT_EQ(parseCategoryName("recursion"), static_cast<uint32_t>(FuzzCategory::Recursion));
    EXPECT_EQ(parseCategoryName("closure"), static_cast<uint32_t>(FuzzCategory::Closure));
    EXPECT_EQ(parseCategoryName("string"), static_cast<uint32_t>(FuzzCategory::String));
    EXPECT_EQ(parseCategoryName("array"), static_cast<uint32_t>(FuzzCategory::Array));
    EXPECT_EQ(parseCategoryName("class"), static_cast<uint32_t>(FuzzCategory::Class));
    EXPECT_EQ(parseCategoryName("errorpath"), static_cast<uint32_t>(FuzzCategory::ErrorPath));
    EXPECT_EQ(parseCategoryName("logic"), static_cast<uint32_t>(FuzzCategory::Logic));
    EXPECT_EQ(parseCategoryName("all"), static_cast<uint32_t>(FuzzCategory::All));
}

TEST(FuzzCliHelpers, ParseCategoryName_Invalid) {
    EXPECT_EQ(parseCategoryName("invalid"), 0u);
    EXPECT_EQ(parseCategoryName(""), 0u);
}

TEST(FuzzCliHelpers, FormatSummaryText_Basic) {
    FuzzSummary s;
    s.totalRuns = 10;
    s.agreements = 8;
    s.parseFailures = 1;
    s.runtimeErrors = 1;
    s.crashes = 0;
    s.disagreements = 0;
    s.seed = 42;
    std::string text = formatSummaryText(s);
    EXPECT_NE(text.find("总执行: 10"), std::string::npos);
    EXPECT_NE(text.find("三后端一致: 8"), std::string::npos);
    EXPECT_NE(text.find("无崩溃"), std::string::npos);
}

TEST(FuzzCliHelpers, FormatSummaryText_WithCrashes) {
    FuzzSummary s;
    s.totalRuns = 5;
    s.crashes = 1;
    s.seed = 1;
    FuzzResult r;
    r.source = "print(1);\n";
    r.errorPhase = "runtime";
    r.errorMessage = "test crash";
    s.crashCases.push_back(r);
    std::string text = formatSummaryText(s);
    EXPECT_NE(text.find("崩溃: 1"), std::string::npos);
    EXPECT_NE(text.find("print(1);"), std::string::npos);
}

TEST(FuzzCliHelpers, FormatSummaryJson_Basic) {
    FuzzSummary s;
    s.totalRuns = 5;
    s.agreements = 5;
    s.seed = 42;
    std::string json = formatSummaryJson(s);
    EXPECT_NE(json.find("\"totalRuns\": 5"), std::string::npos);
    EXPECT_NE(json.find("\"agreements\": 5"), std::string::npos);
    EXPECT_NE(json.find("\"crashes\": 0"), std::string::npos);
}

TEST(FuzzCliHelpers, FormatSummaryJson_WithCrashes) {
    FuzzSummary s;
    s.totalRuns = 1;
    s.crashes = 1;
    s.seed = 1;
    FuzzResult r;
    r.source = "print(1);\n";
    r.errorPhase = "runtime";
    s.crashCases.push_back(r);
    std::string json = formatSummaryJson(s);
    EXPECT_NE(json.find("\"crashes\": 1"), std::string::npos);
    EXPECT_NE(json.find("print(1);"), std::string::npos);
}
