#pragma once

// ============================================================
// BytecodeTracePanel — 字节码执行轨迹面板（第三波 P0-3）
// ------------------------------------------------------------
// 单步执行时记录 IP / OpCode / 操作数栈快照序列，时间轴回放，
// 让学习者直观理解栈式 VM 的 push/pop 平衡、寄存器分配、跳转解析。
//
// 两个子页：
//   1. 执行轨迹：消费 controller_->getVmCurrentIP() / getCurrentOpCodeName() / getVmStack()
//      + 自动捕获（订阅 vmRunPaused 信号）+ 手动捕获按钮 + 历史回放
//   2. OpCode 教学库：常见 OpCode 语义说明（OP_CONST / OP_ADD / OP_CALL / OP_RETURN 等）
//
// 本面板仅消费 IdeController 已有 API + 自带 BytecodeTraceLibrary 静态场景库，
// 不修改引擎层。
// ============================================================

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

class IdeController;
class GuidedTour;

// ---- 教学场景库数据结构 ----

/// 单个 OpCode 教学条目
struct OpCodeDocEntry {
    std::string opCodeName;    // OpCode 名（如 "OP_CONST"）
    std::string category;      // 分类（const / arith / control / call / container / closure）
    std::string operandFormat; // 操作数格式说明
    std::string stackEffect;   // 栈效果（如 "push 1"）
    std::string semantics;     // 语义说明
    std::string exampleCode;   // 触发该 OpCode 的样例代码片段
};

/// BytecodeTraceLibrary — 静态 OpCode 教学库
class BytecodeTraceLibrary {
public:
    static const std::vector<OpCodeDocEntry>& opCodeDocs();
};

// ---- 主面板 ----

class BytecodeTracePanel : public QWidget {
    Q_OBJECT
public:
    explicit BytecodeTracePanel(QWidget* parent = nullptr);
    // AUDIT-P0 fix: 析构时反注册 IdeController 的 vmStateChanged 监听器。
    ~BytecodeTracePanel() override;

    // OPT-1: setController 注册 vmStateChanged 监听器，替代 500ms QTimer 轮询。
    // 实现移至 .cpp（调用 addVmStateChangedListener 需 IdeController 完整类型定义）
    void setController(IdeController* controller);

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    void loadSampleRequested(const QString& code);
    void sourceLineRequested(int line);

protected:
    /// 面板显示时恢复自动捕获（若用户已勾选），隐藏时停止 QTimer
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onCaptureNow();
    void onAutoCaptureToggled(bool checked);
    void onClearTrace();
    void onTraceRowSelected();
    void onDocSelected(int index);
    void onLoadDocCode();

private:
    /// OPT-1: vmStateChanged 监听回调——仅当面板可见且开启自动捕获时即时捕获。
    void onVmStateChanged() {
        if (isVisible() && autoCaptureCheck_ && autoCaptureCheck_->isChecked()) {
            captureCurrentState();
        }
    }

    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageTraceBtn_ = nullptr;
    QPushButton* pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：执行轨迹
    QLabel* liveStatusLabel_ = nullptr;
    QPushButton* captureBtn_ = nullptr;
    QCheckBox* autoCaptureCheck_ = nullptr;
    QTimer* autoTimer_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QTableWidget* traceTable_ = nullptr;
    QTextBrowser* stackDetail_ = nullptr;

    // 子页 2：OpCode 教学库
    QListWidget* docList_ = nullptr;
    QTextBrowser* docDetail_ = nullptr;
    QPushButton* loadCodeBtn_ = nullptr;
    int currentDocIdx_ = -1;

    // 轨迹历史
    struct TraceEntry {
        int step;
        std::size_t ip;
        std::string opCodeName;
        int frameCount;
        std::vector<std::string> stackSnapshot; // 栈顶到栈底的字符串表示
        int line;
    };
    // AUDIT-P1 fix: 改用 std::deque 实现 O(1) 头部删除（原 vector erase(begin()) 是 O(n)）
    std::deque<TraceEntry> traceHistory_;
    int stepCounter_ = 0;
    static constexpr int kMaxTraceEntries = 1000;
    // OPT-2 fix: autoTimer 安全网指纹——避免 VM 状态未变时盲目累积重复轨迹。
    // 仅在 captureCurrentState 内 push_back 之前计算并比对。
    std::string lastCaptureFingerprint_;

    // 构造辅助
    void buildTracePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    // 数据填充
    void captureCurrentState();
    void refreshTraceTable();
    void populateDocs();
    void showDoc(int index);
};
