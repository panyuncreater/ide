// ============================================================
// LearnerProgress.h — 学习进度持久化（功能 6）
// ------------------------------------------------------------
// LearnerProgressStore 单例管理 LearnerProgress 数据：
//   - completed:    已完成的活动 ID → bool
//   - attemptCount: 每个活动的尝试次数
//   - lastAccessTime: 最后访问时间（unix timestamp）
//   - currentStage: 当前阶段（0-4）
//
// 持久化：
//   - JSON 文件 ~/.minilang_progress.json（路径由 QStandardPaths 决定）
//   - 使用 QJsonDocument / QJsonObject / QJsonArray
//   - 加载失败/格式不匹配时回退到空进度，不崩溃
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
};

// ============================================================
// LearnerProgressStore — 进度持久化单例
// ============================================================
class LearnerProgressStore {
public:
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
