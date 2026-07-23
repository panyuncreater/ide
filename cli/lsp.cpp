// ============================================================
// cli/lsp.cpp - minilang-lsp 命令行入口
// ------------------------------------------------------------
// R164 工具链拓展：Language Server Protocol 服务器入口。
// 核心逻辑在 cli/lsp_core.h/cpp 中（便于单元测试）。
//
// 启动模式：
//   --stdio    使用 stdio 作为传输层（默认，LSP 标准模式）
//   --help     显示帮助信息
//   --version  显示版本信息
//
// 退出码：
//   0  正常退出（收到 exit 通知，且之前已收到 shutdown 请求）
//   1  异常退出（收到 exit 通知但之前未收到 shutdown，或 stdin EOF）
// ============================================================
#include "cli/lsp_core.h"

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

/// 解析命令行参数
struct MainArgs {
    bool useStdio = true; // 默认 stdio 模式
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
            break;
        }
    }
    return args;
}

/// 在 Windows 下将 stdin/stdout 设置为二进制模式
/// 避免 \r\n 转换破坏 Content-Length 分帧
void setBinaryMode() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

/// 运行 LSP 服务器主循环
/// 返回退出码：0=正常退出（收到 exit），1=异常退出（EOF）
int runLspServer() {
    using namespace minilang_lsp;

    JsonRpcTransport transport;
    LspRequestHandler handler;

    while (!handler.shouldExit()) {
        // 阻塞读取一条 JSON-RPC 消息
        auto message = transport.read();
        if (!message) {
            // stdin EOF（客户端断开连接）
            // LSP 规范：若未收到 exit 通知，应返回 1
            return handler.isShutdownRequested() ? 0 : 1;
        }

        // 分发消息并获取响应
        std::vector<QJsonObject> responses = handler.handleMessage(*message);

        // 写出所有响应/通知
        for (const auto& resp : responses) {
            transport.write(resp);
        }
    }

    // 收到 exit 通知正常退出
    // LSP 规范：若之前收到 shutdown 请求，返回 0；否则返回 1
    return handler.isShutdownRequested() ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    // 1. 解析参数
    MainArgs args = parseArgs(argc, argv);

    // 2. 处理 --help / --version（不需要 QCoreApplication）
    if (args.showHelp) {
        minilang_lsp::printHelp();
        return 0;
    }
    if (args.showVersion) {
        minilang_lsp::printVersion();
        return 0;
    }

    // 3. 参数解析错误
    if (args.parseError) {
        std::fprintf(stderr, "错误: %s\n", args.errorMessage.c_str());
        std::fprintf(stderr, "使用 --help 查看用法\n");
        return 2;
    }

    // 4. 设置 stdio 为二进制模式（Windows 必须，避免 \r\n 转换）
    setBinaryMode();

    // 5. 初始化 QCoreApplication（QJsonDocument 部分功能需要 Qt 初始化）
    QCoreApplication app(argc, argv);

    // 6. 运行 LSP 主循环
    return runLspServer();
}
