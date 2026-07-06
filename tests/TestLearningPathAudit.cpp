// ============================================================
// TestLearningPathAudit.cpp — 学习路径地图数据完整性测试（功能 6）
// ------------------------------------------------------------
// 测试覆盖三个套件：
//   1. LearningPathDataAudit.* (5 用例) — 活动数据完整性
//   2. LearningPathProgressAudit.* (6 用例) — 进度持久化与计算
//   3. LearningPathPrereqAudit.* (4 用例) — 前置依赖关系
//
// 测试约束：
//   - 使用 GoogleTest 框架（项目约定）
//   - LearnerProgressStore 测试不写真实文件——通过 setFilePathForTesting
//     注入临时路径，并通过 resetForTesting() 清空状态
//   - 测试目标仅链接 LearnerProgress.cpp + LearningPathData.cpp，不链接
//     LearningPathPanel.cpp（后者依赖 Qt Widgets + PanelAnimator）
// ============================================================

#include <gtest/gtest.h>

#include "gui/LearningPathData.h"
#include "gui/LearnerProgress.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>

#include <set>
#include <string>
#include <vector>

// ============================================================
// 辅助：每个测试前重置 LearnerProgressStore 单例
// ============================================================
class LearningPathTestBase : public ::testing::Test {
protected:
    void SetUp() override {
        // 重置单例状态，避免跨测试污染
        LearnerProgressStore::instance().resetForTesting();
        // 注入临时文件路径，避免污染用户家目录
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        QString tempPath = tempDir_->path() + QDir::separator() +
                           QString::fromUtf8("test_progress.json");
        LearnerProgressStore::instance().setFilePathForTesting(tempPath);
    }
    void TearDown() override {
        LearnerProgressStore::instance().resetForTesting();
        tempDir_.reset();
    }
    std::unique_ptr<QTemporaryDir> tempDir_;
};

// ============================================================
// 套件 1：LearningPathDataAudit — 活动数据完整性
// ============================================================

TEST(LearningPathDataAudit, HasAtLeast18Activities) {
    const auto& acts = LearningPathData::activities();
    EXPECT_GE(acts.size(), 18u)
        << "学习路径活动数应 >= 18（5+4+4+4+4=20）";
}

TEST(LearningPathDataAudit, ActivityIdsAreUnique) {
    const auto& acts = LearningPathData::activities();
    std::set<std::string> ids;
    for (const auto& a : acts) {
        EXPECT_FALSE(a.id.empty()) << "活动 id 不能为空";
        auto [_, inserted] = ids.insert(a.id);
        EXPECT_TRUE(inserted) << "活动 id 重复: " << a.id;
    }
    EXPECT_EQ(ids.size(), acts.size());
}

TEST(LearningPathDataAudit, CriticalActivityIdsPresent) {
    const auto& acts = LearningPathData::activities();
    std::set<std::string> ids;
    for (const auto& a : acts) ids.insert(a.id);

    // 阶段零：5 个核心活动
    EXPECT_NE(ids.find("welcome"),              ids.end());
    EXPECT_NE(ids.find("token-puzzle"),         ids.end());
    EXPECT_NE(ids.find("ast-toy"),              ids.end());
    EXPECT_NE(ids.find("vm-sandbox"),           ids.end());
    // 阶段一/二/三：lab-01~08
    EXPECT_NE(ids.find("lab-01"),               ids.end());
    EXPECT_NE(ids.find("lab-02"),               ids.end());
    EXPECT_NE(ids.find("lab-03"),               ids.end());
    EXPECT_NE(ids.find("lab-04"),               ids.end());
    EXPECT_NE(ids.find("lab-05"),               ids.end());
    EXPECT_NE(ids.find("lab-06"),               ids.end());
    EXPECT_NE(ids.find("lab-07"),               ids.end());
    EXPECT_NE(ids.find("lab-08"),               ids.end());
    // 阶段四：bug-hunt 三档
    EXPECT_NE(ids.find("bug-hunt-beginner"),    ids.end());
    EXPECT_NE(ids.find("bug-hunt-intermediate"), ids.end());
    EXPECT_NE(ids.find("bug-hunt-expert"),      ids.end());
}

TEST(LearningPathDataAudit, StageInRange) {
    const auto& acts = LearningPathData::activities();
    for (const auto& a : acts) {
        EXPECT_GE(a.stage, 0) << a.id << ": stage 不能为负";
        EXPECT_LE(a.stage, 4) << a.id << ": stage 不能超过 4";
    }
}

TEST(LearningPathDataAudit, PrerequisitesReferenceExistingIds) {
    const auto& acts = LearningPathData::activities();
    std::set<std::string> ids;
    for (const auto& a : acts) ids.insert(a.id);

    for (const auto& a : acts) {
        for (const auto& preId : a.prerequisites) {
            EXPECT_NE(ids.find(preId), ids.end())
                << a.id << ": 前置 " << preId << " 在活动列表中不存在";
        }
    }
}

// ============================================================
// 套件 2：LearningPathProgressAudit — 进度持久化与计算
// ============================================================

TEST_F(LearningPathTestBase, FreshProgressHasNoCompletion) {
    const auto& data = LearnerProgressStore::instance().data();
    EXPECT_TRUE(data.completed.empty());
    EXPECT_TRUE(data.attemptCount.empty());
    EXPECT_EQ(data.currentStage, 0);
}

TEST_F(LearningPathTestBase, MarkCompletedAffectsUnlock) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 初始：lab-02 前置为 lab-01 + ast-toy，应锁定
    EXPECT_FALSE(store.isUnlocked("lab-02", all));

    // 完成 lab-01 后仍锁定（ast-toy 未完成）
    store.markCompleted("lab-01");
    EXPECT_FALSE(store.isUnlocked("lab-02", all));

    // 完成 ast-toy 后解锁
    store.markCompleted("ast-toy");
    EXPECT_TRUE(store.isUnlocked("lab-02", all));
}

TEST_F(LearningPathTestBase, StageProgressCalculation) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 阶段零 5 个活动，初始 0%
    EXPECT_EQ(store.stageProgress(0, all), 0);

    // 完成 welcome 后应 > 0%（5 个活动完成 1 个 = 20%）
    store.markCompleted("welcome");
    int sp = store.stageProgress(0, all);
    EXPECT_GT(sp, 0);
    EXPECT_LE(sp, 100);

    // 完成阶段零全部活动后应 100%
    store.markCompleted("code-journey");
    store.markCompleted("token-puzzle");
    store.markCompleted("ast-toy");
    store.markCompleted("vm-sandbox");
    EXPECT_EQ(store.stageProgress(0, all), 100);
}

TEST_F(LearningPathTestBase, OverallProgressCalculation) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    EXPECT_EQ(store.overallProgress(all), 0);

    // 完成一个活动后总进度 > 0
    store.markCompleted("welcome");
    int op = store.overallProgress(all);
    EXPECT_GT(op, 0);
    EXPECT_LE(op, 100);
}

TEST_F(LearningPathTestBase, NextRecommendedReturnsUnlockedUncompleted) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 初始推荐：阶段零无前置的活动之一
    std::string rec = store.nextRecommended(all);
    EXPECT_FALSE(rec.empty());

    // 推荐活动必须未完成
    const auto* act = LearningPathData::findById(rec);
    ASSERT_NE(act, nullptr);
    auto cit = store.data().completed.find(rec);
    bool isCompleted = (cit != store.data().completed.end() && cit->second);
    EXPECT_FALSE(isCompleted) << "推荐活动 " << rec << " 不应已完成";

    // 推荐活动必须已解锁
    EXPECT_TRUE(store.isUnlocked(rec, all))
        << "推荐活动 " << rec << " 必须已解锁";

    // 完成全部活动后推荐应为空
    for (const auto& a : all) {
        store.markCompleted(a.id);
    }
    EXPECT_TRUE(store.nextRecommended(all).empty());
}

TEST_F(LearningPathTestBase, ResetClearsAllProgress) {
    auto& store = LearnerProgressStore::instance();

    store.markCompleted("welcome");
    store.markCompleted("lab-01");
    store.recordAttempt("lab-02");
    ASSERT_FALSE(store.data().completed.empty());
    ASSERT_FALSE(store.data().attemptCount.empty());

    store.reset();
    EXPECT_TRUE(store.data().completed.empty());
    EXPECT_TRUE(store.data().attemptCount.empty());
    EXPECT_TRUE(store.data().lastAccessTime.empty());
    EXPECT_EQ(store.data().currentStage, 0);
}

// ============================================================
// 套件 3：LearningPathPrereqAudit — 前置依赖关系
// ============================================================

TEST(LearningPathPrereqAudit, PrerequisitesHaveNoCycle) {
    // 通过 DFS 检测环：从每个活动出发，沿 prerequisites 边走，若回到起点则有环
    const auto& all = LearningPathData::activities();

    std::map<std::string, const LearningActivity*> idMap;
    for (const auto& a : all) idMap[a.id] = &a;

    for (const auto& start : all) {
        std::set<std::string> visited;
        std::vector<std::string> stack;
        stack.push_back(start.id);

        while (!stack.empty()) {
            std::string cur = stack.back();
            stack.pop_back();

            if (visited.count(cur)) {
                // 已访问过——若等于起点则有环
                if (cur == start.id) {
                    FAIL() << "检测到环：从 " << start.id << " 出发可回到自身";
                }
                continue;
            }
            visited.insert(cur);

            auto it = idMap.find(cur);
            if (it == idMap.end()) continue;
            for (const auto& preId : it->second->prerequisites) {
                if (preId == start.id) {
                    FAIL() << "检测到环：" << start.id << " → ... → " << cur
                           << " → " << start.id;
                }
                stack.push_back(preId);
            }
        }
    }
    SUCCEED();
}

TEST(LearningPathPrereqAudit, StageZeroActivitiesHaveNoPrerequisites) {
    const auto& all = LearningPathData::activities();
    for (const auto& a : all) {
        if (a.stage == 0) {
            EXPECT_TRUE(a.prerequisites.empty())
                << a.id << ": 阶段零活动不应有前置依赖";
        }
    }
}

TEST(LearningPathPrereqAudit, Lab02PrereqContainsLab01) {
    const auto* lab02 = LearningPathData::findById("lab-02");
    ASSERT_NE(lab02, nullptr);
    ASSERT_FALSE(lab02->prerequisites.empty());

    bool hasLab01 = false;
    for (const auto& pre : lab02->prerequisites) {
        if (pre == "lab-01") { hasLab01 = true; break; }
    }
    EXPECT_TRUE(hasLab01) << "lab-02 的前置必须包含 lab-01";
}

TEST(LearningPathPrereqAudit, BugHuntExpertPrereqContainsIntermediate) {
    const auto* expert = LearningPathData::findById("bug-hunt-expert");
    ASSERT_NE(expert, nullptr);
    ASSERT_FALSE(expert->prerequisites.empty());

    bool hasIntermediate = false;
    for (const auto& pre : expert->prerequisites) {
        if (pre == "bug-hunt-intermediate") { hasIntermediate = true; break; }
    }
    EXPECT_TRUE(hasIntermediate)
        << "bug-hunt-expert 的前置必须包含 bug-hunt-intermediate（必须按顺序通关）";
}
