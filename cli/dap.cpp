// ============================================================
// cli/dap.cpp - minilang-dap 调试适配器 main 入口
// ------------------------------------------------------------
// 通过 stdio 与编辑器（VS Code 等）通信，实现 DAP 协议。
// 核心逻辑在 dap_core.h/cpp 中，便于 gtest 单元测试。
// ============================================================

#include "cli/dap_core.h"

#include <QtCore/QCoreApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct MainArgs {
    bool useStdio = true;
    bool showHelp = false;
    bool showVersion = false;
    bool parseError = false;
    std::string errorMessage;
};

MainArgs parseArgs(int argc, char* argv[]) {
    MainArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdio") {
            args.useStdio = true;
        } else if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            args.showVersion = true;
        } else {
            args.parseError = true;
            args.errorMessage = "未知参数: " + arg;
        }
    }
    return args;
}

void setBinaryMode() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

int runDapServer() {
    using namespace minilang_dap;
    DapTransport transport;
    DapRequestHandler handler;

    while (!handler.shouldExit()) {
        auto message = transport.read();
        if (!message) {
            // EOF 或读取错误
            return 0;
        }

        std::vector<QJsonObject> responses = handler.handleMessage(*message);
        for (const auto& resp : responses) {
            transport.write(resp);
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    MainArgs args = parseArgs(argc, argv);

    if (args.showHelp) {
        minilang_dap::printHelp();
        return 0;
    }

    if (args.showVersion) {
        minilang_dap::printVersion();
        return 0;
    }

    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        minilang_dap::printHelp();
        return 2;
    }

    setBinaryMode();

    // QCoreApplication 用于初始化 Qt 资源（VM 不需要事件循环）
    QCoreApplication app(argc, argv);

    return runDapServer();
}
