/**
 * @file ActivityBar.cpp
 * @brief 左侧活动栏（功能导航侧栏）实现
 *
 * 职责：以图标+文字项展示各教学/工具面板入口，管理当前选中项并通知主窗口切换。
 */
#include "ActivityBar.h"
#include "QFluent/ToolButton.h"
#include <QEvent>
#include <QPainter>
#include <QVBoxLayout>

// ============================================================
// ActivityBar 实现
// 活动栏（VS Code 风格左侧图标条）：以注册制管理图标项的添加、
// 选中切换与左侧面板联动，绘制选中指示条。
// ============================================================

ActivityBar::ActivityBar(QWidget* parent) : QFrame(parent) {
    setFixedWidth(48);
    setObjectName("ActivityBar");
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 12, 0, 0);
    layout_->setSpacing(4);
    layout_->addStretch();
}

/// 以 id 添加带图标与文字的活动项，返回其索引。
int ActivityBar::addItem(const QString& id, const QString& text, Fluent::IconType icon) {
    // 唯一性校验
    if (indexOf(id) >= 0)
        return -1;

    auto* btn = new TransparentToolButton(icon, this);
    btn->setFixedSize(36, 36);
    btn->setIconSize(QSize(20, 20));
    btn->setToolTip(text);
    btn->setCheckable(true);

    int index = items_.size();
    items_.append({btn, id, text});

    // Insert before the stretch
    layout_->insertWidget(layout_->count() - 1, btn, 0, Qt::AlignHCenter);

    connect(btn, &QToolButton::clicked, this, [this, index]() { setCurrentIndex(index); });

    if (currentIndex_ < 0) {
        setCurrentIndex(index);
    }

    return index;
}

/// 以自动生成 id 添加活动项，返回其索引。
int ActivityBar::addItem(const QString& text, Fluent::IconType icon) {
    // 旧式调用：用索引转字符串作为 id
    QString autoId = QString::number(items_.size());
    return addItem(autoId, text, icon);
}

/// 按索引设置当前选中活动项并刷新高亮。
void ActivityBar::setCurrentIndex(int index) {
    if (index < 0 || index >= items_.size())
        return;
    if (currentIndex_ == index)
        return;
    currentIndex_ = index;
    updateSelection();
    emit currentChanged(index);
    emit currentChangedById(items_[index].id);
}

/// 按 id 设置当前选中项；成功返回 true。
bool ActivityBar::setCurrentId(const QString& id) {
    int idx = indexOf(id);
    if (idx < 0)
        return false;
    setCurrentIndex(idx);
    return true;
}

/// 返回当前选中活动的 id。
QString ActivityBar::currentId() const {
    if (currentIndex_ < 0 || currentIndex_ >= items_.size())
        return QString();
    return items_[currentIndex_].id;
}

/// 返回指定 id 对应活动项的索引；未找到返回 -1。
int ActivityBar::indexOf(const QString& id) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id)
            return i;
    }
    return -1;
}

/// 根据当前索引刷新各按钮的选中态视觉。
void ActivityBar::updateSelection() {
    for (int i = 0; i < items_.size(); ++i) {
        items_[i].button->setChecked(i == currentIndex_);
    }
    update();
}

/// 重写绘制：渲染活动栏背景与选中指示条。
void ActivityBar::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    // Draw selection indicator bar (left side, 2px wide, theme color)
    if (currentIndex_ < 0 || currentIndex_ >= items_.size())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Find the selected button's geometry
    QWidget* btn = items_[currentIndex_].button;
    QRect btnRect = btn->geometry();

    // 选中指示条（2px 宽主题色竖条，垂直居中于按钮）
    QColor accent = palette().color(QPalette::Highlight);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRect(0, btnRect.y() + 8, 2, btnRect.height() - 16), 1.0, 1.0);
}
