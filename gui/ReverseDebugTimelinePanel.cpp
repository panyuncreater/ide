// ============================================================
// ReverseDebugTimelinePanel.cpp — 反向调试时间轴面板实现
// ------------------------------------------------------------
// 渲染执行历史为水平时间轴，支持点击回滚。
// L18 基础设施（Interpreter 状态快照/回滚）已就绪，本面板提供 GUI 消费端。
// ============================================================

#include "gui/ReverseDebugTimelinePanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

ReverseDebugTimelinePanel::ReverseDebugTimelinePanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    statusLabel_ = new QLabel("执行后可查看时间轴（支持点击回滚到任意历史步）", this);
    mainLayout->addWidget(statusLabel_);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setMinimumHeight(80);

    timelineWidget_ = new QWidget();
    scrollArea_->setWidget(timelineWidget_);
    mainLayout->addWidget(scrollArea_);

    setLayout(mainLayout);
}

ReverseDebugTimelinePanel::~ReverseDebugTimelinePanel() = default;

void ReverseDebugTimelinePanel::setEntries(const std::vector<TimelineEntry>& entries) {
    entries_ = entries;
    rebuildTimeline();
}

void ReverseDebugTimelinePanel::rebuildTimeline() {
    // 清除旧内容
    if (timelineWidget_->layout()) {
        QLayoutItem* item;
        while ((item = timelineWidget_->layout()->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete timelineWidget_->layout();
    }

    auto* layout = new QHBoxLayout(timelineWidget_);
    layout->setSpacing(2);
    layout->setContentsMargins(4, 4, 4, 4);

    if (entries_.empty()) {
        statusLabel_->setText("无执行历史（需启用 FullState 模式运行）");
        return;
    }

    statusLabel_->setText(QString("共 %1 步 — 点击任意步回滚").arg(entries_.size()));

    for (const auto& entry : entries_) {
        auto* btn = new QPushButton(QString("L%1").arg(entry.line), timelineWidget_);
        btn->setToolTip(QString::fromStdString(entry.label));
        btn->setFixedSize(36, 36);
        btn->setStyleSheet("QPushButton { font-size: 9px; border-radius: 4px; }");

        int idx = entry.stepIndex;
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            if (rollbackCb_) {
                rollbackCb_(idx);
            }
            emit rollbackRequested(idx);
        });

        layout->addWidget(btn);
    }

    layout->addStretch();
    timelineWidget_->setLayout(layout);
}
