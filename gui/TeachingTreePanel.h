// ============================================================
// TeachingTreePanel.h — 教学面板树形导航
// ------------------------------------------------------------
// 替代原 LearningHubDialog 弹窗模式：在左侧停靠区呈现一棵
// 类似资源管理器的可折叠树，4 大分类（入门导览 / 编译前端 /
// 执行引擎 / 深入实战）+ 顶部「代码编辑器」入口。
//
// 点击叶子节点 → 发射 panelRequested(panelId) 信号，由 Ide 切换
// 中央 centerStack_ 显示对应教学面板；点击「代码编辑器」→
// 发射 panelRequested("editor") 切回代码编辑器+输出面板。
// ============================================================

#pragma once

#include <QMap>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;

class TeachingTreePanel : public QWidget {
    Q_OBJECT
public:
    explicit TeachingTreePanel(QWidget* parent = nullptr);

    /// 选中指定面板 ID 的叶子节点（用于跨面板跳转时同步树高亮）
    /// panelId 取值见构造函数中的 4 大分类；"editor" 选中代码编辑器项
    void setCurrentPanel(const QString& panelId);

signals:
    /// 用户点击叶子节点时发射
    /// panelId 取值："editor" / "welcome" / "code-journey" / "learning-path"
    /// / "glossary" / "pipeline" / "token-puzzle" / "ast-toy" /
    /// "syntax-explorer" / "backend-compare" / "vm-sandbox" /
    /// "memory-model" / "ir-transform" / "bytecode-trace" /
    /// "call-stack" / "variable-inspector" / "breakpoint-condition" /
    /// "bug-hunt" / "exception-flow" / "closure-inspector" /
    /// "profile-dashboard" / "lab-manual"
    void panelRequested(const QString& panelId);

private slots:
    /// 搜索框文本变化 → 按包含匹配过滤叶子节点
    void onSearchChanged(const QString& text);

private:
    QTreeWidget* tree_ = nullptr;
    QTreeWidgetItem* editorItem_ = nullptr; // 「代码编辑器」特殊项
    /// panelId → tree item 映射，用于 setCurrentPanel 高亮
    QMap<QString, QTreeWidgetItem*> idToItem_;

    // B1: 顶部标题横幅
    QWidget* headerBanner_ = nullptr;
    QLabel* bannerTitle_ = nullptr;    // 「学习中心」
    QLabel* bannerSubtitle_ = nullptr; // 「点击下方任意主题开始学习」
    // B3: 搜索框
    QLineEdit* searchEdit_ = nullptr;

    void buildTree();
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemActivated(QTreeWidgetItem* item, int column);
};
