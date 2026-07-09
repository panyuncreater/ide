// ============================================================
// TestAstToyAudit.cpp — AST 搭建玩具题目库数据完整性测试
// ------------------------------------------------------------
// 功能 4（MINILANG_IDE_IMPROVEMENT_PLAN.md）：覆盖 AstToyLibrary
// （6 道题：42 / 1+2 / 1+2*3 / (1+2)*3 / print(x) / var x=1+2;）。
//
// 验证维度：
//   1. 题目数 == 6
//   2. level 字段 1-6 连续
//   3. 每题 targetExpression 非空
//   4. 每题 teachingPoint 非空
//   5. 每题 hint 非空
//   6. difficulty 在 1-3 范围
//   7. 题目 1 的 answer 只有 1 个节点（叶子）
//   8. 题目 2 的 answer 根节点 label 含 "+"，2 个子节点
//   9. 题目 3 的 answer 根节点 label 含 "+"，右子树根 label 含 "*"
//  10. 题目 4 的 answer 根节点 label 含 "*"，左子树根 label 含 "+"
//  11. 题目 3 和题目 4 的 targetExpression 都含 "1"、"2"、"3"，但 answer 树结构不同
//  12. 题目 5 的 answer 根节点 label 含 "print" 或 "Print"
//  13. 题目 6 的 answer 根节点 label 含 "var" 或 "VarDecl"
//
// 测试框架：GoogleTest（项目已集成，doctest 不可用，见 TestErrorHintEngine.cpp）
// 依赖约束：仅链接 minilang_core + AstToyLevels.cpp，不依赖 IdeController.h
//
// 注：AstToyLevels.cpp 为独立编译单元（仅依赖 Qt6::Core，不依赖
//     Qt6::Widgets / IdeController），已在 tests/CMakeLists.txt 中显式
//     加入测试目标。
// ============================================================

#include <gtest/gtest.h>

#include "gui/AstToyLevels.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <algorithm>
#include <set>
#include <string>

// ============================================================
// 辅助：判断 label 是否包含某子串（大小写敏感）
// ============================================================
static bool labelContains(const std::string& label, const std::string& sub) {
    return label.find(sub) != std::string::npos;
}

// ============================================================
// 辅助：判断 label 是否包含某子串（大小写不敏感）
// ============================================================
static bool labelContainsCI(const std::string& label, const std::string& sub) {
    if (sub.empty()) return true;
    if (label.size() < sub.size()) return false;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    return toLower(label).find(toLower(sub)) != std::string::npos;
}

// ============================================================
// 辅助：递归计算 TreeNode 节点总数
// ============================================================
static int treeNodeCount(const AstToyLevel::TreeNode& node) {
    int n = 1;
    for (const auto& c : node.children) n += treeNodeCount(c);
    return n;
}

// ============================================================
// 辅助：递归判断 TreeNode 是否含某子串 label
// ============================================================
static bool treeContainsLabel(const AstToyLevel::TreeNode& node,
                              const std::string& sub) {
    if (labelContainsCI(node.label, sub)) return true;
    for (const auto& c : node.children) {
        if (treeContainsLabel(c, sub)) return true;
    }
    return false;
}

// ============================================================
// 辅助：递归比较两棵树是否拓扑相同（label + children 顺序）
// ============================================================
static bool treeEquals(const AstToyLevel::TreeNode& a,
                       const AstToyLevel::TreeNode& b) {
    if (a.label != b.label) return false;
    if (a.children.size() != b.children.size()) return false;
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (!treeEquals(a.children[i], b.children[i])) return false;
    }
    return true;
}

// ============================================================
// AstToyLibraryAudit — 数据完整性
// ============================================================

TEST(AstToyLibraryAudit, HasExpectedLevelCount) {
    const auto& ls = AstToyLibrary::levels();
    EXPECT_EQ(ls.size(), 6u);
}

TEST(AstToyLibraryAudit, LevelsAreSequentialOneToSix) {
    const auto& ls = AstToyLibrary::levels();
    ASSERT_EQ(ls.size(), 6u);
    for (size_t i = 0; i < ls.size(); ++i) {
        EXPECT_EQ(ls[i].level, static_cast<int>(i + 1))
            << "level 字段应为 1-6 连续";
    }
}

TEST(AstToyLibraryAudit, TargetExpressionsNonEmpty) {
    const auto& ls = AstToyLibrary::levels();
    for (const auto& lv : ls) {
        EXPECT_FALSE(lv.targetExpression.empty())
            << "level " << lv.level << ": targetExpression 不能为空";
    }
}

TEST(AstToyLibraryAudit, TeachingPointsNonEmpty) {
    const auto& ls = AstToyLibrary::levels();
    for (const auto& lv : ls) {
        EXPECT_FALSE(lv.teachingPoint.empty())
            << "level " << lv.level << ": teachingPoint 不能为空";
    }
}

TEST(AstToyLibraryAudit, HintsNonEmpty) {
    const auto& ls = AstToyLibrary::levels();
    for (const auto& lv : ls) {
        EXPECT_FALSE(lv.hint.empty())
            << "level " << lv.level << ": hint 不能为空";
    }
}

TEST(AstToyLibraryAudit, DifficultyInRange) {
    const auto& ls = AstToyLibrary::levels();
    for (const auto& lv : ls) {
        EXPECT_GE(lv.difficulty, 1)
            << "level " << lv.level << ": difficulty 至少为 1";
        EXPECT_LE(lv.difficulty, 3)
            << "level " << lv.level << ": difficulty 至多为 3";
    }
}

TEST(AstToyLibraryAudit, Level1AnswerIsSingleLeafNode) {
    // 题目 1：42 — 最简：只有一个叶子节点
    const auto* lv = AstToyLibrary::findByLevel(1);
    ASSERT_NE(lv, nullptr);
    EXPECT_EQ(treeNodeCount(lv->answer), 1)
        << "题目 1 的 answer 应该只有 1 个节点";
    EXPECT_TRUE(lv->answer.children.empty())
        << "题目 1 的 answer 应该是叶子节点（无子节点）";
    EXPECT_FALSE(lv->answer.label.empty())
        << "题目 1 的 answer label 不能为空";
}

TEST(AstToyLibraryAudit, Level2AnswerRootIsAddWithTwoChildren) {
    // 题目 2：1 + 2 — 二元运算：根 + 两个叶子
    const auto* lv = AstToyLibrary::findByLevel(2);
    ASSERT_NE(lv, nullptr);
    EXPECT_TRUE(labelContains(lv->answer.label, "+"))
        << "题目 2 的 answer 根节点 label 应含 '+'，实际为: " << lv->answer.label;
    EXPECT_EQ(lv->answer.children.size(), 2u)
        << "题目 2 的 answer 根节点应有 2 个子节点";
}

TEST(AstToyLibraryAudit, Level3AnswerRootAddRightSubtreeMul) {
    // 题目 3：1 + 2 * 3 — 优先级：根 + ，右子树 *
    const auto* lv = AstToyLibrary::findByLevel(3);
    ASSERT_NE(lv, nullptr);
    EXPECT_TRUE(labelContains(lv->answer.label, "+"))
        << "题目 3 的 answer 根节点 label 应含 '+'，实际为: " << lv->answer.label;
    ASSERT_GE(lv->answer.children.size(), 2u)
        << "题目 3 的 answer 根节点至少 2 个子节点";
    // 右子树（第二个子节点）的根 label 应含 "*"
    const auto& rightChild = lv->answer.children[1];
    EXPECT_TRUE(labelContains(rightChild.label, "*"))
        << "题目 3 的 answer 右子树根 label 应含 '*'，实际为: " << rightChild.label;
}

TEST(AstToyLibraryAudit, Level4AnswerRootMulLeftSubtreeAdd) {
    // 题目 4：(1 + 2) * 3 — 括号改变结构：根 * ，左子树 +
    const auto* lv = AstToyLibrary::findByLevel(4);
    ASSERT_NE(lv, nullptr);
    EXPECT_TRUE(labelContains(lv->answer.label, "*"))
        << "题目 4 的 answer 根节点 label 应含 '*'，实际为: " << lv->answer.label;
    ASSERT_GE(lv->answer.children.size(), 2u)
        << "题目 4 的 answer 根节点至少 2 个子节点";
    // 左子树（第一个子节点）的根 label 应含 "+"
    const auto& leftChild = lv->answer.children[0];
    EXPECT_TRUE(labelContains(leftChild.label, "+"))
        << "题目 4 的 answer 左子树根 label 应含 '+'，实际为: " << leftChild.label;
}

TEST(AstToyLibraryAudit, Level3AndLevel4SameNumbersButDifferentTree) {
    // 题目 3 vs 题目 4：targetExpression 都含 "1"、"2"、"3"，但 answer 树结构不同
    const auto* lv3 = AstToyLibrary::findByLevel(3);
    const auto* lv4 = AstToyLibrary::findByLevel(4);
    ASSERT_NE(lv3, nullptr);
    ASSERT_NE(lv4, nullptr);

    // 两题的 targetExpression 都应含数字 1/2/3
    for (const char* num : {"1", "2", "3"}) {
        EXPECT_NE(lv3->targetExpression.find(num), std::string::npos)
            << "题目 3 的 targetExpression 应含 '" << num << "'";
        EXPECT_NE(lv4->targetExpression.find(num), std::string::npos)
            << "题目 4 的 targetExpression 应含 '" << num << "'";
    }

    // 两题的 answer 树结构应不同（拓扑不一致）
    EXPECT_FALSE(treeEquals(lv3->answer, lv4->answer))
        << "题目 3 与题目 4 的 answer 树结构应不同（仅括号差异应导致不同 AST）";

    // 同时根节点 label 应不同：题 3 是 +，题 4 是 *
    EXPECT_NE(lv3->answer.label, lv4->answer.label)
        << "题目 3 与题目 4 的根节点 label 应不同";
}

TEST(AstToyLibraryAudit, Level5AnswerRootIsPrint) {
    // 题目 5：print(x) — 函数调用：根 Print
    const auto* lv = AstToyLibrary::findByLevel(5);
    ASSERT_NE(lv, nullptr);
    // label 含 "print" 或 "Print"（大小写不敏感即可）
    EXPECT_TRUE(labelContainsCI(lv->answer.label, "print"))
        << "题目 5 的 answer 根节点 label 应含 'print' 或 'Print'，实际为: "
        << lv->answer.label;
}

TEST(AstToyLibraryAudit, Level6AnswerRootIsVarDecl) {
    // 题目 6：var x = 1 + 2; — 完整语句：根 VarDecl
    const auto* lv = AstToyLibrary::findByLevel(6);
    ASSERT_NE(lv, nullptr);
    // label 含 "var" 或 "VarDecl"（大小写不敏感即可）
    bool hasVar = labelContainsCI(lv->answer.label, "var") ||
                  labelContainsCI(lv->answer.label, "VarDecl");
    EXPECT_TRUE(hasVar)
        << "题目 6 的 answer 根节点 label 应含 'var' 或 'VarDecl'，实际为: "
        << lv->answer.label;
    // VarDecl 应至少有 1 个子节点（initializer 表达式）
    EXPECT_GE(lv->answer.children.size(), 1u)
        << "题目 6 的 VarDecl 应至少有 1 个子节点（initializer）";
}

// ============================================================
// 附加测试：拓扑结构与核心教学时刻验证
// ============================================================

TEST(AstToyLibraryAudit, Level3MulIsUnderAdd) {
    // 教学点验证：题目 3 中 * 必须在 + 下面（先结合，优先级更高）
    const auto* lv = AstToyLibrary::findByLevel(3);
    ASSERT_NE(lv, nullptr);
    // 根是 +，且 * 必须在子树中
    EXPECT_TRUE(labelContains(lv->answer.label, "+"));
    EXPECT_TRUE(treeContainsLabel(lv->answer, "*"))
        << "题目 3 的 answer 树中应存在 * 节点（在 + 之下）";
}

TEST(AstToyLibraryAudit, Level4AddIsUnderMul) {
    // 教学点验证：题目 4 中 + 必须在 * 下面（括号强制 + 先结合）
    const auto* lv = AstToyLibrary::findByLevel(4);
    ASSERT_NE(lv, nullptr);
    // 根是 *，且 + 必须在子树中
    EXPECT_TRUE(labelContains(lv->answer.label, "*"));
    EXPECT_TRUE(treeContainsLabel(lv->answer, "+"))
        << "题目 4 的 answer 树中应存在 + 节点（在 * 之下）";
}

TEST(AstToyLibraryAudit, FindByLevelReturnsCorrectInstance) {
    // findByLevel 应返回与 levels() 中相同实例
    const auto& ls = AstToyLibrary::levels();
    for (int i = 1; i <= 6; ++i) {
        const auto* p = AstToyLibrary::findByLevel(i);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->level, i);
        // 与 levels() 中的对应条目指针一致
        EXPECT_EQ(p, &ls[i - 1]);
    }
    // 越界返回 nullptr
    EXPECT_EQ(AstToyLibrary::findByLevel(0), nullptr);
    EXPECT_EQ(AstToyLibrary::findByLevel(7), nullptr);
    EXPECT_EQ(AstToyLibrary::findByLevel(-1), nullptr);
}

TEST(AstToyLibraryAudit, LevelCountConstantIsSix) {
    EXPECT_EQ(AstToyLibrary::levelCount(), 6);
}

// ============================================================
// ROUND56 fix: 验证「用真实 Parser 验证」按钮对所有题目都能成功解析
// 复刻 AstBuilderToyPanel::onVerifyWithRealParser 的包装逻辑
// 用户报告：点「用真实 Parser 验证」会报错，按钮失效
// ============================================================
TEST(AstToyLibraryAudit, VerifyWithRealParserAllLevelsParse) {
    const auto& ls = AstToyLibrary::levels();
    ASSERT_FALSE(ls.empty());
    for (const auto& lv : ls) {
        const std::string& expr = lv.targetExpression;
        ASSERT_FALSE(expr.empty());
        // 复刻 onVerifyWithRealParser 的 isStatement 判定
        bool isStatement = (expr.back() == ';') || expr.find("var ") == 0 || expr.find("print(") == 0;
        std::string source;
        if (isStatement) {
            source = expr;
            if (source.back() != ';')
                source += ";";
        } else {
            source = "var _toy_tmp = " + expr + ";";
        }
        Lexer lexer;
        auto tokens = lexer.scan(source);
        ASSERT_FALSE(lexer.getDiagnostics().hasErrors())
            << "level " << lv.level << " lexing failed for source: " << source;
        Parser parser;
        auto ast = parser.parse(tokens);
        // 收集诊断
        std::string errs;
        for (const auto& d : parser.getDiagnostics().all()) {
            if (d.isError())
                errs += d.format() + "\n";
        }
        EXPECT_EQ(errs, "") << "level " << lv.level << " parsing failed for source: " << source;
        EXPECT_NE(ast, nullptr) << "level " << lv.level << " parser returned null AST for source: " << source;
    }
}
