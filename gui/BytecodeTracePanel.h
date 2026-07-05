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

#include <QWidget>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QListWidget>
#include <string>
#include <vector>
#include <cstddef>

class IdeController;

// ---- 教学场景库数据结构 ----

/// 单个 OpCode 教学条目
struct OpCodeDocEntry {
    std::string opCodeName;    // OpCode 名（如 "OP_CONST"）
    std::string category;       // 分类（const / arith / control / call / container / closure）
    std::string operandFormat;  // 操作数格式说明
    std::string stackEffect;    // 栈效果（如 "push 1"）
    std::string semantics;      // 语义说明
    std::string exampleCode;    // 触发该 OpCode 的样例代码片段
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

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    void loadSampleRequested(const QString& code);

private slots:
    void onCaptureNow();
    void onAutoCaptureToggled(bool checked);
    void onClearTrace();
    void onTraceRowSelected();
    void onDocSelected(int index);
    void onLoadDocCode();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton*    pageTraceBtn_   = nullptr;
    QPushButton*    pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_          = nullptr;

    // 子页 1：执行轨迹
    QLabel*       liveStatusLabel_  = nullptr;
    QPushButton* captureBtn_        = nullptr;
    QCheckBox*   autoCaptureCheck_ = nullptr;
    QTimer*      autoTimer_        = nullptr;
    QPushButton* clearBtn_          = nullptr;
    QTableWidget* traceTable_       = nullptr;
    QTextBrowser* stackDetail_      = nullptr;

    // 子页 2：OpCode 教学库
    QListWidget* docList_           = nullptr;
    QTextBrowser* docDetail_       = nullptr;
    QPushButton* loadCodeBtn_      = nullptr;
    int          currentDocIdx_    = -1;

    // 轨迹历史
    struct TraceEntry {
        int         step;
        std::size_t ip;
        std::string opCodeName;
        int         frameCount;
        std::vector<std::string> stackSnapshot;  // 栈顶到栈底的字符串表示
        int         line;
    };
    std::vector<TraceEntry> traceHistory_;
    int                     stepCounter_ = 0;
    static constexpr int    kMaxTraceEntries = 1000;

    // 构造辅助
    void buildTracePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    // 数据填充
    void captureCurrentState();
    void refreshTraceTable();
    void populateDocs();
    void showDoc(int index);
};
