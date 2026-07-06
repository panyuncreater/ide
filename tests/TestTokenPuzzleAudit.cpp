// ============================================================
// TestTokenPuzzleAudit.cpp — 交互式 Token 拼图游戏关卡数据完整性测试（功能 3）
// ------------------------------------------------------------
// 覆盖 TokenPuzzleLibrary（5 个关卡：var/print/and/注释/复杂表达式）。
//
// 验证维度：
//   1. 关卡数 == 5
//   2. level 字段 1-5 连续
//   3. 每关 targetCode 非空
//   4. 每关 tokens 非空且长度 ≥ 3
//   5. shuffledTokens 与 tokens 元素相同（仅顺序不同，多重集相等）
//   6. shuffledTokens 与 tokens 顺序不完全相同（确实打乱了）
//   7. 每关 hint 非空
//   8. 每关 teachingPoint 非空
//   9. difficulty 在 1-3 范围
//  10. 关卡 1 的 difficulty == 1（最简单）
//  11. 关卡 5 的 difficulty == 3（最难）
//  12. 关卡 2 的 targetCode 含 "print"（教学点：字符串是一个 token）
//
// 测试框架：GoogleTest（项目已集成，doctest 不可用）
// 依赖约束：仅链接 TokenPuzzleData.cpp（独立编译单元，不依赖 IdeController.h）
// ============================================================

#include <gtest/gtest.h>

#include "gui/TokenPuzzleData.h"

#include <algorithm>
#include <string>
#include <vector>

// ============================================================
// TokenPuzzleLibraryAudit — 关卡数据完整性
// ============================================================

TEST(TokenPuzzleLibraryAudit, HasExpectedLevelCount) {
    const auto& levels = TokenPuzzleLibrary::levels();
    EXPECT_EQ(levels.size(), 5u);
    EXPECT_EQ(TokenPuzzleLibrary::levelCount(), 5);
}

TEST(TokenPuzzleLibraryAudit, LevelNumbersAreConsecutiveFrom1To5) {
    const auto& levels = TokenPuzzleLibrary::levels();
    ASSERT_EQ(levels.size(), 5u);
    for (size_t i = 0; i < levels.size(); ++i) {
        EXPECT_EQ(levels[i].level, static_cast<int>(i + 1))
            << "关卡索引 " << i << " 的 level 字段应为 " << (i + 1);
    }
}

TEST(TokenPuzzleLibraryAudit, TargetCodeIsNonEmpty) {
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.targetCode.empty()) << "关卡 " << lv.level << " targetCode 不能为空";
    }
}

TEST(TokenPuzzleLibraryAudit, TokensNonEmptyAndAtLeast3) {
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.tokens.empty()) << "关卡 " << lv.level << " tokens 不能为空";
        EXPECT_GE(lv.tokens.size(), 3u) << "关卡 " << lv.level << " tokens 长度应 ≥ 3";
    }
}

TEST(TokenPuzzleLibraryAudit, ShuffledTokensArePermutationOfTokens) {
    // shuffledTokens 与 tokens 元素相同（仅顺序不同）——多重集相等
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        ASSERT_EQ(lv.shuffledTokens.size(), lv.tokens.size())
            << "关卡 " << lv.level << " shuffledTokens 数量应与 tokens 相同";
        std::vector<std::string> sortedTokens = lv.tokens;
        std::vector<std::string> sortedShuffled = lv.shuffledTokens;
        std::sort(sortedTokens.begin(), sortedTokens.end());
        std::sort(sortedShuffled.begin(), sortedShuffled.end());
        EXPECT_EQ(sortedTokens, sortedShuffled)
            << "关卡 " << lv.level << " shuffledTokens 应为 tokens 的排列（多重集相等）";
    }
}

TEST(TokenPuzzleLibraryAudit, ShuffledTokensAreActuallyShuffled) {
    // shuffledTokens 与 tokens 顺序不完全相同（确实打乱了）
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        ASSERT_EQ(lv.shuffledTokens.size(), lv.tokens.size());
        bool differs = false;
        for (size_t i = 0; i < lv.tokens.size(); ++i) {
            if (lv.shuffledTokens[i] != lv.tokens[i]) {
                differs = true;
                break;
            }
        }
        EXPECT_TRUE(differs) << "关卡 " << lv.level << " shuffledTokens 顺序应与 tokens 不同（确实打乱）";
    }
}

TEST(TokenPuzzleLibraryAudit, HintIsNonEmpty) {
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.hint.empty()) << "关卡 " << lv.level << " hint 不能为空";
    }
}

TEST(TokenPuzzleLibraryAudit, TeachingPointIsNonEmpty) {
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_FALSE(lv.teachingPoint.empty()) << "关卡 " << lv.level << " teachingPoint 不能为空";
    }
}

TEST(TokenPuzzleLibraryAudit, DifficultyInRange1To3) {
    const auto& levels = TokenPuzzleLibrary::levels();
    for (const auto& lv : levels) {
        EXPECT_GE(lv.difficulty, 1) << "关卡 " << lv.level << " difficulty 应 ≥ 1";
        EXPECT_LE(lv.difficulty, 3) << "关卡 " << lv.level << " difficulty 应 ≤ 3";
    }
}

TEST(TokenPuzzleLibraryAudit, Level1DifficultyIs1) {
    const auto& levels = TokenPuzzleLibrary::levels();
    ASSERT_FALSE(levels.empty());
    EXPECT_EQ(levels[0].difficulty, 1) << "关卡 1 应为最简单（difficulty == 1）";
}

TEST(TokenPuzzleLibraryAudit, Level5DifficultyIs3) {
    const auto& levels = TokenPuzzleLibrary::levels();
    ASSERT_GE(levels.size(), 5u);
    EXPECT_EQ(levels[4].difficulty, 3) << "关卡 5 应为最难（difficulty == 3）";
}

TEST(TokenPuzzleLibraryAudit, Level2TargetCodeContainsPrint) {
    // 教学点验证：关卡 2 是 print("hello");——字符串是一个 token
    const auto& levels = TokenPuzzleLibrary::levels();
    ASSERT_GE(levels.size(), 2u);
    EXPECT_NE(levels[1].targetCode.find("print"), std::string::npos)
        << "关卡 2 targetCode 应包含 \"print\"";
}
