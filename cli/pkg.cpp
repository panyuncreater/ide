// ============================================================
// cli/pkg.cpp - minilang-pkg 包管理器 main 入口
// ------------------------------------------------------------
// 第 8 个 CLI 工具 minilang-pkg。
// 实现包管理：install/list/init/add 命令。
// 核心逻辑在 pkg_core.h/cpp 中，便于 gtest 单元测试。
// ============================================================
#include "cli/pkg_core.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char* argv[]) {
    // 创建 QCoreApplication 但不 exec()（仅需 Qt 文件/JSON 功能）
    QCoreApplication app(argc, argv);

    auto args = minilang_pkg::parseArgs(argc, argv);
    auto result = minilang_pkg::processCommand(args);

    if (!result.output.empty()) {
        std::fputs(result.output.c_str(), stdout);
        // 确保输出以换行结尾
        if (result.output.back() != '\n') {
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
    }

    return result.exitCode;
}
