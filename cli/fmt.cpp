// ============================================================
// cli/fmt.cpp - minilang-fmt 命令行入口
// ------------------------------------------------------------
// R109 代码格式化 CLI 工具：将 Formatter 抽出为独立 minilang-fmt 命令。
// 核心逻辑在 cli/fmt_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功（--check 模式下：所有文件已格式化）
//   1  --check 模式下：至少一个文件需要格式化
//   2  错误
// ============================================================
#include "cli/fmt_core.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace minilang_fmt;

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

    // 6. 返回退出码
    if (anyError) {
        return 2;
    }
    if (args.mode == ProcessMode::Check && anyNeedsFormat) {
        return 1;
    }
    return 0;
}
