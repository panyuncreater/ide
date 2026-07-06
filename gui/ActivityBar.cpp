#include "ActivityBar.h"
#include "QFluent/ToolButton.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QEvent>

ActivityBar::ActivityBar(QWidget* parent)
    : QFrame(parent) {
    setFixedWidth(48);
    setObjectName("ActivityBar");
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 12, 0, 0);
    layout_->setSpacing(4);
    layout_->addStretch();
}

int ActivityBar::addItem(const QString& id, const QString& text, Fluent::IconType icon) {
    // 唯一性校验
    if (indexOf(id) >= 0) return -1;

    auto* btn = new TransparentToolButton(icon, this);
    btn->setFixedSize(36, 36);
    btn->setIconSize(QSize(20, 20));
    btn->setToolTip(text);
    btn->setCheckable(true);

    int index = items_.size();
    items_.append({btn, id, text});

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

int ActivityBar::addItem(const QString& text, Fluent::IconType icon) {
    // 旧式调用：用索引转字符串作为 id
    QString autoId = QString::number(items_.size());
    return addItem(autoId, text, icon);
}

void ActivityBar::setCurrentIndex(int index) {
    if (index < 0 || index >= items_.size()) return;
    if (currentIndex_ == index) return;
    currentIndex_ = index;
    updateSelection();
    emit currentChanged(index);
    emit currentChangedById(items_[index].id);
}

bool ActivityBar::setCurrentId(const QString& id) {
    int idx = indexOf(id);
    if (idx < 0) return false;
    setCurrentIndex(idx);
    return true;
}

QString ActivityBar::currentId() const {
    if (currentIndex_ < 0 || currentIndex_ >= items_.size()) return QString();
    return items_[currentIndex_].id;
}

int ActivityBar::indexOf(const QString& id) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) return i;
    }
    return -1;
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

    // 选中指示条（2px 宽主题色竖条，垂直居中于按钮）
    QColor accent = palette().color(QPalette::Highlight);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRect(0, btnRect.y() + 8, 2, btnRect.height() - 16), 1.0, 1.0);
}
