// ============================================================
// PanelCatalog.h — 教学面板导航数据统一目录（P2-1 fix F1/F17）
// ------------------------------------------------------------
// 解决问题：原 TeachingTreePanel 内嵌静态 4 分类 24 面板数据，
// 与 app/ide.cpp::onActivityRequested 的 if-else 路由表存在两份并行
// 描述（id ↔ 显示名 ↔ 分类）。新增/重命名面板时需同时维护两处，
// 易遗漏。本目录抽取为单一数据源（Single Source of Truth）。
//
// 同时废弃 gui/LearningHubDialog.h/.cpp——该弹窗在第十四轮重构
// 已被 TeachingTreePanel 替代，仅作为编译单元保留。本次彻底移除。
//
// 数据结构：
//   PanelEntry { id, label, emoji, level } —— 单个教学面板元数据（level 难度分级）
//   PanelCategory { title, emoji, description, leaves } —— 分类节点
//
// UX-R fix: 分类按「编译原理学习曲线」重组为 6 大类（入门导览 → 编译前端 →
// 执行引擎 → 内存与优化 → 调试与观测 → 深入实战），原「执行引擎」23 项平铺
// 拆分为 3 类；每个面板标注难度分级（1=入门 / 2=进阶 / 3=高级），供导航树
// 徽标与帮助弹窗展示，帮助初学者按难度循序渐进。
//
// API：
//   PanelCatalog::categories() —— 返回 6 大分类 + 全部叶子节点（树形导航用）
//   PanelCatalog::findById(id) —— 按 id 查找 PanelEntry（nullptr = 未找到）
//   PanelCatalog::canonicalPanelId(activityId) —— 处理别名（lab-* / bug-hunt-* /
//                                  op-priority-challenge / welcome / freeform-project）
//                                  返回空字符串表示无映射（需调用方特殊处理）
//   PanelCatalog::allPanelIds() —— 返回所有叶子 id 列表（用于校验/枚举）
//   PanelCatalog::levelName(level) —— 难度分级显示名（入门/进阶/高级）
//   PanelCatalog::categoryTitleOf(id) —— 面板所属分类标题（未找到返回空串）
// ============================================================

#pragma once

#include <optional>
#include <string>
#include <vector>

/// 单个教学面板元数据
struct PanelEntry {
    const char* id;    // 面板唯一标识（如 "vm-sandbox"）
    const char* label; // 显示名（如 "VM 沙盒"，const char* 字面量；由消费方 TeachingTreePanel 通过 mlTr(leaf.label)
                       // 包裹国际化）
    const char* emoji; // 前缀 emoji
    int level = 1;     // UX-R fix: 难度分级（1=入门 / 2=进阶 / 3=高级），导航树徽标与帮助弹窗展示
};

/// 教学面板分类节点
struct PanelCategory {
    const char* title;       // 分类标题（如 "入门导览"）
    const char* emoji;       // 分类前缀 emoji
    const char* description; // UX-R fix: 分类学习目标一句话说明（导航树 tooltip 展示）
    std::vector<PanelEntry> leaves;
};

/// 教学面板统一目录
/// 实现位于 gui/PanelCatalog.cpp（独立编译单元，仅依赖 STL + Qt6::Core 用于 mlTr）
class PanelCatalog {
public:
    /// 返回 6 大分类 + 全部叶子节点（TeachingTreePanel 构建树用），按学习曲线递进排序
    static const std::vector<PanelCategory>& categories();

    /// 按 id 查找 PanelEntry（找不到返回 nullptr）
    static const PanelEntry* findById(const std::string& id);

    /// 处理活动 id 到面板 id 的别名映射：
    ///   "lab-01" ~ "lab-08" → "lab-manual"
    ///   "bug-hunt-beginner" / "bug-hunt-intermediate" / "bug-hunt-expert" → "bug-hunt"
    ///   "op-priority-challenge" → "ast-toy"
    ///   其他 id 原样返回（如果是有效面板 id）
    /// 返回空字符串表示无映射（welcome/freeform-project 等需调用方特殊处理）
    static std::string canonicalPanelId(const std::string& activityId);

    /// 返回所有叶子节点的 id 列表（用于校验）
    static std::vector<std::string> allPanelIds();

    /// UX-R fix: 难度分级显示名（1="入门" / 2="进阶" / 3="高级"，越界返回 "入门"）
    static const char* levelName(int level);

    /// UX-R fix: 返回面板所属分类标题（未找到返回空字符串）
    static std::string categoryTitleOf(const std::string& id);
};
