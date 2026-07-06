// ============================================================
// TestBugHuntDifficultyAudit.cpp — Bug 狩猎分级题库重构数据完整性测试
// ------------------------------------------------------------
// 功能 9（Bug 狩猎分级题库重构）数据完整性审计：
//   - 总题目数 == 15（原 10 道历史 Bug + 5 道入门级 BUG-READ-01..05）
//   - 题目 ID 唯一
//   - 必需的入门级题目 ID 齐全：BUG-READ-01/02/03/04/05
//   - 必需的进阶级题目 ID 齐全：BUG-DEF-1 / BUG-REGVM-2 / BUG-REPL-1 / BUG-F-04
//   - 必需的专家级题目 ID 齐全：BUG-IR-POP-2 / BUG-CP-1 / BUG-UV-1 / BUG-DBG-1
//     （及 BUG-CP-2 / BUG-MOD-1）
//   - 各难度题目数符合预期：入门 5 / 进阶 4 / 专家 6
//   - 每题 difficulty 字段已设置
//   - 每题 sourceCode / expectedBehavior 非空
//   - 入门级题目的 hints 至少 3 条
//   - 入门级 BUG-READ-01 的 sourceCode 含 "7 / 2"
//   - 入门级 BUG-READ-04 的 sourceCode 含 "for"
//
// 注：BugHuntLibrary.cpp 为独立编译单元，仅依赖 Qt6::Core + 标准库，
//     不依赖 IdeController.h，可被测试目标安全链接。
// ============================================================

#include <gtest/gtest.h>

#include "gui/BugHuntPanel.h"
#include "lexer/Lexer.h"

#include <map>
#include <set>
#include <string>
#include <vector>

// ============================================================
// BugHuntDifficultyAudit — 单测试套件，覆盖功能 9 全部数据不变量
// ============================================================

// 1. 总题目数 == 15（原 10 + 入门级 5）
TEST(BugHuntDifficultyAudit, HasExpectedTotalItemCount) {
    const auto& items = BugHuntLibrary::items();
    EXPECT_EQ(items.size(), 15u);
}

// 2. 题目 ID 唯一
TEST(BugHuntDifficultyAudit, IdsAreUnique) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) {
        EXPECT_FALSE(it.id.empty()) << "题目 id 不能为空";
        auto [_, inserted] = ids.insert(it.id);
        EXPECT_TRUE(inserted) << "题目 id 重复: " << it.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

// 3. 必需的入门级题目 ID 齐全：BUG-READ-01/02/03/04/05
TEST(BugHuntDifficultyAudit, CriticalBeginnerIdsPresent) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) ids.insert(it.id);
    EXPECT_NE(ids.find("BUG-READ-01"), ids.end());
    EXPECT_NE(ids.find("BUG-READ-02"), ids.end());
    EXPECT_NE(ids.find("BUG-READ-03"), ids.end());
    EXPECT_NE(ids.find("BUG-READ-04"), ids.end());
    EXPECT_NE(ids.find("BUG-READ-05"), ids.end());
}

// 4. 必需的进阶级题目 ID 齐全：BUG-DEF-1 / BUG-REGVM-2 / BUG-REPL-1 / BUG-F-04
TEST(BugHuntDifficultyAudit, CriticalIntermediateIdsPresent) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) ids.insert(it.id);
    EXPECT_NE(ids.find("BUG-DEF-1"),   ids.end());
    EXPECT_NE(ids.find("BUG-REGVM-2"), ids.end());
    EXPECT_NE(ids.find("BUG-REPL-1"),  ids.end());
    EXPECT_NE(ids.find("BUG-F-04"),    ids.end());
}

// 5. 必需的专家级题目 ID 齐全：BUG-IR-POP-2 / BUG-CP-1 / BUG-UV-1 / BUG-DBG-1
//    （含 BUG-CP-2 / BUG-MOD-1，见用例 8）
TEST(BugHuntDifficultyAudit, CriticalExpertIdsPresent) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) ids.insert(it.id);
    EXPECT_NE(ids.find("BUG-IR-POP-2"), ids.end());
    EXPECT_NE(ids.find("BUG-CP-1"),     ids.end());
    EXPECT_NE(ids.find("BUG-UV-1"),     ids.end());
    EXPECT_NE(ids.find("BUG-DBG-1"),    ids.end());
    EXPECT_NE(ids.find("BUG-CP-2"),     ids.end());  // 专家级额外项
    EXPECT_NE(ids.find("BUG-MOD-1"),    ids.end());  // 专家级额外项
}

// 6. 入门级题目数 == 5
TEST(BugHuntDifficultyAudit, BeginnerCountIsFive) {
    const auto& items = BugHuntLibrary::items();
    int count = 0;
    for (const auto& it : items) {
        if (it.difficulty == BugHuntDifficulty::BEGINNER) ++count;
    }
    EXPECT_EQ(count, 5);
}

// 7. 进阶级题目数 == 4
TEST(BugHuntDifficultyAudit, IntermediateCountIsFour) {
    const auto& items = BugHuntLibrary::items();
    int count = 0;
    for (const auto& it : items) {
        if (it.difficulty == BugHuntDifficulty::INTERMEDIATE) ++count;
    }
    EXPECT_EQ(count, 4);
}

// 8. 专家级题目数 == 6（含 BUG-CP-2 / BUG-MOD-1）
TEST(BugHuntDifficultyAudit, ExpertCountIsSix) {
    const auto& items = BugHuntLibrary::items();
    int count = 0;
    for (const auto& it : items) {
        if (it.difficulty == BugHuntDifficulty::EXPERT) ++count;
    }
    EXPECT_EQ(count, 6);
}

// 9. 每题 difficulty 字段已设置（非默认值——这里验证三档分布合理：
//    进阶级题目 ID 必须为 INTERMEDIATE，专家级题目 ID 必须为 EXPERT）
TEST(BugHuntDifficultyAudit, DifficultyFieldCorrectlyAssigned) {
    const auto& items = BugHuntLibrary::items();
    // 期望的 (id -> difficulty) 映射
    std::map<std::string, BugHuntDifficulty> expected;
    // 入门级
    expected["BUG-READ-01"] = BugHuntDifficulty::BEGINNER;
    expected["BUG-READ-02"] = BugHuntDifficulty::BEGINNER;
    expected["BUG-READ-03"] = BugHuntDifficulty::BEGINNER;
    expected["BUG-READ-04"] = BugHuntDifficulty::BEGINNER;
    expected["BUG-READ-05"] = BugHuntDifficulty::BEGINNER;
    // 进阶级
    expected["BUG-DEF-1"]   = BugHuntDifficulty::INTERMEDIATE;
    expected["BUG-REGVM-2"] = BugHuntDifficulty::INTERMEDIATE;
    expected["BUG-REPL-1"]  = BugHuntDifficulty::INTERMEDIATE;
    expected["BUG-F-04"]    = BugHuntDifficulty::INTERMEDIATE;
    // 专家级
    expected["BUG-IR-POP-2"] = BugHuntDifficulty::EXPERT;
    expected["BUG-CP-1"]     = BugHuntDifficulty::EXPERT;
    expected["BUG-UV-1"]     = BugHuntDifficulty::EXPERT;
    expected["BUG-DBG-1"]    = BugHuntDifficulty::EXPERT;
    expected["BUG-CP-2"]     = BugHuntDifficulty::EXPERT;
    expected["BUG-MOD-1"]    = BugHuntDifficulty::EXPERT;

    for (const auto& it : items) {
        auto iter = expected.find(it.id);
        ASSERT_NE(iter, expected.end()) << "未在期望映射中定义的题目 ID: " << it.id;
        EXPECT_EQ(it.difficulty, iter->second)
            << it.id << ": difficulty 分级与期望不符";
    }
}

// 10. 每题 sourceCode 非空
TEST(BugHuntDifficultyAudit, SourceCodeNonEmpty) {
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        EXPECT_FALSE(it.sourceCode.empty()) << it.id << ": sourceCode 为空";
    }
}

// 11. 每题 expectedBehavior 非空
TEST(BugHuntDifficultyAudit, ExpectedBehaviorNonEmpty) {
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        EXPECT_FALSE(it.expectedBehavior.empty()) << it.id << ": expectedBehavior 为空";
    }
}

// 12. 入门级题目的 hints 至少 3 条
TEST(BugHuntDifficultyAudit, BeginnerHintsAtLeastThree) {
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        if (it.difficulty != BugHuntDifficulty::BEGINNER) continue;
        EXPECT_GE(it.hints.size(), 3u)
            << it.id << ": 入门级题目 hints 应至少 3 条，实际 " << it.hints.size();
    }
}

// 13. 入门级题目 BUG-READ-01 的 sourceCode 含 "7 / 2"
TEST(BugHuntDifficultyAudit, Read01SourceCodeContainsDivision) {
    const auto& items = BugHuntLibrary::items();
    const BugHuntItem* target = nullptr;
    for (const auto& it : items) {
        if (it.id == "BUG-READ-01") { target = &it; break; }
    }
    ASSERT_NE(target, nullptr);
    EXPECT_NE(target->sourceCode.find("7 / 2"), std::string::npos)
        << "BUG-READ-01 的 sourceCode 应包含 '7 / 2'";
}

// 14. 入门级题目 BUG-READ-04 的 sourceCode 含 "for"
TEST(BugHuntDifficultyAudit, Read04SourceCodeContainsForLoop) {
    const auto& items = BugHuntLibrary::items();
    const BugHuntItem* target = nullptr;
    for (const auto& it : items) {
        if (it.id == "BUG-READ-04") { target = &it; break; }
    }
    ASSERT_NE(target, nullptr);
    EXPECT_NE(target->sourceCode.find("for"), std::string::npos)
        << "BUG-READ-04 的 sourceCode 应包含 'for'";

    // 同时验证该题 difficulty 为 BEGINNER
    EXPECT_EQ(target->difficulty, BugHuntDifficulty::BEGINNER);
}

// ============================================================
// 附加测试：入门级题目的 sourceCode 可被 Lexer 解析
// （确保教学代码本身不是乱码，与现有 TeachingPanelsBugHunt 审计一致）
// ============================================================

TEST(BugHuntDifficultyAudit, BeginnerSourceCodeIsLexable) {
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        if (it.difficulty != BugHuntDifficulty::BEGINNER) continue;
        Lexer lexer;
        auto tokens = lexer.scan(it.sourceCode);
        size_t mainStreamCount = 0;
        for (const auto& tk : tokens) {
            if (tk.type != TokenType::TK_EOF) ++mainStreamCount;
        }
        EXPECT_GT(mainStreamCount, 0u)
            << it.id << ": sourceCode 无有效 token";
    }
}

// 附加测试：三档难度分布总和等于总题数（无遗漏分级）
TEST(BugHuntDifficultyAudit, DifficultyPartitionSumsToTotal) {
    const auto& items = BugHuntLibrary::items();
    int beginner = 0, intermediate = 0, expert = 0;
    for (const auto& it : items) {
        switch (it.difficulty) {
            case BugHuntDifficulty::BEGINNER:     ++beginner; break;
            case BugHuntDifficulty::INTERMEDIATE: ++intermediate; break;
            case BugHuntDifficulty::EXPERT:       ++expert; break;
        }
    }
    EXPECT_EQ(beginner + intermediate + expert, (int)items.size());
    EXPECT_EQ(beginner, 5);
    EXPECT_EQ(intermediate, 4);
    EXPECT_EQ(expert, 6);
}
