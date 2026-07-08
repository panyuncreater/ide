// ============================================================
// LearningPathData.h — 学习路径活动数据定义（功能 6）
// ------------------------------------------------------------
// 定义 4 阶段学习路径地图的所有活动项（welcome 导览 / lab-01~08 /
// Bug 狩猎三档 / 自由项目等），含前置依赖关系。
//
// 本文件仅包含数据结构与静态数据接口声明，不依赖 IdeController.h
// 或任何引擎层文件——与 BugHuntLibrary.cpp / SyntaxProductionLibrary.cpp
// 的设计模式一致，便于测试目标独立链接。
//
// 阶段划分：
//   阶段零（0）：首次接触（welcome / journey / token-puzzle / ast-toy / vm-sandbox）
//   阶段一（1）：编译前端（lab-01 / lab-02 / syntax-explorer / op-priority-challenge）
//   阶段二（2）：执行引擎（lab-03 / lab-04 / lab-05 / backend-compare）
//   阶段三（3）：深入理解（lab-06 / lab-07 / ir-transform / profile-dashboard）
//   阶段四（4）：实战训练（bug-hunt-beginner / intermediate / expert / freeform）
// ============================================================

#pragma once

#include <string>
#include <vector>

// ============================================================
// ActivityType — 活动类型分类
// ============================================================
enum class ActivityType {
    LAB,       // 实验（lab-01~08）
    PUZZLE,    // 拼图游戏（token-puzzle）
    TOY,       // 玩具搭建（ast-toy）
    SANDBOX,   // 沙盒（vm-sandbox）
    CHALLENGE, // 挑战（op-priority-challenge / bug-hunt-*）
    FREEFORM   // 自由项目
};

/// 将 ActivityType 转为字符串（用于 UI 显示与测试断言）
inline const char* activityTypeToString(ActivityType t) {
    switch (t) {
    case ActivityType::LAB:
        return "LAB";
    case ActivityType::PUZZLE:
        return "PUZZLE";
    case ActivityType::TOY:
        return "TOY";
    case ActivityType::SANDBOX:
        return "SANDBOX";
    case ActivityType::CHALLENGE:
        return "CHALLENGE";
    case ActivityType::FREEFORM:
        return "FREEFORM";
    }
    return "UNKNOWN";
}

// ============================================================
// LearningActivity — 单个学习活动项
// ============================================================
struct LearningActivity {
    std::string id;                         // 唯一标识（如 "lab-01" / "welcome" / "bug-hunt-beginner"）
    std::string title;                      // 显示名称
    std::string description;                // 简短描述
    int stage = 0;                          // 所属阶段 (0-4)
    std::vector<std::string> prerequisites; // 前置活动 ID 列表（必须全部完成才能解锁）
    int estimatedMinutes = 0;               // 预计耗时（分钟）
    ActivityType type = ActivityType::LAB;  // 活动类型
    std::string iconHint;                   // 图标提示（emoji 或 icon name）
};

// ============================================================
// LearningPathData — 学习路径活动静态数据
// ------------------------------------------------------------
// 所有活动数据嵌入 .cpp 文件，与 BugHuntLibrary.cpp 模式一致。
// 通过 activities() 接口提供只读访问，首次调用时初始化、后续
// 调用零开销。
// ============================================================
class LearningPathData {
public:
    /// 返回所有学习活动（按 stage + id 排序）
    static const std::vector<LearningActivity>& activities();

    /// 根据 ID 查找活动，找不到返回 nullptr
    static const LearningActivity* findById(const std::string& id);

    /// 返回指定阶段的所有活动
    static std::vector<const LearningActivity*> byStage(int stage);

    /// 阶段总数（5：0~4）
    static int stageCount() { return 5; }
};
