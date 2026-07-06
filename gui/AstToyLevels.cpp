// ============================================================
// AstToyLevels.cpp — AST 搭建玩具题目静态数据实现（功能 4）
// ------------------------------------------------------------
// 6 道题由浅入深，覆盖叶子节点 → 二元运算 → 优先级 → 括号改变结构 →
// 函数调用 → 完整语句。核心教学时刻为第 3 题 vs 第 4 题：
//   - "1 + 2 * 3"   →  根是 +，右子树是 *
//   - "(1 + 2) * 3" →  根是 *，左子树是 +
// 同样的数字和运算符，仅括号不同就导致完全不同的树结构。
//
// 设计原则：
//   - 仅依赖 Qt6::Core（实际仅依赖标准库 + AstToyLevels.h），不依赖
//     Qt6::Widgets / IdeController.h，可被 minilang_tests 测试目标安全链接
//   - 静态数据嵌入本文件，与 BugHuntLibrary.cpp / LearningPathData.cpp
//     设计模式一致
//   - 标签命名约定见 AstToyLevels.h 文件头注释
// ============================================================

#include "gui/AstToyLevels.h"

// ============================================================
// AstToyLibrary 实现
// ============================================================
const std::vector<AstToyLevel>& AstToyLibrary::levels() {
    static const std::vector<AstToyLevel> kLevels = {
        // ==================== 题目 1：42 — 最简叶子节点 ====================
        AstToyLevel{
            1,
            "42",
            "最简 AST：单个表达式只有 1 个叶子节点",
            "点击「Number」按钮，输入 42，然后点击「检查」",
            1,
            AstToyLevel::TreeNode{"Number:42"}
        },

        // ==================== 题目 2：1 + 2 — 二元运算 ====================
        AstToyLevel{
            2,
            "1 + 2",
            "二元运算：根节点是运算符 +，左右各一个叶子",
            "先放 Add 节点作为根，选中它，再依次添加 Number:1 和 Number:2 作为子节点",
            1,
            AstToyLevel::TreeNode{
                "BinaryOp:+",
                {
                    AstToyLevel::TreeNode{"Number:1"},
                    AstToyLevel::TreeNode{"Number:2"},
                }
            }
        },

        // ==================== 题目 3：1 + 2 * 3 — 优先级 ====================
        // 根是 +，右子树是 * (* 优先级更高，先结合)
        AstToyLevel{
            3,
            "1 + 2 * 3",
            "运算符优先级：* 比 + 优先级高，所以 * 在 + 下面（先结合）",
            "根是 +，左子树是 Number:1，右子树是 * 节点（其下是 Number:2 和 Number:3）",
            2,
            AstToyLevel::TreeNode{
                "BinaryOp:+",
                {
                    AstToyLevel::TreeNode{"Number:1"},
                    AstToyLevel::TreeNode{
                        "BinaryOp:*",
                        {
                            AstToyLevel::TreeNode{"Number:2"},
                            AstToyLevel::TreeNode{"Number:3"},
                        }
                    },
                }
            }
        },

        // ==================== 题目 4：(1 + 2) * 3 — 括号改变结构 ====================
        // 根是 *，左子树是 + (括号强制 + 先结合)
        AstToyLevel{
            4,
            "(1 + 2) * 3",
            "括号改变结构：() 强制 + 先结合，所以 + 在 * 下面",
            "根是 *，左子树是 + 节点（其下是 Number:1 和 Number:2），右子树是 Number:3",
            2,
            AstToyLevel::TreeNode{
                "BinaryOp:*",
                {
                    AstToyLevel::TreeNode{
                        "BinaryOp:+",
                        {
                            AstToyLevel::TreeNode{"Number:1"},
                            AstToyLevel::TreeNode{"Number:2"},
                        }
                    },
                    AstToyLevel::TreeNode{"Number:3"},
                }
            }
        },

        // ==================== 题目 5：print(x) — 函数调用 ====================
        AstToyLevel{
            5,
            "print(x)",
            "函数调用：根是 Print 节点，子节点是参数表达式",
            "根是 Print，子节点是 Identifier:x",
            2,
            AstToyLevel::TreeNode{
                "Print",
                {
                    AstToyLevel::TreeNode{"Identifier:x"},
                }
            }
        },

        // ==================== 题目 6：var x = 1 + 2; — 完整语句 ====================
        AstToyLevel{
            6,
            "var x = 1 + 2;",
            "完整语句：VarDecl 节点包含变量名 + initializer 表达式",
            "根是 VarDecl:x，子节点是 initializer 表达式 +（其下是 Number:1 和 Number:2）",
            3,
            AstToyLevel::TreeNode{
                "VarDecl:x",
                {
                    AstToyLevel::TreeNode{
                        "BinaryOp:+",
                        {
                            AstToyLevel::TreeNode{"Number:1"},
                            AstToyLevel::TreeNode{"Number:2"},
                        }
                    },
                }
            }
        },
    };
    return kLevels;
}

const AstToyLevel* AstToyLibrary::findByLevel(int level) {
    const auto& ls = levels();
    for (const auto& l : ls) {
        if (l.level == level) return &l;
    }
    return nullptr;
}
