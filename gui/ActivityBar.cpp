#include "ActivityBar.h"
#include "QFluent/ToolButton.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QEvent>

ActivityBar::ActivityBar(QWidget* parent)
    : QFrame(parent) {
    setFixedWidth(32);
    setObjectName("ActivityBar");
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 8, 0, 0);
    layout_->setSpacing(2);
    layout_->addStretch();
}

int ActivityBar::addItem(const QString& text, Fluent::IconType icon) {
    auto* btn = new TransparentToolButton(icon, this);
    btn->setFixedSize(28, 28);
    btn->setIconSize(QSize(16, 16));
    btn->setToolTip(text);
    btn->setCheckable(true);

    int index = items_.size();
    items_.append({btn, text});

    // Insert before the stretch
    layout_->insertWidget(layout_->count() - 1, btn, 0, Qt::AlignHCenter);

    connect(btn, &QToolButton::clicked, this, [this, index]() {
        setCurrentIndex(index);
    });

    if (currentIndex_ < 0) {
        setCurrentIndex(index);
    }

    return index;
}

void ActivityBar::setCurrentIndex(int index) {
    if (index < 0 || index >= items_.size()) return;
    if (currentIndex_ == index) return;
    currentIndex_ = index;
    updateSelection();
    emit currentChanged(index);
}

void ActivityBar::updateSelection() {
    for (int i = 0; i < items_.size(); ++i) {
        items_[i].button->setChecked(i == currentIndex_);
    }
    update();
}

void ActivityBar::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    // Draw selection indicator bar (left side, 2px wide, theme color)
    if (currentIndex_ < 0 || currentIndex_ >= items_.size()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Find the selected button's geometry
    QWidget* btn = items_[currentIndex_].button;
    QRect btnRect = btn->geometry();

    // Draw left indicator bar (第八轮：2px 宽主题蓝竖条)
    QColor accent = palette().color(QPalette::Highlight);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRect(0, btnRect.y() + 6, 2, btnRect.height() - 12), 1.0, 1.0);
}
