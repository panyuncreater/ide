// ============================================================
// SandboxLevels.h — VM 栈沙盒关卡数据定义（功能 5）
// ------------------------------------------------------------
// 教学目标：通过亲手操作理解"栈式 VM 就是 push 和 pop"。
// 5 关卡，由浅入深：
//   1. 计算 1 + 2           — 最基本的 push-pop
//   2. 计算 1 + 2 * 3       — 运算顺序由指令决定
//   3. 计算 (1 + 2) * 3     — 同样的数不同的指令顺序
//   4. 打印 "hello"         — 字符串也是值
//   5. 自由模式             — 自己探索
//
// 本头文件仅依赖 STL，不依赖 Qt Widgets / IdeController，
// 可被测试目标（minilang_tests）安全链接。SandboxLevels.cpp
// 为独立编译单元，仅依赖 Qt6::Core（QString 用于 mlTr，但
// 关卡数据均为 std::string，i18n 关闭时纯 STL）。
// ============================================================

#pragma once

#include <string>
#include <vector>
#include "gui/I18n.h"  // mlTr() 国际化（头文件内联使用）

// ---- 操作类型 ----

/// 沙盒指令类型
enum class SandboxOpType {
    PUSH_INT,     // 压入 int 常量，operand 是数字字符串
    PUSH_STRING,  // 压入 string 常量，operand 是字符串内容
    ADD,          // 二元加（pop 两个，push 结果）
    SUB,          // 二元减
    MUL,          // 二元乘
    DIV,          // 二元除（除数为 0 报错）
    MOD,          // 二元取模（除数为 0 报错）
    NEG,          // 一元负（pop 一个，push -x）
    PRINT,        // pop 一个并输出
    HALT          // 结束
};

/// 单条沙盒指令
struct SandboxOp {
    SandboxOpType type;
    std::string   operand;  // 操作数（PUSH_INT 时是数字字符串，PUSH_STRING 时是字符串内容，其他类型忽略）
};

/// 单个关卡定义
struct SandboxLevel {
    int                         level;             // 1-5
    std::string                 goal;              // 目标描述（如 "计算 1 + 2"）
    std::vector<SandboxOp>      availableOps;      // 可用指令按钮列表
    std::vector<SandboxOp>      expectedSequence;  // 正确顺序的指令序列（自由模式可为空）
    std::string                 expectedOutput;    // 期望输出（如 "3"）
    std::string                 teachingPoint;     // 教学点
    std::string                 hint;              // 提示文本
    int                         difficulty;        // 1-3（⭐/⭐⭐/⭐⭐⭐）
};

// ---- 关卡库 ----

/// SandboxLibrary — 静态关卡数据
/// 实现位于 gui/SandboxLevels.cpp（独立编译单元，仅依赖 Qt6::Core）。
class SandboxLibrary {
public:
/// 返回栈沙盒关卡数据（静态数据）。
    static const std::vector<SandboxLevel>& levels();
};

// ---- 辅助工具 ----

/// 将 SandboxOpType 转为可读字符串（用于 UI 显示与测试诊断）
/// 注：此函数在头文件中 inline 实现，避免测试目标再链接额外源文件。
inline const char* sandboxOpTypeName(SandboxOpType t) {
    switch (t) {
        case SandboxOpType::PUSH_INT:    return "PUSH_INT";
        case SandboxOpType::PUSH_STRING: return "PUSH_STRING";
        case SandboxOpType::ADD:         return "ADD";
        case SandboxOpType::SUB:         return "SUB";
        case SandboxOpType::MUL:         return "MUL";
        case SandboxOpType::DIV:         return "DIV";
        case SandboxOpType::MOD:         return "MOD";
        case SandboxOpType::NEG:         return "NEG";
        case SandboxOpType::PRINT:       return "PRINT";
        case SandboxOpType::HALT:        return "HALT";
    }
    return "UNKNOWN";
}
