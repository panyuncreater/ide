#pragma once

// ============================================================
// ReverseDebugTimelinePanel.h — 反向调试时间轴面板
// ------------------------------------------------------------
// 消费 ExecutionTraceRecorder 的 TraceSnapshot 列表，渲染为水平时间轴。
// 点击任意历史步调用 Interpreter::restoreFromSnapshot 回滚状态。
//
// 依赖：Qt6::Widgets (QScrollArea/QPainter)、ExecutionTraceRecorder、
//       Interpreter（restoreFromSnapshot）。
// 注册：PanelCatalog "执行引擎" 分类，id="reverse-timeline"
// ============================================================

#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

class QScrollArea;
class QLabel;
class QPushButton;

/// 反向调试时间轴面板——可视化执行历史步并支持点击回滚
class ReverseDebugTimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit ReverseDebugTimelinePanel(QWidget* parent = nullptr);
    ~ReverseDebugTimelinePanel() override;

    /// 设置快照数据（由 IdeController 在执行结束后注入）
    struct TimelineEntry {
        int stepIndex = 0;
        int line = 0;
        std::string label; // 简短描述（如 "L12: x = 42"）
    };
    void setEntries(const std::vector<TimelineEntry>& entries);

    /// 设置回滚回调（点击某步时调用）
    void setRollbackCallback(std::function<void(int stepIndex)> cb) { rollbackCb_ = std::move(cb); }

signals:
    /// 请求回滚到指定步
    void rollbackRequested(int stepIndex);

private:
    void rebuildTimeline();

    std::vector<TimelineEntry> entries_;
    std::function<void(int)> rollbackCb_;

    // UI
    QScrollArea* scrollArea_ = nullptr;
    QWidget* timelineWidget_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};
