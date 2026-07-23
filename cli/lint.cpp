// ============================================================
// cli/lint.cpp - minilang-lint 命令行入口
// ------------------------------------------------------------
// R162 工具链拓展：将 LintPass 抽出为独立 minilang-lint 命令。
// 核心逻辑在 cli/lint_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功（无警告、无错误）
//   1  有 lint 警告（源码可解析但存在静态问题）
//   2  错误（参数解析失败、文件不存在、词法/语法错误等）
// ============================================================
#include "cli/lint_core.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace minilang_lint;

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

    // 5. 遍历处理文件
    bool anyWarning = false;
    bool anyError = false;

    for (const auto& file : args.files) {
        LintProcessResult r = processFile(file, args.options, args.format, args.quiet);
        if (!r.ok) {
            std::fprintf(stderr, "%s\n", r.errorMessage.c_str());
            anyError = true;
            continue;
        }

        // 输出 lint 结果到 stdout
        if (!r.output.empty()) {
            std::fputs(r.output.c_str(), stdout);
        }

        if (r.hasWarnings) {
            anyWarning = true;
        }
    }

    // 6. 返回退出码
    if (anyError) {
        return 2;
    }
    if (anyWarning) {
        return 1;
    }
    return 0;
}
