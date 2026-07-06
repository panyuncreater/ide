// ============================================================
// VmStackSandboxPanel.h — VM 栈沙盒面板（功能 5）
// ------------------------------------------------------------
// 教学目标：通过亲手操作理解"栈式 VM 就是 push 和 pop"。
//
// UI 布局：
//   顶部：关卡选择 QComboBox + 目标显示 QLabel
//   中部左：可用指令按钮区（QVBoxLayout + QPushButton 列表）
//   中部中：操作数栈可视化（QListWidget，栈顶在顶部）
//   中部右：已执行指令序列（QListWidget，可滚动查看历史）
//   底部：输出区 QTextEdit + [↩ 撤销] [🔄 重置] [✓ 检查] 按钮 + 反馈 QLabel
//
// 本面板为纯前端游戏（不依赖真实 VM/引擎层），内置小型栈状态机模拟。
// SandboxLevels.cpp 为独立编译单元，便于测试目标链接。
// ============================================================

#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QString>
#include <string>
#include <vector>

#include "gui/SandboxLevels.h"

class VmStackSandboxPanel : public QWidget {
    Q_OBJECT
public:
    explicit VmStackSandboxPanel(QWidget* parent = nullptr);

signals:
    /// 完成关卡时发射（levelId 形如 "level-1" / "level-2" ... "level-5"）
    void activityCompleted(const QString& levelId);

private slots:
    void onLevelChanged(int index);
    void onUndo();
    void onReset();
    void onCheck();

private:
    // ---- UI 控件 ----
    QComboBox*  levelCombo_       = nullptr;
    QLabel*     goalLabel_        = nullptr;
    QLabel*     teachingPointLabel_ = nullptr;
    QWidget*    opButtonsHost_    = nullptr;  // 可用指令按钮的容器
    QListWidget* stackList_       = nullptr;  // 操作数栈可视化
    QListWidget* historyList_     = nullptr;  // 已执行指令序列
    QTextEdit*  outputEdit_       = nullptr;
    QPushButton* undoBtn_         = nullptr;
    QPushButton* resetBtn_        = nullptr;
    QPushButton* checkBtn_        = nullptr;
    QLabel*     feedbackLabel_    = nullptr;

    // ---- 栈状态机 ----
    std::vector<std::string>  stack_;        // 操作数栈（栈顶在 vector 末尾，UI 显示时反向）
    std::vector<SandboxOp>    history_;      // 已执行指令序列
    std::vector<std::string>  outputs_;      // 输出列表
    std::vector<std::vector<std::string>> snapshots_;  // 每步前的栈快照（用于撤销）
    std::vector<std::vector<std::string>> outputSnapshots_;  // 每步前的输出快照
    int  currentLevelIndex_ = 0;
    bool halted_ = false;  // HALT 指令后停止接收新指令，需重置

    // ---- 内部方法 ----
    void loadLevel(int index);
    void rebuildOpButtons();
    void executeOp(const SandboxOp& op);
    void refreshStackView();
    void refreshHistoryView();
    void refreshOutputView();
    void setFeedback(const QString& text, bool isError = false);
    QString opDisplayText(const SandboxOp& op) const;
    QString opButtonText(const SandboxOp& op) const;

    /// 检查答案：比对 history_ 与 expectedSequence
    bool checkAnswer(QString* diag = nullptr) const;
};
