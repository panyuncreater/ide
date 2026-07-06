// ============================================================
// TestMagicCommandsAudit.cpp — REPL %magic 命令系统测试（功能 11）
// ------------------------------------------------------------
// 两个测试套件：
//   1. MagicCommandsLibraryAudit.*（6 用例）— 命令元数据完整性
//   2. MagicCommandsHandlerAudit.*（7 用例，全部 controller=nullptr）
//      — handler 分发逻辑与友好错误处理
//
// 注：MagicCommands.cpp 已在 cmake/minilang_core.cmake 的
//     MINILANG_CORE_SOURCES 中，测试目标通过链接 minilang_core 自动获得。
//     commands() / %help / %version 不依赖 IdeController，可独立验证。
// ============================================================

#include <gtest/gtest.h>

#include "gui/MagicCommands.h"

#include <set>
#include <string>

// ============================================================
// 套件 1：MagicCommandsLibraryAudit — 命令元数据完整性
// ============================================================

TEST(MagicCommandsLibraryAudit, HasAtLeastTenCommands) {
    const auto& cmds = MagicCommands::commands();
    EXPECT_GE(cmds.size(), 10u);
}

TEST(MagicCommandsLibraryAudit, CommandNamesAreUnique) {
    const auto& cmds = MagicCommands::commands();
    std::set<std::string> names;
    for (const auto& cmd : cmds) {
        EXPECT_FALSE(cmd.name.empty()) << "命令名不能为空";
        auto [_, inserted] = names.insert(cmd.name);
        EXPECT_TRUE(inserted) << "命令名重复: " << cmd.name;
    }
    EXPECT_EQ(names.size(), cmds.size());
}

TEST(MagicCommandsLibraryAudit, CriticalCommandsPresent) {
    const auto& cmds = MagicCommands::commands();
    std::set<std::string> names;
    for (const auto& cmd : cmds) names.insert(cmd.name);
    // 验证 10 个必需命令均已注册
    EXPECT_NE(names.find("help"), names.end());
    EXPECT_NE(names.find("disassemble"), names.end());
    EXPECT_NE(names.find("ir"), names.end());
    EXPECT_NE(names.find("compare"), names.end());
    EXPECT_NE(names.find("profile"), names.end());
    EXPECT_NE(names.find("memory"), names.end());
    EXPECT_NE(names.find("ast"), names.end());
    EXPECT_NE(names.find("tokens"), names.end());
    EXPECT_NE(names.find("reset"), names.end());
    EXPECT_NE(names.find("version"), names.end());
}

TEST(MagicCommandsLibraryAudit, DescriptionsNonEmpty) {
    const auto& cmds = MagicCommands::commands();
    for (const auto& cmd : cmds) {
        EXPECT_FALSE(cmd.description.empty()) << cmd.name << ": description 不能为空";
    }
}

TEST(MagicCommandsLibraryAudit, SyntaxStartsWithPercent) {
    const auto& cmds = MagicCommands::commands();
    for (const auto& cmd : cmds) {
        EXPECT_FALSE(cmd.syntax.empty()) << cmd.name << ": syntax 不能为空";
        EXPECT_EQ(cmd.syntax[0], '%') << cmd.name << ": syntax 必须以 % 开头";
    }
}

TEST(MagicCommandsLibraryAudit, VersionOutputContainsMiniLang) {
    // %version 不依赖 IdeController，可独立验证
    std::string output = MagicCommands::handle("%version", nullptr);
    EXPECT_NE(output.find("MiniLang"), std::string::npos)
        << "%version 输出应包含 'MiniLang'，实际: " << output;
}

// ============================================================
// 套件 2：MagicCommandsHandlerAudit — handler 分发逻辑（controller=nullptr）
// ============================================================

TEST(MagicCommandsHandlerAudit, HelpOutputContainsAllCommandNames) {
    // %help 不依赖 IdeController
    std::string output = MagicCommands::handle("%help", nullptr);
    EXPECT_FALSE(output.empty()) << "%help 不应返回空";
    // 遍历 commands 列表，验证每个命令名都出现在 %help 输出中
    for (const auto& cmd : MagicCommands::commands()) {
        EXPECT_NE(output.find(cmd.name), std::string::npos)
            << "%help 输出应包含命令名 '" << cmd.name << "'";
    }
}

TEST(MagicCommandsHandlerAudit, VersionOutputContainsMiniLangIDE) {
    std::string output = MagicCommands::handle("%version", nullptr);
    EXPECT_NE(output.find("MiniLang IDE"), std::string::npos)
        << "%version 输出应包含 'MiniLang IDE'，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, DisassembleReturnsFriendlyErrorWhenControllerNull) {
    std::string output = MagicCommands::handle("%disassemble", nullptr);
    // 应返回友好错误（含 "Error" 或 "未设置"）
    bool hasError = (output.find("Error") != std::string::npos) ||
                    (output.find("未设置") != std::string::npos);
    EXPECT_TRUE(hasError) << "%disassemble 在 controller=null 时应返回友好错误，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, IrReturnsFriendlyErrorWhenControllerNull) {
    std::string output = MagicCommands::handle("%ir", nullptr);
    bool hasError = (output.find("Error") != std::string::npos) ||
                    (output.find("未设置") != std::string::npos);
    EXPECT_TRUE(hasError) << "%ir 在 controller=null 时应返回友好错误，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, UnknownCommandReturnsUnknownMessage) {
    std::string output = MagicCommands::handle("%unknown", nullptr);
    EXPECT_NE(output.find("Unknown magic command"), std::string::npos)
        << "%unknown 应返回 'Unknown magic command'，实际: " << output;
    EXPECT_NE(output.find("unknown"), std::string::npos)
        << "%unknown 输出应包含命令名 'unknown'，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, EmptyInputNotTreatedAsMagicCommand) {
    // 空输入不应被当作 magic 命令
    std::string output = MagicCommands::handle("", nullptr);
    EXPECT_TRUE(output.empty()) << "空输入应返回空字符串，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, NonMagicInputNotHandled) {
    // 非 % 开头的输入不应被 handle 处理
    std::string output = MagicCommands::handle("print(1);", nullptr);
    EXPECT_TRUE(output.empty()) << "非 % 开头输入应返回空字符串，实际: " << output;

    // 含前导空白的非 magic 输入也不应处理
    std::string output2 = MagicCommands::handle("   var x = 1;", nullptr);
    EXPECT_TRUE(output2.empty()) << "前导空白 + 非 % 输入应返回空字符串，实际: " << output2;
}
