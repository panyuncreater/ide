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
// 6 大分类 + 全部叶子节点（UX-R fix: 按编译原理学习曲线递进重组）
// ------------------------------------------------------------
// 原 4 分类中「执行引擎」23 项平铺，初学者难以定位；拆分为：
//   执行引擎（怎么跑起来）/ 内存与优化（怎么变快变省）/ 调试与观测（怎么看清楚）
// 每个面板标注难度分级（1=入门 / 2=进阶 / 3=高级），分类内按难度递增排序。
// ============================================================
/// 返回全部面板分类树（含叶子面板元数据，静态数据）。
const std::vector<PanelCategory>& PanelCatalog::categories() {
    static const std::vector<PanelCategory> kCategories = {
        {"入门导览",
         "🌱",
         "第一次用？从这里开始：建立对编译全流程的心智地图",
         {
             {"welcome", "欢迎向导", "🚀", 1},
             {"code-journey", "代码生命旅程", "🛤️", 1},
             {"learning-path", "学习路径地图", "🗺️", 1},
             {"course-system", "教学课程系统", "📚", 1},
             {"glossary", "术语表", "📚", 1},
         }},
        {"编译前端",
         "🔤",
         "源码怎么变成 Token 和语法树：词法分析与语法分析",
         {
             {"pipeline", "编译管线可视化", "🔗", 1},
             {"token-puzzle", "Token 拼图", "🧩", 1},
             {"ast-toy", "AST 构建器", "🌳", 1},
             {"syntax-explorer", "语法浏览器", "📖", 1},
             {"ast-editor", "AST 可视化编辑器", "🎨", 2},
             {"lint-explorer", "静态分析探索器", "🔍", 2},
         }},
        {"执行引擎",
         "⚙️",
         "代码怎么跑起来：三后端、字节码与虚拟机执行",
         {
             {"vm-sandbox", "VM 沙盒", "🎮", 1},
             {"step-explainer", "执行步骤讲解", "📝", 1},
             {"backend-compare", "三后端对比", "⚖️", 2},
             {"bytecode-trace", "字节码追踪", "📡", 2},
             {"backend-parallel", "三后端并行可视化", "🔀", 2},
             {"performance-race", "三后端性能竞赛", "🏁", 2},
             {"jit-visualizer", "JIT 编译可视化", "⚡", 3},
             {"coroutine-visualizer", "协程/生成器可视化", "🔄", 3},
         }},
        {"内存与优化",
         "🧠",
         "值怎么存、垃圾怎么回收、代码怎么变快：内存模型与 IR 优化",
         {
             {"memory-model", "内存模型", "🧠", 2},
             {"memory-layout", "内存布局可视化", "🧠", 2},
             {"gc-visualizer", "GC 垃圾回收可视化", "🗑️", 2},
             {"ir-transform", "IR 优化回放", "✨", 2},
             {"inline-cache", "内联缓存可视化", "💡", 3},
             {"loop-unrolling", "循环展开可视化", "🔁", 3},
             {"escape-analysis", "逃逸分析可视化", "🚪", 3},
             {"register-allocator", "寄存器分配可视化", "🎯", 3},
         }},
        {"调试与观测",
         "🔬",
         "把运行中的程序看清楚：断点、调用栈、变量与时间旅行调试",
         {
             {"call-stack", "调用栈检查器", "📚", 1},
             {"variable-inspector", "变量检查器", "🔍", 1},
             {"breakpoint-condition", "条件断点", "🛑", 2},
             {"watch-expressions", "观察表达式", "👀", 2},
             {"watchpoint", "数据断点", "🔬", 2},
             {"execution-timeline", "可回放执行时间轴", "🎞️", 2},
             {"reverse-timeline", "反向调试时间轴", "⏪", 3},
         }},
        {"深入实战",
         "🏗️",
         "学完动手练：实验、练习、找 Bug 与性能分析实战",
         {
             {"lab-manual", "实验手册", "📘", 1},
             {"exercise-grader", "交互式练习评分", "📝", 1},
             {"exception-flow", "异常流可视化", "⚡", 2},
             {"closure-inspector", "闭包检查器", "🔒", 2},
             {"bug-hunt", "Bug 狩猎", "🐛", 2},
             {"module-system", "模块系统可视化", "📦", 2},
             {"profile-dashboard", "性能仪表盘", "📊", 2},
             {"fuzz-playground", "模糊测试游乐场", "🎲", 3},
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

// ============================================================
// UX-R fix: 难度分级与分类归属查询
// ============================================================
/// 难度分级显示名（越界兼容返回 "入门"，保证 UI 展示永远有效）。
const char* PanelCatalog::levelName(int level) {
    switch (level) {
    case 2:
        return "进阶";
    case 3:
        return "高级";
    default:
        return "入门";
    }
}

/// 返回面板所属分类标题（未找到返回空字符串）。
std::string PanelCatalog::categoryTitleOf(const std::string& id) {
    for (const auto& cat : categories()) {
        for (const auto& leaf : cat.leaves) {
            if (id == leaf.id) {
                return cat.title;
            }
        }
    }
    return std::string();
}
