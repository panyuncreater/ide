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
#include <QSet>
#include <QStringList>
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

    /// 切换某面板的收藏状态（★ 置顶）。持久化到 QSettings。
    void toggleFavorite(const QString& panelId);

signals:
    /// 用户点击叶子节点时发射
    void panelRequested(const QString& panelId);

private slots:
    /// 搜索框文本变化 → 按包含匹配过滤叶子节点
    void onSearchChanged(const QString& text);
    /// 分类展开/折叠状态变化 → 持久化到 QSettings
    void onItemExpanded(QTreeWidgetItem* item);
    void onItemCollapsed(QTreeWidgetItem* item);

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

    // 收藏分类节点（动态构建，置顶于 4 大分类之前）
    QTreeWidgetItem* favoritesCatItem_ = nullptr;
    // 最近访问分类节点（动态构建，置顶于收藏之后）
    QTreeWidgetItem* recentCatItem_ = nullptr;
    // 当前收藏的 panelId 集合（持久化）
    QSet<QString> favorites_;
    // 最近访问的 panelId 列表（最多 5 条，最新在前，持久化）
    QStringList recentPanels_;

    // 搜索开始前各分类的折叠状态快照（搜索清空时恢复，避免覆盖用户偏好）
    QSet<QString> collapsedBeforeSearch_;
    bool searchActive_ = false;

    void buildTree();
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemActivated(QTreeWidgetItem* item, int column);

    // 收藏与最近访问
    void loadFavoritesAndRecent();
    void saveFavorites() const;
    void saveRecent() const;
    void pushRecent(const QString& panelId);
    void rebuildFavoritesSection();
    void rebuildRecentSection();
    /// 分类节点的显示标题（用于 QSettings key）
    QString categoryTitle(const QTreeWidgetItem* catItem) const;
};
