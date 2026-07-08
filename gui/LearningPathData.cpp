// ============================================================
// LearningPathData.cpp — 学习路径活动静态数据实现（功能 6）
// ------------------------------------------------------------
// 5 阶段共 22 个学习活动，覆盖 welcome 导览 / lab-01~08 / 4 个游戏化
// 面板 / 4 个挑战 / 4 个实战训练 / 自由项目。
//
// 设计原则：
//   1. 阶段零所有活动无前置（首次启动即可解锁）
//   2. 阶段一活动前置为阶段零关键节点（welcome + 2 个游戏化面板）
//   3. lab-N 通常前置为 lab-(N-1)（递进学习）
//   4. bug-hunt-intermediate 前置为 bug-hunt-beginner
//   5. bug-hunt-expert 前置为 bug-hunt-intermediate（必须按顺序通关）
// ============================================================

#include "gui/LearningPathData.h"

#include <algorithm>

// ============================================================
// LearningPathData 实现
// ============================================================
const std::vector<LearningActivity>& LearningPathData::activities() {
    static const std::vector<LearningActivity> kActivities = {
        // ==================== 阶段零：首次接触 ====================
        LearningActivity{
            "welcome", "👋 Welcome 导览",
            "📍 第一次打开 IDE，跟着 3 步小向导走一圈，先建立「代码是怎么变成程序」的直觉。",
            0, {}, 5, ActivityType::TOY, "wave"
        },
        LearningActivity{
            "code-journey", "🚀 代码生命旅程",
            "📍 一张静态信息图，把 print(1+2*3) 从敲下源码到冒出结果的全过程摊给你看：词法→语法→字节码→执行。点进去看即算完成。",
            0, {}, 5, ActivityType::TOY, "rocket"
        },
        LearningActivity{
            "token-puzzle", "🧩 Token 拼图游戏",
            "📍 5 关拼图小游戏，由浅入深。玩着玩着你就懂了：词法分析说白了，就是把字符流切成一块块 Token。",
            0, {}, 15, ActivityType::PUZZLE, "puzzle-piece"
        },
        LearningActivity{
            "ast-toy", "🌳 AST 搭建玩具",
            "📍 6 道拖拽题，亲手把 AST 节点拼起来。用 1+2*3 和 (1+2)*3 一比，优先级怎么决定树长啥样，一下就懂。",
            0, {}, 20, ActivityType::TOY, "tree"
        },
        LearningActivity{
            "vm-sandbox", "💻 VM 栈沙盒",
            "📍 自己动手 push/pop，模拟一台栈式虚拟机。试过几次就明白：每条指令其实都只在拨弄栈顶那几个数。",
            0, {}, 15, ActivityType::SANDBOX, "stack"
        },
        // P1-F2/F fix: 浏览型面板轻量活动——进入即完成，避免"永远未完成"误导
        LearningActivity{
            "visited-glossary", "📚 参考资料：术语表",
            "📍 一份按字母排好、能搜索的核心术语表（NaN-boxing、COW、upvalue、SSA、lowering 都在里头）。卡壳时随时翻。",
            0, {}, 5, ActivityType::TOY, "book"
        },
        LearningActivity{
            "visited-pipeline", "🔗 参考资料：编译管线可视化",
            "📍 把整条编译管线画给你看：Lexer → Parser → AST → Compiler → Bytecode → VM。一条龙，不迷路。",
            0, {}, 5, ActivityType::TOY, "link"
        },
        LearningActivity{
            "visited-memory-model", "🧠 参考资料：内存模型动画",
            "📍 内存长啥样？这里用动画讲 NaN-boxing 的 8 字节 Value 布局、mark-sweep 垃圾回收的分步过程，还附 6 种堆对象的说明。",
            0, {}, 5, ActivityType::TOY, "brain"
        },
        LearningActivity{
            "visited-bytecode-trace", "📡 参考资料：字节码追踪",
            "📍 把 StackVM 的字节码一条条追着看，取指、解码、执行这个循环到底怎么转，一目了然。",
            0, {}, 5, ActivityType::TOY, "signal"
        },
        LearningActivity{
            "visited-exception-flow", "⚡ 参考资料：异常流可视化",
            "📍 try/catch/finally 出错后怎么一路往上冒？这里有路径图解、8 个场景和 6 个传播阶段帮你捋顺。",
            0, {}, 5, ActivityType::TOY, "bolt"
        },
        LearningActivity{
            "visited-closure-inspector", "🔒 参考资料：闭包检查器",
            "📍 闭包的 upvalue 从生到死画给你看，配 8 个捕获场景，再比一比 by-ref / by-value / heap-escape 三种结局。",
            0, {}, 5, ActivityType::TOY, "lock"
        },

        // ==================== 阶段一：编译前端 ====================
        LearningActivity{
            "lab-01", "📖 实验 1：词法分析",
            "📍 词法分析这一关：亲手把源码切成带类型的 Token。看清 Lexer 的状态机是怎么扫的、为啥认「最长匹配」，注释又是怎么被单独剔掉的。",
            1, {"token-puzzle"}, 30, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-02", "📖 实验 2：递归下降解析",
            "📍 语法分析这一关：用递归下降把 Token 串拼成 AST。拿 1+2*3 和 (1+2)*3 对照，优先级和左/右结合性怎么塑造树形，一下就有感觉了。",
            1, {"lab-01", "ast-toy"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "syntax-explorer", "📜 语法探索器",
            "📍 10 条核心语法产生式，每条配 EBNF、人话翻译和可运行的样例代码。想查语法就来这。",
            1, {"lab-01"}, 20, ActivityType::TOY, "book"
        },
        LearningActivity{
            "op-priority-challenge", "🏆 挑战：运算符优先级链",
            "📍 自己攒一条 a + b * c - d / e 这样的优先级链，先猜猜 AST 会长成什么树，再验证。",
            1, {"lab-02"}, 15, ActivityType::CHALLENGE, "trophy"
        },

        // ==================== 阶段二：执行引擎 ====================
        LearningActivity{
            "lab-03", "📖 实验 3：树遍历解释器",
            "📍 语义分析 + 解释执行：看编译器怎么查作用域、类型、定义和参数个数，再用 Visitor 模式遍历 AST，把程序真正跑起来。",
            2, {"lab-02", "vm-sandbox"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-04", "📖 实验 4：栈式字节码 VM",
            "📍 目标代码生成：把 AST 翻成栈式字节码，再在 StackVM 上单步跑。每条指令怎么 push/pop 操作数栈，看得清清楚楚。",
            2, {"lab-03"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-05", "📖 实验 5：寄存器式 VM + IR",
            "📍 中间代码 + 优化：弄懂 IR 三地址码和 SSA 虚拟寄存器，看常量折叠、复制传播、死代码消除怎么给代码瘦身，最后 lowering 到寄存器式 VM。",
            2, {"lab-04"}, 75, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "backend-compare", "⚖️ 三后端对比面板",
            "📍 同一份源码，依次扔给 Interpreter、StackVM、RegisterVM 跑一遍，输出和耗时摆在一起比。",
            2, {"lab-04"}, 15, ActivityType::SANDBOX, "compare"
        },

        // ==================== 阶段三：深入理解 ====================
        LearningActivity{
            "lab-06", "📖 实验 6：三后端一致性",
            "📍 三后端一致性：亲手验证三条路径对同一份源码结果必须一样。整数除法截断、and-or 短路这些统一语义，到底意味着什么。",
            3, {"lab-05", "backend-compare"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-07", "📖 实验 7：内存模型",
            "📍 内存模型：搞懂 NaN-boxing 的 8 字节 Value、COW 写时复制和 mark-sweep GC。共享的数组为什么等到真要写时才分家，看完就明白。",
            3, {"lab-05"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "ir-transform", "⚙️ IR 变换过程面板",
            "📍 AST 降成 IR，再走 3 个优化 pass（常量折叠 / DCE / 复制传播），而且能一步步回放给你看。",
            3, {"lab-05"}, 30, ActivityType::TOY, "git-commit"
        },
        LearningActivity{
            "profile-dashboard", "📊 性能剖析仪表盘",
            "📍 6 个性能场景，三后端耗时摆一排，再列出指令计数 Top 10 的热点 OpCode。瓶颈在哪，一眼瞅见。",
            3, {"lab-06"}, 20, ActivityType::SANDBOX, "chart"
        },

        // ==================== 阶段四：实战训练 ====================
        LearningActivity{
            "lab-08", "📖 实验 8：Bug 狩猎方法论",
            "📍 Bug 狩猎方法论实战：读真实项目历史 Bug 的根因分析，练出「预测→对比→定位」的排错直觉，还有三后端对称审计的眼光。",
            4, {"lab-06"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "bug-hunt-beginner", "🐛 Bug 狩猎：入门级",
            "📍 5 道阅读题：预测输出、定位报错行、猜格式化结果、比性能、推内存。先拿这几道热热身。",
            4, {"lab-08"}, 30, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "bug-hunt-intermediate", "🐛 Bug 狩猎：进阶级",
            "📍 4 道进阶题，全来自真实项目 Bug：默认参数、RegisterVM、REPL、Formatter。动真格的了。",
            4, {"bug-hunt-beginner", "lab-07"}, 60, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "bug-hunt-expert", "🐛 Bug 狩猎：专家级",
            "📍 6 道硬核题：IR 栈泄漏(P0)、常量池去重(P1)、闭包 upvalue(P1)、调试器行号(P1)、嵌套索引栈泄漏(P1)、模块路径(P2)。敢不敢挑战？",
            4, {"bug-hunt-intermediate", "ir-transform"}, 90, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "freeform-project", "🎯 自由项目：实现一个新特性",
            "📍 自由发挥：给 MiniLang 设计一个新特性，比如 switch 语句、字符串模板、模式匹配——随你折腾。",
            4, {"bug-hunt-intermediate"}, 180, ActivityType::FREEFORM, "rocket"
        },
    };
    return kActivities;
}

const LearningActivity* LearningPathData::findById(const std::string& id) {
    const auto& all = activities();
    for (const auto& a : all) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

std::vector<const LearningActivity*> LearningPathData::byStage(int stage) {
    std::vector<const LearningActivity*> result;
    if (stage < 0 || stage > 4) return result;
    const auto& all = activities();
    for (const auto& a : all) {
        if (a.stage == stage) result.push_back(&a);
    }
    return result;
}
