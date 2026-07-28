// ============================================================
// VmStackSandboxPanel.h — VM 栈沙盒面板（功能 5）
// ------------------------------------------------------------
// 教学目标：通过亲手操作理解"栈式 VM 就是 push 和 pop"。
//
// 双子页结构（QStackedWidget 切换）：
//   页 1「栈沙盒」：
//     顶部：关卡选择 QComboBox + 目标显示 QLabel
//     中部左：可用指令按钮区（QVBoxLayout + QPushButton 列表）
//     中部中：操作数栈可视化（QListWidget，栈顶在顶部）
//     中部右：已执行指令序列（QListWidget，可滚动查看历史）
//     底部：输出区 QTextEdit + [↩ 撤销] [🔄 重置] [✓ 检查] 按钮 + 反馈 QLabel
//   页 2「真实字节码追踪」：
//     顶部：「编译并加载」按钮 + IP 指示器 + 状态标签
//     中部左：真实字节码指令序列 QListWidget（[IP] OpCode operand）
//     中部右上：当前栈状态 QListWidget（栈顶在上方）
//     中部右下：寄存器视图 QTableWidget（RegisterVM 模式 32 个寄存器）
//     底部：「单步执行」/「运行到底」/「重置」按钮
//
// 页 1 为纯前端游戏（内置小型栈状态机模拟），页 2 通过 IdeController
// 调用真实 VM 单步 API（vmStep/vmReset/getVmStack/getVmCurrentIP 等）
// 让学习者透明观察真实字节码执行过程。
// SandboxLevels.cpp 为独立编译单元，便于测试目标链接。
// ============================================================

#pragma once

#include <QComboBox>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

#include "gui/SandboxLevels.h"

class IdeController;

class VmStackSandboxPanel : public QWidget {
    Q_OBJECT
public:
    /// 构造 VM 栈沙盒面板；parent 为父控件。
    explicit VmStackSandboxPanel(QWidget* parent = nullptr);
    // AUDIT-P0 fix: 析构时反注册 IdeController 监听器。
    ~VmStackSandboxPanel() override;

    /// 绑定 IdeController，启用「真实字节码追踪」页的真实 VM 单步功能。
    /// 不绑定 controller 时面板仍可使用（栈沙盒页完整可用，追踪页提示未绑定）。
    void setController(IdeController* controller);

signals:
    /// 完成关卡时发射（levelId 形如 "level-1" / "level-2" ... "level-5"）
    void activityCompleted(const QString& levelId);

private slots:
    /// 关卡变更回调。
    void onLevelChanged(int index);
    /// 撤销一步操作。
    void onUndo();
    /// 重置当前关卡。
    void onReset();
    /// 校验当前答案。
    void onCheck();
    /// P1-3 fix (F14): 用真实 Lexer+Parser+Compiler+StackVM 执行当前关卡对应的 MiniLang 源码
    void onVerifyWithRealStackVM();

    // ---- 真实字节码追踪页 slots ----
    /// 编译并加载到跟踪页。
    void onCompileAndLoad();
    /// 跟踪页单步执行。
    void onTraceStep();
    /// 跟踪页全速执行。
    void onTraceRunAll();
    /// 跟踪页重置。
    void onTraceReset();

private:
    // ---- 页切换 ----
    QPushButton* pageSandboxBtn_ = nullptr; ///< 「栈沙盒」子页按钮
    QPushButton* pageTraceBtn_ = nullptr;   ///< 「真实字节码追踪」子页按钮
    QStackedWidget* pageStack_ = nullptr;   ///< 子页堆叠容器

    // ---- 沙盒页 UI 控件 ----
    QComboBox* levelCombo_ = nullptr;
    QLabel* goalLabel_ = nullptr;
    QLabel* teachingPointLabel_ = nullptr;
    QList<QPushButton*> levelChips_;     // 关卡芯片按钮栏（与 levelCombo_ 双向同步）
    QWidget* opButtonsHost_ = nullptr;   // 可用指令按钮的容器
    QListWidget* stackList_ = nullptr;   // 操作数栈可视化
    QListWidget* historyList_ = nullptr; // 已执行指令序列
    QTextEdit* outputEdit_ = nullptr;
    QPushButton* undoBtn_ = nullptr;
    QPushButton* resetBtn_ = nullptr;
    QPushButton* checkBtn_ = nullptr;
    QPushButton* verifyBtn_ = nullptr;    ///< P1-3 fix: 用真实 StackVM 验证
    QPushButton* gotoTraceBtn_ = nullptr; ///< 验证后跳转到追踪页
    QLabel* feedbackLabel_ = nullptr;
    QLabel* sandboxOpHintLabel_ = nullptr; ///< 沙盒操作对应的真实 OpCode 提示

    // ---- 追踪页 UI 控件 ----
    QWidget* tracePage_ = nullptr;
    QListWidget* bytecodeList_ = nullptr;   ///< 真实字节码指令列表
    QListWidget* traceStackView_ = nullptr; ///< 追踪时的栈状态
    QTableWidget* registerTable_ = nullptr; ///< 寄存器视图
    QTextEdit* traceOutputEdit_ = nullptr;  ///< 追踪时的输出区
    QPushButton* compileBtn_ = nullptr;     ///< 「编译并加载」
    QPushButton* stepBtn_ = nullptr;        ///< 「单步执行」
    QPushButton* runAllBtn_ = nullptr;      ///< 「运行到底」
    QPushButton* resetTraceBtn_ = nullptr;  ///< 「重置」
    QLabel* ipIndicator_ = nullptr;         ///< IP 指示器
    QLabel* traceStatusLabel_ = nullptr;    ///< 状态标签

    // ---- 追踪状态 ----
    IdeController* controller_ = nullptr;
    std::vector<int> bytecodeOffsets_; ///< 指令偏移量映射（list 行号 → 字节偏移）
    int currentIp_ = 0;                ///< 当前 IP（字节偏移）

    // ---- 栈状态机（沙盒页） ----
    std::vector<std::string> stack_;                        // 操作数栈（栈顶在 vector 末尾，UI 显示时反向）
    std::vector<SandboxOp> history_;                        // 已执行指令序列
    std::vector<std::string> outputs_;                      // 输出列表
    std::vector<std::vector<std::string>> snapshots_;       // 每步前的栈快照（用于撤销）
    std::vector<std::vector<std::string>> outputSnapshots_; // 每步前的输出快照
    int currentLevelIndex_ = 0;
    bool halted_ = false; // HALT 指令后停止接收新指令，需重置
    // AUDIT-P2 fix: onTraceRunAll 长循环期间 processEvents 会让渡控制权，
    // 用户可点击 stepBtn_/resetTraceBtn_ 触发重入，与正在跑的循环共享 vm_
    // 状态导致崩溃。traceRunning_ 守卫阻止重入。
    bool traceRunning_ = false;
    // BUG-96 fix (P3): refreshTraceViews 重入守卫——onTraceRunAll 循环内
    // processEvents 期间，VM 状态变更监听器/事件回调可能再次触发 refreshTraceViews，
    // 与外层正在执行的刷新逻辑竞争修改 QListWidget/QTableWidget 视图状态。
    // 该标志在 refreshTraceViews 入口置位、出口复位，重入调用直接返回。
    bool traceRefreshing_ = false;

    // ---- 内部方法 ----
    /// 加载指定关卡。
    void loadLevel(int index);
    /// 重建操作按钮。
    void rebuildOpButtons();
    /// 执行一条沙盒指令。
    void executeOp(const SandboxOp& op);
    /// 刷新栈视图。
    void refreshStackView();
    /// 刷新历史视图。
    void refreshHistoryView();
    /// 刷新输出视图。
    void refreshOutputView();
    /// 设置反馈信息文本。
    void setFeedback(const QString& text, bool isError = false);
    /// 指令的历史展示文本。
    QString opDisplayText(const SandboxOp& op) const;
    /// 指令的按钮展示文本。
    QString opButtonText(const SandboxOp& op) const;

    /// 检查答案：比对 history_ 与 expectedSequence
    bool checkAnswer(QString* diag = nullptr) const;

    /// 刷新关卡芯片按钮栏的状态（current 高亮等，所有关卡均解锁）
    void refreshLevelChips();

    // ---- 追踪页内部方法 ----
    /// 构建字节码跟踪子页。
    void buildTracePage(QWidget* host);
    /// 编译当前关卡源码并填充 bytecodeList_，重置 VM
    void loadBytecodeFromCurrentLevel();
    /// 刷新字节码列表高亮（已执行=绿色背景，当前 IP=蓝色边框）+ 栈状态 + 寄存器视图
    void refreshTraceViews();
    /// 刷新寄存器表（RegisterVM 模式 32 个寄存器，StackVM 模式显示提示）
    void refreshRegisterTable();
    /// 沙盒 SandboxOpType → 真实 OpCode 名称映射（用于沙盒页底部提示）
    static QHash<SandboxOpType, QString> buildSandboxToRealOpMap();
    /// 切换到追踪页
    void switchToTracePage();
};
