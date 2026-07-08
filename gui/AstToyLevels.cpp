// ============================================================
// AstToyLevels.cpp — AST 搭建玩具题目静态数据实现（功能 4）
// ------------------------------------------------------------
// 6 道题由浅入深，覆盖叶子节点 → 二元运算 → 优先级 → 括号改变结构 →
// 函数调用 → 完整语句。核心教学时刻为第 3 题 vs 第 4 题：
//   - "1 + 2 * 3"   →  根是 +，右子树是 *
//   - "(1 + 2) * 3" →  根是 *，左子树是 +
// 同样的数字和运算符，仅括号不同就导致完全不同的树结构。
//
// 教学理念：抽象语法树（AST）是"代码的结构化骨架"。源码是一维的
// 文本，AST 是二维的树——运算符当根、操作数当树枝。搭建 AST 的过程，
// 就是让学习者亲手"看见"优先级与括号如何变成树的形状。
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
            "🌳 最简单的 AST 像一张「单人卡片」——没有父母也没有子女，自己就是一个完整的节点。\n"
            "• 一个字面量（数字 42）就是一棵「只有叶子、没有枝干」的树。\n"
            "• 节点：Number:42（叶子，children 为空）。\n"
            "• 联想：再复杂的程序，最底层也都是这样一颗颗「原子节点」拼出来的。",
            "💡 点「Number」按钮，输入 42，确认它成为树中唯一节点，再点「检查」。\n"
            "   目标：让树里恰好只有 1 个节点，且它是一片叶子。",
            1,
            AstToyLevel::TreeNode{"Number:42"}
        },

        // ==================== 题目 2：1 + 2 — 二元运算 ====================
        AstToyLevel{
            2,
            "1 + 2",
            "🌳 二元运算的 AST 像一棵小树——树根是运算符 +，伸出两根「树枝」分别挂着 1 和 2。\n"
            "• Parser 看到 + 时，会先把左边解析成一棵子树挂到左枝，右边解析成另一棵挂到右枝。\n"
            "• 结构：\n"
            "    BinaryOp:+\n"
            "     ├─ Number:1\n"
            "     └─ Number:2\n"
            "• 联想：运算符是「家长」，两个操作数是「孩子」，家长永远在上方。",
            "💡 先放 Add(+) 节点当根；选中它，添加左子节点 Number:1、右子节点 Number:2。\n"
            "   检查要点：根节点 label 含 '+'，且恰好有 2 个子节点。",
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
            "🌳 优先级 = 树的高度：* 比 + 先算，所以在树里 * 长在 + 的「下方」（更靠近叶子）。\n"
            "• 黄金法则：树根永远是「最后算」的运算符。谁后算，谁就高高在上当根。\n"
            "• 结构：\n"
            "    BinaryOp:+\n"
            "     ├─ Number:1\n"
            "     └─ BinaryOp:*        ← 乘法优先级高，先结合，沉到下层\n"
            "          ├─ Number:2\n"
            "          └─ Number:3\n"
            "• 求值路径：先 2*3=6，再 1+6=7。树的「自底向上」正好对应运算的先后顺序。",
            "💡 根是 +；它的右子树是 *(2,3)，因为乘法优先级高、先结合；左子树是 Number:1。\n"
            "   易错：别把 2 和 3 直接挂到 + 下面——那会变成 (1+2)*3，是下一题的结构！",
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
            "🌳 括号 = 强行改变树形：加上括号后，+ 被「关」进括号里成为先算的部分，于是 + 跑到树的下层，* 升到树根。\n"
            "• 和上一题数字、运算符完全相同，只因多了括号，整棵树彻底反转！这就是「语法结构决定语义」的直观证据。\n"
            "• 结构：\n"
            "    BinaryOp:*\n"
            "     ├─ BinaryOp:+        ← 括号强制加法先结合，沉到下层\n"
            "     │    ├─ Number:1\n"
            "     │    └─ Number:2\n"
            "     └─ Number:3\n"
            "• 求值路径：先 1+2=3，再 3*3=9。",
            "💡 根是 *；它的左子树是 +(1,2)（括号强制先结合）；右子树是 Number:3。\n"
            "   对比上一题：把 1+2 用括号包起来，就相当于告诉 Parser「先算我」，于是 + 先于 * 成为子树。",
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
            "🌳 函数调用也是节点：print 是一个「动作」节点，它的子节点是传给它的参数表达式。\n"
            "• 这里参数是变量 x，所以 Print 挂着一个 Identifier:x 叶子作为「实参子树」。\n"
            "• 结构：\n"
            "    Print\n"
            "     └─ Identifier:x\n"
            "• 联想：函数像一台机器，参数是喂进去的原料；机器节点在下，原料节点在上（挂在它下面）。",
            "💡 根是 Print 节点；选中它，添加子节点 Identifier:x（代表变量 x 的引用）。\n"
            "   检查要点：根节点 label 含 'print' 或 'Print'。",
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
            "🌳 完整语句 = 更大的树：变量声明 VarDecl 是根，它除了记录变量名 x，还挂着一个「初始化表达式子树」（这里是 1+2 的加法树）。\n"
            "• 一条语句的 AST 往往是「声明节点 + 表达式子树」的组合——声明管「名字和类型」，表达式子树管「值怎么算」。\n"
            "• 结构：\n"
            "    VarDecl:x\n"
            "     └─ BinaryOp:+\n"
            "          ├─ Number:1\n"
            "          └─ Number:2\n"
            "• 联想：VarDecl 是「装水的杯子」，initializer 是「水」——杯子挂着一个描述水怎么来的子树。",
            "💡 根是 VarDecl:x；为它添加初始化表达式子树——一个 +(Number:1, Number:2)。\n"
            "   检查要点：根节点 label 含 'var' 或 'VarDecl'，且至少有 1 个子节点（initializer）。",
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
    const auto& lvls = levels();
    for (const auto& l : lvls) {
        if (l.level == level) return &l;
    }
    return nullptr;
}
