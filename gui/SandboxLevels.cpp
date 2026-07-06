// ============================================================
// SandboxLevels.cpp — VM 栈沙盒关卡数据实现（功能 5）
// ------------------------------------------------------------
// 从 VmStackSandboxPanel.cpp 拆分而来，避免测试目标链接
// VmStackSandboxPanel.cpp 时引入 Qt6::Widgets 依赖。
// 本文件仅包含 SandboxLibrary::levels() 静态数据，依赖 STL
// + Qt6::Core（仅因 PCH 拉入 QString）。无任何引擎层或
// GUI 层依赖。
// ============================================================

#include "gui/SandboxLevels.h"

// ============================================================
// SandboxLibrary — 5 个关卡数据
// ------------------------------------------------------------
// 关卡设计由浅入深：
//   关卡 1：1 + 2 = 3
//     push 1 → push 2 → add → print
//     教学点：最基本的 push-pop
//
//   关卡 2：1 + 2 * 3 = 7（运算顺序由指令决定）
//     push 1 → push 2 → push 3 → mul → add → print
//     教学点：栈式 VM 中运算顺序由指令的先后决定，不是
//             由源码表达式优先级决定——必须先算乘法，再算加法。
//
//   关卡 3：(1 + 2) * 3 = 9（同数不同顺序）
//     push 1 → push 2 → add → push 3 → mul → print
//     教学点：与关卡 2 使用相同的可用指令集，但执行顺序不同，
//             得到不同结果——这正是栈式 VM 的"指令即语义"。
//
//   关卡 4：打印 "hello"
//     push "hello" → print
//     教学点：字符串也是值，可以被 push 到栈上，也能被 print。
//
//   关卡 5：自由模式
//     全部指令可用，无 expectedSequence（自由探索）。
//     教学点：自由组合指令，观察栈的变化与运算结果。
// ============================================================

const std::vector<SandboxLevel>& SandboxLibrary::levels() {
    static const std::vector<SandboxLevel> kLevels = {
        // ---- 关卡 1：1 + 2 = 3 ----
        SandboxLevel{
            1,
            "计算 1 + 2，输出结果 3",
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            "3",
            "最基本的 push-pop：先压入两个操作数，再用 ADD 弹出它们并压入结果。",
            "提示：先点 PUSH_INT 1，再点 PUSH_INT 2，最后点 ADD 和 PRINT。",
            1
        },
        // ---- 关卡 2：1 + 2 * 3 = 7 ----
        SandboxLevel{
            2,
            "计算 1 + 2 * 3，输出结果 7（注意：先乘后加）",
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::PUSH_INT, "3"},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::MUL,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::PUSH_INT, "3"},
                {SandboxOpType::MUL,      ""},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            "7",
            "运算顺序由指令决定：源码表达式 1 + 2 * 3 优先算乘法，所以指令必须先 MUL 后 ADD。",
            "提示：先把三个数都压入栈，再用 MUL 计算 2*3=6，最后用 ADD 计算 1+6=7。",
            2
        },
        // ---- 关卡 3：(1 + 2) * 3 = 9 ----
        SandboxLevel{
            3,
            "计算 (1 + 2) * 3，输出结果 9（注意：先加后乘）",
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::PUSH_INT, "3"},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::MUL,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            {
                {SandboxOpType::PUSH_INT, "1"},
                {SandboxOpType::PUSH_INT, "2"},
                {SandboxOpType::ADD,      ""},
                {SandboxOpType::PUSH_INT, "3"},
                {SandboxOpType::MUL,      ""},
                {SandboxOpType::PRINT,    ""},
            },
            "9",
            "同数不同顺序：与关卡 2 使用相同的可用指令，但执行顺序不同（先 ADD 后 MUL）得到不同结果。",
            "提示：先压 1 和 2，用 ADD 得到 3，再压 3，最后用 MUL 得到 9。",
            2
        },
        // ---- 关卡 4：打印 "hello" ----
        SandboxLevel{
            4,
            "打印字符串 hello",
            {
                {SandboxOpType::PUSH_STRING, "hello"},
                {SandboxOpType::PRINT,       ""},
            },
            {
                {SandboxOpType::PUSH_STRING, "hello"},
                {SandboxOpType::PRINT,       ""},
            },
            "hello",
            "字符串也是值：PUSH_STRING 将字符串压入栈，PRINT 可以输出它。",
            "提示：先点 PUSH_STRING hello，再点 PRINT。",
            1
        },
        // ---- 关卡 5：自由模式 ----
        SandboxLevel{
            5,
            "自由模式：尝试任意指令组合，观察栈的变化",
            {
                {SandboxOpType::PUSH_INT,    "1"},
                {SandboxOpType::PUSH_INT,    "2"},
                {SandboxOpType::PUSH_INT,    "3"},
                {SandboxOpType::PUSH_INT,    "10"},
                {SandboxOpType::PUSH_STRING, "hello"},
                {SandboxOpType::PUSH_STRING, "world"},
                {SandboxOpType::ADD,  ""},
                {SandboxOpType::SUB,  ""},
                {SandboxOpType::MUL,  ""},
                {SandboxOpType::DIV,  ""},
                {SandboxOpType::MOD,  ""},
                {SandboxOpType::NEG,  ""},
                {SandboxOpType::PRINT, ""},
                {SandboxOpType::HALT,  ""},
            },
            // 自由模式无 expectedSequence
            {},
            "",
            "自由探索：尝试 NEG（一元负）、DIV/MOD（除法/取模）、SUB（减法）等指令，理解栈式 VM 的工作机制。",
            "提示：尝试 PUSH_INT 5 → PUSH_INT 3 → SUB（结果 2），或 PUSH_INT 10 → PUSH_INT 4 → MOD（结果 2）。",
            3
        },
    };
    return kLevels;
}
