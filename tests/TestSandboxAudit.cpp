// ============================================================
// TestSandboxAudit.cpp — VM 栈沙盒关卡数据完整性测试（功能 5）
// ------------------------------------------------------------
// 覆盖 SandboxLibrary（5 个 VM 栈沙盒关卡）。
//
// 验证维度：
//   - 关卡数 == 5
//   - level 字段 1-5 连续
//   - 每关 goal 非空
//   - 每关 availableOps 非空
//   - 每关 expectedSequence 非空（关卡 5 自由模式可为空，但 availableOps 非空）
//   - 每关 teachingPoint 非空
//   - 每关 hint 非空
//   - difficulty 在 1-3 范围
//   - 关卡 1 的 expectedSequence 含 PUSH_INT, ADD, PRINT
//   - 关卡 2 和 3 的 availableOps 相同但 expectedSequence 不同
//   - 关卡 4 的 availableOps 含 PUSH_STRING
//   - 关卡 1 的 expectedOutput 是 "3"
//   - 关卡 2 的 expectedOutput 是 "7"（1 + 2*3 = 7）
//   - 关卡 3 的 expectedOutput 是 "9"（(1+2)*3 = 9）
//
// 注：项目使用 GoogleTest 框架（参考 tests/TestBugHuntVariantAudit.cpp）。
//     SandboxLibrary::levels() 实现位于 gui/SandboxLevels.cpp 独立编译单元，
//     仅依赖 STL + Qt6::Core（用于 PCH），不依赖 Qt6::Widgets / IdeController，
//     可被测试目标安全链接。
// ============================================================

#include <gtest/gtest.h>

#include "gui/SandboxLevels.h"

#include <set>
#include <string>

// ============================================================
// 辅助：判断 SandboxOp 是否为指定类型
// ============================================================
namespace {
bool hasOpType(const std::vector<SandboxOp>& ops, SandboxOpType t) {
    for (const auto& o : ops) {
        if (o.type == t) return true;
    }
    return false;
}
} // namespace

// ============================================================
// SandboxLibraryAudit — 数据完整性测试套件
// ============================================================

// 1. 关卡数 == 5
TEST(SandboxLibraryAudit, HasExpectedLevelCount) {
    const auto& levels = SandboxLibrary::levels();
    EXPECT_EQ(levels.size(), 5u);
}

// 2. level 字段 1-5 连续
TEST(SandboxLibraryAudit, LevelNumbersAreConsecutive) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_EQ(levels.size(), 5u);
    for (size_t i = 0; i < levels.size(); ++i) {
        EXPECT_EQ(levels[i].level, static_cast<int>(i + 1))
            << "第 " << i << " 个关卡的 level 字段应为 " << (i + 1);
    }
}

// 3. 每关 goal 非空
TEST(SandboxLibraryAudit, GoalsNonEmpty) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.goal.empty()) << "关卡 " << lv.level << ": goal 为空";
    }
}

// 4. 每关 availableOps 非空
TEST(SandboxLibraryAudit, AvailableOpsNonEmpty) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.availableOps.empty()) << "关卡 " << lv.level << ": availableOps 为空";
    }
}

// 5. 每关 expectedSequence 非空（关卡 5 自由模式可为空，但 availableOps 非空）
TEST(SandboxLibraryAudit, ExpectedSequenceNonEmptyExceptFreeMode) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        if (lv.level == 5) {
            // 自由模式：expectedSequence 可为空，但 availableOps 必须非空
            EXPECT_FALSE(lv.availableOps.empty()) << "关卡 5（自由模式）: availableOps 为空";
        } else {
            EXPECT_FALSE(lv.expectedSequence.empty())
                << "关卡 " << lv.level << ": expectedSequence 为空（自由模式除外）";
        }
    }
}

// 6. 每关 teachingPoint 非空
TEST(SandboxLibraryAudit, TeachingPointsNonEmpty) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.teachingPoint.empty()) << "关卡 " << lv.level << ": teachingPoint 为空";
    }
}

// 7. 每关 hint 非空
TEST(SandboxLibraryAudit, HintsNonEmpty) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.hint.empty()) << "关卡 " << lv.level << ": hint 为空";
    }
}

// 8. difficulty 在 1-3 范围
TEST(SandboxLibraryAudit, DifficultyInRange) {
    const auto& levels = SandboxLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_GE(lv.difficulty, 1) << "关卡 " << lv.level << ": difficulty < 1";
        EXPECT_LE(lv.difficulty, 3) << "关卡 " << lv.level << ": difficulty > 3";
    }
}

// 9. 关卡 1 的 expectedSequence 含 PUSH_INT, ADD, PRINT（基本 push-pop）
TEST(SandboxLibraryAudit, Level1HasBasicPushAddPrint) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 1u);
    const auto& seq = levels[0].expectedSequence;
    EXPECT_TRUE(hasOpType(seq, SandboxOpType::PUSH_INT))
        << "关卡 1 expectedSequence 应含 PUSH_INT";
    EXPECT_TRUE(hasOpType(seq, SandboxOpType::ADD))
        << "关卡 1 expectedSequence 应含 ADD";
    EXPECT_TRUE(hasOpType(seq, SandboxOpType::PRINT))
        << "关卡 1 expectedSequence 应含 PRINT";
}

// 10. 关卡 2 和 3 的 availableOps 相同但 expectedSequence 不同（同数不同顺序）
TEST(SandboxLibraryAudit, Level2And3SameOpsDifferentSequence) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 3u);
    const auto& ops2 = levels[1].availableOps;
    const auto& ops3 = levels[2].availableOps;
    // availableOps 数量相同
    EXPECT_EQ(ops2.size(), ops3.size()) << "关卡 2 与 3 的 availableOps 数量应相同";
    // 逐项相同（类型 + 操作数）
    for (size_t i = 0; i < ops2.size() && i < ops3.size(); ++i) {
        EXPECT_EQ(ops2[i].type, ops3[i].type)
            << "关卡 2/3 第 " << i << " 个 availableOps 的类型不一致";
        EXPECT_EQ(ops2[i].operand, ops3[i].operand)
            << "关卡 2/3 第 " << i << " 个 availableOps 的操作数不一致";
    }
    // expectedSequence 不同
    const auto& seq2 = levels[1].expectedSequence;
    const auto& seq3 = levels[2].expectedSequence;
    bool differ = false;
    if (seq2.size() != seq3.size()) {
        differ = true;
    } else {
        for (size_t i = 0; i < seq2.size(); ++i) {
            if (seq2[i].type != seq3[i].type || seq2[i].operand != seq3[i].operand) {
                differ = true;
                break;
            }
        }
    }
    EXPECT_TRUE(differ) << "关卡 2 与 3 的 expectedSequence 应不同（同数不同顺序）";
}

// 11. 关卡 4 的 availableOps 含 PUSH_STRING
TEST(SandboxLibraryAudit, Level4HasPushString) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 4u);
    EXPECT_TRUE(hasOpType(levels[3].availableOps, SandboxOpType::PUSH_STRING))
        << "关卡 4 availableOps 应含 PUSH_STRING";
}

// 12. 关卡 1 的 expectedOutput 是 "3"
TEST(SandboxLibraryAudit, Level1ExpectedOutputIs3) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 1u);
    EXPECT_EQ(levels[0].expectedOutput, "3")
        << "关卡 1 expectedOutput 应为 \"3\"";
}

// 13. 关卡 2 的 expectedOutput 是 "7"（1 + 2*3 = 7）
TEST(SandboxLibraryAudit, Level2ExpectedOutputIs7) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 2u);
    EXPECT_EQ(levels[1].expectedOutput, "7")
        << "关卡 2 expectedOutput 应为 \"7\"（1 + 2*3 = 7）";
}

// 14. 关卡 3 的 expectedOutput 是 "9"（(1+2)*3 = 9）
TEST(SandboxLibraryAudit, Level3ExpectedOutputIs9) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 3u);
    EXPECT_EQ(levels[2].expectedOutput, "9")
        << "关卡 3 expectedOutput 应为 \"9\"（(1+2)*3 = 9）";
}

// ============================================================
// 附加测试：expectedSequence 自洽性 — 按 expectedSequence 执行应得到 expectedOutput
// ============================================================

namespace {

/// 在测试内复现沙盒栈状态机，验证 expectedSequence 确实能得到 expectedOutput
bool simulateRun(const std::vector<SandboxOp>& seq, std::string& output, std::string& diag) {
    std::vector<std::string> stack;
    std::vector<std::string> outputs;
    for (const auto& op : seq) {
        switch (op.type) {
            case SandboxOpType::PUSH_INT:
            case SandboxOpType::PUSH_STRING:
                stack.push_back(op.operand);
                break;
            case SandboxOpType::ADD:
            case SandboxOpType::SUB:
            case SandboxOpType::MUL:
            case SandboxOpType::DIV:
            case SandboxOpType::MOD: {
                if (stack.size() < 2) {
                    diag = "栈不足 2 个值";
                    return false;
                }
                std::string b = stack.back(); stack.pop_back();
                std::string a = stack.back(); stack.pop_back();
                try {
                    long long ia = std::stoll(a);
                    long long ib = std::stoll(b);
                    long long r = 0;
                    switch (op.type) {
                        case SandboxOpType::ADD: r = ia + ib; break;
                        case SandboxOpType::SUB: r = ia - ib; break;
                        case SandboxOpType::MUL: r = ia * ib; break;
                        case SandboxOpType::DIV:
                            if (ib == 0) { diag = "除零"; return false; }
                            r = ia / ib; break;
                        case SandboxOpType::MOD:
                            if (ib == 0) { diag = "取模除零"; return false; }
                            r = ia % ib; break;
                        default: break;
                    }
                    stack.push_back(std::to_string(r));
                } catch (...) {
                    diag = "数值解析失败";
                    return false;
                }
                break;
            }
            case SandboxOpType::NEG: {
                if (stack.empty()) { diag = "栈为空"; return false; }
                std::string a = stack.back(); stack.pop_back();
                try {
                    long long ia = std::stoll(a);
                    stack.push_back(std::to_string(-ia));
                } catch (...) {
                    diag = "数值解析失败";
                    return false;
                }
                break;
            }
            case SandboxOpType::PRINT: {
                if (stack.empty()) { diag = "PRINT 栈为空"; return false; }
                std::string v = stack.back(); stack.pop_back();
                outputs.push_back(v);
                break;
            }
            case SandboxOpType::HALT:
                break;
        }
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (i > 0) output += "\n";
        output += outputs[i];
    }
    return true;
}

} // namespace

// 15. 关卡 1 的 expectedSequence 执行后输出 "3"
TEST(SandboxLibraryAudit, Level1SequenceProducesExpectedOutput) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 1u);
    std::string output, diag;
    ASSERT_TRUE(simulateRun(levels[0].expectedSequence, output, diag)) << diag;
    EXPECT_EQ(output, levels[0].expectedOutput);
}

// 16. 关卡 2 的 expectedSequence 执行后输出 "7"
TEST(SandboxLibraryAudit, Level2SequenceProducesExpectedOutput) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 2u);
    std::string output, diag;
    ASSERT_TRUE(simulateRun(levels[1].expectedSequence, output, diag)) << diag;
    EXPECT_EQ(output, levels[1].expectedOutput);
}

// 17. 关卡 3 的 expectedSequence 执行后输出 "9"
TEST(SandboxLibraryAudit, Level3SequenceProducesExpectedOutput) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 3u);
    std::string output, diag;
    ASSERT_TRUE(simulateRun(levels[2].expectedSequence, output, diag)) << diag;
    EXPECT_EQ(output, levels[2].expectedOutput);
}

// 18. 关卡 4 的 expectedSequence 执行后输出 "hello"
TEST(SandboxLibraryAudit, Level4SequenceProducesExpectedOutput) {
    const auto& levels = SandboxLibrary::levels();
    ASSERT_GE(levels.size(), 4u);
    std::string output, diag;
    ASSERT_TRUE(simulateRun(levels[3].expectedSequence, output, diag)) << diag;
    EXPECT_EQ(output, levels[3].expectedOutput);
    EXPECT_EQ(output, "hello");
}
