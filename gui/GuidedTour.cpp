// ============================================================
// GuidedTour.cpp — 轻量级新手引导组件实现
// ------------------------------------------------------------
// 简化方案：不使用全屏遮罩层，而是给目标 widget 临时设置
// TeachingTheme::primary() 蓝色 2px 边框高亮，并在其附近
// 显示气泡（QFrame，圆角 8px，白底，最大宽度 360px）。
// 气泡含标题/分隔线/说明/步骤指示器/跳过+下一步按钮。
//
// 定位策略：气泡优先显示在目标下方，空间不足依次尝试上方/
// 右侧/左侧，均失败则回退到 host 底部居中；最终钳制到
// host 边界内避免溢出。无目标（target==nullptr）时居中显示。
// ============================================================

#include "gui/GuidedTour.h"
#include "gui/I18n.h"
#include "gui/TeachingTheme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// ============================================================
// 构造 / 析构
// ============================================================

GuidedTour::GuidedTour(QWidget* host, QObject* parent) : QObject(parent), host_(host) {}

GuidedTour::~GuidedTour() {
    // 析构时恢复高亮目标样式并销毁气泡，避免遗留蓝色边框与资源泄漏。
    // AUDIT-P0 fix: bubble_ 现为 QPointer，hideOverlay 中 deleteLater bubble_，
    // 同时支持 bubble_ 已被父对象析构销毁的情况（QPointer 自动置 null）。
    hideOverlay();
}

// ============================================================
// addStep — 追加一步引导
// ============================================================

/// 新增一个引导步骤（目标控件、标题、正文与按钮文案）。
void GuidedTour::addStep(QWidget* target, const QString& title, const QString& description,
                         const QString& primaryBtnText, const QString& secondaryBtnText) {
    steps_.append({target, title, description, primaryBtnText, secondaryBtnText});
}

// ============================================================
// start — 创建气泡（懒加载）并从第 0 步开始
// ============================================================

/// 启动引导：显示第一个步骤并启用遮罩。
void GuidedTour::start() {
    if (steps_.isEmpty())
        return;

    if (!bubble_) {
        // ---- 创建气泡容器（host_ 子 widget，随 host_ 自动释放）----
        bubble_ = new QWidget(host_);
        bubble_->setObjectName("guidedTourBubble");
        bubble_->setVisible(false);
        // 最大宽度 360px；高度由内容决定
        bubble_->setMaximumWidth(360);
        // WA_StyledBackground 确保 QSS background 在普通 QWidget 上生效
        bubble_->setAttribute(Qt::WA_StyledBackground, true);
        // 气泡外观：白底 + 1px 边框 + 8px 圆角
        bubble_->setStyleSheet(QString("#guidedTourBubble { background: %1; border: 1px solid %2;"
                                       "  border-radius: 8px; }")
                                   .arg(TeachingTheme::surface().name(), TeachingTheme::border().name()));

        auto* layout = new QVBoxLayout(bubble_);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);

        // ---- 标题（14px 加粗）----
        bubbleTitle_ = new QLabel(bubble_);
        bubbleTitle_->setStyleSheet(
            QString("font-size: 14px; font-weight: bold; color: %1;").arg(TeachingTheme::textPrimary().name()));
        layout->addWidget(bubbleTitle_);

        // ---- 分隔线（1px 主题边框色）----
        auto* separator = new QFrame(bubble_);
        separator->setFrameShape(QFrame::NoFrame);
        separator->setFixedHeight(1);
        separator->setStyleSheet(QString("background-color: %1;").arg(TeachingTheme::border().name()));
        layout->addWidget(separator);

        // ---- 说明（12px，可多行，支持简单 HTML）----
        bubbleDesc_ = new QLabel(bubble_);
        bubbleDesc_->setWordWrap(true);
        bubbleDesc_->setTextFormat(Qt::RichText);
        // 显式约束宽度上限，确保长文本在 adjustSize 前正确换行
        bubbleDesc_->setMaximumWidth(360 - 28); // 360 - 左右各 14px margin
        bubbleDesc_->setStyleSheet(QString("font-size: 12px; color: %1;").arg(TeachingTheme::textSecondary().name()));
        layout->addWidget(bubbleDesc_);

        // ---- 底部行：步骤指示器 + 跳过 + 下一步 ----
        auto* bottomRow = new QHBoxLayout();
        bottomRow->setContentsMargins(0, 0, 0, 0);
        bottomRow->setSpacing(8);

        stepIndicator_ = new QLabel(bubble_);
        stepIndicator_->setStyleSheet(QString("font-size: 11px; color: %1;").arg(TeachingTheme::textHint().name()));
        bottomRow->addWidget(stepIndicator_);
        bottomRow->addStretch(1);

        // 次按钮：透明背景 + 边框
        secondaryBtn_ = new QPushButton(bubble_);
        secondaryBtn_->setStyleSheet(QString("QPushButton { background: transparent; color: %1;"
                                             "  border: 1px solid %2; border-radius: 4px;"
                                             "  padding: 6px 12px; font-size: 12px; }"
                                             "QPushButton:hover { background: %3; }")
                                         .arg(TeachingTheme::textSecondary().name(), TeachingTheme::border().name(),
                                              TeachingTheme::surfaceHover().name()));
        bottomRow->addWidget(secondaryBtn_);

        // 主按钮：主题色填充 + 白字
        primaryBtn_ = new QPushButton(bubble_);
        primaryBtn_->setStyleSheet(QString("QPushButton { background: %1; color: white; border: none;"
                                           "  border-radius: 4px; padding: 6px 16px; font-size: 12px; }"
                                           "QPushButton:hover { background: %2; }")
                                       .arg(TeachingTheme::primary().name(), TeachingTheme::primaryHover().name()));
        bottomRow->addWidget(primaryBtn_);

        layout->addLayout(bottomRow);

        connect(primaryBtn_, &QPushButton::clicked, this, &GuidedTour::next);
        connect(secondaryBtn_, &QPushButton::clicked, this, &GuidedTour::skip);
    }

    showStep(0);
}

// ============================================================
// showStep — 显示第 index 步
// ============================================================

/// 显示指定索引的步骤，定位气泡到目标控件。
void GuidedTour::showStep(int index) {
    if (index < 0 || index >= steps_.size())
        return;
    if (!bubble_)
        return;

    currentIndex_ = index;
    const Step& step = steps_.at(index);

    // ---- 恢复上一步被高亮 widget 的原始样式 ----
    if (highlightedTarget_) {
        highlightedTarget_->setStyleSheet(savedStyleSheet_);
        highlightedTarget_.clear();
        savedStyleSheet_.clear();
    }

    // ---- 高亮当前目标 widget（target 为 nullptr 时跳过，气泡将居中显示）----
    if (step.target) {
        savedStyleSheet_ = step.target->styleSheet();
        step.target->setStyleSheet(
            QString("border: 2px solid %1; border-radius: 4px;").arg(TeachingTheme::primary().name()));
        highlightedTarget_ = step.target;
    }

    // ---- 填充气泡内容 ----
    bubbleTitle_->setText(step.title);
    bubbleDesc_->setText(step.description);
    stepIndicator_->setText(mlTr("步骤 %1 / %2").arg(index + 1).arg(steps_.size()));
    primaryBtn_->setText(step.primaryBtnText);
    secondaryBtn_->setText(step.secondaryBtnText);

    // ---- 计算目标矩形并初步定位气泡 ----
    // OPT-2 fix: 移除 show() 之前的 adjustSize()——首次显示前 Qt 布局尚未完成，
    // sizeHint 可能不精确，导致 positionBubble 用的 bubble_->size() 偏差。
    // 先用当前 size 初步定位，show() 之后再通过 QTimer::singleShot(0, ...) 在
    // 事件循环空闲时 adjustSize() 并重新定位，确保首次气泡几何已就绪。
    QRect targetRect;
    if (step.target) {
        // 目标在 host_ 坐标系中的矩形
        targetRect = QRect(step.target->mapTo(host_, QPoint(0, 0)), step.target->size());
    } else {
        // 无目标：在 host_ 中央显示（零尺寸矩形触发居中逻辑）
        targetRect = QRect(host_->rect().center(), QSize(0, 0));
    }
    positionBubble(targetRect);

    bubble_->show();
    bubble_->raise();

    // 延迟到事件循环空闲：此时布局已计算完成，adjustSize 得到精确 sizeHint，
    // 再用新尺寸重新定位气泡，修正首次显示定位偏差。
    QTimer::singleShot(0, this, [this, targetRect]() {
        if (!bubble_)
            return;
        bubble_->adjustSize();
        positionBubble(targetRect);
    });

    emit stepChanged(index);
}

// ============================================================
// positionBubble — 根据剩余空间决定气泡位置
// 优先级：下方 > 上方 > 右侧 > 左侧 > 兜底（host 底部居中）
// targetRect 为 host_ 坐标系；最终钳制到 host_ 边界内。
// ============================================================

/// 依据目标控件矩形计算并定位引导气泡位置。
void GuidedTour::positionBubble(const QRect& targetRect) {
    if (!bubble_ || !host_)
        return;

    const QSize bubbleSize = bubble_->size();
    const QRect hostRect = host_->rect();
    const int gap = 8;

    // 无目标（target == nullptr，targetRect 为零尺寸）：host_ 中央显示
    if (targetRect.isNull()) {
        int x = hostRect.center().x() - bubbleSize.width() / 2;
        int y = hostRect.center().y() - bubbleSize.height() / 2;
        x = qBound(0, x, qMax(0, hostRect.width() - bubbleSize.width()));
        y = qBound(0, y, qMax(0, hostRect.height() - bubbleSize.height()));
        bubble_->move(x, y);
        return;
    }

    // 默认水平居中于目标，置于目标下方
    int x = targetRect.center().x() - bubbleSize.width() / 2;
    int y = targetRect.bottom() + gap;
    bool placed = false;

    // 1. 下方
    if (y + bubbleSize.height() <= hostRect.height()) {
        placed = true;
    } else {
        // 2. 上方
        y = targetRect.top() - gap - bubbleSize.height();
        if (y >= 0) {
            placed = true;
        } else {
            // 3. 右侧
            y = targetRect.center().y() - bubbleSize.height() / 2;
            x = targetRect.right() + gap;
            if (x + bubbleSize.width() <= hostRect.width()) {
                placed = true;
            } else {
                // 4. 左侧
                x = targetRect.left() - gap - bubbleSize.width();
                if (x >= 0) {
                    placed = true;
                }
            }
        }
    }

    if (!placed) {
        // 兜底：host 底部居中
        x = hostRect.center().x() - bubbleSize.width() / 2;
        y = hostRect.height() - bubbleSize.height() - gap;
    }

    // 钳制到 host 边界内
    x = qBound(0, x, qMax(0, hostRect.width() - bubbleSize.width()));
    y = qBound(0, y, qMax(0, hostRect.height() - bubbleSize.height()));

    bubble_->move(x, y);
}

// ============================================================
// next — 下一步；超出末步则完成引导
// ============================================================

/// 进入下一步；末步则结束引导。
void GuidedTour::next() {
    const int nextIndex = currentIndex_ + 1;
    if (nextIndex >= steps_.size()) {
        hideOverlay();
        emit finished(true);
        return;
    }
    showStep(nextIndex);
}

// ============================================================
// skip — 跳过引导
// ============================================================

/// 跳过剩余引导并隐藏遮罩。
void GuidedTour::skip() {
    hideOverlay();
    emit finished(false);
}

// ============================================================
// hideOverlay — 隐藏气泡并恢复目标 widget 原样式
// ============================================================

/// 隐藏遮罩与气泡，结束引导态。
void GuidedTour::hideOverlay() {
    if (highlightedTarget_) {
        highlightedTarget_->setStyleSheet(savedStyleSheet_);
        highlightedTarget_.clear();
        savedStyleSheet_.clear();
    }
    // AUDIT-P0 fix: bubble_ 改为 deleteLater 释放，避免反复 start/hideOverlay 累积泄漏。
    // 原 hideOverlay 仅 hide() 不 delete，多次触发主引导或面板引导会泄漏 bubble_。
    // QPointer 保证若 bubble_ 已被 host_ 析构链销毁则此处不再访问。
    if (bubble_) {
        bubble_->hide();
        bubble_->deleteLater();
        bubble_ = nullptr;
    }
    currentIndex_ = -1;
}
