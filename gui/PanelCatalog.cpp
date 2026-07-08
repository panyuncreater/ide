// ============================================================
// PanelCatalog.cpp — 教学面板导航数据统一目录实现（P2-1 fix F1/F17）
// ------------------------------------------------------------
// 从 TeachingTreePanel.cpp 抽取静态 categories 数据为单一数据源。
// 从 app/ide.cpp::onActivityRequested 抽取别名映射规则。
// 删除 gui/LearningHubDialog.h/.cpp（已被 TeachingTreePanel 替代）。
// ============================================================

#include "gui/PanelCatalog.h"

#include <algorithm>

// ============================================================
// 4 大分类 + 全部叶子节点（与原 TeachingTreePanel::buildTree 一致）
// ============================================================
/// 返回全部面板分类树（含叶子面板元数据，静态数据）。
const std::vector<PanelCategory>& PanelCatalog::categories() {
    static const std::vector<PanelCategory> kCategories = {
        {"入门导览",
         "🌱",
         {
             {"welcome", "欢迎向导", "🚀"},
             {"code-journey", "代码生命旅程", "🛤️"},
             {"learning-path", "学习路径地图", "🗺️"},
             {"glossary", "术语表", "📚"},
         }},
        {"编译前端",
         "🔤",
         {
             {"pipeline", "编译管线可视化", "🔗"},
             {"token-puzzle", "Token 拼图", "🧩"},
             {"ast-toy", "AST 构建器", "🌳"},
             {"syntax-explorer", "语法浏览器", "📖"},
         }},
        {"执行引擎",
         "⚙️",
         {
             {"backend-compare", "三后端对比", "⚖️"},
             {"vm-sandbox", "VM 沙盒", "🎮"},
             {"memory-model", "内存模型", "🧠"},
             {"ir-transform", "IR 优化回放", "✨"},
             {"bytecode-trace", "字节码追踪", "📡"},
             {"call-stack", "调用栈检查器", "📚"},
             {"variable-inspector", "变量检查器", "🔍"},
             {"breakpoint-condition", "条件断点", "🛑"},
         }},
        {"深入实战",
         "🏗️",
         {
             {"bug-hunt", "Bug 狩猎", "🐛"},
             {"exception-flow", "异常流可视化", "⚡"},
             {"closure-inspector", "闭包检查器", "🔒"},
             {"profile-dashboard", "性能仪表盘", "📊"},
             {"lab-manual", "实验手册", "📘"},
         }},
    };
    return kCategories;
}

// ============================================================
// 按 id 查找 PanelEntry
// ============================================================
/// 按面板 id 在分类树中查找对应条目；未找到返回 nullptr。
const PanelEntry* PanelCatalog::findById(const std::string& id) {
    for (const auto& cat : categories()) {
        for (const auto& leaf : cat.leaves) {
            if (id == leaf.id) {
                return &leaf;
            }
        }
    }
    return nullptr;
}

// ============================================================
// 活动 id → 面板 id 的别名映射
// ------------------------------------------------------------
// 处理四类别名：
//   1. lab-*  → lab-manual（实验手册章节）
//   2. bug-hunt-* → bug-hunt（Bug 狩猎难度变体）
//   3. op-priority-challenge → ast-toy（运算符优先级挑战路由到 AST 玩具）
//   4. visited-* → 浏览型面板进入即完成活动（P1-F2/F）
// 有效面板 id 原样返回。welcome / freeform-project 等需调用方特殊处理的 id
// 返回空字符串。
// ============================================================
static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

/// 将学习活动 id 规范化为其对应的面板 id。
std::string PanelCatalog::canonicalPanelId(const std::string& activityId) {
    // 别名：lab-01 ~ lab-99 → lab-manual
    if (startsWith(activityId, "lab-") && activityId != "lab-manual") {
        return "lab-manual";
    }
    // 别名：bug-hunt-beginner / intermediate / expert → bug-hunt
    if (startsWith(activityId, "bug-hunt-")) {
        return "bug-hunt";
    }
    // 别名：运算符优先级挑战 → AST 玩具
    if (activityId == "op-priority-challenge") {
        return "ast-toy";
    }
    // P1-F2/F fix: 浏览型面板的"进入即完成"活动别名
    // 学习路径中的 visited-* 活动对应一个真实面板，需能被 canonicalPanelId 识别。
    if (activityId == "visited-glossary")
        return "glossary";
    if (activityId == "visited-pipeline")
        return "pipeline";
    if (activityId == "visited-memory-model")
        return "memory-model";
    if (activityId == "visited-bytecode-trace")
        return "bytecode-trace";
    if (activityId == "visited-exception-flow")
        return "exception-flow";
    if (activityId == "visited-closure-inspector")
        return "closure-inspector";
    // 校验是否为已注册面板 id
    if (findById(activityId) != nullptr) {
        return activityId;
    }
    // welcome / freeform-project / 未知 id：返回空，调用方特殊处理
    return std::string();
}

// ============================================================
// 所有叶子 id 列表（用于校验/枚举）
// ============================================================
/// 返回分类树中所有叶子面板的 id 列表。
std::vector<std::string> PanelCatalog::allPanelIds() {
    std::vector<std::string> ids;
    for (const auto& cat : categories()) {
        for (const auto& leaf : cat.leaves) {
            ids.emplace_back(leaf.id);
        }
    }
    return ids;
}
