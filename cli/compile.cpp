// ============================================================
// cli/compile.cpp - minilang-compile 命令行入口
// ------------------------------------------------------------
// P2-11 预编译模块 CLI 工具：将 MiniLang 源码编译为 .minic 预编译模块文件。
// 核心逻辑在 cli/compile_core.h/cpp 中（便于单元测试）。
//
// 退出码：
//   0  成功
//   2  错误
// ============================================================
#include "cli/compile_core.h"

#include <cstdio>

int main(int argc, char* argv[]) {
    using namespace minilang_compile;

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

    // 4. 编译模块（--register 选择 RegisterVM 路径，生成 MLRC 格式）
    ModuleResult r = args.useRegisterVM ? compileModuleToFileRegister(args.sourceFile, args.outputFile)
                                        : compileModuleToFile(args.sourceFile, args.outputFile);
    if (!r.ok) {
        std::fprintf(stderr, "错误: %s\n", r.errorMessage.c_str());
        return 2;
    }

    // 5. 输出成功信息
    std::printf("编译成功: %s → %s (%d 函数, %d 导出)\n", args.sourceFile.c_str(), r.outputFile.c_str(),
                r.functionCount, r.exportCount);
    return 0;
}
