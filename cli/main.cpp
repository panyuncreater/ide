// ============================================================
// cli/main.cpp - minilang 统一命令行入口
// ------------------------------------------------------------
// 将所有 CLI 工具合并为单一 minilang 可执行文件，通过子命令分发：
//   minilang fmt [args...]       代码格式化
//   minilang lint [args...]      静态分析
//   minilang lsp [args...]       语言服务器
//   minilang dap [args...]       调试适配器
//   minilang fuzz [args...]      模糊测试
//   minilang pkg [args...]       包管理器
//   minilang compile [args...]   预编译模块
//   minilang coverage [args...]  覆盖率分析
//   minilang doc [args...]       文档生成
//
// 原独立可执行文件（minilang_fmt/lint/...）仍保留（向后兼容），
// 本文件提供 cargo/git 风格的统一入口体验。
//
// 退出码: 子命令的退出码透传
// ============================================================

#include "cli/compile_core.h"
#include "cli/coverage_core.h"
#include "cli/doc_core.h"
#include "cli/fmt_core.h"
#include "cli/fuzz_core.h"
#include "cli/lint_core.h"
#include "cli/pkg_core.h"

// LSP/DAP 需要 QCoreApplication，延迟到子命令内部创建
// 前向声明子命令入口函数（定义在各自独立编译单元中避免 QCoreApplication 污染）
int lsp_main(int argc, char* argv[]);
int dap_main(int argc, char* argv[]);

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

void printUsage() {
    std::printf(
        "MiniLang CLI Toolkit v1.0\n"
        "\n"
        "用法: minilang <command> [options...]\n"
        "\n"
        "可用命令:\n"
        "  fmt        代码格式化（--check/--write/--dry-run）\n"
        "  lint       静态分析（--quiet/--rule/--format）\n"
        "  lsp        Language Server Protocol 服务器\n"
        "  dap        Debug Adapter Protocol 服务器\n"
        "  fuzz       模糊测试（generate/mutate/file 三种模式）\n"
        "  pkg        包管理器（init/install/publish）\n"
        "  compile    预编译模块（.mini → .minic）\n"
        "  coverage   行级覆盖率分析（text/lcov 输出）\n"
        "  doc        API 文档生成（markdown/html/json）\n"
        "\n"
        "使用 'minilang <command> --help' 查看各命令详细帮助。\n");
}

void printVersion() {
    std::printf("minilang 1.0.0\n");
}

} // namespace

// ---- 各子命令运行逻辑 ----

static int runFormat(const minilang_fmt::CliArgs& args) {
    using namespace minilang_fmt;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "错误: 未指定文件\n");
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    bool anyNeedsFormat = false;
    bool anyError = false;

    for (const auto& file : args.files) {
        ProcessResult r = processFile(file, args.mode, args.options);
        if (!r.ok) {
            std::fprintf(stderr, "%s\n", r.errorMessage.c_str());
            anyError = true;
            continue;
        }
        switch (args.mode) {
        case ProcessMode::Check:
            if (r.needsFormat) {
                std::printf("%s: 需要格式化\n", file.c_str());
                anyNeedsFormat = true;
            } else {
                std::printf("%s: 已格式化\n", file.c_str());
            }
            break;
        case ProcessMode::Write:
            if (r.needsFormat) {
                std::printf("%s: 已格式化\n", file.c_str());
            } else {
                std::printf("%s: 无变化\n", file.c_str());
            }
            break;
        case ProcessMode::DryRun:
            std::fputs(r.output.c_str(), stdout);
            break;
        }
    }

    if (anyError) return 2;
    if (args.mode == ProcessMode::Check && anyNeedsFormat) return 1;
    return 0;
}

static int runLint(const minilang_lint::CliArgs& args) {
    using namespace minilang_lint;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "错误: 未指定文件\n");
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    bool anyWarning = false;
    bool anyError = false;

    for (const auto& file : args.files) {
        LintProcessResult r = processFile(file, args.options, args.format, args.quiet);
        if (!r.ok) {
            std::fprintf(stderr, "%s\n", r.errorMessage.c_str());
            anyError = true;
            continue;
        }
        if (!r.output.empty()) {
            std::fputs(r.output.c_str(), stdout);
        }
        if (r.hasWarnings) {
            anyWarning = true;
        }
    }

    if (anyError) return 2;
    if (anyWarning) return 1;
    return 0;
}

static int runFuzz(const minilang_fuzz::CliArgs& args) {
    using namespace minilang_fuzz;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    FuzzSummary summary;
    if (args.options.mode == FuzzMode::File) {
        if (args.files.empty()) {
            std::fprintf(stderr, "错误: 文件模式需要指定文件\n");
            std::fprintf(stderr, "使用 --help 查看用法\n");
            return 2;
        }
        bool anyError = false;
        for (const auto& file : args.files) {
            FuzzSummary s = processFile(file, args.options);
            if (!s.ok) {
                std::fprintf(stderr, "错误: 无法读取文件 %s (%s)\n", file.c_str(),
                             s.errorMessage.c_str());
                anyError = true;
                continue;
            }
            summary.totalRuns += s.totalRuns;
            summary.crashes += s.crashes;
            summary.disagreements += s.disagreements;
            summary.agreements += s.agreements;
            summary.parseFailures += s.parseFailures;
            summary.compileFailures += s.compileFailures;
            summary.runtimeErrors += s.runtimeErrors;
            summary.totalDurationMs += s.totalDurationMs;
            for (auto& c : s.crashCases)
                summary.crashCases.push_back(c);
            for (auto& d : s.disagreementCases)
                summary.disagreementCases.push_back(d);
        }
        if (anyError) return 2;
    } else {
        summary = runFuzzBatch(args.options);
    }

    std::string output = (args.format == OutputFormat::Json) ? formatSummaryJson(summary)
                                                             : formatSummaryText(summary);
    std::fputs(output.c_str(), stdout);

    if (summary.crashes > 0 || summary.disagreements > 0) return 1;
    return 0;
}

static int runCompile(const minilang_compile::CliArgs& args) {
    using namespace minilang_compile;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    ModuleResult r = args.useRegisterVM ? compileModuleToFileRegister(args.sourceFile, args.outputFile)
                                        : compileModuleToFile(args.sourceFile, args.outputFile);
    if (!r.ok) {
        std::fprintf(stderr, "错误: %s\n", r.errorMessage.c_str());
        return 2;
    }

    std::printf("编译成功: %s → %s (%d 函数, %d 导出)\n", args.sourceFile.c_str(),
                r.outputFile.c_str(), r.functionCount, r.exportCount);
    return 0;
}

static int runCoverage(const minilang_coverage::CliArgs& args) {
    using namespace minilang_coverage;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "错误: 未指定文件\n");
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    CoverageReport report = analyzeFiles(args.files, args.options);

    if (args.options.format == OutputFormat::Lcov) {
        std::fputs(formatReportLcov(report).c_str(), stdout);
    } else {
        std::fputs(formatReportText(report, args.options).c_str(), stdout);
    }

    if (!report.ok) return 2;
    if (args.options.failUnder >= 0.0 && report.ratio < args.options.failUnder) {
        std::fprintf(stderr, "覆盖率 %.2f%% 低于阈值 %.2f%%\n", report.ratio, args.options.failUnder);
        return 1;
    }
    return 0;
}

static int runDoc(const minilang_doc::CliArgs& args) {
    using namespace minilang_doc;

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "错误: 未指定文件\n");
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    std::ostringstream combinedOutput;
    bool anyError = false;
    int totalEntries = 0;

    for (const auto& file : args.files) {
        DocProcessResult r = processFile(file, args.format);
        if (!r.ok) {
            std::fprintf(stderr, "%s\n", r.errorMessage.c_str());
            anyError = true;
            continue;
        }
        combinedOutput << r.output;
        totalEntries += r.entryCount;
    }

    if (anyError) return 2;

    std::string output = combinedOutput.str();
    if (args.outputFile.empty()) {
        std::fputs(output.c_str(), stdout);
    } else {
        std::ofstream ofs(args.outputFile, std::ios::trunc);
        if (!ofs.is_open()) {
            std::fprintf(stderr, "错误: 无法写入文件: %s\n", args.outputFile.c_str());
            return 2;
        }
        ofs << output;
    }

    std::fprintf(stderr, "已生成 %d 个条目的文档\n", totalEntries);
    return 0;
}

static int runPkg(const minilang_pkg::CliArgs& args) {
    auto result = minilang_pkg::processCommand(args);

    if (!result.output.empty()) {
        std::fputs(result.output.c_str(), stdout);
        if (result.output.back() != '\n') {
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
    }

    return result.exitCode;
}

// ---- 各子命令入口（非 Qt） ----

static int cmd_fmt(int argc, char* argv[]) {
    using namespace minilang_fmt;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_fmt::printVersion(); return 0; }
    return runFormat(args);
}

static int cmd_lint(int argc, char* argv[]) {
    using namespace minilang_lint;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_lint::printVersion(); return 0; }
    return runLint(args);
}

static int cmd_fuzz(int argc, char* argv[]) {
    using namespace minilang_fuzz;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_fuzz::printVersion(); return 0; }
    return runFuzz(args);
}

static int cmd_compile(int argc, char* argv[]) {
    using namespace minilang_compile;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_compile::printVersion(); return 0; }
    return runCompile(args);
}

static int cmd_coverage(int argc, char* argv[]) {
    using namespace minilang_coverage;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_coverage::printVersion(); return 0; }
    return runCoverage(args);
}

static int cmd_doc(int argc, char* argv[]) {
    using namespace minilang_doc;
    auto args = parseArgs(argc, argv);
    if (args.showHelp) { printHelp(); return 0; }
    if (args.showVersion) { minilang_doc::printVersion(); return 0; }
    return runDoc(args);
}

static int cmd_pkg(int argc, char* argv[]) {
    auto args = minilang_pkg::parseArgs(argc, argv);
    if (args.showHelp) { std::fputs(minilang_pkg::helpString().c_str(), stdout); return 0; }
    if (args.showVersion) { std::printf("minilang-pkg %s\n", minilang_pkg::versionString().c_str()); return 0; }
    return runPkg(args);
}

// ---- main ----

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string cmd = argv[1];

    // --help / --version 在子命令之前
    if (cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-v") {
        printVersion();
        return 0;
    }

    // 子命令分发：将 argv[1] 移除，传递 argv[0] + argv[2..N] 给子命令
    // 技巧：直接传 argc-1, argv+1 即可（argv[1] 变成子命令的 argv[0]）
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    if (cmd == "fmt") return cmd_fmt(sub_argc, sub_argv);
    if (cmd == "lint") return cmd_lint(sub_argc, sub_argv);
    if (cmd == "lsp") return lsp_main(sub_argc, sub_argv);
    if (cmd == "dap") return dap_main(sub_argc, sub_argv);
    if (cmd == "fuzz") return cmd_fuzz(sub_argc, sub_argv);
    if (cmd == "pkg") return cmd_pkg(sub_argc, sub_argv);
    if (cmd == "compile") return cmd_compile(sub_argc, sub_argv);
    if (cmd == "coverage") return cmd_coverage(sub_argc, sub_argv);
    if (cmd == "doc") return cmd_doc(sub_argc, sub_argv);

    std::fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    printUsage();
    return 1;
}
