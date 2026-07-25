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
//   PanelEntry { id, label, emoji }   —— 单个教学面板元数据
//   PanelCategory { title, emoji, leaves } —— 分类节点
//
// API：
//   PanelCatalog::categories() —— 返回 4 大分类 + 全部叶子节点（树形导航用）
//   PanelCatalog::findById(id) —— 按 id 查找 PanelEntry（nullptr = 未找到）
//   PanelCatalog::canonicalPanelId(activityId) —— 处理别名（lab-* / bug-hunt-* /
//                                  op-priority-challenge / welcome / freeform-project）
//                                  返回空字符串表示无映射（需调用方特殊处理）
//   PanelCatalog::allPanelIds() —— 返回所有叶子 id 列表（用于校验/枚举）
// ============================================================

#pragma once

#include <optional>
#include <string>
#include <vector>

/// 单个教学面板元数据
struct PanelEntry {
    const char* id; // 面板唯一标识（如 "vm-sandbox"）
    const char* label; // 显示名（如 "VM 沙盒"，const char* 字面量；由消费方 TeachingTreePanel 通过 mlTr(leaf.label)
                       // 包裹国际化）
    const char* emoji; // 前缀 emoji
};

/// 教学面板分类节点
struct PanelCategory {
    const char* title; // 分类标题（如 "入门导览"）
    const char* emoji; // 分类前缀 emoji
    std::vector<PanelEntry> leaves;
};

/// 教学面板统一目录
/// 实现位于 gui/PanelCatalog.cpp（独立编译单元，仅依赖 STL + Qt6::Core 用于 mlTr）
class PanelCatalog {
public:
    /// 返回 4 大分类 + 全部叶子节点（TeachingTreePanel 构建树用）
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
};
