// ============================================================
// cli/dap.cpp - minilang-dap 调试适配器 main 入口
// ------------------------------------------------------------
// 通过 stdio 与编辑器（VS Code 等）通信，实现 DAP 协议。
// 核心逻辑在 dap_core.h/cpp 中，便于 gtest 单元测试。
//
// L21: 同步暂停（pause 请求）支持
//   DAP 的 continue 请求会同步执行 VM 直到断点/结束。期间需要能接收 pause 请求。
//   实现：doContinue 循环每 kPausePollInterval 步调用 stdinPollCallback_，
//   回调非阻塞检查 stdin 是否有数据待读。若有，设置 pauseRequested_ 标志，
//   doContinue 退出，控制权返回 main 循环读取并处理 pause 消息。
//   跨平台实现：
//     Windows: PeekNamedPipe（读取管道待读字节数）
//     Unix:    poll(FILE* stdin)（POLLIN 事件）
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
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
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

/// L21: 非阻塞检查 stdin 是否有数据待读。
/// doContinue 循环每 kPausePollInterval 步调用此函数。
/// @return true 表示 stdin 有数据（可能是 pause 请求）；false 表示无数据。
bool stdinHasData() {
#ifdef _WIN32
    // Windows: stdin 可能是管道或控制台。用 PeekNamedPipe 检查管道待读字节数。
    // 若 PeekNamedPipe 失败（stdin 不是管道，如控制台），回退到 _kbhit。
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) {
        return false;
    }
    // 先尝试管道
    DWORD bytesAvailable = 0;
    if (PeekNamedPipe(hStdin, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
        return bytesAvailable > 0;
    }
    // 管道检查失败，可能是控制台或重定向。用 _kbhit 检查控制台输入。
    // 注意：_kbhit 仅对控制台有效，对重定向管道返回 0。
    return _kbhit() != 0;
#else
    // Unix: poll(stdin, timeout=0) 非阻塞检查 POLLIN 事件。
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret = ::poll(&pfd, 1, 0); // timeout=0 非阻塞
    return ret > 0 && (pfd.revents & POLLIN);
#endif
}

int runDapServer() {
    using namespace minilang_dap;
    DapTransport transport;
    DapRequestHandler handler;

    // L21: 注入 stdin 非阻塞轮询回调，使 doContinue 能在循环中检测 pause 请求。
    handler.session().setStdinPollCallback(stdinHasData);

    while (!handler.shouldExit()) {
        auto message = transport.read();
        if (!message) {
            // EOF 或读取错误
            return 0;
        }

        // 处理消息前清除 pause 标志（避免上次 pause 的残留标志影响本次 continue）
        handler.session().clearPauseRequest();

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
