// ============================================================
// cli/coverage.cpp - minilang-coverage 命令行入口
// ------------------------------------------------------------
// R110 行级覆盖率工具：基于 VM 指令行号信息与 stepCallback 收集行级覆盖率。
// 核心逻辑在 cli/coverage_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功（覆盖率 >= 阈值，或未指定阈值）
//   1  覆盖率低于 --fail-under 指定的阈值
//   2  错误
// ============================================================
#include "cli/coverage_core.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace minilang_coverage;

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

    // 4. 无文件参数
    if (args.files.empty()) {
        std::fprintf(stderr, "错误: 未指定文件\n");
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    // 5. 分析覆盖率
    CoverageReport report = analyzeFiles(args.files, args.options);

    // 6. 输出报告
    if (args.options.format == OutputFormat::Lcov) {
        std::fputs(formatReportLcov(report).c_str(), stdout);
    } else {
        std::fputs(formatReportText(report, args.options).c_str(), stdout);
    }

    // 7. 返回退出码
    // 7.1 任一文件分析失败 → 退出码 2
    if (!report.ok) {
        return 2;
    }
    // 7.2 覆盖率低于阈值 → 退出码 1
    if (args.options.failUnder >= 0.0 && report.ratio < args.options.failUnder) {
        std::fprintf(stderr, "覆盖率 %.2f%% 低于阈值 %.2f%%\n", report.ratio, args.options.failUnder);
        return 1;
    }
    return 0;
}
