// ============================================================
// TeachingSubPageBar.cpp — 教学面板子页切换栏统一组件实现
// ------------------------------------------------------------

#include "gui/TeachingSubPageBar.h"

#include <QSettings>

#include "gui/PanelAnimator.h"
#include "gui/TeachingTheme.h"

TeachingSubPageBar::TeachingSubPageBar(QWidget* parent) : QWidget(parent) {
    buttonBar_ = new QHBoxLayout(this);
    buttonBar_->setContentsMargins(0, 0, 0, 0);
    buttonBar_->setSpacing(4);
    // 占位，按钮在 addPage 时插入到 stretch 之前
    buttonBar_->addStretch();

    stack_ = new QStackedWidget;
    // stack_ 不加入 buttonBar_，由调用方 addToLayout
}

int TeachingSubPageBar::addPage(const QString& buttonText, QWidget* page) {
    auto* btn = new QPushButton(buttonText);
    btn->setObjectName("teachingSubPageBtn");
    btn->setCheckable(true);
    const int idx = buttons_.size();
    buttons_.append(btn);
    // 插入到 stretch（最后一个 item）之前
    buttonBar_->insertWidget(buttonBar_->count() - 1, btn);
    stack_->addWidget(page);

    // 互斥 + 切换
    connect(btn, &QPushButton::clicked, this, [this, idx]() { setCurrentIndex(idx); });
    if (idx == 0) {
        btn->setChecked(true);
    }
    return idx;
}

void TeachingSubPageBar::setCurrentIndex(int idx) {
    if (idx < 0 || idx >= stack_->count() || idx == stack_->currentIndex()) {
        return;
    }
    stack_->setCurrentIndex(idx);
    // 更新按钮选中态
    for (int i = 0; i < buttons_.size(); ++i) {
        buttons_[i]->setChecked(i == idx);
    }
    updateButtonStyles();
    // 播放滑入动画（初始恢复时跳过）
    if (!suppressAnim_ && stack_->currentWidget()) {
        PanelAnimator::slideInWidget(stack_->currentWidget());
    }
    emit currentChanged(idx);
}

QPushButton* TeachingSubPageBar::buttonAt(int idx) const {
    if (idx < 0 || idx >= buttons_.size()) {
        return nullptr;
    }
    return buttons_[idx];
}

void TeachingSubPageBar::restoreFromSettings(const QString& key) {
    const int saved = QSettings().value(key, 0).toInt();
    if (saved >= 0 && saved < stack_->count()) {
        suppressAnim_ = true;
        setCurrentIndex(saved);
        suppressAnim_ = false;
    }
}

void TeachingSubPageBar::saveToSettings(const QString& key) const {
    QSettings().setValue(key, stack_->currentIndex());
}

void TeachingSubPageBar::updateButtonStyles() {
    // 统一主题色高亮：选中用 TeachingTheme::primary，未选中用次按钮样式
    const QString primaryStyle = QString("QPushButton { background: %1; color: white; "
                                         "border: none; border-radius: 5px; padding: 6px 14px; "
                                         "font-weight: bold; }"
                                         "QPushButton:hover { background: %2; }")
                                     .arg(TeachingTheme::primary().name(), TeachingTheme::primaryHover().name());
    const QString secondaryStyle = QString("QPushButton { background: transparent; color: %1; "
                                           "border: 1px solid %2; border-radius: 5px; padding: 6px 14px; }"
                                           "QPushButton:hover { background: %3; }")
                                       .arg(TeachingTheme::textPrimary().name(), TeachingTheme::border().name(),
                                            TeachingTheme::surfaceHover().name());
    for (int i = 0; i < buttons_.size(); ++i) {
        buttons_[i]->setStyleSheet(i == stack_->currentIndex() ? primaryStyle : secondaryStyle);
    }
}
