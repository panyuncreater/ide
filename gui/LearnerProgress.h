// ============================================================
// LearnerProgress.h — 学习进度持久化（功能 6）
// ------------------------------------------------------------
// LearnerProgressStore 单例管理 LearnerProgress 数据：
//   - completed:    已完成的活动 ID → bool
//   - attemptCount: 每个活动的尝试次数
//   - lastAccessTime: 最后访问时间（unix timestamp）
//   - currentStage: 当前阶段（0-4）
//   - levelStars:   关卡细粒度星级（P0-2 fix F7）
//   - score:        每活动得分（0-100，P2-3 fix F9 学情画像）
//   - bestStars:    每活动历史最佳星级（与 levelStars 不同——
//                   levelStars 针对子关卡，bestStars 针对活动整体）
//   - spentMinutes: 每活动累计花费分钟数（P2-3 fix F9）
//   - failCount:    每活动失败次数（P2-3 fix F9 薄弱点判定依据）
//
// 持久化：
//   - JSON 文件 ~/.minilang_progress.json（路径由 QStandardPaths 决定）
//   - 使用 QJsonDocument / QJsonObject / QJsonArray
//   - 加载失败/格式不匹配时回退到空进度，不崩溃
//   - 旧版本 JSON 缺失新字段时按默认值 0 处理，向后兼容
//
// 测试支持：
//   - resetForTesting() 清空内部状态
//   - 测试可通过设置临时文件路径或调用 setFilePathForTesting() 注入
// ============================================================

#pragma once

#include "gui/LearningPathData.h"

#include <QString>
#include <map>
#include <string>
#include <cstdint>
#include <vector>

// ============================================================
// LearnerProgress — 进度数据
// ============================================================
struct LearnerProgress {
    std::map<std::string, bool>    completed;        // 已完成的活动
    std::map<std::string, int>     attemptCount;     // 尝试次数
    std::map<std::string, int64_t> lastAccessTime;   // 最后访问时间（unix timestamp）
    int currentStage = 0;                             // 当前阶段（0-4）
    // P0-2 fix (F7): 细粒度关卡星级持久化。
    // key = 关卡 ID（如 "token-puzzle-1"），value = 星级（-1=未完成，0=跳过，1-3=星级）。
    // 游戏面板（TokenPuzzle/AstToy/VmSandbox）在构造时读取、在关卡完成时写入，
    // 解决原 levelStars_ 仅存内存、重启即丢失的问题。
    std::map<std::string, int>     levelStars;
    // P2-3 fix (F9): 学情画像——多维进度数据，让进度从「打卡」升级为「画像」。
    // score:        key = 活动 ID, value = 得分（0-100）。recordScore 取最大值（保留历史最佳）。
    // bestStars:    key = 活动 ID, value = 1-3 星级（与 levelStars 不同——levelStars 针对子关卡，
    //               bestStars 针对活动整体，由 markActivityScore 在完成时一并记录）。
    // spentMinutes: key = 活动 ID, value = 累计花费分钟数。addSpentMinutes 累加。
    // failCount:    key = 活动 ID, value = 失败次数（用于 getWeakPoints 薄弱点判定）。
    std::map<std::string, int>     score;
    std::map<std::string, int>     bestStars;
    std::map<std::string, int>     spentMinutes;
    std::map<std::string, int>     failCount;
};

// ============================================================
// WeakPoint — 薄弱点活动信息（P2-3 fix F9）
// ============================================================
struct WeakPoint {
    std::string activityId;   // 活动 ID
    int         attempts = 0;  // 尝试次数
    int         fails    = 0;  // 失败次数
    int         score    = 0;  // 当前得分（0 表示无得分记录）
};

// ============================================================
// LearnerProgressStore — 进度持久化单例
// ============================================================
class LearnerProgressStore {
public:
/// 返回全局单例实例。
    static LearnerProgressStore& instance();

    /// 从 JSON 文件加载进度（找不到文件或格式不匹配时清空，不崩溃）
    bool load();

    /// 保存进度到 JSON 文件
    bool save() const;

    /// 获取只读数据引用
    const LearnerProgress& data() const { return data_; }

    // ---- 修改接口 ----

    /// 标记活动为已完成（同时记录访问时间）
    void markCompleted(const std::string& activityId);

    /// 记录一次尝试（不改变完成状态，仅递增 attemptCount + 更新访问时间）
    void recordAttempt(const std::string& activityId);

    /// P1-2 fix (F8): 根据 activities 完成情况重算 currentStage。
    /// 规则：currentStage = "完成比例 >= 50% 的最高阶段 + 1"（即下一未完成阶段），
    /// 若所有阶段都已 >= 50%，则设为最后一个阶段；若全部 100% 完成，则等于 stageCount。
    /// 调用方需传入完整活动列表（如 LearningPathData::activities()）。
    void recomputeStage(const std::vector<LearningActivity>& all);

    /// P0-2 fix (F7): 记录关卡的星级（细粒度进度持久化）
    /// levelId 如 "token-puzzle-1"，stars: -1=未完成, 0=跳过, 1-3=星级
    void markLevelStars(const std::string& levelId, int stars);

    /// P0-2 fix (F7): 查询关卡的星级（-1 表示未完成/未记录）
    int getLevelStars(const std::string& levelId) const;

    /// P0-2 fix (F7): 检查一组关卡是否全部已完成（stars >= 0）。
    /// 用于"仅当全部子关卡完成时才标记父活动为已完成"逻辑。
    bool areAllLevelsCompleted(const std::vector<std::string>& levelIds) const;

    /// P2-3 fix (F9): 记录活动得分（保留历史最佳——仅当新分 >= 旧分时覆盖）。
    /// activityId: 活动 ID, score: 0-100。可选 stars 1-3 用于活动整体星级记录。
    void recordScore(const std::string& activityId, int score, int stars = 0);

    /// P2-3 fix (F9): 查询活动得分（0 表示未记录）。
    int getScore(const std::string& activityId) const;

    /// P2-3 fix (F9): 查询活动历史最佳星级（0 表示未记录）。
    int getBestStars(const std::string& activityId) const;

    /// P2-3 fix (F9): 累加活动花费时间（分钟）。负数视为 0。
    void addSpentMinutes(const std::string& activityId, int minutes);

    /// P2-3 fix (F9): 查询活动累计花费时间（分钟，0 表示未记录）。
    int getSpentMinutes(const std::string& activityId) const;

    /// P2-3 fix (F9): 记录一次失败（failCount += 1，不改变完成状态）。
    void recordFailure(const std::string& activityId);

    /// P2-3 fix (F9): 查询活动失败次数。
    int getFailCount(const std::string& activityId) const;

    /// P2-3 fix (F9): 返回薄弱点活动列表——筛选条件：
    ///   - 未完成
    ///   - attemptCount >= minAttempts（默认 3 次）或 failCount >= 1
    /// 按失败次数降序、尝试次数降序排序。all 提供活动元数据。
    /// maxCount 限制返回数量（默认 5，0 表示不限制）。
    std::vector<WeakPoint> getWeakPoints(
        const std::vector<LearningActivity>& all,
        int minAttempts = 3,
        std::size_t maxCount = 5) const;

    /// P2-3 fix (F9): 计算预计剩余时间（分钟）——所有未完成且已解锁活动的 estimatedMinutes 之和。
    /// all 提供活动元数据。
    int estimatedRemainingMinutes(const std::vector<LearningActivity>& all) const;

    /// P2-3 fix (F9): 计算累计花费总时间（分钟）——所有活动 spentMinutes 之和。
    int totalSpentMinutes() const;

    /// 重置所有进度（清空 completed/attemptCount/lastAccessTime，currentStage=0）
    void reset();

    /// 测试专用：清空内部状态（与 reset() 等价，但显式标注测试用途）
    void resetForTesting() { reset(); }

    /// 测试专用：注入文件路径，避免污染用户家目录
    void setFilePathForTesting(const QString& path);

    // ---- 查询接口 ----

    /// 检查活动是否已解锁（前置全部完成）
    bool isUnlocked(const std::string& activityId,
                    const std::vector<LearningActivity>& all) const;

    /// 阶段进度百分比（0-100），分母为该阶段活动总数
    int stageProgress(int stage,
                      const std::vector<LearningActivity>& all) const;

    /// 总体进度百分比（0-100），分母为所有活动总数
    int overallProgress(const std::vector<LearningActivity>& all) const;

    /// 返回下一步推荐活动 ID：
    ///   - 优先：未完成 + 已解锁 + stage 最小
    ///   - 同 stage 内优先：无前置的、attemptCount 最少、estimatedMinutes 最少
    ///   - 全部完成时返回空字符串
    std::string nextRecommended(const std::vector<LearningActivity>& all) const;

private:
    LearnerProgressStore() = default;
    LearnerProgressStore(const LearnerProgressStore&) = delete;
    LearnerProgressStore& operator=(const LearnerProgressStore&) = delete;

    LearnerProgress data_;
    QString filePathOverride_;  // 测试注入路径（空时使用默认路径）

    QString filePath() const;  // 默认 ~/.minilang_progress.json 或 Qt 标准路径
};
