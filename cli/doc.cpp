// ============================================================
// cli/doc.cpp - minilang-doc 命令行入口
// ------------------------------------------------------------
// R162 B2 工具链拓展：将 DocGenerator 抽出为独立 minilang-doc 命令。
// 核心逻辑在 cli/doc_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功（文档生成完成）
//   2  错误（参数解析失败、文件不存在、词法/语法错误等）
// ============================================================
#include "cli/doc_core.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    using namespace minilang_doc;

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

    // 5. 遍历处理文件，合并所有文档输出
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

    // 6. 输出结果
    if (anyError) {
        return 2;
    }

    std::string output = combinedOutput.str();
    if (args.outputFile.empty()) {
        // 输出到 stdout
        std::fputs(output.c_str(), stdout);
    } else {
        // 输出到文件
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
