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
    // P1-F2/F fix: 22 原始活动 + 6 个浏览型面板轻量 visited-* 活动（glossary/pipeline/
    // memory-model/bytecode-trace/exception-flow/closure-inspector）= 28
    EXPECT_EQ(acts.size(), 28u)
        << "学习路径活动数应为 28（22 原始 + 6 visited-*）";
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

    // 学习限制已全部移除：所有活动无条件解锁，学员可自由访问任意章节/关卡。
    // 原 prerequisites 字段保留在数据中，但 isUnlocked 不再检查它。
    // 初始：lab-02 即可直接访问（曾需 lab-01 + ast-toy 前置）
    EXPECT_TRUE(store.isUnlocked("lab-02", all));

    // 完成 lab-01 后仍解锁（解锁状态不依赖完成进度）
    store.markCompleted("lab-01");
    EXPECT_TRUE(store.isUnlocked("lab-02", all));

    // 完成 ast-toy 后仍解锁
    store.markCompleted("ast-toy");
    EXPECT_TRUE(store.isUnlocked("lab-02", all));

    // 无效活动 id 仍返回 false
    EXPECT_FALSE(store.isUnlocked("nonexistent-activity", all));
}

TEST_F(LearningPathTestBase, StageProgressCalculation) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 阶段零 11 个活动（5 原始 + 6 visited-*），初始 0%
    EXPECT_EQ(store.stageProgress(0, all), 0);

    // 完成 welcome 后应 > 0%（11 个活动完成 1 个）
    store.markCompleted("welcome");
    int sp = store.stageProgress(0, all);
    EXPECT_GT(sp, 0);
    EXPECT_LE(sp, 100);

    // 完成阶段零全部活动后应 100%
    store.markCompleted("code-journey");
    store.markCompleted("token-puzzle");
    store.markCompleted("ast-toy");
    store.markCompleted("vm-sandbox");
    // P1-F2/F fix: 阶段零新增 6 个浏览型面板轻量活动，也需标记完成才能 100%
    store.markCompleted("visited-glossary");
    store.markCompleted("visited-pipeline");
    store.markCompleted("visited-memory-model");
    store.markCompleted("visited-bytecode-trace");
    store.markCompleted("visited-exception-flow");
    store.markCompleted("visited-closure-inspector");
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

// ============================================================
// 套件 4：LearningPathRichProgressAudit — 富维度进度数据（P2-3 fix F9）
// ------------------------------------------------------------
// 测试覆盖：
//   - recordScore / getScore / getBestStars
//   - addSpentMinutes / getSpentMinutes / totalSpentMinutes
//   - recordFailure / getFailCount
//   - getWeakPoints 薄弱点筛选与排序
//   - estimatedRemainingMinutes 剩余时间汇总
//   - load/save 往返持久化（含新字段）
// ============================================================

TEST_F(LearningPathTestBase, RecordScoreKeepsBest) {
    auto& store = LearnerProgressStore::instance();

    // 首次记录得分 80
    store.recordScore("lab-01", 80, 2);
    EXPECT_EQ(store.getScore("lab-01"), 80);
    EXPECT_EQ(store.getBestStars("lab-01"), 2);

    // 第二次得分 60（更低）——不应覆盖历史最佳
    store.recordScore("lab-01", 60, 1);
    EXPECT_EQ(store.getScore("lab-01"), 80)
        << "recordScore 必须保留历史最佳得分，60 不应覆盖 80";
    EXPECT_EQ(store.getBestStars("lab-01"), 2)
        << "recordScore 必须保留历史最佳星级，1 星不应覆盖 2 星";

    // 第三次得分 95（更高）——应覆盖
    store.recordScore("lab-01", 95, 3);
    EXPECT_EQ(store.getScore("lab-01"), 95);
    EXPECT_EQ(store.getBestStars("lab-01"), 3);
}

TEST_F(LearningPathTestBase, RecordScoreClampsOutOfRange) {
    auto& store = LearnerProgressStore::instance();
    // 负数截断到 0
    store.recordScore("lab-01", -50);
    EXPECT_EQ(store.getScore("lab-01"), 0);
    // 超过 100 截断到 100
    store.recordScore("lab-01", 200);
    EXPECT_EQ(store.getScore("lab-01"), 100);
}

TEST_F(LearningPathTestBase, SpentMinutesAccumulates) {
    auto& store = LearnerProgressStore::instance();

    EXPECT_EQ(store.getSpentMinutes("lab-01"), 0);

    store.addSpentMinutes("lab-01", 10);
    EXPECT_EQ(store.getSpentMinutes("lab-01"), 10);

    store.addSpentMinutes("lab-01", 5);
    EXPECT_EQ(store.getSpentMinutes("lab-01"), 15);

    // 负数或零应被忽略
    store.addSpentMinutes("lab-01", -3);
    store.addSpentMinutes("lab-01", 0);
    EXPECT_EQ(store.getSpentMinutes("lab-01"), 15)
        << "负数或零分钟不应改变累计时间";

    // totalSpentMinutes 应汇总所有活动
    store.addSpentMinutes("lab-02", 8);
    EXPECT_EQ(store.totalSpentMinutes(), 23);  // 15 + 8
}

TEST_F(LearningPathTestBase, RecordFailureIncrementsCount) {
    auto& store = LearnerProgressStore::instance();

    EXPECT_EQ(store.getFailCount("lab-02"), 0);

    store.recordFailure("lab-02");
    store.recordFailure("lab-02");
    store.recordFailure("lab-02");
    EXPECT_EQ(store.getFailCount("lab-02"), 3);
}

TEST_F(LearningPathTestBase, WeakPointsDetection) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 初始无薄弱点（无尝试无失败）
    EXPECT_TRUE(store.getWeakPoints(all).empty());

    // 对 welcome 尝试 4 次（>= 默认阈值 3）——应识别为薄弱点
    for (int i = 0; i < 4; ++i) store.recordAttempt("welcome");
    auto wp = store.getWeakPoints(all);
    ASSERT_EQ(wp.size(), 1u);
    EXPECT_EQ(wp[0].activityId, "welcome");
    EXPECT_EQ(wp[0].attempts, 4);

    // 对 lab-01 失败 2 次（>= 1）——也应识别，且失败次数更高的排前
    store.recordFailure("lab-01");
    store.recordFailure("lab-01");
    wp = store.getWeakPoints(all);
    ASSERT_EQ(wp.size(), 2u);
    EXPECT_EQ(wp[0].activityId, "lab-01");  // fails=2 排前
    EXPECT_EQ(wp[0].fails, 2);
    EXPECT_EQ(wp[1].activityId, "welcome");
    EXPECT_EQ(wp[1].fails, 0);

    // maxCount 限制
    wp = store.getWeakPoints(all, 3, 1);
    ASSERT_EQ(wp.size(), 1u);

    // 已完成的活动不应出现在薄弱点中
    store.markCompleted("welcome");
    wp = store.getWeakPoints(all);
    ASSERT_EQ(wp.size(), 1u);
    EXPECT_EQ(wp[0].activityId, "lab-01");
}

TEST_F(LearningPathTestBase, EstimatedRemainingMinutesExcludesCompletedAndLocked) {
    auto& store = LearnerProgressStore::instance();
    const auto& all = LearningPathData::activities();

    // 学习限制已移除：所有活动无条件解锁，剩余时间 = 所有未完成活动之和。
    // 初始：剩余时间 > 0（含所有未完成活动，包括曾需前置的 lab-02 等）
    int initial = store.estimatedRemainingMinutes(all);
    EXPECT_GT(initial, 0);

    // 完成 welcome 后，剩余时间应减少（welcome.estimatedMinutes）
    store.markCompleted("welcome");
    int after = store.estimatedRemainingMinutes(all);
    EXPECT_LT(after, initial)
        << "完成一个活动后剩余时间应减少";

    // lab-02 曾需前置（lab-01 + ast-toy），但现在无条件解锁——
    // 完成其前置不会增加剩余时间（lab-02 从一开始就被计入）。
    // 完成前置活动反而会减少剩余时间（lab-01 / ast-toy 自身时间被扣除）。
    store.markCompleted("lab-01");
    store.markCompleted("ast-toy");
    int afterPrereq = store.estimatedRemainingMinutes(all);
    EXPECT_LT(afterPrereq, after)
        << "完成 lab-01 + ast-toy 后剩余时间应减少（它们自身时间被扣除）";
}

TEST_F(LearningPathTestBase, LoadSaveRoundTripPreservesRichFields) {
    auto& store = LearnerProgressStore::instance();

    // 写入富维度数据
    store.recordScore("lab-01", 85, 2);
    store.addSpentMinutes("lab-01", 12);
    store.recordFailure("lab-02");
    store.recordAttempt("lab-03");
    store.markCompleted("lab-04");

    ASSERT_TRUE(store.save());

    // 重置后重新加载
    LearnerProgressStore::instance().resetForTesting();
    EXPECT_EQ(store.getScore("lab-01"), 0);
    EXPECT_EQ(store.getSpentMinutes("lab-01"), 0);
    EXPECT_EQ(store.getFailCount("lab-02"), 0);

    ASSERT_TRUE(store.load());

    // 验证往返持久化正确
    EXPECT_EQ(store.getScore("lab-01"), 85);
    EXPECT_EQ(store.getBestStars("lab-01"), 2);
    EXPECT_EQ(store.getSpentMinutes("lab-01"), 12);
    EXPECT_EQ(store.getFailCount("lab-02"), 1);

    // 完成状态也应保留
    auto cit = store.data().completed.find("lab-04");
    ASSERT_NE(cit, store.data().completed.end());
    EXPECT_TRUE(cit->second);
}

TEST_F(LearningPathTestBase, ResetClearsRichFields) {
    auto& store = LearnerProgressStore::instance();

    store.recordScore("lab-01", 90, 3);
    store.addSpentMinutes("lab-01", 20);
    store.recordFailure("lab-02");

    store.reset();
    EXPECT_TRUE(store.data().score.empty());
    EXPECT_TRUE(store.data().bestStars.empty());
    EXPECT_TRUE(store.data().spentMinutes.empty());
    EXPECT_TRUE(store.data().failCount.empty());
    EXPECT_EQ(store.totalSpentMinutes(), 0);
}
