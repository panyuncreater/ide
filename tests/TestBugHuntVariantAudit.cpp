// ============================================================
// TestBugHuntVariantAudit.cpp — 变体挑战题库数据完整性测试
// ------------------------------------------------------------
// 覆盖 BugHuntVariantLibrary（基于 BugHuntLibrary 父题生成的 7 个变体）。
//
// 验证维度：
//   - 变体数量符合预期（>= 6）
//   - 变体 id 唯一
//   - 关键变体 ID 齐全（7 个父题各一个变体）
//   - 每个变体 parentId 非空
//   - parentId 必须存在于 BugHuntLibrary::items() 的 id 集合中
//   - 每个变体 sourceCode 非空且可被 Lexer 解析
//   - 每个变体 challengeGoal 非空
//   - description 含差异说明（包含"差异"关键词）
//
// 注：BugHuntVariantLibrary::variants() 实现位于 gui/BugHuntPanel.cpp，
//     需在 tests/CMakeLists.txt 中显式加入该源文件才能链接通过。
// ============================================================

#include <gtest/gtest.h>

#include "gui/BugHuntPanel.h"
#include "lexer/Lexer.h"

#include <set>
#include <string>

// ============================================================
// BugHuntVariantLibrary — 数据完整性
// ============================================================

TEST(BugHuntVariantAudit, HasExpectedVariantCount) {
    const auto& variants = BugHuntVariantLibrary::variants();
    // 设计为 7 个变体（基于 7 个父题：CP-1/UV-1/IR-POP-2/DEF-1/MOD-1/DBG-1/REGVM-2）
    EXPECT_GE(variants.size(), 6u);
}

TEST(BugHuntVariantAudit, VariantIdsAreUnique) {
    const auto& variants = BugHuntVariantLibrary::variants();
    std::set<std::string> ids;
    for (const auto& v : variants) {
        EXPECT_FALSE(v.id.empty()) << "变体 id 不能为空";
        auto [_, inserted] = ids.insert(v.id);
        EXPECT_TRUE(inserted) << "变体 id 重复: " << v.id;
    }
    EXPECT_EQ(ids.size(), variants.size());
}

TEST(BugHuntVariantAudit, CriticalVariantIdsPresent) {
    // 验证关键变体 ID 都已收录
    const auto& variants = BugHuntVariantLibrary::variants();
    std::set<std::string> ids;
    for (const auto& v : variants)
        ids.insert(v.id);
    EXPECT_NE(ids.find("variant-cp-1-param"), ids.end());      // 常量池去重变体
    EXPECT_NE(ids.find("variant-uv-1-closure"), ids.end());    // 闭包变体
    EXPECT_NE(ids.find("variant-ir-pop-2-expr"), ids.end());   // IR 栈泄漏变体
    EXPECT_NE(ids.find("variant-def-1-default"), ids.end());   // 默认参数变体
    EXPECT_NE(ids.find("variant-mod-1-cycle"), ids.end());     // 模块循环导入延迟加载变体
    EXPECT_NE(ids.find("variant-dbg-1-condition"), ids.end()); // 调试条件断点变体
    EXPECT_NE(ids.find("variant-regvm-2-vreg"), ids.end());    // 寄存器 vreg 变体
}

TEST(BugHuntVariantAudit, ParentIdsNonEmpty) {
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_FALSE(v.parentId.empty()) << v.id << ": parentId 不能为空";
    }
}

TEST(BugHuntVariantAudit, ParentIdsExistInBugHuntLibrary) {
    // 每个变体的 parentId 必须是 BugHuntLibrary 中存在的父题 ID
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> parentIds;
    for (const auto& it : items)
        parentIds.insert(it.id);

    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_NE(parentIds.find(v.parentId), parentIds.end())
            << v.id << ": parentId " << v.parentId << " 在 BugHuntLibrary 中不存在";
    }
}

TEST(BugHuntVariantAudit, SourceCodeIsLexable) {
    // 每个变体的 sourceCode 必须能被 Lexer 解析为非空 token 流
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_FALSE(v.sourceCode.empty()) << v.id << ": sourceCode 为空";
        Lexer lexer;
        auto tokens = lexer.scan(v.sourceCode);
        // 至少应包含 EOF；非空源码应有多个 token
        EXPECT_FALSE(tokens.empty()) << v.id << ": sourceCode 无法 lex";
        size_t mainStreamCount = 0;
        for (const auto& tk : tokens) {
            if (tk.type != TokenType::TK_EOF)
                ++mainStreamCount;
        }
        EXPECT_GT(mainStreamCount, 0u) << v.id << ": sourceCode 无有效 token";
    }
}

TEST(BugHuntVariantAudit, ChallengeGoalsNonEmpty) {
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_FALSE(v.challengeGoal.empty()) << v.id << ": challengeGoal 为空";
    }
}

TEST(BugHuntVariantAudit, DescriptionContainsDifferenceExplanation) {
    // 每个变体的 description 必须包含差异说明（含"差异"关键词）
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_FALSE(v.description.empty()) << v.id << ": description 为空";
        EXPECT_NE(v.description.find("差异"), std::string::npos)
            << v.id << ": description 缺少差异说明（应含'差异'关键词）";
        // 同时应包含新挑战点说明
        EXPECT_NE(v.description.find("挑战点"), std::string::npos)
            << v.id << ": description 缺少新挑战点说明（应含'挑战点'关键词）";
    }
}

// ============================================================
// 附加测试：变体关键字段完整性
// ============================================================

TEST(BugHuntVariantAudit, AllCriticalFieldsNonEmpty) {
    // 验证所有变体的关键字段非空
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        EXPECT_FALSE(v.title.empty()) << v.id << ": title 为空";
        EXPECT_FALSE(v.expectedBehavior.empty()) << v.id << ": expectedBehavior 为空";
        EXPECT_FALSE(v.hint.empty()) << v.id << ": hint 为空";
    }
}
