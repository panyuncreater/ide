// ============================================================
// SandboxLevels.cpp — VM 栈沙盒关卡数据实现（功能 5）
// ------------------------------------------------------------
// 从 VmStackSandboxPanel.cpp 拆分而来，避免测试目标链接
// VmStackSandboxPanel.cpp 时引入 Qt6::Widgets 依赖。
/// 返回全部栈沙盒关卡定义（静态数据）。
// 本文件仅包含 SandboxLibrary::levels() 静态数据，依赖 STL
// + Qt6::Core（仅因 PCH 拉入 QString）。无任何引擎层或
// GUI 层依赖。
//
// 教学理念：栈式虚拟机（Stack VM）的"思维模型"极其简单——
// 一切运算都发生在一摞盘子上：只能从顶部放（PUSH）和取（POP）。
// 本库通过亲手排列指令，让学习者体会"运算顺序由指令先后决定"
// 这一核心直觉，并对比"同数不同顺序得到不同结果"。
// ============================================================

#include "gui/SandboxLevels.h"

/// 返回全部栈沙盒关卡定义（静态数据）。
const std::vector<SandboxLevel>& SandboxLibrary::levels() {
    static const std::vector<SandboxLevel> kLevels = {
        // ---- 关卡 1：1 + 2 = 3 ----
        SandboxLevel{
            1,
            "🎮 计算 1 + 2，输出结果 3",
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
            "💻 栈就像一摞盘子——只能从顶部放（PUSH）和取（POP）。\n"
            "• 这条指令流做了什么：\n"
            "    ① PUSH 1     栈:[1]\n"
            "    ② PUSH 2     栈:[1, 2]\n"
            "    ③ ADD        弹出栈顶两个(2,1)，相加得 3，压回   栈:[3]\n"
            "    ④ PRINT      弹出 3 并输出 → 屏幕显示 3\n"
            "• 核心认知：运算数先「堆」在栈上，运算符来了再把它们「消费」掉。没有栈，运算就无从谈起。",
            "💡 步骤：点 PUSH_INT 1（栈出现 1）→ PUSH_INT 2（栈变 [1,2]）→ ADD（弹出 1 和 2，压入 3，栈变 [3]）→ PRINT（弹出 3 并打印）。\n"
            "   注意 ADD 永远吃「最上面两个」，顺序很重要：先压 1 再压 2，弹出来时是先 2 后 1，但加法交换律让结果一样。",
            1
        },
        // ---- 关卡 2：1 + 2 * 3 = 7（运算顺序由指令决定） ----
        SandboxLevel{
            2,
            "🎮 计算 1 + 2 * 3，输出结果 7（注意：先乘后加）",
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
            "💻 关键认知：栈式 VM 的运算顺序完全由「指令的先后」决定，而不是由源码里运算符的符号决定。\n"
            "• 源码 1 + 2 * 3 之所以等于 7，是因为编译器把它翻译成了「先算 2*3」的指令序列：\n"
            "    PUSH 1 · PUSH 2 · PUSH 3 · MUL(2*3=6) · ADD(1+6=7) · PRINT\n"
            "    栈变化：[1] → [1,2] → [1,2,3] → [1,6] → [7] → 打印 7\n"
            "• 如果反过来先 ADD 再 MUL，就会变成 (1+2)*3=9——顺序一变，语义全变。",
            "💡 先把三个数都压栈：[1, 2, 3]；再用 MUL 让栈顶两个(2,3)相乘得 6 → [1, 6]；最后 ADD 让 1+6=7 → [7]；PRINT。\n"
            "   为什么不是先加？因为乘法优先级高，编译器把「先乘」排在了指令前面，栈只是忠实地执行顺序。",
            2
        },
        // ---- 关卡 3：(1 + 2) * 3 = 9（同数不同顺序） ----
        SandboxLevel{
            3,
            "🎮 计算 (1 + 2) * 3，输出结果 9（注意：先加后乘）",
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
            "💻 同数不同顺序：关卡 2 和 3 用完全相同的「可用指令按钮」，只是点击顺序不同（括号改变了结合顺序），结果就从 7 变成 9。\n"
            "• (1+2)*3 的指令序列（注意 ADD 提前了）：\n"
            "    PUSH 1 · PUSH 2 · ADD(1+2=3) · PUSH 3 · MUL(3*3=9) · PRINT\n"
            "    栈变化：[1] → [1,2] → [3] → [3,3] → [9] → 打印 9\n"
            "• 这正是栈式 VM 的精髓——「指令即语义」：程序真正的含义不在源码符号里，而在你排出来的指令顺序中。",
            "💡 先算括号里的：PUSH 1、PUSH 2、ADD 得到 3（栈:[3]）；再 PUSH 3（栈:[3,3]）；最后 MUL 得 9。\n"
            "   对比关卡 2：括号把「加」的优先级抬到「乘」之上，于是 ADD 排在 MUL 前面。",
            2
        },
        // ---- 关卡 4：打印 "hello" ----
        SandboxLevel{
            4,
            "🎮 打印字符串 hello",
            {
                {SandboxOpType::PUSH_STRING, "hello"},
                {SandboxOpType::PRINT,       ""},
            },
            {
                {SandboxOpType::PUSH_STRING, "hello"},
                {SandboxOpType::PRINT,       ""},
            },
            "hello",
            "💻 字符串也是「值」：在 MiniLang 里，字符串和整数一样，可以被压入栈、参与运算、被打印。\n"
            "• 指令序列：PUSH_STRING 「hello」（把整个字符串当成一个值塞进栈）→ PRINT（取栈顶输出）。\n"
            "    栈变化：[ ] → [「hello」] → 打印 hello\n"
            "• 你会发现输出区直接出现 hello，中间没有任何「计算」——因为这里操作的不是数字，而是一个现成的字符串值。",
            "💡 点 PUSH_STRING hello（栈里出现 「hello」 这个整体）→ 再点 PRINT（弹出并输出）。\n"
            "   与关卡 1 的区别：这里压的是字符串值而非整数，PRINT 原样输出它。",
            1
        },
        // ---- 关卡 5：自由模式 ----
        SandboxLevel{
            5,
            "🎮 自由模式：尝试任意指令组合，观察栈的变化",
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
            "💻 自由探索：现在所有指令都解锁了。\n"
            "• NEG 是一元取负——只弹 1 个值，压回它的相反数（如 5 → -5）。\n"
            "• SUB / DIV / MOD 都是二元运算，吃栈顶两个：先弹出的作右操作数，后弹出的作左操作数（即 a - b 中 a 先入栈）。\n"
            "• 栈式 VM 没有「变量名」概念，所有运算都靠栈顶那几个值——这正是它比寄存器 VM 更简单、也更「笨」的地方：人要知道数据在栈的哪一层。",
            "💡 实验：\n"
            "    • PUSH 5 → PUSH 3 → SUB → 输出 2（5-3）\n"
            "    • PUSH 10 → PUSH 4 → MOD → 输出 2（10%4 的余数）\n"
            "    • PUSH 5 → NEG → PRINT → 输出 -5（一元取负）\n"
            "   观察栈顶数字如何被一条条指令「吃掉」又「吐出」。",
            3
        },
    };
    return kLevels;
}
