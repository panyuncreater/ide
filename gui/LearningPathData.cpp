// ============================================================
// LearningPathData.cpp — 学习路径活动静态数据实现（功能 6）
// ------------------------------------------------------------
// 4 阶段共 20 个学习活动，覆盖 welcome 导览 / lab-01~08 / 4 个游戏化
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
            "welcome", "Welcome 导览",
            "首次启动 IDE 的 3 步交互式导览，建立「代码→程序」的直觉认知",
            0, {}, 5, ActivityType::TOY, "wave"
        },
        LearningActivity{
            "code-journey", "代码生命旅程动画",
            "30 秒动画展示 print(1+2*3) 从源码到结果的全过程：词法→语法→字节码→执行",
            0, {}, 5, ActivityType::TOY, "rocket"
        },
        LearningActivity{
            "token-puzzle", "Token 拼图游戏",
            "5 关由浅入深的 Token 排序游戏，理解「词法分析就是切分字符流」",
            0, {}, 15, ActivityType::PUZZLE, "puzzle-piece"
        },
        LearningActivity{
            "ast-toy", "AST 搭建玩具",
            "6 道题拖拽组装 AST 节点，通过 1+2*3 vs (1+2)*3 体验优先级对树结构的影响",
            0, {}, 20, ActivityType::TOY, "tree"
        },
        LearningActivity{
            "vm-sandbox", "VM 栈沙盒",
            "手动 push/pop 操作模拟栈式 VM，理解「每条指令操作栈顶」",
            0, {}, 15, ActivityType::SANDBOX, "stack"
        },

        // ==================== 阶段一：编译前端 ====================
        LearningActivity{
            "lab-01", "实验 1：词法分析",
            "从字符到 Token——理解词法分析器的状态机设计",
            1, {"token-puzzle"}, 30, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-02", "实验 2：递归下降解析",
            "构造 AST——理解优先级与结合性的递归实现",
            1, {"lab-01", "ast-toy"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "syntax-explorer", "语法探索器",
            "10 条核心产生式 EBNF + 自然语言 + 可运行样例代码",
            1, {"lab-01"}, 20, ActivityType::TOY, "book"
        },
        LearningActivity{
            "op-priority-challenge", "挑战：运算符优先级链",
            "自创一条 a + b * c - d / e 的优先级链，预测 AST 结构",
            1, {"lab-02"}, 15, ActivityType::CHALLENGE, "trophy"
        },

        // ==================== 阶段二：执行引擎 ====================
        LearningActivity{
            "lab-03", "实验 3：树遍历解释器",
            "Visitor 模式递归遍历 AST 求值——理解解释器执行模型",
            2, {"lab-02", "vm-sandbox"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-04", "实验 4：栈式字节码 VM",
            "编译到字节码并执行——理解 push/pop 操作数栈模型",
            2, {"lab-03"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-05", "实验 5：寄存器式 VM + IR",
            "IR 三地址码 lowering 到 RegBytecode——理解虚拟寄存器与栈式 VM 的取舍",
            2, {"lab-04"}, 75, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "backend-compare", "三后端对比面板",
            "同一源码顺序运行 Interpreter / StackVM / RegisterVM，对比输出与耗时",
            2, {"lab-04"}, 15, ActivityType::SANDBOX, "compare"
        },

        // ==================== 阶段三：深入理解 ====================
        LearningActivity{
            "lab-06", "实验 6：三后端一致性",
            "整数除法截断 / and-or 短路 / 类型注解——验证三后端语义等价",
            3, {"lab-05", "backend-compare"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "lab-07", "实验 7：内存模型",
            "NaN-boxing 8 字节 Value / COW 写时复制 / GcManager mark-sweep",
            3, {"lab-05"}, 60, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "ir-transform", "IR 变换过程面板",
            "AST → IR lowering + 3 个优化 pass（常量折叠 / DCE / 复制传播）+ IR 优化逐步回放",
            3, {"lab-05"}, 30, ActivityType::TOY, "git-commit"
        },
        LearningActivity{
            "profile-dashboard", "性能剖析仪表盘",
            "6 个性能场景三后端耗时 + 指令计数 Top 10 热点 OpCode",
            3, {"lab-06"}, 20, ActivityType::SANDBOX, "chart"
        },

        // ==================== 阶段四：实战训练 ====================
        LearningActivity{
            "lab-08", "实验 8：Bug 狩猎方法论",
            "阅读项目历史真实 Bug 的根因分析，理解 Bug 狩猎的方法论与三后端一致性视角",
            4, {"lab-06"}, 45, ActivityType::LAB, "lab"
        },
        LearningActivity{
            "bug-hunt-beginner", "Bug 狩猎：入门级",
            "5 道阅读理解型题目——预测输出、定位异常行、格式化预测、性能直觉、内存推理",
            4, {"lab-08"}, 30, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "bug-hunt-intermediate", "Bug 狩猎：进阶级",
            "4 道基于项目历史真实 Bug 的训练题（默认参数 / RegisterVM / REPL / Formatter）",
            4, {"bug-hunt-beginner", "lab-07"}, 60, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "bug-hunt-expert", "Bug 狩猎：专家级",
            "6 道深度题：IR 栈泄漏 P0 / 常量池去重 P1 / 闭包 upvalue P1 / 调试器行号 P1 / 嵌套索引栈泄漏 P1 / 模块路径 P2",
            4, {"bug-hunt-intermediate", "ir-transform"}, 90, ActivityType::CHALLENGE, "bug"
        },
        LearningActivity{
            "freeform-project", "自由项目：实现一个新特性",
            "自主设计一个 MiniLang 新特性（如 switch 语句 / 字符串模板 / 模式匹配）",
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
