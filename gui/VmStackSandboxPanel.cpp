// ============================================================
// VmStackSandboxPanel.cpp — VM 栈沙盒面板实现（功能 5）
// ============================================================

#include "gui/VmStackSandboxPanel.h"
#include "gui/I18n.h"
#include "gui/PanelAnimator.h"
#include "gui/TeachingTheme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QMessageBox>
#include <QFrame>
#include <sstream>
#include <cstdlib>
#include <stdexcept>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（CaptionLabel / StrongBodyLabel）

// ============================================================
// 构造
// ============================================================

VmStackSandboxPanel::VmStackSandboxPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // ---- 顶部：关卡选择 + 目标显示 ----
    auto* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->addWidget(new QLabel(mlTr("关卡："), this));
    levelCombo_ = new QComboBox(this);
    for (const auto& lv : SandboxLibrary::levels()) {
        QString title = QString::fromStdString(
            "第 " + std::to_string(lv.level) + " 关 — " + lv.goal);
        levelCombo_->addItem(title);
    }
    topLayout->addWidget(levelCombo_, 1);
    mainLayout->addLayout(topLayout);

    goalLabel_ = new StrongBodyLabel(this);
    goalLabel_->setWordWrap(true);
    goalLabel_->setStyleSheet(QString::fromUtf8(
        "padding: 4px; background: %1; border-left: 3px solid %2;")
        .arg(TeachingTheme::surface().name(), TeachingTheme::primary().name()));
    mainLayout->addWidget(goalLabel_);

    teachingPointLabel_ = new CaptionLabel(this);
    teachingPointLabel_->setWordWrap(true);
    teachingPointLabel_->setStyleSheet(QString::fromUtf8(
        "padding: 4px; background: %1; border-left: 3px solid %2;")
        .arg(TeachingTheme::surface().name(), TeachingTheme::warning().name()));
    mainLayout->addWidget(teachingPointLabel_);

    // ---- 中部：三栏（可用指令 / 操作数栈 / 已执行序列） ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // 左栏：可用指令
    auto* opBox = new QGroupBox(mlTr("可用指令（点击执行）"), splitter);
    auto* opLayout = new QVBoxLayout(opBox);
    opLayout->setContentsMargins(4, 4, 4, 4);
    opButtonsHost_ = opBox;  // 复用 GroupBox 作为容器， rebuildOpButtons 会往 opLayout 加按钮
    splitter->addWidget(opBox);

    // 中栏：操作数栈可视化
    auto* stackBox = new QGroupBox(mlTr("操作数栈（栈顶在上方）"), splitter);
    auto* stackLayout = new QVBoxLayout(stackBox);
    stackLayout->setContentsMargins(4, 4, 4, 4);
    stackList_ = new QListWidget(stackBox);
    stackList_->setStyleSheet(
        "QListWidget { background: #101820; color: #90ffd0; font-family: Consolas, monospace; }"
        "QListWidget::item { padding: 4px; border-bottom: 1px solid #303040; }");
    stackLayout->addWidget(stackList_);
    splitter->addWidget(stackBox);

    // 右栏：已执行指令序列
    auto* historyBox = new QGroupBox(mlTr("已执行指令序列"), splitter);
    auto* historyLayout = new QVBoxLayout(historyBox);
    historyLayout->setContentsMargins(4, 4, 4, 4);
    historyList_ = new QListWidget(historyBox);
    historyList_->setStyleSheet(
        "QListWidget { font-family: Consolas, monospace; }");
    historyLayout->addWidget(historyList_);
    splitter->addWidget(historyBox);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({260, 200, 260});
    mainLayout->addWidget(splitter, 1);

    // ---- 底部：输出区 + 操作按钮 + 反馈 ----
    auto* bottomBox = new QGroupBox(mlTr("输出"), this);
    auto* bottomLayout = new QVBoxLayout(bottomBox);
    bottomLayout->setContentsMargins(4, 4, 4, 4);
    outputEdit_ = new QTextEdit(bottomBox);
    outputEdit_->setReadOnly(true);
    outputEdit_->setMaximumHeight(80);
    outputEdit_->setStyleSheet(
        "QTextEdit { background: #101820; color: #ffd090; font-family: Consolas, monospace; }");
    bottomLayout->addWidget(outputEdit_);
    mainLayout->addWidget(bottomBox);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    undoBtn_   = new QPushButton(mlTr("↩ 撤销"), this);
    resetBtn_  = new QPushButton(mlTr("🔄 重置"), this);
    checkBtn_  = new PrimaryPushButton(mlTr("✓ 检查"), this);
    btnRow->addWidget(undoBtn_);
    btnRow->addWidget(resetBtn_);
    btnRow->addStretch();
    btnRow->addWidget(checkBtn_);
    mainLayout->addLayout(btnRow);

    feedbackLabel_ = new QLabel(this);
    feedbackLabel_->setWordWrap(true);
    feedbackLabel_->setStyleSheet(QString::fromUtf8(
        "padding: 4px; background: %1; border-left: 3px solid %2;")
        .arg(TeachingTheme::surface().name(), TeachingTheme::textHint().name()));
    mainLayout->addWidget(feedbackLabel_);

    // ---- 信号连接 ----
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VmStackSandboxPanel::onLevelChanged);
    connect(undoBtn_,  &QPushButton::clicked, this, &VmStackSandboxPanel::onUndo);
    connect(resetBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onReset);
    connect(checkBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onCheck);

    // ---- 初始加载第一关 ----
    if (!SandboxLibrary::levels().empty()) {
        loadLevel(0);
    }
}

// ============================================================
// 关卡加载与按钮重建
// ============================================================

void VmStackSandboxPanel::onLevelChanged(int index) {
    loadLevel(index);
}

void VmStackSandboxPanel::loadLevel(int index) {
    const auto& levels = SandboxLibrary::levels();
    if (index < 0 || index >= static_cast<int>(levels.size())) return;
    currentLevelIndex_ = index;
    const auto& lv = levels[index];

    goalLabel_->setText(QString::fromStdString(
        "<b>目标：</b>" + lv.goal +
        "　<font color='#707070'>难度：" + std::string(lv.difficulty, '*') + "</font>"));
    teachingPointLabel_->setText(QString::fromStdString(
        "<b>教学点：</b>" + lv.teachingPoint +
        "　<font color='#707070'>提示：" + lv.hint + "</font>"));

    stack_.clear();
    history_.clear();
    outputs_.clear();
    snapshots_.clear();
    outputSnapshots_.clear();
    halted_ = false;
    refreshStackView();
    refreshHistoryView();
    refreshOutputView();
    setFeedback(mlTr("已加载关卡，请按目标执行指令。"));

    rebuildOpButtons();
}

void VmStackSandboxPanel::rebuildOpButtons() {
    // 找到 opButtonsHost_ 内的 QVBoxLayout 并清空所有子按钮
    auto* host = qobject_cast<QGroupBox*>(opButtonsHost_);
    if (!host) return;
    auto* layout = qobject_cast<QVBoxLayout*>(host->layout());
    if (!layout) return;
    // 清空除 stretch 外的所有项
    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const auto& levels = SandboxLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= static_cast<int>(levels.size())) return;
    const auto& ops = levels[currentLevelIndex_].availableOps;

    for (const auto& op : ops) {
        auto* btn = new QPushButton(opButtonText(op), host);
        btn->setStyleSheet(
            "QPushButton { padding: 6px; text-align: left; font-family: Consolas, monospace; }"
            "QPushButton:hover { background: #d0e0ff; }");
        connect(btn, &QPushButton::clicked, this, [this, op]() {
            executeOp(op);
        });
        layout->addWidget(btn);
    }
    layout->addStretch();
}

// ============================================================
// 栈状态机执行
// ============================================================

namespace {

/// 将字符串解析为 long long，失败时返回 false
bool parseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

void VmStackSandboxPanel::executeOp(const SandboxOp& op) {
    if (halted_) {
        setFeedback(mlTr("已 HALT。请点击 [🔄 重置] 重新开始本关。"), true);
        return;
    }

    // 保存撤销快照（执行前的栈与输出）
    snapshots_.push_back(stack_);
    outputSnapshots_.push_back(outputs_);

    auto showError = [this](const QString& msg) {
        // 撤销快照（这条指令未真正执行）
        snapshots_.pop_back();
        outputSnapshots_.pop_back();
        setFeedback(msg, true);
    };

    switch (op.type) {
        case SandboxOpType::PUSH_INT:
        case SandboxOpType::PUSH_STRING:
            stack_.push_back(op.operand);
            break;

        case SandboxOpType::ADD:
        case SandboxOpType::SUB:
        case SandboxOpType::MUL:
        case SandboxOpType::DIV:
        case SandboxOpType::MOD: {
            if (stack_.size() < 2) {
                QString opName = QString::fromUtf8(sandboxOpTypeName(op.type));
                showError(QString(mlTr("❌ %1 需要栈顶有两个值！当前栈只有 %2 个值。"))
                          .arg(opName).arg(static_cast<int>(stack_.size())));
                return;
            }
            std::string b = stack_.back(); stack_.pop_back();
            std::string a = stack_.back(); stack_.pop_back();
            long long ia = 0, ib = 0;
            if (!parseInt(a, ia) || !parseInt(b, ib)) {
                QString opName = QString::fromUtf8(sandboxOpTypeName(op.type));
                showError(QString(mlTr("❌ %1 需要两个 int 操作数，但栈顶值不是有效整数（%2, %3）。"))
                          .arg(opName)
                          .arg(QString::fromStdString(a))
                          .arg(QString::fromStdString(b)));
                // 已 pop，需还原
                stack_.push_back(a);
                stack_.push_back(b);
                return;
            }
            long long r = 0;
            switch (op.type) {
                case SandboxOpType::ADD: r = ia + ib; break;
                case SandboxOpType::SUB: r = ia - ib; break;
                case SandboxOpType::MUL: r = ia * ib; break;
                case SandboxOpType::DIV:
                    if (ib == 0) {
                        showError(mlTr("❌ 除数不能为 0！"));
                        // 还原栈
                        stack_.push_back(a);
                        stack_.push_back(b);
                        return;
                    }
                    r = ia / ib;  // 整数除法截断向零（与 MiniLang 三后端一致）
                    break;
                case SandboxOpType::MOD:
                    if (ib == 0) {
                        showError(mlTr("❌ 取模的除数不能为 0！"));
                        // 还原栈
                        stack_.push_back(a);
                        stack_.push_back(b);
                        return;
                    }
                    r = ia % ib;
                    break;
                default: break;
            }
            stack_.push_back(std::to_string(r));
            break;
        }

        case SandboxOpType::NEG: {
            if (stack_.empty()) {
                showError(mlTr("❌ NEG 需要栈顶有一个值！当前栈为空。"));
                return;
            }
            std::string a = stack_.back(); stack_.pop_back();
            long long ia = 0;
            if (!parseInt(a, ia)) {
                showError(QString(mlTr("❌ NEG 需要 int 操作数，但栈顶值不是有效整数（%1）。"))
                          .arg(QString::fromStdString(a)));
                stack_.push_back(a);
                return;
            }
            stack_.push_back(std::to_string(-ia));
            break;
        }

        case SandboxOpType::PRINT: {
            if (stack_.empty()) {
                showError(mlTr("❌ PRINT 需要栈顶有一个值！当前栈为空。"));
                return;
            }
            std::string v = stack_.back(); stack_.pop_back();
            outputs_.push_back(v);
            break;
        }

        case SandboxOpType::HALT:
            halted_ = true;
            break;
    }

    history_.push_back(op);
    refreshStackView();
    refreshHistoryView();
    refreshOutputView();

    // 即时反馈：成功执行
    QString opName = QString::fromUtf8(sandboxOpTypeName(op.type));
    setFeedback(QString(mlTr("✓ 已执行 %1。")).arg(opName));
}

void VmStackSandboxPanel::onUndo() {
    if (history_.empty() || snapshots_.empty()) {
        setFeedback(mlTr("没有可撤销的指令。"), true);
        return;
    }
    // 弹出 history 与快照
    history_.pop_back();
    stack_ = std::move(snapshots_.back());
    snapshots_.pop_back();
    outputs_ = std::move(outputSnapshots_.back());
    outputSnapshots_.pop_back();
    halted_ = false;  // 撤销 HALT 后允许继续
    refreshStackView();
    refreshHistoryView();
    refreshOutputView();
    setFeedback(mlTr("已撤销最后一条指令。"));
}

void VmStackSandboxPanel::onReset() {
    stack_.clear();
    history_.clear();
    outputs_.clear();
    snapshots_.clear();
    outputSnapshots_.clear();
    halted_ = false;
    refreshStackView();
    refreshHistoryView();
    refreshOutputView();
    setFeedback(mlTr("已重置本关。"));
}

void VmStackSandboxPanel::onCheck() {
    QString diag;
    if (checkAnswer(&diag)) {
        setFeedback(mlTr("✅ 通关！指令序列正确，输出符合预期。"));
        const auto& levels = SandboxLibrary::levels();
        if (currentLevelIndex_ >= 0 && currentLevelIndex_ < static_cast<int>(levels.size())) {
            QString levelId = QString("level-%1").arg(levels[currentLevelIndex_].level);
            emit activityCompleted(levelId);
        }
    } else {
        setFeedback(diag, true);
    }
}

// ============================================================
// 答案检查
// ============================================================

bool VmStackSandboxPanel::checkAnswer(QString* diag) const {
    const auto& levels = SandboxLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= static_cast<int>(levels.size())) {
        if (diag) *diag = mlTr("未加载任何关卡。");
        return false;
    }
    const auto& lv = levels[currentLevelIndex_];

    // 自由模式（关卡 5）：expectedSequence 为空，永远算"通关"
    if (lv.expectedSequence.empty()) {
        if (diag) *diag = mlTr("🎉 自由模式：可以自由探索，无需检查答案。");
        return true;
    }

    // 1. 检查 history_ 与 expectedSequence 的长度与每条指令
    if (history_.size() != lv.expectedSequence.size()) {
        if (diag) {
            *diag = QString(mlTr("❌ 指令数量不对。期望 %1 条，实际 %2 条。"))
                    .arg(static_cast<int>(lv.expectedSequence.size()))
                    .arg(static_cast<int>(history_.size()));
        }
        return false;
    }
    for (size_t i = 0; i < history_.size(); ++i) {
        if (history_[i].type != lv.expectedSequence[i].type ||
            history_[i].operand != lv.expectedSequence[i].operand) {
            if (diag) {
                *diag = QString(mlTr("❌ 第 %1 条指令不对。期望 %2，实际 %3。"))
                        .arg(static_cast<int>(i + 1))
                        .arg(opDisplayText(lv.expectedSequence[i]))
                        .arg(opDisplayText(history_[i]));
            }
            return false;
        }
    }

    // 2. 检查输出是否符合预期
    if (!lv.expectedOutput.empty()) {
        std::string joined;
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (i > 0) joined += "\n";
            joined += outputs_[i];
        }
        if (joined != lv.expectedOutput) {
            if (diag) {
                *diag = QString(mlTr("❌ 输出不对。期望 \"%1\"，实际 \"%2\"。"))
                        .arg(QString::fromStdString(lv.expectedOutput))
                        .arg(QString::fromStdString(joined));
            }
            return false;
        }
    } else {
        // 没有期望输出（如自由模式或 HALT 关卡），要求至少执行了 PRINT
        if (outputs_.empty()) {
            if (diag) *diag = mlTr("💡 你已经算出了结果，记得用 PRINT 输出它！");
            return false;
        }
    }

    return true;
}

// ============================================================
// UI 刷新
// ============================================================

void VmStackSandboxPanel::refreshStackView() {
    stackList_->clear();
    // 栈顶在顶部：从后往前加
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        QString text;
        if (it == stack_.rbegin()) {
            text = QString::fromStdString(*it) + mlTr("  ← 栈顶");
        } else {
            text = QString::fromStdString(*it);
        }
        stackList_->addItem(text);
    }
    if (stack_.empty()) {
        stackList_->addItem(mlTr("（栈为空）"));
    }
    PanelAnimator::fadeInWidget(stackList_);
}

void VmStackSandboxPanel::refreshHistoryView() {
    historyList_->clear();
    for (size_t i = 0; i < history_.size(); ++i) {
        QString text = QString("%1. %2")
                       .arg(static_cast<int>(i + 1))
                       .arg(opDisplayText(history_[i]));
        historyList_->addItem(text);
    }
    if (history_.empty()) {
        historyList_->addItem(mlTr("（尚未执行指令）"));
    }
}

void VmStackSandboxPanel::refreshOutputView() {
    std::string joined;
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (i > 0) joined += "\n";
        joined += outputs_[i];
    }
    outputEdit_->setPlainText(QString::fromStdString(joined));
}

void VmStackSandboxPanel::setFeedback(const QString& text, bool isError) {
    feedbackLabel_->setText(text);
    if (isError) {
        feedbackLabel_->setStyleSheet(
            "padding: 4px; background: #fff0f0; border-left: 3px solid #c03030;");
    } else {
        feedbackLabel_->setStyleSheet(
            "padding: 4px; background: #f0fff0; border-left: 3px solid #30a030;");
    }
}

QString VmStackSandboxPanel::opDisplayText(const SandboxOp& op) const {
    QString name = QString::fromUtf8(sandboxOpTypeName(op.type));
    if (op.type == SandboxOpType::PUSH_INT || op.type == SandboxOpType::PUSH_STRING) {
        return QString("%1 %2").arg(name).arg(QString::fromStdString(op.operand));
    }
    return name;
}

QString VmStackSandboxPanel::opButtonText(const SandboxOp& op) const {
    QString name = QString::fromUtf8(sandboxOpTypeName(op.type));
    if (op.type == SandboxOpType::PUSH_INT) {
        return QString("PUSH_INT %1").arg(QString::fromStdString(op.operand));
    }
    if (op.type == SandboxOpType::PUSH_STRING) {
        return QString("PUSH_STRING \"%1\"").arg(QString::fromStdString(op.operand));
    }
    return name;
}
