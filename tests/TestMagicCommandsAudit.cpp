// ============================================================
// TestMagicCommandsAudit.cpp — REPL %magic 命令系统测试（功能 11）
// ------------------------------------------------------------
// 两个测试套件：
//   1. MagicCommandsLibraryAudit.*（6 用例）— 命令元数据完整性
//   2. MagicCommandsHandlerAudit.*（12 用例，全部 controller=nullptr）
//      — handler 分发逻辑、友好错误处理、参数代码分析
//
// ROUND-69 更新：新增参数代码分析测试（%ast/%tokens/%disassemble/%ir 带参数时
// 使用临时 Lexer/Parser/Compiler 分析参数代码，不依赖 IdeController）。
//
// 注：MagicCommands.cpp 已在 cmake/minilang_core.cmake 的
//     MINILANG_CORE_SOURCES 中，测试目标通过链接 minilang_core 自动获得。
//     commands() / %help / %version / 带参数的分析命令不依赖 IdeController，
//     可独立验证。
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
    std::string output = MagicCommands::handle("%version", nullptr);
    EXPECT_NE(output.find("MiniLang"), std::string::npos)
        << "%version 输出应包含 'MiniLang'，实际: " << output;
}

// ============================================================
// 套件 2：MagicCommandsHandlerAudit — handler 分发逻辑（controller=nullptr）
// ============================================================

TEST(MagicCommandsHandlerAudit, HelpOutputContainsAllCommandNames) {
    std::string output = MagicCommands::handle("%help", nullptr);
    EXPECT_FALSE(output.empty()) << "%help 不应返回空";
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

TEST(MagicCommandsHandlerAudit, DisassembleNoArgsReturnsErrorWhenControllerNull) {
    std::string output = MagicCommands::handle("%disassemble", nullptr);
    bool hasError = (output.find("Error") != std::string::npos) ||
                    (output.find("未设置") != std::string::npos) ||
                    (output.find("暂无数据") != std::string::npos);
    EXPECT_TRUE(hasError) << "%disassemble 无参数且 controller=null 时应返回友好提示，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, DisassembleWithArgWorksWithoutController) {
    std::string output = MagicCommands::handle("%disassemble 1 + 2;", nullptr);
    EXPECT_NE(output.find("字节码"), std::string::npos)
        << "%disassemble 带参数应能编译参数代码并输出字节码，实际: " << output;
    EXPECT_NE(output.find("Chunk"), std::string::npos)
        << "%disassemble 带参数应输出 Chunk 信息，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, IrNoArgsReturnsErrorWhenControllerNull) {
    std::string output = MagicCommands::handle("%ir", nullptr);
    bool hasError = (output.find("Error") != std::string::npos) ||
                    (output.find("未设置") != std::string::npos) ||
                    (output.find("暂无数据") != std::string::npos);
    EXPECT_TRUE(hasError) << "%ir 无参数且 controller=null 时应返回友好提示，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, IrWithArgWorksWithoutController) {
    std::string output = MagicCommands::handle("%ir var x = 1;", nullptr);
    EXPECT_NE(output.find("IR"), std::string::npos)
        << "%ir 带参数应能构建 IR 并输出，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, AstWithArgWorksWithoutController) {
    std::string output = MagicCommands::handle("%ast 1 + 2;", nullptr);
    EXPECT_NE(output.find("AST"), std::string::npos)
        << "%ast 带参数应能解析参数代码并输出 AST，实际: " << output;
    EXPECT_NE(output.find("BinaryOp"), std::string::npos)
        << "%ast 1+2 应输出 BinaryOp 节点，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, TokensWithArgWorksWithoutController) {
    std::string output = MagicCommands::handle("%tokens var x = 42;", nullptr);
    EXPECT_NE(output.find("Token"), std::string::npos)
        << "%tokens 带参数应能词法分析参数代码并输出 Token 表，实际: " << output;
    EXPECT_NE(output.find("VAR"), std::string::npos)
        << "%tokens var x=42 应包含 VAR token，实际: " << output;
    EXPECT_NE(output.find("INT_LIT"), std::string::npos)
        << "%tokens var x=42 应包含 INT_LIT token，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, UnknownCommandReturnsUnknownMessage) {
    std::string output = MagicCommands::handle("%unknown", nullptr);
    EXPECT_NE(output.find("Unknown magic command"), std::string::npos)
        << "%unknown 应返回 'Unknown magic command'，实际: " << output;
    EXPECT_NE(output.find("unknown"), std::string::npos)
        << "%unknown 输出应包含命令名 'unknown'，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, EmptyInputNotTreatedAsMagicCommand) {
    std::string output = MagicCommands::handle("", nullptr);
    EXPECT_TRUE(output.empty()) << "空输入应返回空字符串，实际: " << output;
}

TEST(MagicCommandsHandlerAudit, NonMagicInputNotHandled) {
    std::string output = MagicCommands::handle("print(1);", nullptr);
    EXPECT_TRUE(output.empty()) << "非 % 开头输入应返回空字符串，实际: " << output;

    std::string output2 = MagicCommands::handle("   var x = 1;", nullptr);
    EXPECT_TRUE(output2.empty()) << "前导空白 + 非 % 输入应返回空字符串，实际: " << output2;
}

TEST(MagicCommandsHandlerAudit, LeadingWhitespaceBeforePercentIsHandled) {
    std::string output = MagicCommands::handle("   %version", nullptr);
    EXPECT_NE(output.find("MiniLang"), std::string::npos)
        << "前导空白 + %version 应正常处理，实际: " << output;
}
