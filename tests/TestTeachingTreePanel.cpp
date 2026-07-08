// ============================================================
// TestTeachingTreePanel.cpp — PanelCatalog + 教学树导航数据审计
// ------------------------------------------------------------
// N5 fix: TeachingTreePanel 和 PanelCatalog 的测试覆盖。
// ============================================================

#include <gtest/gtest.h>
#include "gui/PanelCatalog.h"
#include "gui/LearningPathData.h"

#include <set>
#include <string>

TEST(TeachingTreePanelAudit, HasFourCategories) {
    const auto& cats = PanelCatalog::categories();
    EXPECT_EQ(cats.size(), 4u) << "PanelCatalog should have 4 categories";
}

TEST(TeachingTreePanelAudit, CategoryTitlesNonEmpty) {
    const auto& cats = PanelCatalog::categories();
    for (size_t i = 0; i < cats.size(); ++i) {
        EXPECT_NE(std::string(cats[i].title), "") << "Category " << i << " title empty";
        EXPECT_NE(std::string(cats[i].emoji), "") << "Category " << i << " emoji empty";
    }
}

TEST(TeachingTreePanelAudit, PanelIdsAreUnique) {
    auto ids = PanelCatalog::allPanelIds();
    std::set<std::string> uniqueIds(ids.begin(), ids.end());
    EXPECT_EQ(uniqueIds.size(), ids.size()) << "Duplicate panel IDs found";
}

TEST(TeachingTreePanelAudit, PanelIdsAreNonEmpty) {
    const auto& cats = PanelCatalog::categories();
    for (const auto& cat : cats) {
        for (const auto& leaf : cat.leaves) {
            EXPECT_NE(std::string(leaf.id), "") << "Panel ID empty in " << cat.title;
            EXPECT_NE(std::string(leaf.label), "") << "Panel label empty for " << leaf.id;
        }
    }
}

TEST(TeachingTreePanelAudit, HasExpectedPanelCount) {
    auto ids = PanelCatalog::allPanelIds();
    EXPECT_GE(ids.size(), 18u) << "Too few panels";
    EXPECT_LE(ids.size(), 30u) << "Too many panels";
}

TEST(TeachingTreePanelAudit, CanonicalIdMapsLabManual) {
    for (int i = 1; i <= 8; ++i) {
        std::string labId = "lab-0" + std::to_string(i);
        EXPECT_EQ(PanelCatalog::canonicalPanelId(labId), "lab-manual")
            << labId << " not mapped to lab-manual";
    }
}

TEST(TeachingTreePanelAudit, CanonicalIdMapsBugHunt) {
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-beginner"), "bug-hunt");
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-intermediate"), "bug-hunt");
    EXPECT_EQ(PanelCatalog::canonicalPanelId("bug-hunt-expert"), "bug-hunt");
}

TEST(TeachingTreePanelAudit, CanonicalIdMapsOpPriority) {
    EXPECT_EQ(PanelCatalog::canonicalPanelId("op-priority-challenge"), "ast-toy");
}

TEST(TeachingTreePanelAudit, CanonicalIdPassThrough) {
    auto ids = PanelCatalog::allPanelIds();
    for (const auto& id : ids) {
        std::string canonical = PanelCatalog::canonicalPanelId(id);
        EXPECT_FALSE(canonical.empty())
            << "Valid panel ID " << id << " maps to empty canonical";
    }
}

TEST(TeachingTreePanelAudit, FindByIdReturnsValidForAllIds) {
    auto ids = PanelCatalog::allPanelIds();
    for (const auto& id : ids) {
        const PanelEntry* entry = PanelCatalog::findById(id);
        ASSERT_NE(entry, nullptr) << "findById failed for: " << id;
        EXPECT_NE(std::string(entry->id), "");
        EXPECT_NE(std::string(entry->label), "");
    }
}

TEST(TeachingTreePanelAudit, FindByIdReturnsNullForUnknown) {
    EXPECT_EQ(PanelCatalog::findById("nonexistent-panel-xyz"), nullptr);
    EXPECT_EQ(PanelCatalog::findById(""), nullptr);
}

TEST(TeachingTreePanelAudit, LearningPathActivitiesMapToValidPanels) {
    const auto& activities = LearningPathData::activities();
    for (const auto& act : activities) {
        if (act.id == "welcome" || act.id == "freeform-project") continue;
        std::string canonical = PanelCatalog::canonicalPanelId(act.id);
        EXPECT_FALSE(canonical.empty())
            << "Learning path activity " << act.id << " has no panel mapping";
    }
}
