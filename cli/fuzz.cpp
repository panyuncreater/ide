// ============================================================
// cli/fuzz.cpp - minilang-fuzz 命令行入口
// ------------------------------------------------------------
// R164 工具链拓展：模糊测试器入口。
// 核心逻辑在 cli/fuzz_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功（无崩溃、无分歧）
//   1  发现崩溃或三后端分歧（已记录用例）
//   2  错误（参数解析失败、文件不存在等）
// ============================================================
#include "cli/fuzz_core.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace minilang_fuzz;

    // 1. 解析参数
    CliArgs args = parseArgs(argc, argv);

    // 2. 处理 --help / --version
    if (args.showHelp) {
        printHelp();
        return 0;
    }
    if (args.showVersion) {
        printVersion();
        return 0;
    }

    // 3. 参数解析错误
    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    // 4. 执行模糊测试
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
            // BUG-93 fix: 改用 ok 标志检查文件错误（替代 crashes<0 哨兵值）
            if (!s.ok) {
                std::fprintf(stderr, "错误: 无法读取文件 %s (%s)\n", file.c_str(),
                             s.errorMessage.c_str());
                anyError = true;
                continue;
            }
            // 合并汇总
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
        if (anyError)
            return 2;
    } else {
        summary = runFuzzBatch(args.options);
    }

    // 5. 输出汇总
    std::string output = (args.format == OutputFormat::Json) ? formatSummaryJson(summary) : formatSummaryText(summary);
    std::fputs(output.c_str(), stdout);

    // 6. 返回退出码
    if (summary.crashes > 0 || summary.disagreements > 0) {
        return 1;
    }
    return 0;
}
