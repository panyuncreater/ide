// ============================================================
// TestTeachingTreePanel.cpp — PanelCatalog + 教学树导航数据审计
// ------------------------------------------------------------
// N5 fix: TeachingTreePanel 和 PanelCatalog 的测试覆盖。
// 验证教学树目录（PanelCatalog）的分类结构、面板 ID 唯一性、
// 规范 ID 映射，以及与 LearningPath 活动的一致性。
// ============================================================

#include <gtest/gtest.h>
#include "gui/PanelCatalog.h"
#include "gui/LearningPathData.h"

#include <set>
#include <string>

// 验证 PanelCatalog 恰好包含 4 个顶层分类（categories 数量）。
TEST(TeachingTreePanelAudit, HasFourCategories) {
    const auto& cats = PanelCatalog::categories();
    EXPECT_EQ(cats.size(), 4u) << "PanelCatalog should have 4 categories";
}

// 验证每个分类的标题（title）与图标（emoji）均非空。
TEST(TeachingTreePanelAudit, CategoryTitlesNonEmpty) {
    const auto& cats = PanelCatalog::categories();
    for (size_t i = 0; i < cats.size(); ++i) {
        EXPECT_NE(std::string(cats[i].title), "") << "Category " << i << " title empty";
        EXPECT_NE(std::string(cats[i].emoji), "") << "Category " << i << " emoji empty";
    }
}

// 验证所有面板 ID（含跨分类）全局唯一，无重复。
TEST(TeachingTreePanelAudit, PanelIdsAreUnique) {
    auto ids = PanelCatalog::allPanelIds();
    std::set<std::string> uniqueIds(ids.begin(), ids.end());
    EXPECT_EQ(uniqueIds.size(), ids.size()) << "Duplicate panel IDs found";
}

// 验证每个分类下的叶子面板（leaf）的 id 与 label 均非空。
TEST(TeachingTreePanelAudit, PanelIdsAreNonEmpty) {
    const auto& cats = PanelCatalog::categories();
    for (const auto& cat : cats) {
        for (const auto& leaf : cat.leaves) {
            EXPECT_NE(std::string(leaf.id), "") << "Panel ID empty in " << cat.title;
            EXPECT_NE(std::string(leaf.label), "") << "Panel label empty for " << leaf.id;
        }
    }
}

// 验证面板总数在合理区间 [18, 40]（既不过少也不过滥）。
// 上限从 30 提升到 40：第五波教学面板拓展（循环展开/逃逸分析/寄存器分配/三后端性能竞赛）
// 后面板总数达 32，原 30 上限已过时。
TEST(TeachingTreePanelAudit, HasExpectedPanelCount) {
    auto ids = PanelCatalog::allPanelIds();
    EXPECT_GE(ids.size(), 18u) << "Too few panels";
    EXPECT_LE(ids.size(), 40u) << "Too many panels";
}

// 验证 lab-01 ~ lab-08 等实验手册 ID 都被规范映射到 "lab-manual"。
TEST(TeachingTreePanelAudit, CanonicalIdMapsLabManual) {
    for (int i = 1; i <= 8; ++i) {
        std::string labId = "lab-0" + std::to_string(i);
        EXPECT_EQ(PanelCatalog::canonicalPanelId(labId), "lab-manual")
            << labId << " not mapped to lab-manual";
    }
}

// 验证 bug-hunt 系列 ID 都被规范映射到 "bug-hunt"。
TEST(TeachingTreePanelAudit, CanonicalIdMapsBugHunt) {
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-beginner"), "bug-hunt");
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-intermediate"), "bug-hunt");
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-expert"), "bug-hunt");
}

// 验证 op-priority-challenge 被规范映射到 "ast-toy"（AST 玩具面板）。
TEST(TeachingTreePanelAudit, CanonicalIdMapsOpPriority) {
    EXPECT_EQ(PanelCatalog::canonicalPanelId("op-priority-challenge"), "ast-toy");
}

// 验证所有合法面板 ID 经规范映射后都得到非空规范 ID（不漏映射）。
TEST(TeachingTreePanelAudit, CanonicalIdPassThrough) {
    auto ids = PanelCatalog::allPanelIds();
    for (const auto& id : ids) {
        std::string canonical = PanelCatalog::canonicalPanelId(id);
        EXPECT_FALSE(canonical.empty())
            << "Valid panel ID " << id << " maps to empty canonical";
    }
}

// 验证 PanelCatalog::findById 对全部面板 ID 都能返回非空有效条目。
TEST(TeachingTreePanelAudit, FindByIdReturnsValidForAllIds) {
    auto ids = PanelCatalog::allPanelIds();
    for (const auto& id : ids) {
        const PanelEntry* entry = PanelCatalog::findById(id);
        ASSERT_NE(entry, nullptr) << "findById failed for: " << id;
        EXPECT_NE(std::string(entry->id), "");
        EXPECT_NE(std::string(entry->label), "");
    }
}

// 验证 findById 对未知 ID 与空字符串均返回 nullptr（防御性）。
TEST(TeachingTreePanelAudit, FindByIdReturnsNullForUnknown) {
    EXPECT_EQ(PanelCatalog::findById("nonexistent-panel-xyz"), nullptr);
    EXPECT_EQ(PanelCatalog::findById(""), nullptr);
}

// 验证 LearningPath 的每个活动（除 welcome/freeform 外）都能映射到某个有效面板。
TEST(TeachingTreePanelAudit, LearningPathActivitiesMapToValidPanels) {
    const auto& activities = LearningPathData::activities();
    for (const auto& act : activities) {
        if (act.id == "welcome" || act.id == "freeform-project") continue;
        std::string canonical = PanelCatalog::canonicalPanelId(act.id);
        EXPECT_FALSE(canonical.empty())
            << "Learning path activity " << act.id << " has no panel mapping";
    }
}
