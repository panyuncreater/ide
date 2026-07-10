// ============================================================
// TeachingTreePanel.cpp — 教学面板树形导航实现
// ============================================================

#include "gui/TeachingTreePanel.h"
#include "gui/I18n.h"
#include "gui/PanelCatalog.h" // P2-1 fix: 抽取统一面板目录

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPalette>
#include <QSignalBlocker> // AUDIT-P2 fix: setCurrentPanel 重置搜索时阻塞信号
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

TeachingTreePanel::TeachingTreePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // B1: 顶部标题横幅 —— 渐变背景（#268BD2 → #1E6FA3），白字，padding 12px
    headerBanner_ = new QWidget(this);
    headerBanner_->setObjectName("teachingBanner");
    headerBanner_->setAttribute(Qt::WA_StyledBackground, true);
    auto* bannerLayout = new QVBoxLayout(headerBanner_);
    bannerLayout->setContentsMargins(12, 12, 12, 12);
    bannerLayout->setSpacing(2);
    // 🎓 emoji 用 UTF-8 字节序列构造，避免 MSVC 源码编码问题
    const QString kHatEmoji = QString::fromUtf8("\xF0\x9F\x8E\x93"); // 🎓
    bannerTitle_ = new QLabel(kHatEmoji + QString::fromUtf8("  学习中心"), headerBanner_);
    bannerTitle_->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; color: white;"));
    bannerSubtitle_ = new QLabel(QString::fromUtf8("点击下方任意主题开始学习"), headerBanner_);
    bannerSubtitle_->setStyleSheet(QStringLiteral("font-size: 11px; color: rgba(255,255,255,200);"));
    bannerLayout->addWidget(bannerTitle_);
    bannerLayout->addWidget(bannerSubtitle_);
    headerBanner_->setStyleSheet(QStringLiteral("#teachingBanner {"
                                                "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                                                "    stop:0 #268BD2, stop:1 #1E6FA3);"
                                                "}"));
    layout->addWidget(headerBanner_);

    // B3: 搜索框
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setObjectName("teachingSearchEdit");
    searchEdit_->setPlaceholderText(QString::fromUtf8("搜索面板..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setStyleSheet(QStringLiteral("#teachingSearchEdit {"
                                              "  background: #FFFFFF;"
                                              "  border: none;"
                                              "  border-bottom: 1px solid #E0E0E0;"
                                              "  padding: 6px 10px;"
                                              "  font-size: 12px;"
                                              "  color: #1E1E1E;"
                                              "}"
                                              "#teachingSearchEdit:focus {"
                                              "  border-bottom: 2px solid #268BD2;"
                                              "}"));
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
    tree_->setStyleSheet(QStringLiteral("QTreeWidget#teachingTree {"
                                        "  background: #FFFFFF;"
                                        "  border: none;"
                                        "  outline: none;"
                                        "}"
                                        "QTreeWidget#teachingTree::item {"
                                        "  padding: 6px 4px;"
                                        "  border-radius: 4px;"
                                        "}"
                                        "QTreeWidget#teachingTree::item:hover {"
                                        "  background: rgba(38, 139, 210, 0.08);"
                                        "}"
                                        "QTreeWidget#teachingTree::item:selected {"
                                        "  background: #268BD2;"
                                        "  color: white;"
                                        "}"
                                        "QTreeWidget#teachingTree::branch:has-siblings:!adjoins-item {"
                                        "  background: transparent;"
                                        "}"));

    buildTree();

    // 点击叶子节点 → 发射信号
    connect(tree_, &QTreeWidget::itemClicked, this, &TeachingTreePanel::onItemClicked);
    // 键盘 Enter / 双击也触发（无障碍支持）
    connect(tree_, &QTreeWidget::itemActivated, this, &TeachingTreePanel::onItemActivated);

    layout->addWidget(tree_, 1);
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
    sep->setFlags(Qt::NoItemFlags); // 不可选、不可点
    sep->setText(0, QString());
    // 设置占位高度，QTreeWidget 不直接支持 separator，用空行模拟
    sep->setSizeHint(0, QSize(-1, 4));

    // 4 大分类
    for (const auto& cat : categories) {
        auto* catItem = new QTreeWidgetItem(tree_);
        QString catText = QString::fromUtf8(cat.emoji) + " " + mlTr(cat.title);
        catItem->setText(0, catText);
        QFont catFont = catItem->font(0);
        catFont.setBold(true);
        catItem->setFont(0, catFont);
        // 分类节点不存 panelId，标记为 category
        catItem->setData(0, Qt::UserRole, QStringLiteral("category"));
        // 分类文字颜色稍浅
        QColor catColor = palette().color(QPalette::Text);
        // 取 85% 不透明度让分类标题略灰
        catColor.setAlphaF(0.85f);
        catItem->setForeground(0, catColor);

        for (const auto& leaf : cat.leaves) {
            auto* leafItem = new QTreeWidgetItem(catItem);
            QString leafText = QString::fromUtf8(leaf.emoji) + " " + mlTr(leaf.label);
            leafItem->setText(0, leafText);
            leafItem->setData(0, Qt::UserRole, QString::fromUtf8(leaf.id));
            // B5: 叶子节点 tooltip —— PanelCatalog 无 description 字段，
            // 用简单的「点击进入 <label>」提示，悬停即可预览入口行为
            leafItem->setToolTip(0, QString::fromUtf8("点击进入 ") + mlTr(leaf.label));
            idToItem_[QString::fromUtf8(leaf.id)] = leafItem;
        }
        // 默认全部折叠，下面单独展开前两个分类
    }

    // B4: 默认展开前两个分类（入门导览 + 编译前端），让新手指引最相关的
    // 面板立即可见，同时保留后续分类的折叠状态减少视觉噪声
    int expandedCount = 0;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* top = tree_->topLevelItem(i);
        if (top && top->data(0, Qt::UserRole).toString() == QStringLiteral("category")) {
            tree_->expandItem(top);
            if (++expandedCount >= 2)
                break; // 只展开前两个 category
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
            // AUDIT-P2 fix: 搜索期间被 collapseItem 折叠的分类在清空搜索后保持折叠，
            // 与搜索前展开状态不一致。清空搜索时展开所有分类（与构造默认状态一致）。
            if (anyVisible)
                tree_->expandItem(top);
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
}
