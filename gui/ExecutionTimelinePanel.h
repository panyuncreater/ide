#pragma once

// ============================================================
// ExecutionTimelinePanel — 可回放执行时间轴面板（R114 阶段 1）
// ------------------------------------------------------------
// 三后端统一的执行轨迹回放面板。订阅 traceRecorder() 全局单例，
// 录制期间定时刷新步骤列表，用户可通过 QSlider / 步进按钮 / 列表点击
// 随机访问任意步快照，右侧详情区显示栈 / locals / globals / callStack。
//
// 三后端支持：
//   - Interpreter：AST 节点级（opOrNodeName = "IfStmt" / "BinaryOp" 等）
//   - StackVM：字节码级（opOrNodeName = "OP_ADD" / "OP_CALL" 等）
//   - RegisterVM：寄存器指令级（opOrNodeName = "REG_ADD" 等）
//
// 与 BytecodeTracePanel 的区别：
//   - BytecodeTracePanel 仅 VM 路径 + 仅 1000 条 + 仅栈快照
//   - 本面板三后端统一 + 5000 条环形缓冲 + 完整快照（栈/locals/globals/callStack）
//   - 本面板支持随机访问任意步（BytecodeTracePanel 仅顺序追加）
//
// 不依赖 IdeController（直接用 traceRecorder() 单例），可独立测试。
// 仅在录制时通过 setBackendType() 告知面板当前后端类型，用于显示标签。
// ============================================================

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTextBrowser>
#include <QWidget>

#include "debug/ExecutionTraceRecorder.h"

class IdeController;
class QTimer;

class ExecutionTimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit ExecutionTimelinePanel(QWidget* parent = nullptr);
    ~ExecutionTimelinePanel() override = default;

    /// 设置控制器（用于读取当前后端类型 + 录制启停联动）
    /// 不调用也能工作（默认 StackVM 后端，录制启停通过面板按钮控制）。
    void setController(IdeController* controller);

    /// 设置当前后端类型（用于显示标签 + 录制时传递给 VmStepper）
    void setBackendType(TraceBackend backend);

signals:
    /// 用户点击「返回编辑器」按钮
    /// 标题重复修复后：面板不再自建标题栏，该 signal 保留以兼容 ide.cpp 既有 connect；
    /// 返回编辑器实际由外壳 TeachingPanelHeader 统一处理。
    void returnToEditorRequested();

    /// 用户点击「新手引导」按钮（同上，由外壳标题栏统一处理）
    void guidedTourRequested(const QString& panelId);

    /// 录制状态切换（用于 IdeController 联动 VmStepper/Interpreter 的 recorder）
    void recordingToggled(bool enabled, TraceBackend backend);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onRecordingToggled(bool checked);
    void onClearClicked();
    void onFirstStep();
    void onPrevStep();
    void onNextStep();
    void onLastStep();
    void onSliderMoved(int value);
    void onStepSelected(int row);
    void onRefreshTimer();

private:
    IdeController* controller_ = nullptr;
    TraceBackend backend_ = TraceBackend::StackVM;

    // 工具栏
    QPushButton* recordBtn_ = nullptr; // 切换录制（checkable）
    QPushButton* clearBtn_ = nullptr;
    QPushButton* firstBtn_ = nullptr;
    QPushButton* prevBtn_ = nullptr;
    QPushButton* nextBtn_ = nullptr;
    QPushButton* lastBtn_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* stepLabel_ = nullptr;    // "123 / 456"
    QLabel* backendLabel_ = nullptr; // "StackVM" / "RegisterVM" / "Interpreter"

    // 中央区域
    QListWidget* stepList_ = nullptr;
    QTextBrowser* detailView_ = nullptr;

    // 刷新定时器（录制期间 200ms 刷新一次）
    QTimer* refreshTimer_ = nullptr;

    // 当前选中的步骤索引（-1 表示未选中）
    int currentStepIdx_ = -1;

    // 上次刷新时的快照数量（用于增量更新 stepList_）
    size_t lastSeenSize_ = 0;

    // 构造辅助
    void buildToolbar(QWidget* host);
    void buildCentralArea(QWidget* host);

    // 数据填充
    void refreshStepList();
    void showStepDetail(size_t idx);
    void updateStepLabel();
    void updateSliderRange();
    void navigateToStep(int idx);
    /// UX：根据当前数据 / 位置同步导航按钮与清空按钮的可用性
    void updateControlsEnabled();
    /// UX：未录制 / 已清空时在详情区展示引导文案
    void showEmptyHint();

    /// 后端类型显示名
    static QString backendName(TraceBackend b);
    /// 格式化步骤列表项文本
    static QString formatStepItem(const TraceSnapshot& snap);
    /// 格式化详情 HTML
    static QString formatDetailHtml(const TraceSnapshot& snap);
};
