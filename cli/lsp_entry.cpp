// ============================================================
// cli/lsp_entry.cpp - LSP 子命令入口（供 minilang 统一二进制调用）
// ------------------------------------------------------------
// 封装原 lsp.cpp 的 main 逻辑为 lsp_main() 函数。
// LSP 需要 QCoreApplication 事件循环，在此函数内部创建。
// ============================================================
#include "cli/lsp_core.h"

#include <QtCore/QCoreApplication>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int lsp_main(int argc, char* argv[]) {
    // 解析参数
    bool useStdio = true;
    bool showHelp = false;
    bool showVersion = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdio") {
            useStdio = true;
        } else if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            showVersion = true;
        }
    }

    if (showHelp) {
        std::printf(
            "minilang lsp - MiniLang Language Server\n\n"
            "用法: minilang lsp [--stdio] [--help] [--version]\n\n"
            "选项:\n"
            "  --stdio    使用 stdio 传输层（默认）\n"
            "  --help     显示帮助\n"
            "  --version  显示版本\n");
        return 0;
    }
    if (showVersion) {
        std::printf("minilang-lsp 1.0.0\n");
        return 0;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    QCoreApplication app(argc, argv);
    (void)useStdio; // stdio 是唯一支持的模式

    return minilang_lsp::runLspServer();
}
