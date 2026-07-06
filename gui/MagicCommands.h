// ============================================================
// MagicCommands.h — REPL %magic 命令系统（功能 11）
// ------------------------------------------------------------
// 在 REPL 交互场景中通过 % 前缀命令快速调用已有面板能力。
// 命令清单：help / disassemble / ir / compare / profile / memory
//           / ast / tokens / reset / version
//
// 设计约束：
//   - MagicCommands::commands() 与 %help / %version 不依赖 IdeController，
//     可在测试目标中独立验证（controller=nullptr）
//   - 其他 handler 在 controller=nullptr 时返回友好错误，不崩溃
//   - MagicCommands.cpp 仅依赖 IdeController 头文件声明 + 内联方法，
//     不依赖 app/*.cpp 实现（测试目标仅链接 minilang_core）
// ============================================================

#pragma once

#include <string>
#include <vector>

class IdeController;

/// 单个 magic 命令的元数据
struct MagicCommand {
    std::string name;        // 命令名（如 "help"，不含 % 前缀）
    std::string syntax;      // 语法（如 "%help"）
    std::string description; // 简短描述
};

/// REPL %magic 命令分发器
class MagicCommands {
public:
    /// 获取所有已注册的 magic 命令元数据（静态数据，不依赖 IdeController）
    static const std::vector<MagicCommand>& commands();

    /// 处理 %xxx 命令，返回输出文本。
    /// - input: 用户原始输入（可能含前导空白 + % 前缀）
    /// - controller: IdeController 指针，可为 nullptr（测试场景）
    ///
    /// 行为约定：
    ///   - 空/非 % 开头输入：返回空字符串（表示不处理）
    ///   - 未知命令：返回 "Unknown magic command: xxx"
    ///   - %help / %version：不依赖 controller
    ///   - 数据类命令（disassemble/ir/...）：controller=null 时返回友好错误
    static std::string handle(const std::string& input, IdeController* controller);
};
