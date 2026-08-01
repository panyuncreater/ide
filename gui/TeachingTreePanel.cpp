// ============================================================
// TeachingTreePanel.cpp — 教学面板树形导航实现
// ============================================================

#include "gui/TeachingTreePanel.h"
#include "gui/I18n.h"
#include "gui/PanelCatalog.h"  // P2-1 fix: 抽取统一面板目录
#include "gui/TeachingTheme.h" // UX-R fix: 硬编码颜色迁移到语义色

#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPalette>
#include <QSettings>
#include <QSignalBlocker> // AUDIT-P2 fix: setCurrentPanel 重置搜索时阻塞信号
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

TeachingTreePanel::TeachingTreePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // B1: 顶部标题横幅 —— 渐变背景（#268BD2 → #1E6FA3），白字，padding 12px
    // BUG-UI-FIX (2026-07-15): 子 QLabel 只设 color 不设 background 会踩 Qt 陷阱——
    // QLabel 一旦有 styleSheet，Qt 自动置 WA_StyledBackground=true，
    // 于是 QLabel 用 palette(Window) 填充背景。而主窗口 applyFluentStyle
    // 已把 QPalette::Window 设为 ideBgMain()(#FFFFFF)，导致白字白底不可见。
    // 修复：改用父容器的后代选择器 #teachingBanner QLabel 统一设置背景透明，
    // 未来新增子 label 也自动继承这条规则，避免二次踩坑。
    headerBanner_ = new QWidget(this);
    headerBanner_->setObjectName("teachingBanner");
    headerBanner_->setAttribute(Qt::WA_StyledBackground, true);
    auto* bannerLayout = new QVBoxLayout(headerBanner_);
    bannerLayout->setContentsMargins(12, 12, 12, 12);
    bannerLayout->setSpacing(2);
    // 🎓 emoji 用 UTF-8 字节序列构造，避免 MSVC 源码编码问题
    const QString kHatEmoji = QString::fromUtf8("\xF0\x9F\x8E\x93"); // 🎓
    bannerTitle_ = new QLabel(kHatEmoji + QString::fromUtf8("  学习中心"), headerBanner_);
    bannerTitle_->setObjectName("teachingBannerTitle");
    bannerSubtitle_ = new QLabel(QString::fromUtf8("按推荐顺序从上往下学 · 「进阶/高级」可稍后再看"), headerBanner_);
    bannerSubtitle_->setObjectName("teachingBannerSubtitle");
    bannerLayout->addWidget(bannerTitle_);
    bannerLayout->addWidget(bannerSubtitle_);
    // 统一样式：类型选择器 QWidget#teachingBanner 强化匹配，后代 QLabel 显式
    // background:transparent + color:white，防止被祖先 palette 覆盖成白底白字。
    // UX-R fix: 渐变/文字色从硬编码（#268BD2/#1E6FA3/white）迁移到 TeachingTheme 语义色。
    const QColor onPrimary = TeachingTheme::onPrimary();
    headerBanner_->setStyleSheet(
        QStringLiteral("QWidget#teachingBanner {"
                       "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                       "    stop:0 %1, stop:1 %2);"
                       "}"
                       "QWidget#teachingBanner QLabel {"
                       "  background: transparent;"
                       "  color: %3;"
                       "  border: none;"
                       "}"
                       "QLabel#teachingBannerTitle {"
                       "  font-size: 15px;"
                       "  font-weight: 600;"
                       "}"
                       "QLabel#teachingBannerSubtitle {"
                       "  font-size: 11px;"
                       "  color: rgba(%4,%5,%6,220);"
                       "}")
            .arg(TeachingTheme::primary().name(), TeachingTheme::primaryPressed().name(), onPrimary.name())
            .arg(onPrimary.red())
            .arg(onPrimary.green())
            .arg(onPrimary.blue()));
    layout->addWidget(headerBanner_);

    // B3: 搜索框
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setObjectName("teachingSearchEdit");
    searchEdit_->setPlaceholderText(QString::fromUtf8("搜索面板..."));
    searchEdit_->setClearButtonEnabled(true);
    // UX-R fix: 硬编码颜色迁移到 TeachingTheme 语义色
    searchEdit_->setStyleSheet(QStringLiteral("#teachingSearchEdit {"
                                              "  background: %1;"
                                              "  border: none;"
                                              "  border-bottom: 1px solid %2;"
                                              "  padding: 6px 10px;"
                                              "  font-size: 12px;"
                                              "  color: %3;"
                                              "}"
                                              "#teachingSearchEdit:focus {"
                                              "  border-bottom: 2px solid %4;"
                                              "}")
                                   .arg(TeachingTheme::surface().name(), TeachingTheme::border().name(),
                                        TeachingTheme::textPrimary().name(), TeachingTheme::primary().name()));
    connect(searchEdit_, &QLineEdit::textChanged, this, &TeachingTreePanel::onSearchChanged);
    layout->addWidget(searchEdit_);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName("teachingTree");
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setIndentation(16);
    tree_->setExpandsOnDoubleClick(true);
    tree_->setAnimated(true);
    // 隐藏 horizontal scrollbar，让长标题截断
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 单列树
    tree_->setColumnCount(1);
    tree_->header()->setStretchLastSection(true);

    // B2: 树样式美化 —— 中性浅色背景 + hover/选中态主题色
    // UX-R fix: 硬编码颜色迁移到 TeachingTheme 语义色
    tree_->setStyleSheet(QStringLiteral("QTreeWidget#teachingTree {"
                                        "  background: %1;"
                                        "  border: none;"
                                        "  outline: none;"
                                        "}"
                                        "QTreeWidget#teachingTree::item {"
                                        "  padding: 6px 4px;"
                                        "  border-radius: 4px;"
                                        "}"
                                        "QTreeWidget#teachingTree::item:hover {"
                                        "  background: %2;"
                                        "}"
                                        "QTreeWidget#teachingTree::item:selected {"
                                        "  background: %3;"
                                        "  color: %4;"
                                        "}"
                                        "QTreeWidget#teachingTree::branch:has-siblings:!adjoins-item {"
                                        "  background: transparent;"
                                        "}")
                             .arg(TeachingTheme::surface().name(), TeachingTheme::primaryHoverBg().name(),
                                  TeachingTheme::primary().name(), TeachingTheme::onPrimary().name()));

    buildTree();

    // 点击叶子节点 → 发射信号
    connect(tree_, &QTreeWidget::itemClicked, this, &TeachingTreePanel::onItemClicked);
    // 键盘 Enter / 双击也触发（无障碍支持）
    connect(tree_, &QTreeWidget::itemActivated, this, &TeachingTreePanel::onItemActivated);
    // 分类展开/折叠状态持久化
    connect(tree_, &QTreeWidget::itemExpanded, this, &TeachingTreePanel::onItemExpanded);
    connect(tree_, &QTreeWidget::itemCollapsed, this, &TeachingTreePanel::onItemCollapsed);

    layout->addWidget(tree_, 1);

    // 加载收藏与最近访问，构建动态分类
    loadFavoritesAndRecent();
    rebuildFavoritesSection();
    rebuildRecentSection();
}

void TeachingTreePanel::buildTree() {
    // P2-1 fix: 从 PanelCatalog 取统一面板导航数据（原内嵌静态数组已迁移）
    const auto& categories = PanelCatalog::categories();

    // 顶部「代码编辑器」特殊项（非分类，直接点击切换回编辑器）
    editorItem_ = new QTreeWidgetItem(tree_);
    editorItem_->setText(0, mlTr("📝 代码编辑器"));
    // UserRole 存储 panelId
    editorItem_->setData(0, Qt::UserRole, QStringLiteral("editor"));
    // 加粗显示，区别于分类
    QFont editorFont = editorItem_->font(0);
    editorFont.setBold(true);
    editorItem_->setFont(0, editorFont);
    idToItem_[QStringLiteral("editor")] = editorItem_;

    // 分隔符项（视觉分隔）
    auto* sep = new QTreeWidgetItem(tree_);
    sep->setFlags(Qt::NoItemFlags);
    sep->setText(0, QString());
    sep->setSizeHint(0, QSize(-1, 9));
    auto* sepLine = new QFrame(tree_);
    sepLine->setFrameShape(QFrame::HLine);
    sepLine->setFrameShadow(QFrame::Plain);
    sepLine->setFixedHeight(1);
    sepLine->setStyleSheet(QStringLiteral("QFrame { background: %1; border: none; max-height: 1px; }")
                               .arg(TeachingTheme::border().name()));
    tree_->setItemWidget(sep, 0, sepLine);

    // 收藏分类（动态，初始为空，loadFavoritesAndRecent 后填充）
    favoritesCatItem_ = new QTreeWidgetItem(tree_);
    favoritesCatItem_->setText(0, mlTr("★ 收藏"));
    QFont favFont = favoritesCatItem_->font(0);
    favFont.setBold(true);
    favoritesCatItem_->setFont(0, favFont);
    favoritesCatItem_->setData(0, Qt::UserRole, QStringLiteral("category"));
    favoritesCatItem_->setHidden(true); // 无收藏时隐藏

    // 最近访问分类（动态，初始为空）
    recentCatItem_ = new QTreeWidgetItem(tree_);
    recentCatItem_->setText(0, mlTr("🕑 最近访问"));
    QFont recentFont = recentCatItem_->font(0);
    recentFont.setBold(true);
    recentCatItem_->setFont(0, recentFont);
    recentCatItem_->setData(0, Qt::UserRole, QStringLiteral("category"));
    recentCatItem_->setHidden(true);

    // 6 大分类（UX-R fix: 按学习曲线递进排序，分类 tooltip 展示学习目标）
    for (const auto& cat : categories) {
        auto* catItem = new QTreeWidgetItem(tree_);
        QString catText = QString::fromUtf8(cat.emoji) + " " + mlTr(cat.title);
        catItem->setText(0, catText);
        QFont catFont = catItem->font(0);
        catFont.setBold(true);
        catItem->setFont(0, catFont);
        // 分类节点不存 panelId，标记为 category
        catItem->setData(0, Qt::UserRole, QStringLiteral("category"));
        // UX-R fix: 分类 tooltip = 学习目标一句话，初学者悬停即知这一类学什么
        catItem->setToolTip(0, mlTr(cat.description));
        // 分类文字颜色稍浅
        QColor catColor = palette().color(QPalette::Text);
        // 取 85% 不透明度让分类标题略灰
        catColor.setAlphaF(0.85f);
        catItem->setForeground(0, catColor);

        for (const auto& leaf : cat.leaves) {
            auto* leafItem = new QTreeWidgetItem(catItem);
            QString leafText = QString::fromUtf8(leaf.emoji) + " " + mlTr(leaf.label);
            // UX-R fix: 难度徽标 —— 进阶/高级面板加后缀提示，入门面板保持简洁；
            // 初学者一眼就能看出哪些面板可以稍后再看，避免误入高级主题受挫
            if (leaf.level >= 2) {
                leafText += QStringLiteral(" ·") + mlTr(PanelCatalog::levelName(leaf.level));
            }
            leafItem->setText(0, leafText);
            leafItem->setData(0, Qt::UserRole, QString::fromUtf8(leaf.id));
            // B5/UX-R fix: 叶子节点 tooltip —— 难度分级 + 入口行为提示
            leafItem->setToolTip(0, mlTr("难度：") + mlTr(PanelCatalog::levelName(leaf.level)) + QStringLiteral("\n") +
                                        QString::fromUtf8("点击进入 ") + mlTr(leaf.label));
            idToItem_[QString::fromUtf8(leaf.id)] = leafItem;
        }
        // 默认全部折叠，下面单独展开前两个分类
    }

    // B4: 恢复用户上次展开状态（QSettings 持久化），无记录时默认展开前两个分类
    const QStringList savedExpanded =
        QSettings().value(QStringLiteral("teachingTree/expandedCategories")).toStringList();
    const QSet<QString> expandedSet(savedExpanded.begin(), savedExpanded.end());
    if (expandedSet.isEmpty()) {
        // 首次使用：默认展开前两个分类（入门导览 + 编译前端）
        int expandedCount = 0;
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* top = tree_->topLevelItem(i);
            if (top && top->data(0, Qt::UserRole).toString() == QStringLiteral("category")) {
                // 跳过收藏/最近访问分类（动态分类，由数据驱动展开）
                if (top == favoritesCatItem_ || top == recentCatItem_)
                    continue;
                tree_->expandItem(top);
                if (++expandedCount >= 2)
                    break;
            }
        }
    } else {
        // 恢复：仅展开 savedExpanded 中记录的分类
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* top = tree_->topLevelItem(i);
            if (!top || top->data(0, Qt::UserRole).toString() != QStringLiteral("category"))
                continue;
            if (top == favoritesCatItem_ || top == recentCatItem_)
                continue;
            const QString title = categoryTitle(top);
            if (expandedSet.contains(title)) {
                tree_->expandItem(top);
            } else {
                tree_->collapseItem(top);
            }
        }
    }

    // 默认选中「代码编辑器」项
    tree_->setCurrentItem(editorItem_);
}

void TeachingTreePanel::onItemClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item)
        return;
    QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty() || id == QStringLiteral("category"))
        return;
    pushRecent(id);
    emit panelRequested(id);
}

void TeachingTreePanel::onItemActivated(QTreeWidgetItem* item, int column) {
    onItemClicked(item, column);
}

void TeachingTreePanel::setCurrentPanel(const QString& panelId) {
    auto it = idToItem_.find(panelId);
    if (it == idToItem_.end())
        return;
    QTreeWidgetItem* item = it.value();
    // AUDIT-P2 fix: 跳转前重置搜索过滤，确保目标项可见。
    // 原实现在搜索过滤激活时直接 setCurrentItem(hidden item)，选中态被设置
    // 但用户不可见——高亮"失效"，且树仍只显示过滤结果，与实际面板不一致。
    // 跳转即重置过滤符合用户心智模型（跨面板导航应跳出搜索上下文）。
    if (searchEdit_ && !searchEdit_->text().isEmpty()) {
        QSignalBlocker blocker(searchEdit_); // 避免 clear() 重入 onSearchChanged
        searchEdit_->clear();
        onSearchChanged(QString()); // 显式恢复全部项可见
    }
    // 展开父分类（如果是叶子节点）
    if (auto* parent = item->parent()) {
        tree_->expandItem(parent);
    }
    tree_->setCurrentItem(item);
    // 同步高亮（QTreeWidget 的当前项已有视觉高亮，这里无需额外处理）
}

void TeachingTreePanel::onSearchChanged(const QString& text) {
    const QString needle = text.trimmed().toLower();
    // 搜索开始（首次进入非空）：记录各分类折叠状态快照
    if (!needle.isEmpty() && !searchActive_) {
        searchActive_ = true;
        collapsedBeforeSearch_.clear();
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* top = tree_->topLevelItem(i);
            if (!top || top->data(0, Qt::UserRole).toString() != QStringLiteral("category"))
                continue;
            if (!top->isExpanded()) {
                collapsedBeforeSearch_.insert(categoryTitle(top));
            }
        }
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* top = tree_->topLevelItem(i);
        if (!top)
            continue;
        const QString role = top->data(0, Qt::UserRole).toString();
        // 编辑器项与分隔符项不参与过滤，始终可见
        if (role == QStringLiteral("editor") || role.isEmpty()) {
            top->setHidden(false);
            continue;
        }
        if (role != QStringLiteral("category")) {
            top->setHidden(false);
            continue;
        }
        bool anyVisible = false;
        if (needle.isEmpty()) {
            // 清空搜索：恢复全部叶子可见
            for (int j = 0; j < top->childCount(); ++j) {
                if (auto* child = top->child(j))
                    child->setHidden(false);
            }
            anyVisible = top->childCount() > 0;
            // 搜索结束：恢复用户搜索前的折叠状态（而非强制全展开）
            if (searchActive_) {
                if (anyVisible) {
                    // collapsedBeforeSearch_ 中记录的保持折叠，其余展开
                    const QString title = categoryTitle(top);
                    if (collapsedBeforeSearch_.contains(title)) {
                        tree_->collapseItem(top);
                    } else {
                        tree_->expandItem(top);
                    }
                }
            } else {
                // 非搜索触发的清空（如 setCurrentPanel 重置）：展开所有分类
                if (anyVisible)
                    tree_->expandItem(top);
            }
        } else {
            // 非空搜索：按文本包含匹配（不区分大小写）过滤叶子
            for (int j = 0; j < top->childCount(); ++j) {
                auto* child = top->child(j);
                if (!child)
                    continue;
                const bool match = child->text(0).toLower().contains(needle);
                child->setHidden(!match);
                if (match)
                    anyVisible = true;
            }
            // 有匹配则展开分类让结果可见，无匹配则折叠
            if (anyVisible)
                tree_->expandItem(top);
            else
                tree_->collapseItem(top);
        }
        // 无匹配时隐藏整个分类，避免空分类标题残留
        top->setHidden(!anyVisible && !needle.isEmpty());
    }
    // 搜索完全清空后重置 searchActive_ 标志
    if (needle.isEmpty() && searchActive_) {
        searchActive_ = false;
        collapsedBeforeSearch_.clear();
    }
}

void TeachingTreePanel::onItemExpanded(QTreeWidgetItem* item) {
    if (!item || item->data(0, Qt::UserRole).toString() != QStringLiteral("category"))
        return;
    // 收藏/最近访问分类不持久化（动态内容）
    if (item == favoritesCatItem_ || item == recentCatItem_)
        return;
    // 收集当前所有展开的分类标题并保存
    QStringList expanded;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* top = tree_->topLevelItem(i);
        if (!top || top->data(0, Qt::UserRole).toString() != QStringLiteral("category"))
            continue;
        if (top == favoritesCatItem_ || top == recentCatItem_)
            continue;
        if (top->isExpanded())
            expanded << categoryTitle(top);
    }
    QSettings().setValue(QStringLiteral("teachingTree/expandedCategories"), expanded);
}

void TeachingTreePanel::onItemCollapsed(QTreeWidgetItem* item) {
    if (!item || item->data(0, Qt::UserRole).toString() != QStringLiteral("category"))
        return;
    if (item == favoritesCatItem_ || item == recentCatItem_)
        return;
    // 复用 onItemExpanded 的收集逻辑
    onItemExpanded(item);
}

QString TeachingTreePanel::categoryTitle(const QTreeWidgetItem* catItem) const {
    if (!catItem)
        return {};
    // 用 text(0) 去掉 emoji 前缀作为 key（emoji 后跟空格）
    QString t = catItem->text(0);
    const int spaceIdx = t.indexOf(' ');
    if (spaceIdx >= 0)
        t = t.mid(spaceIdx + 1);
    return t;
}

void TeachingTreePanel::loadFavoritesAndRecent() {
    favorites_ = QSet<QString>(QSettings().value(QStringLiteral("teaching/favorites")).toStringList().begin(),
                               QSettings().value(QStringLiteral("teaching/favorites")).toStringList().end());
    recentPanels_ = QSettings().value(QStringLiteral("teaching/recent")).toStringList();
    // 截断到 5 条
    while (recentPanels_.size() > 5)
        recentPanels_.removeLast();
}

void TeachingTreePanel::saveFavorites() const {
    QSettings().setValue(QStringLiteral("teaching/favorites"), QStringList(favorites_.begin(), favorites_.end()));
}

void TeachingTreePanel::saveRecent() const {
    QSettings().setValue(QStringLiteral("teaching/recent"), recentPanels_);
}

void TeachingTreePanel::pushRecent(const QString& panelId) {
    if (panelId.isEmpty() || panelId == QStringLiteral("editor"))
        return;
    // 去重并置顶
    recentPanels_.removeAll(panelId);
    recentPanels_.prepend(panelId);
    while (recentPanels_.size() > 5)
        recentPanels_.removeLast();
    saveRecent();
    rebuildRecentSection();
}

void TeachingTreePanel::rebuildFavoritesSection() {
    if (!favoritesCatItem_)
        return;
    // 清空旧子项
    while (favoritesCatItem_->childCount() > 0) {
        auto* child = favoritesCatItem_->takeChild(0);
        delete child;
    }
    // 按收藏顺序添加（保持 favorites_ 插入顺序，用 QStringList 保序）
    const QStringList favList = QSettings().value(QStringLiteral("teaching/favorites")).toStringList();
    for (const auto& id : favList) {
        if (!idToItem_.contains(id))
            continue;
        auto* src = idToItem_[id];
        auto* leaf = new QTreeWidgetItem(favoritesCatItem_);
        leaf->setText(0, src->text(0));
        leaf->setData(0, Qt::UserRole, id);
        leaf->setToolTip(0, src->toolTip(0));
    }
    favoritesCatItem_->setHidden(favoritesCatItem_->childCount() == 0);
}

void TeachingTreePanel::rebuildRecentSection() {
    if (!recentCatItem_)
        return;
    while (recentCatItem_->childCount() > 0) {
        auto* child = recentCatItem_->takeChild(0);
        delete child;
    }
    for (const auto& id : recentPanels_) {
        if (!idToItem_.contains(id))
            continue;
        auto* src = idToItem_[id];
        auto* leaf = new QTreeWidgetItem(recentCatItem_);
        leaf->setText(0, src->text(0));
        leaf->setData(0, Qt::UserRole, id);
        leaf->setToolTip(0, src->toolTip(0));
    }
    recentCatItem_->setHidden(recentCatItem_->childCount() == 0);
}

void TeachingTreePanel::toggleFavorite(const QString& panelId) {
    if (panelId.isEmpty() || panelId == QStringLiteral("editor"))
        return;
    if (favorites_.contains(panelId)) {
        favorites_.remove(panelId);
    } else {
        favorites_.insert(panelId);
        // 维护保序的 QStringList（saveFavorites 用 QSet 会乱序，这里用 QStringList 保序）
        QStringList favList = QSettings().value(QStringLiteral("teaching/favorites")).toStringList();
        if (!favList.contains(panelId))
            favList.append(panelId);
        QSettings().setValue(QStringLiteral("teaching/favorites"), favList);
        saveFavorites(); // 同步 favorites_ 集合（虽不用于顺序，保持一致）
        rebuildFavoritesSection();
        return;
    }
    // 移除收藏
    QStringList favList = QSettings().value(QStringLiteral("teaching/favorites")).toStringList();
    favList.removeAll(panelId);
    QSettings().setValue(QStringLiteral("teaching/favorites"), favList);
    rebuildFavoritesSection();
}
