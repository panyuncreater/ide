// ============================================================
// cli/dap_entry.cpp - DAP 子命令入口（供 minilang 统一二进制调用）
// ------------------------------------------------------------
// 封装原 dap.cpp 的 main 逻辑为 dap_main() 函数。
// DAP 需要 QCoreApplication（通过 minilang_core 间接依赖 Qt6::Core）。
// ============================================================
#include "cli/dap_core.h"

#include <QtCore/QCoreApplication>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int dap_main(int argc, char* argv[]) {
    // 解析参数
    bool showHelp = false;
    bool showVersion = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            showVersion = true;
        }
    }

    if (showHelp) {
        std::printf(
            "minilang dap - MiniLang Debug Adapter\n\n"
            "用法: minilang dap [--help] [--version]\n\n"
            "通过 stdio 提供 DAP 协议服务（VS Code 等编辑器集成）。\n");
        return 0;
    }
    if (showVersion) {
        std::printf("minilang-dap 1.0.0\n");
        return 0;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    QCoreApplication app(argc, argv);
    return minilang_dap::runDapServer();
}
