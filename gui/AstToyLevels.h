// ============================================================
// AstToyLevels.h — AST 搭建玩具题目数据定义（功能 4）
// ------------------------------------------------------------
// 通过拖拽/点击组装 AST 节点，让学习者理解「AST 是代码的结构化表示」。
// 6 道题由浅入深，核心教学时刻为第 3 题 vs 第 4 题——同样的数字和运算符，
// 仅括号不同就导致完全不同的树结构（优先级 vs 括号改变结构）。
//
// 设计原则：
//   - 本文件仅含数据结构与静态数据接口声明，不依赖 IdeController.h
//     或任何引擎层文件——与 BugHuntLibrary.cpp / LearningPathData.cpp
//     的设计模式一致，便于测试目标独立链接。
//   - AstToyLevels.cpp 仅依赖 Qt6::Core（QString），不依赖 Qt6::Widgets，
//     可安全加入 minilang_tests 测试目标（测试目标不链接 app/ 下文件链）。
//   - 静态数据嵌入 .cpp，与 BugHuntLibrary.cpp 模式一致。
//
// 题目设计：
//   1. 42            — 最简：只有一个叶子节点
//   2. 1 + 2         — 二元运算：根 + 两个叶子
//   3. 1 + 2 * 3     — 优先级：* 在 + 下面
//   4. (1 + 2) * 3   — 括号改变结构：+ 在 * 下面
//   5. print(x)      — 函数调用树
//   6. var x = 1 + 2;— 完整语句（VarDecl 包含 initializer）
//
// 标准答案 TreeNode 标签约定：
//   - 二元运算：  "BinaryOp:+" / "BinaryOp:-" / "BinaryOp:*" / "BinaryOp:/"
//   - 整数字面量："Number:<value>"（如 "Number:42"）
//   - 标识符引用："Identifier:<name>"（如 "Identifier:x"）
//   - print 调用： "Print"
//   - 变量声明：  "VarDecl:<name>"（如 "VarDecl:x"）
// ============================================================

#pragma once

#include <string>
#include <vector>

// ============================================================
// AstToyLevel — 单个 AST 搭建题目定义
// ============================================================
struct AstToyLevel {
    int level = 0;                // 题目序号 1-6
    std::string targetExpression; // 目标表达式（如 "1 + 2 * 3"）
    std::string teachingPoint;    // 教学点（简短一句话，说明本题核心）
    std::string hint;             // 操作提示
    int difficulty = 1;           // 难度 1-3（对应 ⭐ / ⭐⭐ / ⭐⭐⭐）

    // --------------------------------------------------------
    // TreeNode — 标准答案的树结构表示
    // --------------------------------------------------------
    // 第一个节点是根，children 是按代码顺序排列的子节点。
    // 叶子节点 children 为空 vector。
    // label 命名约定见文件头注释。
    struct TreeNode {
        std::string label;              // 节点标签
        std::vector<TreeNode> children; // 子节点（按代码顺序）

        // 默认构造：空节点
        TreeNode() = default;
        // 便捷构造：叶子节点
        explicit TreeNode(std::string l) : label(std::move(l)) {}
        // 完整构造
        TreeNode(std::string l, std::vector<TreeNode> c) : label(std::move(l)), children(std::move(c)) {}
    };

    TreeNode answer; // 标准答案
};

// ============================================================
// AstToyLibrary — AST 搭建题目静态数据
// ------------------------------------------------------------
// 所有题目数据嵌入 .cpp 文件，与 BugHuntLibrary 模式一致。
// 通过 levels() 接口提供只读访问，首次调用时初始化、后续
// 调用零开销。
// ============================================================
class AstToyLibrary {
public:
    /// 返回所有 AST 搭建题目（按 level 1-6 排序）
    static const std::vector<AstToyLevel>& levels();

    /// 根据 level 编号查找题目，找不到返回 nullptr
    static const AstToyLevel* findByLevel(int level);

    /// 题目总数（6）
    static int levelCount() { return 6; }
};
