// ============================================================
// VmStackSandboxPanel.cpp — VM 栈沙盒面板实现（功能 5）
// ============================================================

#include "gui/VmStackSandboxPanel.h"
#include "gui/GuiTextUtils.h" // R75: monospaceFont() 跨机器字体回退链
#include "gui/I18n.h"
#include "gui/LearnerProgress.h" // P0-2 fix (F7): 关卡完成状态持久化
#include "gui/PanelAnimator.h"
#include "gui/ProgressSaveFeedback.h" // P2-UX fix: save 失败 toast 通知
#include "gui/TeachingTheme.h"
// P1-3 fix (F14): 引入真实 Lexer + Parser + Compiler + StackVM 用于对照验证
#include "app/IdeController.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterBytecode.h" // RegBytecodeChunk / regOpName
#include "compiler/VM.h"
#include "gui/BytecodeTracePanel.h" // BytecodeTraceLibrary::opCodeDocs()
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QApplication> // QApplication::processEvents()（onTraceRunAll 长循环让出 UI 线程）
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSplitter>
#include <QStyle> // style()->polish() / unpolish() 用于 QSS 动态属性刷新
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "Label.h"      // QFluentKit（CaptionLabel / StrongBodyLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

// ============================================================
// 构造
// ============================================================

/// 构造 VM 栈沙盒面板：初始化双页结构、关卡数据与栈模型。
VmStackSandboxPanel::VmStackSandboxPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // ---- 子页切换（UX-R fix: 统一 TeachingSubPageBar 组件，替代手写 pageBtn + pageStack_）----
    // 互斥选中态 / 主题色高亮 / 滑入动画由组件内置，按钮栏与堆栈分别加入布局。
    subPageBar_ = new TeachingSubPageBar(this);
    mainLayout->addLayout(subPageBar_->buttonBar());

    // ---- 子页 QSS（UX-R2 fix: 硬编码颜色迁移到 TeachingTheme 语义色）----
    setStyleSheet(
        QStringLiteral(
            "QComboBox#levelCombo { background: %1; border: 1px solid %2; "
            "border-radius: 4px; padding: 4px 8px; }"
            "QComboBox#levelCombo:hover { border-color: %3; }"
            "QPushButton#levelChip { background: %4; border: 1px solid %2; "
            "border-radius: 4px; font-size: 11px; }"
            "QPushButton#levelChip:hover { border-color: %3; background: %5; }"
            "QPushButton#levelChip[current='true'] { background: %3; color: %6; "
            "border-color: %7; font-weight: bold; }"
            "QPushButton#levelChip[locked='true'] { background: %8; color: %9; "
            "border-color: %10; }"
            // 追踪页 QSS
            "QListWidget#bytecodeList { font-family: \"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains "
            "Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace; "
            "border: 1px solid %2; }"
            "QListWidget#bytecodeList::item { padding: 2px 4px; border-bottom: 1px solid %4; }"
            "QListWidget#bytecodeList::item:selected { background: %3; color: %6; }"
            "QTableWidget#registerTable { gridline-color: %2; "
            "font-family: \"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code "
            "Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace; }"
            "QTableWidget#registerTable QHeaderView::section { background: %4; "
            "padding: 4px; border: 1px solid %2; }")
            .arg(TeachingTheme::surface().name(),        // %1 白底
                 TeachingTheme::border().name(),         // %2 边框
                 TeachingTheme::primary().name(),        // %3 主题色
                 TeachingTheme::surfaceHover().name(),   // %4 浅灰背景
                 TeachingTheme::primaryHoverBg().name(), // %5 hover 浅蓝
                 TeachingTheme::onPrimary().name(),      // %6 白色文字
                 TeachingTheme::primaryPressed().name(), // %7 pressed 主题色
                 TeachingTheme::lockedBg().name(),       // %8 锁定背景
                 TeachingTheme::textMuted().name(),      // %9 锁定文字
                 TeachingTheme::statusBorder().name())); // %10 锁定边框

    // ---- 子页堆栈（由 TeachingSubPageBar 持有，addPage 自动加入）----
    mainLayout->addWidget(subPageBar_->stack(), 1);

    // ============================================================
    // 页 1：栈沙盒
    // ============================================================
    auto* sandboxPage = new QWidget(this);
    auto* sandboxLayout = new QVBoxLayout(sandboxPage);
    sandboxLayout->setContentsMargins(0, 0, 0, 0);
    sandboxLayout->setSpacing(4);

    // ---- 关卡芯片栏（独立一行，放在 QComboBox 上方）----
    {
        auto* chipRow = new QHBoxLayout();
        chipRow->setContentsMargins(0, 0, 0, 0);
        chipRow->setSpacing(6);
        const auto& chipLevels = SandboxLibrary::levels();
        for (int i = 0; i < (int)chipLevels.size(); ++i) {
            auto* chip = new QPushButton(sandboxPage);
            chip->setObjectName("levelChip");
            chip->setFixedSize(48, 32);
            const int chipIndex = i;
            connect(chip, &QPushButton::clicked, this, [this, chipIndex]() {
                if (chipIndex >= 0 && chipIndex < levelCombo_->count()) {
                    levelCombo_->setCurrentIndex(chipIndex);
                }
            });
            levelChips_.append(chip);
            chipRow->addWidget(chip);
        }
        chipRow->addStretch();
        sandboxLayout->addLayout(chipRow);
    }

    // ---- 顶部：关卡选择 + 目标显示 ----
    auto* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->addWidget(new QLabel(mlTr("关卡："), sandboxPage));
    levelCombo_ = new QComboBox(sandboxPage);
    levelCombo_->setObjectName("levelCombo");
    for (const auto& lv : SandboxLibrary::levels()) {
        QString title = QString::fromStdString("第 " + std::to_string(lv.level) + " 关 — " + lv.goal);
        levelCombo_->addItem(title);
    }
    topLayout->addWidget(levelCombo_, 1);
    sandboxLayout->addLayout(topLayout);

    goalLabel_ = new StrongBodyLabel(sandboxPage);
    goalLabel_->setWordWrap(true);
    goalLabel_->setStyleSheet(QString::fromUtf8("padding: 4px; background: %1; border-left: 3px solid %2;")
                                  .arg(TeachingTheme::surface().name(), TeachingTheme::primary().name()));
    sandboxLayout->addWidget(goalLabel_);

    teachingPointLabel_ = new CaptionLabel(sandboxPage);
    teachingPointLabel_->setWordWrap(true);
    teachingPointLabel_->setStyleSheet(QString::fromUtf8("padding: 4px; background: %1; border-left: 3px solid %2;")
                                           .arg(TeachingTheme::surface().name(), TeachingTheme::warning().name()));
    sandboxLayout->addWidget(teachingPointLabel_);

    // ---- 中部：三栏（可用指令 / 操作数栈 / 已执行序列） ----
    auto* splitter = new QSplitter(Qt::Horizontal, sandboxPage);

    auto* opBox = new QGroupBox(mlTr("可用指令（点击执行）"), splitter);
    auto* opLayout = new QVBoxLayout(opBox);
    opLayout->setContentsMargins(4, 4, 4, 4);
    opButtonsHost_ = opBox;
    splitter->addWidget(opBox);

    auto* stackBox = new QGroupBox(mlTr("操作数栈（栈顶在上方）"), splitter);
    auto* stackLayout = new QVBoxLayout(stackBox);
    stackLayout->setContentsMargins(4, 4, 4, 4);
    stackList_ = new QListWidget(stackBox);
    stackList_->setStyleSheet(
        QString::fromUtf8("QListWidget { background: %1; color: %2; font-family: \"Cascadia Code\",\"Cascadia "
                          "Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans "
                          "Mono\",\"Courier New\",monospace; }"
                          "QListWidget::item { padding: 4px; border-bottom: 1px solid %3; }")
            .arg(TeachingTheme::surface().name(), TeachingTheme::textPrimary().name(), TeachingTheme::border().name()));
    stackLayout->addWidget(stackList_);
    splitter->addWidget(stackBox);

    auto* historyBox = new QGroupBox(mlTr("已执行指令序列"), splitter);
    auto* historyLayout = new QVBoxLayout(historyBox);
    historyLayout->setContentsMargins(4, 4, 4, 4);
    historyList_ = new QListWidget(historyBox);
    historyList_->setStyleSheet(
        "QListWidget { font-family: \"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code "
        "Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace; }");
    historyLayout->addWidget(historyList_);
    splitter->addWidget(historyBox);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({260, 200, 260});
    sandboxLayout->addWidget(splitter, 1);

    // ---- 底部：输出区 + 操作按钮 + 反馈 ----
    auto* bottomBox = new QGroupBox(mlTr("输出"), sandboxPage);
    auto* bottomLayout = new QVBoxLayout(bottomBox);
    bottomLayout->setContentsMargins(4, 4, 4, 4);
    outputEdit_ = new QTextEdit(bottomBox);
    outputEdit_->setReadOnly(true);
    outputEdit_->setMaximumHeight(80);
    outputEdit_->setStyleSheet(
        QString::fromUtf8("QTextEdit { background: %1; color: %2; font-family: \"Cascadia Code\",\"Cascadia "
                          "Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans "
                          "Mono\",\"Courier New\",monospace; }")
            .arg(TeachingTheme::surfaceHover().name(), TeachingTheme::textPrimary().name()));
    bottomLayout->addWidget(outputEdit_);
    sandboxLayout->addWidget(bottomBox);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    undoBtn_ = new QPushButton(mlTr("↩ 撤销"), sandboxPage);
    resetBtn_ = new QPushButton(mlTr("🔄 重置"), sandboxPage);
    checkBtn_ = new PrimaryPushButton(mlTr("✓ 检查"), sandboxPage);
    verifyBtn_ = new QPushButton(mlTr("🛠 用真实 StackVM 验证"), sandboxPage);
    verifyBtn_->setToolTip(mlTr("调用真实 Lexer+Parser+Compiler+StackVM 执行当前关卡对应的 MiniLang 源码，"
                                "把实际输出与预期输出对照显示"));
    gotoTraceBtn_ = new QPushButton(mlTr("📊 查看追踪"), sandboxPage);
    gotoTraceBtn_->setToolTip(mlTr("切换到「真实字节码追踪」子页，单步观察真实字节码执行"));
    gotoTraceBtn_->setEnabled(false);
    btnRow->addWidget(undoBtn_);
    btnRow->addWidget(resetBtn_);
    btnRow->addWidget(verifyBtn_);
    btnRow->addWidget(gotoTraceBtn_);
    btnRow->addStretch();
    btnRow->addWidget(checkBtn_);
    sandboxLayout->addLayout(btnRow);

    feedbackLabel_ = new QLabel(sandboxPage);
    feedbackLabel_->setWordWrap(true);
    feedbackLabel_->setStyleSheet(QString::fromUtf8("padding: 4px; background: %1; border-left: 3px solid %2;")
                                      .arg(TeachingTheme::surface().name(), TeachingTheme::textHint().name()));
    sandboxLayout->addWidget(feedbackLabel_);

    // 沙盒操作对应的真实 OpCode 提示
    sandboxOpHintLabel_ = new QLabel(sandboxPage);
    sandboxOpHintLabel_->setWordWrap(true);
    sandboxOpHintLabel_->setStyleSheet(
        QString::fromUtf8("padding: 3px; background: %1; border-left: 3px solid %2; "
                          "color: %3; font-size: 11px;")
            .arg(TeachingTheme::surfaceHover().name(), TeachingTheme::info().name(), TeachingTheme::textHint().name()));
    sandboxOpHintLabel_->setText(mlTr("💡 点击左侧指令按钮执行，下方会显示对应的真实 OpCode。"));
    sandboxLayout->addWidget(sandboxOpHintLabel_);

    // ---- 页 1 / 页 2 注册到统一子页切换组件 ----
    // UX-R fix: addPage 同时创建按钮与入栈（首个自动选中），替代手写页切换信号。
    subPageBar_->addPage(mlTr("① 栈沙盒"), sandboxPage);

    // ============================================================
    // 页 2：真实字节码追踪
    // ============================================================
    tracePage_ = new QWidget(this);
    buildTracePage(tracePage_);
    subPageBar_->addPage(mlTr("② 真实字节码追踪"), tracePage_);

    // ---- 页切换（UX-R fix: 已由 TeachingSubPageBar 内置，删除手写互斥切换）----

    // ---- 沙盒页信号连接 ----
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VmStackSandboxPanel::onLevelChanged);
    connect(undoBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onUndo);
    connect(resetBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onReset);
    connect(checkBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onCheck);
    connect(verifyBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onVerifyWithRealStackVM);
    connect(gotoTraceBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::switchToTracePage);

    // ---- 追踪页信号连接 ----
    connect(compileBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onCompileAndLoad);
    connect(stepBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onTraceStep);
    connect(runAllBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onTraceRunAll);
    connect(resetTraceBtn_, &QPushButton::clicked, this, &VmStackSandboxPanel::onTraceReset);

    // ---- 初始加载第一关 ----
    if (!SandboxLibrary::levels().empty()) {
        loadLevel(0);
    }
}

// ============================================================
// setController
// ============================================================

/// 绑定 IDE 控制器，用于调用编译器/VM 等后端能力。
void VmStackSandboxPanel::setController(IdeController* controller) {
    // AUDIT-P0 fix: 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        // 注册 VM 状态变更监听，VM 单步执行后自动刷新追踪视图
        controller_->addVmStateChangedListener(this, [this]() {
            if (subPageBar_ && subPageBar_->currentIndex() == 1) {
                refreshTraceViews();
            }
        });
    } else {
        if (traceStatusLabel_) {
            traceStatusLabel_->setText(mlTr("状态：未绑定 controller（追踪功能不可用）"));
        }
    }
}

// AUDIT-P0 fix: 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调。
VmStackSandboxPanel::~VmStackSandboxPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

// ============================================================
// 追踪页 UI 构建
// ============================================================

/// 构建「字节码跟踪」子页的 UI 与控件连接。
void VmStackSandboxPanel::buildTracePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // ---- 顶部工具条：编译并加载 + IP 指示器 + 状态 ----
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(0, 0, 0, 0);
    topBar->setSpacing(8);
    compileBtn_ = new QPushButton(mlTr("📦 编译并加载"), host);
    compileBtn_->setToolTip(mlTr("编译当前关卡源码为真实字节码并加载到 VM，准备单步追踪"));
    ipIndicator_ = new QLabel(mlTr("IP: -"), host);
    ipIndicator_->setStyleSheet(
        QString::fromUtf8("padding: 2px 8px; background: %1; border: 1px solid %2; border-radius: 3px; "
                          "font-family: \"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source "
                          "Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace; font-weight: bold;")
            .arg(TeachingTheme::surface().name(), TeachingTheme::border().name()));
    traceStatusLabel_ = new QLabel(mlTr("状态：未加载"), host);
    traceStatusLabel_->setStyleSheet(
        QString::fromUtf8("padding: 2px 8px; color: %1;").arg(TeachingTheme::textHint().name()));
    topBar->addWidget(compileBtn_);
    topBar->addWidget(ipIndicator_);
    topBar->addWidget(traceStatusLabel_, 1);
    v->addLayout(topBar);

    // ---- 中部：水平分割（左=字节码列表 / 右=栈+寄存器+输出） ----
    auto* hSplitter = new QSplitter(Qt::Horizontal, host);

    // 左：真实字节码指令列表
    auto* bcBox = new QGroupBox(mlTr("真实字节码指令序列"), hSplitter);
    auto* bcLayout = new QVBoxLayout(bcBox);
    bcLayout->setContentsMargins(4, 4, 4, 4);
    bytecodeList_ = new QListWidget(bcBox);
    bytecodeList_->setObjectName("bytecodeList");
    bcLayout->addWidget(bytecodeList_);
    hSplitter->addWidget(bcBox);

    // 右：垂直分割（栈状态 / 寄存器视图 / 输出）
    auto* rightSplitter = new QSplitter(Qt::Vertical, hSplitter);

    auto* stackBox = new QGroupBox(mlTr("操作数栈（执行前）"), rightSplitter);
    auto* stackLayout = new QVBoxLayout(stackBox);
    stackLayout->setContentsMargins(4, 4, 4, 4);
    traceStackView_ = new QListWidget(stackBox);
    traceStackView_->setStyleSheet(
        QString::fromUtf8("QListWidget { background: %1; color: %2; font-family: \"Cascadia Code\",\"Cascadia "
                          "Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans "
                          "Mono\",\"Courier New\",monospace; }"
                          "QListWidget::item { padding: 3px; border-bottom: 1px solid %3; }")
            .arg(TeachingTheme::surface().name(), TeachingTheme::textPrimary().name(), TeachingTheme::border().name()));
    stackLayout->addWidget(traceStackView_);
    rightSplitter->addWidget(stackBox);

    auto* regBox = new QGroupBox(mlTr("寄存器视图"), rightSplitter);
    auto* regLayout = new QVBoxLayout(regBox);
    regLayout->setContentsMargins(4, 4, 4, 4);
    registerTable_ = new QTableWidget(0, 2, regBox);
    registerTable_->setObjectName("registerTable");
    registerTable_->setHorizontalHeaderLabels({mlTr("寄存器"), mlTr("值")});
    registerTable_->verticalHeader()->setVisible(false);
    registerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    registerTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    registerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    regLayout->addWidget(registerTable_);
    rightSplitter->addWidget(regBox);

    auto* outBox = new QGroupBox(mlTr("程序输出"), rightSplitter);
    auto* outLayout = new QVBoxLayout(outBox);
    outLayout->setContentsMargins(4, 4, 4, 4);
    traceOutputEdit_ = new QTextEdit(outBox);
    traceOutputEdit_->setReadOnly(true);
    traceOutputEdit_->setMaximumHeight(80);
    traceOutputEdit_->setStyleSheet(
        QString::fromUtf8("QTextEdit { background: %1; color: %2; font-family: \"Cascadia Code\",\"Cascadia "
                          "Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans "
                          "Mono\",\"Courier New\",monospace; }")
            .arg(TeachingTheme::surfaceHover().name(), TeachingTheme::textPrimary().name()));
    outLayout->addWidget(traceOutputEdit_);
    rightSplitter->addWidget(outBox);

    rightSplitter->setStretchFactor(0, 2);
    rightSplitter->setStretchFactor(1, 2);
    rightSplitter->setStretchFactor(2, 1);
    rightSplitter->setSizes({120, 120, 80});

    hSplitter->addWidget(rightSplitter);
    hSplitter->setStretchFactor(0, 2);
    hSplitter->setStretchFactor(1, 3);
    hSplitter->setSizes({320, 380});
    v->addWidget(hSplitter, 1);

    // ---- 底部：单步 / 运行到底 / 重置 ----
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(6);
    stepBtn_ = new QPushButton(mlTr("▶ 单步执行"), host);
    runAllBtn_ = new QPushButton(mlTr("⏩ 运行到底"), host);
    resetTraceBtn_ = new QPushButton(mlTr("🔄 重置"), host);
    stepBtn_->setEnabled(false);
    runAllBtn_->setEnabled(false);
    resetTraceBtn_->setEnabled(false);
    btnRow->addWidget(stepBtn_);
    btnRow->addWidget(runAllBtn_);
    btnRow->addWidget(resetTraceBtn_);
    btnRow->addStretch();
    v->addLayout(btnRow);
}

// ============================================================
// 关卡加载与按钮重建
// ============================================================

/// 关卡下拉切换回调：加载新关卡。
void VmStackSandboxPanel::onLevelChanged(int index) {
    loadLevel(index);
}

/// 加载指定索引的关卡：重置栈/历史并刷新视图。
void VmStackSandboxPanel::loadLevel(int index) {
    const auto& levels = SandboxLibrary::levels();
    if (index < 0 || index >= static_cast<int>(levels.size()))
        return;
    currentLevelIndex_ = index;
    const auto& lv = levels[index];

    goalLabel_->setText(QString::fromStdString("<b>目标：</b>" + lv.goal + "　<font color='#707070'>难度：" +
                                               std::string(lv.difficulty, '*') + "</font>"));
    teachingPointLabel_->setText(QString::fromStdString("<b>教学点：</b>" + lv.teachingPoint +
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
    sandboxOpHintLabel_->setText(mlTr("💡 点击左侧指令按钮执行，下方会显示对应的真实 OpCode。"));

    rebuildOpButtons();
    refreshLevelChips();
}

/// 依据当前关卡可用指令重建操作按钮。
void VmStackSandboxPanel::rebuildOpButtons() {
    auto* host = qobject_cast<QGroupBox*>(opButtonsHost_);
    if (!host)
        return;
    auto* layout = qobject_cast<QVBoxLayout*>(host->layout());
    if (!layout)
        return;
    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const auto& levels = SandboxLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= static_cast<int>(levels.size()))
        return;
    const auto& ops = levels[currentLevelIndex_].availableOps;

    for (const auto& op : ops) {
        auto* btn = new QPushButton(opButtonText(op), host);
        btn->setStyleSheet("QPushButton { padding: 6px; text-align: left; font-family: \"Cascadia Code\",\"Cascadia "
                           "Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\",\"DejaVu Sans "
                           "Mono\",\"Courier New\",monospace; }"
                           "QPushButton:hover { background: #d0e0ff; }");
        connect(btn, &QPushButton::clicked, this, [this, op]() { executeOp(op); });
        layout->addWidget(btn);
    }
    layout->addStretch();
}

// ============================================================
// 沙盒 SandboxOpType → 真实 OpCode 名称映射
// ============================================================

QHash<SandboxOpType, QString> VmStackSandboxPanel::buildSandboxToRealOpMap() {
    QHash<SandboxOpType, QString> m;
    m[SandboxOpType::PUSH_INT] = QStringLiteral("OP_INT");
    m[SandboxOpType::PUSH_STRING] = QStringLiteral("OP_STRING");
    m[SandboxOpType::ADD] = QStringLiteral("OP_ADD");
    m[SandboxOpType::SUB] = QStringLiteral("OP_SUBTRACT");
    m[SandboxOpType::MUL] = QStringLiteral("OP_MULTIPLY");
    m[SandboxOpType::DIV] = QStringLiteral("OP_DIVIDE");
    m[SandboxOpType::MOD] = QStringLiteral("OP_MODULO");
    m[SandboxOpType::NEG] = QStringLiteral("OP_NEGATE");
    m[SandboxOpType::PRINT] = QStringLiteral("OP_PRINT");
    m[SandboxOpType::HALT] = QStringLiteral("OP_RETURN");
    return m;
}

// ============================================================
// 栈状态机执行
// ============================================================

namespace {

bool parseInt(const std::string& s, long long& out) {
    if (s.empty())
        return false;
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size())
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

/// 执行一条沙盒指令：更新栈与历史，并刷新相关视图。
void VmStackSandboxPanel::executeOp(const SandboxOp& op) {
    if (halted_) {
        setFeedback(mlTr("已 HALT。请点击 [🔄 重置] 重新开始本关。"), true);
        return;
    }

    snapshots_.push_back(stack_);
    outputSnapshots_.push_back(outputs_);

    auto showError = [this](const QString& msg) {
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
                          .arg(opName)
                          .arg(static_cast<int>(stack_.size())));
            return;
        }
        std::string b = stack_.back();
        stack_.pop_back();
        std::string a = stack_.back();
        stack_.pop_back();
        long long ia = 0, ib = 0;
        if (!parseInt(a, ia) || !parseInt(b, ib)) {
            QString opName = QString::fromUtf8(sandboxOpTypeName(op.type));
            showError(QString(mlTr("❌ %1 需要两个 int 操作数，但栈顶值不是有效整数（%2, %3）。"))
                          .arg(opName)
                          .arg(QString::fromStdString(a))
                          .arg(QString::fromStdString(b)));
            stack_.push_back(a);
            stack_.push_back(b);
            return;
        }
        long long r = 0;
        switch (op.type) {
        case SandboxOpType::ADD:
            r = ia + ib;
            break;
        case SandboxOpType::SUB:
            r = ia - ib;
            break;
        case SandboxOpType::MUL:
            r = ia * ib;
            break;
        case SandboxOpType::DIV:
            if (ib == 0) {
                showError(mlTr("❌ 除数不能为 0！"));
                stack_.push_back(a);
                stack_.push_back(b);
                return;
            }
            r = ia / ib;
            break;
        case SandboxOpType::MOD:
            if (ib == 0) {
                showError(mlTr("❌ 取模的除数不能为 0！"));
                stack_.push_back(a);
                stack_.push_back(b);
                return;
            }
            r = ia % ib;
            break;
        default:
            break;
        }
        stack_.push_back(std::to_string(r));
        break;
    }

    case SandboxOpType::NEG: {
        if (stack_.empty()) {
            showError(mlTr("❌ NEG 需要栈顶有一个值！当前栈为空。"));
            return;
        }
        std::string a = stack_.back();
        stack_.pop_back();
        long long ia = 0;
        if (!parseInt(a, ia)) {
            showError(
                QString(mlTr("❌ NEG 需要 int 操作数，但栈顶值不是有效整数（%1）。")).arg(QString::fromStdString(a)));
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
        std::string v = stack_.back();
        stack_.pop_back();
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

    // 教学联动：在底部显示对应的真实 OpCode 提示
    static const auto kMap = buildSandboxToRealOpMap();
    auto it = kMap.constFind(op.type);
    if (it != kMap.constEnd()) {
        sandboxOpHintLabel_->setText(QString(mlTr("→ 沙盒 %1 对应真实 OpCode: %2")).arg(opName).arg(it.value()));
    }
}

/// 撤销上一步操作，回滚栈状态。
void VmStackSandboxPanel::onUndo() {
    if (history_.empty() || snapshots_.empty()) {
        setFeedback(mlTr("没有可撤销的指令。"), true);
        return;
    }
    history_.pop_back();
    stack_ = std::move(snapshots_.back());
    snapshots_.pop_back();
    outputs_ = std::move(outputSnapshots_.back());
    outputSnapshots_.pop_back();
    halted_ = false;
    refreshStackView();
    refreshHistoryView();
    refreshOutputView();
    setFeedback(mlTr("已撤销最后一条指令。"));
}

/// 重置当前关卡到初始状态。
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
    sandboxOpHintLabel_->setText(mlTr("💡 点击左侧指令按钮执行，下方会显示对应的真实 OpCode。"));
}

/// 校验当前栈状态是否匹配关卡目标答案。
void VmStackSandboxPanel::onCheck() {
    QString diag;
    if (checkAnswer(&diag)) {
        setFeedback(mlTr("✅ 通关！指令序列正确，输出符合预期。"));
        const auto& levels = SandboxLibrary::levels();
        if (currentLevelIndex_ >= 0 && currentLevelIndex_ < static_cast<int>(levels.size())) {
            std::string id = "level-" + std::to_string(levels[currentLevelIndex_].level);
            LearnerProgressStore::instance().markLevelStars(id, 3);
            saveLearnerProgressWithFeedback(this); // P2-UX fix: 失败时弹 toast 避免静默丢失
            QString levelId = QString("level-%1").arg(levels[currentLevelIndex_].level);
            emit activityCompleted(levelId);
        }
        refreshLevelChips();
    } else {
        setFeedback(diag, true);
    }
}

// ============================================================
// P1-3 fix (F14): 用真实 StackVM 验证
// ============================================================

namespace {
std::string levelToMiniLangSource(int levelIdx) {
    switch (levelIdx + 1) {
    case 1:
        return "print(1 + 2);\n";
    case 2:
        return "print(1 + 2 * 3);\n";
    case 3:
        return "print((1 + 2) * 3);\n";
    case 4:
        return "print(\"hello\");\n";
    default:
        return "";
    }
}
} // namespace

/// 用真实 StackVM 编译运行关卡代码，与学员操作结果对照验证。
void VmStackSandboxPanel::onVerifyWithRealStackVM() {
    const auto& levels = SandboxLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= static_cast<int>(levels.size())) {
        setFeedback(mlTr("未加载任何关卡。"), true);
        return;
    }
    const auto& lv = levels[currentLevelIndex_];

    std::string source = levelToMiniLangSource(currentLevelIndex_);
    if (source.empty()) {
        setFeedback(mlTr("💡 自由模式无固定源码，跳过真实 VM 验证。"), true);
        return;
    }

    std::ostringstream os;
    os << "🛠 " << mlTr("用真实 StackVM 验证").toStdString() << "\n";
    os << "📦 " << mlTr("关卡").toStdString() << " " << lv.level << "：" << lv.goal << "\n";
    os << "📝 " << mlTr("MiniLang 源码").toStdString() << ":\n    " << source;

    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        std::string errs;
        for (const auto& d : lexer.getDiagnostics().all()) {
            if (d.isError())
                errs += d.format() + "\n";
        }
        setFeedback(QString::fromUtf8(("❌ " + mlTr("词法错误：\n").toStdString() + errs).c_str()), true);
        return;
    }

    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        std::string errs;
        for (const auto& d : parser.getDiagnostics().all()) {
            if (d.isError())
                errs += d.format() + "\n";
        }
        setFeedback(QString::fromUtf8(("❌ " + mlTr("解析错误：\n").toStdString() + errs).c_str()), true);
        return;
    }

    Compiler compiler;
    auto compileResult = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        std::string errs;
        for (const auto& d : compiler.getDiagnostics().all()) {
            if (d.isError())
                errs += d.format() + "\n";
        }
        setFeedback(QString::fromUtf8(("❌ " + mlTr("编译错误：\n").toStdString() + errs).c_str()), true);
        return;
    }

    VM vm;
    std::string actualOutput;
    vm.setOutputCallback([&](const std::string& s) { actualOutput += s; });
    VMResult vmResult = vm.execute(compileResult);

    os << "--- " << mlTr("真实 StackVM 执行结果").toStdString() << " ---\n";
    if (vmResult == VMResult::VM_OK) {
        os << "✅ " << mlTr("VM 正常结束").toStdString() << "\n";
    } else if (vmResult == VMResult::VM_RUNTIME_ERROR) {
        os << "⚠ " << mlTr("VM 运行时错误").toStdString() << "：" << vm.getLastError() << "\n";
    } else {
        os << "⚠ " << mlTr("VM 栈溢出").toStdString() << "\n";
    }
    os << "📤 " << mlTr("实际输出").toStdString() << ": \"" << actualOutput << "\"\n";
    os << "🎯 " << mlTr("预期输出").toStdString() << ": \"" << lv.expectedOutput << "\"\n";

    if (!lv.expectedOutput.empty()) {
        if (actualOutput == lv.expectedOutput) {
            os << "\n✅ " << mlTr("完美匹配：沙盒指令与真实 StackVM 输出一致！").toStdString();
        } else {
            os << "\n❌ " << mlTr("不一致：实际输出与预期不同。").toStdString();
        }
    } else {
        os << "\n💡 " << mlTr("自由模式：可观察实际输出。").toStdString();
    }

    feedbackLabel_->setText(QString::fromUtf8(os.str().c_str()));
    // 启用「查看追踪」按钮，让用户可跳转到追踪页透明观察执行过程
    gotoTraceBtn_->setEnabled(true);
}

// ============================================================
// 答案检查
// ============================================================

/// 比较当前栈与标准答案；diag 返回差异诊断信息。
bool VmStackSandboxPanel::checkAnswer(QString* diag) const {
    const auto& levels = SandboxLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= static_cast<int>(levels.size())) {
        if (diag)
            *diag = mlTr("未加载任何关卡。");
        return false;
    }
    const auto& lv = levels[currentLevelIndex_];

    if (lv.expectedSequence.empty()) {
        if (diag)
            *diag = mlTr("🎉 自由模式：可以自由探索，无需检查答案。");
        return true;
    }

    if (history_.size() != lv.expectedSequence.size()) {
        if (diag) {
            *diag = QString(mlTr("❌ 指令数量不对。期望 %1 条，实际 %2 条。"))
                        .arg(static_cast<int>(lv.expectedSequence.size()))
                        .arg(static_cast<int>(history_.size()));
        }
        return false;
    }
    for (size_t i = 0; i < history_.size(); ++i) {
        if (history_[i].type != lv.expectedSequence[i].type || history_[i].operand != lv.expectedSequence[i].operand) {
            if (diag) {
                *diag = QString(mlTr("❌ 第 %1 条指令不对。期望 %2，实际 %3。"))
                            .arg(static_cast<int>(i + 1))
                            .arg(opDisplayText(lv.expectedSequence[i]))
                            .arg(opDisplayText(history_[i]));
            }
            return false;
        }
    }

    if (!lv.expectedOutput.empty()) {
        std::string joined;
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (i > 0)
                joined += "\n";
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
        if (outputs_.empty()) {
            if (diag)
                *diag = mlTr("💡 你已经算出了结果，记得用 PRINT 输出它！");
            return false;
        }
    }

    return true;
}

// ============================================================
// UI 刷新（沙盒页）
// ============================================================

/// 刷新栈内容的可视化展示。
void VmStackSandboxPanel::refreshStackView() {
    stackList_->clear();
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
    // 注：移除 fadeInWidget —— QListWidget 刷新无需动画，
    // QGraphicsOpacityEffect 在频繁单步执行时会导致 opacity 卡 0 内容空白。
}

/// 刷新操作历史记录列表。
void VmStackSandboxPanel::refreshHistoryView() {
    historyList_->clear();
    for (size_t i = 0; i < history_.size(); ++i) {
        QString text = QString("%1. %2").arg(static_cast<int>(i + 1)).arg(opDisplayText(history_[i]));
        historyList_->addItem(text);
    }
    if (history_.empty()) {
        historyList_->addItem(mlTr("（尚未执行指令）"));
    }
}

/// 刷新指令执行输出/日志区。
void VmStackSandboxPanel::refreshOutputView() {
    std::string joined;
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (i > 0)
            joined += "\n";
        joined += outputs_[i];
    }
    outputEdit_->setPlainText(QString::fromStdString(joined));
}

/// 设置反馈文本；isError 控制以错误样式（红）还是成功样式展示。
void VmStackSandboxPanel::setFeedback(const QString& text, bool isError) {
    feedbackLabel_->setText(text);
    if (isError) {
        feedbackLabel_->setStyleSheet("padding: 4px; background: #fff0f0; border-left: 3px solid #c03030;");
    } else {
        feedbackLabel_->setStyleSheet("padding: 4px; background: #f0fff0; border-left: 3px solid #30a030;");
    }
}

/// 返回某指令在操作历史中的可读展示文本。
QString VmStackSandboxPanel::opDisplayText(const SandboxOp& op) const {
    QString name = QString::fromUtf8(sandboxOpTypeName(op.type));
    if (op.type == SandboxOpType::PUSH_INT || op.type == SandboxOpType::PUSH_STRING) {
        return QString("%1 %2").arg(name).arg(QString::fromStdString(op.operand));
    }
    return name;
}

/// 返回某指令在按钮上的展示文本。
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

// ============================================================
// 关卡芯片栏状态刷新
// ============================================================

/// 刷新关卡选择芯片（含完成/锁定状态标记）。
void VmStackSandboxPanel::refreshLevelChips() {
    const auto& levels = SandboxLibrary::levels();
    auto& store = LearnerProgressStore::instance();
    for (int i = 0; i < levelChips_.size(); ++i) {
        QPushButton* chip = levelChips_[i];
        if (!chip)
            continue;

        bool isCurrent = (i == currentLevelIndex_);
        bool isCompleted = false;
        if (i < (int)levels.size()) {
            std::string id = "level-" + std::to_string(levels[i].level);
            isCompleted = store.getLevelStars(id) >= 0;
        }

        chip->setProperty("locked", false);
        chip->setProperty("current", isCurrent);
        chip->setEnabled(true);

        QString text;
        if (isCurrent && isCompleted) {
            text = QString::fromUtf8("%1 \xE2\xAD\x90").arg(i + 1);
        } else {
            text = QString::number(i + 1);
        }
        chip->setText(text);

        if (i < (int)levels.size()) {
            const auto& lv = levels[i];
            QString tip = QString::fromUtf8("第 %1 关").arg(lv.level);
            if (!lv.goal.empty()) {
                tip += "\n" + mlTr("目标：") + QString::fromStdString(lv.goal);
            }
            if (!lv.teachingPoint.empty()) {
                tip += "\n" + mlTr("教学点：") + QString::fromStdString(lv.teachingPoint);
            }
            if (isCompleted) {
                tip += "\n" + mlTr("已完成");
            }
            chip->setToolTip(tip);
        }

        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
}

// ============================================================
// 真实字节码追踪页：编译并加载
// ============================================================

/// 切换到字节码跟踪子页。
void VmStackSandboxPanel::switchToTracePage() {
    subPageBar_->setCurrentIndex(1);
}

/// 从当前关卡提取/编译出字节码，供跟踪页单步执行。
void VmStackSandboxPanel::loadBytecodeFromCurrentLevel() {
    bytecodeList_->clear();
    bytecodeOffsets_.clear();
    currentIp_ = 0;

    // AUDIT-P2 fix: 错误路径（空源码/controller null/编译失败）未重置按钮与 VM 状态，
    // 导致跨关卡切换时残留旧 VM 状态，用户点击单步会执行旧关卡指令。
    // 在函数入口统一重置按钮禁用，成功路径末尾再按 bytecodeList_->count() 启用。
    stepBtn_->setEnabled(false);
    runAllBtn_->setEnabled(false);
    resetTraceBtn_->setEnabled(false);

    if (!controller_) {
        traceStatusLabel_->setText(mlTr("状态：未绑定 controller"));
        return;
    }
    // 重置 VM 状态，清空旧关卡的执行残留（成功路径会再次重置以准备新字节码）
    controller_->vmReset();

    std::string source = levelToMiniLangSource(currentLevelIndex_);
    if (source.empty()) {
        traceStatusLabel_->setText(mlTr("状态：自由模式无固定源码，请在主编辑器编写代码后点击「编译并加载」"));
        // 自由模式尝试从主编辑器加载（通过 runFrontendPipeline）
        return;
    }

    // 通过 IdeController 管线编译当前关卡源码
    // runLexer + runParser + runCompiler 三步，runCompiler 内部同步 VmStepper 编译结果
    if (!controller_->runLexer(source)) {
        traceStatusLabel_->setText(mlTr("状态：词法分析失败"));
        return;
    }
    if (!controller_->runParser()) {
        traceStatusLabel_->setText(mlTr("状态：语法分析失败"));
        return;
    }
    if (!controller_->runCompiler()) {
        traceStatusLabel_->setText(mlTr("状态：编译失败"));
        return;
    }

    // 重置 VM 状态，准备单步执行
    controller_->vmReset();

    // 填充字节码列表
    const QFont monoFont = GuiTextUtils::monospaceFont(10);
    const bool useReg = controller_->isVmRegisterMode();

    if (useReg) {
        // RegisterVM 模式：从 Compiler.getLastRegisterResult() 读取寄存器字节码
        const auto& regResult = controller_->compiler().getLastRegisterResult();
        const auto& chunk = regResult.mainChunk;
        size_t offset = 0;
        while (offset < chunk.code.size()) {
            RegOp op = static_cast<RegOp>(chunk.code[offset]);
            std::string line = "[" + std::to_string(offset) + "] " + regOpName(op);
            // 简要显示操作数字节
            size_t instrSize = chunk.instructionSizeAt(offset);
            for (size_t i = 1; i < instrSize && offset + i < chunk.code.size(); ++i) {
                line += " " + std::to_string(chunk.code[offset + i]);
            }
            auto* item = new QListWidgetItem(QString::fromUtf8(line.c_str()));
            item->setFont(monoFont);
            // tooltip：从 BytecodeTraceLibrary 查找 OpCode 文档（RegOp 名称与 OpCode 不同，可能查不到）
            item->setToolTip(QString::fromUtf8(line.c_str()));
            bytecodeList_->addItem(item);
            bytecodeOffsets_.push_back(static_cast<int>(offset));
            offset += instrSize;
        }
    } else {
        // StackVM 模式：从 lastCompileResult().mainChunk 读取栈式字节码
        const auto& compileResult = controller_->lastCompileResult();
        const auto& chunk = compileResult.mainChunk;
        size_t offset = 0;
        while (offset < chunk.code.size()) {
            size_t instrStart = offset;
            std::string line = chunk.disassembleInstruction(offset);
            auto* item = new QListWidgetItem(QString::fromUtf8(line.c_str()));
            item->setFont(monoFont);
            // tooltip：从 BytecodeTraceLibrary 查找 OpCode 文档
            OpCode op = static_cast<OpCode>(chunk.code[instrStart]);
            const char* opName = opCodeName(op);
            for (const auto& doc : BytecodeTraceLibrary::opCodeDocs()) {
                if (doc.opCodeName == opName) {
                    QString tip = QString::fromUtf8(doc.opCodeName.c_str()) + "\n" +
                                  QString::fromUtf8(doc.semantics.c_str()) + "\n" + mlTr("栈效果：") +
                                  QString::fromUtf8(doc.stackEffect.c_str()) + "\n" + mlTr("样例：") +
                                  QString::fromUtf8(doc.exampleCode.c_str());
                    item->setToolTip(tip);
                    break;
                }
            }
            bytecodeList_->addItem(item);
            bytecodeOffsets_.push_back(static_cast<int>(instrStart));
            // offset 已被 disassembleInstruction 推进
        }
    }

    traceStatusLabel_->setText(QString(mlTr("状态：已加载 %1 条指令，等待单步执行")).arg(bytecodeList_->count()));
    stepBtn_->setEnabled(bytecodeList_->count() > 0);
    runAllBtn_->setEnabled(bytecodeList_->count() > 0);
    resetTraceBtn_->setEnabled(true);
    traceOutputEdit_->clear();

    refreshTraceViews();
}

/// 「编译并加载」按钮：编译关卡代码并载入跟踪视图。
// AUDIT-P2 fix: 补齐 traceRunning_ 守卫——onTraceStep/onTraceReset 都有守卫，
// 但 onCompileAndLoad 遗漏。loadBytecodeFromCurrentLevel 内部调用 vmReset()，
// 会在追踪运行期间重置 VM，导致 IP 错乱、字节码列表与执行状态不一致。
void VmStackSandboxPanel::onCompileAndLoad() {
    if (traceRunning_)
        return;
    loadBytecodeFromCurrentLevel();
}

// ============================================================
// 真实字节码追踪页：单步执行
// ============================================================

/// 跟踪页「单步」：执行下一条字节码并刷新寄存器/栈视图。
void VmStackSandboxPanel::onTraceStep() {
    if (!controller_) {
        traceStatusLabel_->setText(mlTr("状态：未绑定 controller"));
        return;
    }
    // AUDIT-P2 fix: onTraceRunAll 运行期间禁止单步（processEvents 重入守卫）
    if (traceRunning_)
        return;
    if (!controller_->isVmInitialized()) {
        traceStatusLabel_->setText(mlTr("状态：VM 未初始化，请先「编译并加载」"));
        return;
    }

    auto result = controller_->vmStep();
    switch (result) {
    case IdeController::VmStepResult::OK:
        traceStatusLabel_->setText(mlTr("状态：单步执行中"));
        break;
    case IdeController::VmStepResult::FINISHED:
        traceStatusLabel_->setText(mlTr("状态：执行完毕 ✅"));
        break;
    case IdeController::VmStepResult::ERROR:
        traceStatusLabel_->setText(
            QString(mlTr("状态：运行时错误 — %1")).arg(QString::fromStdString(controller_->getVmLastError())));
        break;
    case IdeController::VmStepResult::NOT_READY:
        traceStatusLabel_->setText(mlTr("状态：VM 未就绪，请先「编译并加载」"));
        break;
    default:
        break;
    }
    refreshTraceViews();
}

/// 跟踪页「全速运行」：连续执行到结束。
void VmStackSandboxPanel::onTraceRunAll() {
    if (!controller_) {
        traceStatusLabel_->setText(mlTr("状态：未绑定 controller"));
        return;
    }
    // AUDIT-P2 fix: 重入守卫——processEvents 期间用户可再次点击"运行到底"，
    // 导致两个循环并发修改 VM 状态。同时禁用相关按钮防止 step/reset 重入。
    if (traceRunning_)
        return;
    if (!controller_->isVmInitialized()) {
        traceStatusLabel_->setText(mlTr("状态：VM 未初始化，请先「编译并加载」"));
        return;
    }

    traceRunning_ = true;
    if (stepBtn_)
        stepBtn_->setEnabled(false);
    if (resetTraceBtn_)
        resetTraceBtn_->setEnabled(false);
    if (runAllBtn_)
        runAllBtn_->setEnabled(false);

    // 循环单步直到结束/错误，设上限防止死循环
    constexpr int kMaxSteps = 100000;
    // AUDIT-P2 fix: 每 1000 步调用 processEvents 让 UI 重绘，避免长循环
    // （如 for 10000 次 print）UI 完全冻结显示"未响应"。
    constexpr int kYieldInterval = 1000;
    int stepCount = 0;
    auto result = IdeController::VmStepResult::OK;
    while (stepCount < kMaxSteps) {
        result = controller_->vmStep();
        if (result != IdeController::VmStepResult::OK)
            break;
        ++stepCount;
        if ((stepCount % kYieldInterval) == 0) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }

    traceRunning_ = false;
    if (stepBtn_)
        stepBtn_->setEnabled(true);
    if (resetTraceBtn_)
        resetTraceBtn_->setEnabled(true);
    if (runAllBtn_)
        runAllBtn_->setEnabled(true);

    switch (result) {
    case IdeController::VmStepResult::FINISHED:
        traceStatusLabel_->setText(QString(mlTr("状态：运行完毕 ✅（共 %1 步）")).arg(stepCount));
        break;
    case IdeController::VmStepResult::ERROR:
        traceStatusLabel_->setText(
            QString(mlTr("状态：运行时错误 — %1")).arg(QString::fromStdString(controller_->getVmLastError())));
        break;
    default:
        traceStatusLabel_->setText(
            QString(mlTr("状态：运行中止（%1 步，result=%2）")).arg(stepCount).arg(static_cast<int>(result)));
        break;
    }
    refreshTraceViews();
}

/// 跟踪页「重置」：清空执行状态回到指令开头。
void VmStackSandboxPanel::onTraceReset() {
    if (!controller_) {
        traceStatusLabel_->setText(mlTr("状态：未绑定 controller"));
        return;
    }
    // AUDIT-P2 fix: onTraceRunAll 运行期间禁止重置（processEvents 重入守卫）
    if (traceRunning_)
        return;
    controller_->vmReset();
    currentIp_ = 0;
    traceOutputEdit_->clear();
    traceStatusLabel_->setText(mlTr("状态：已重置，可重新单步执行"));
    refreshTraceViews();
}

// ============================================================
// 真实字节码追踪页：视图刷新
// ============================================================

/// 刷新跟踪页的字节码、栈与寄存器全部视图。
void VmStackSandboxPanel::refreshTraceViews() {
    // BUG-96 fix (P3): 重入守卫——onTraceRunAll 主循环每 1000 步调用 processEvents，
    // 期间 VM 状态变更监听器或其它事件回调可能再次进入 refreshTraceViews，
    // 与外层刷新竞争修改 bytecodeList_/traceStackView_/registerTable_ 视图状态。
    // 检测到正在刷新则直接返回（下一次外层刷新会带上最新状态）。
    if (traceRefreshing_)
        return;
    traceRefreshing_ = true;
    if (!controller_) {
        traceRefreshing_ = false;
        return;
    }

    // 1. 更新 IP 指示器
    if (controller_->isVmInitialized()) {
        currentIp_ = static_cast<int>(controller_->getVmCurrentIP());
        QString opName = QString::fromStdString(controller_->getVmCurrentOpCodeName());
        ipIndicator_->setText(QString(mlTr("IP: %1  OpCode: %2")).arg(currentIp_).arg(opName));
    } else {
        ipIndicator_->setText(mlTr("IP: -"));
    }

    // 2. 字节码列表高亮：已执行=绿色浅背景，当前 IP=蓝色边框
    //    找到 currentIp_ 对应的行号（bytecodeOffsets_ 是有序的）
    int currentRow = -1;
    for (size_t i = 0; i < bytecodeOffsets_.size(); ++i) {
        if (bytecodeOffsets_[i] == currentIp_) {
            currentRow = static_cast<int>(i);
            break;
        }
        if (bytecodeOffsets_[i] > currentIp_) {
            currentRow = static_cast<int>(i) - 1;
            break;
        }
    }
    // 如果 currentIp_ 超过最后一个 offset，说明所有指令已执行
    if (currentRow == -1 && !bytecodeOffsets_.empty() && currentIp_ >= bytecodeOffsets_.back()) {
        currentRow = static_cast<int>(bytecodeOffsets_.size()) - 1;
    }

    const QColor kExecutedBg(220, 240, 220); // 浅绿
    const QColor kCurrentBg(200, 220, 255);  // 浅蓝
    for (int i = 0; i < bytecodeList_->count(); ++i) {
        QListWidgetItem* item = bytecodeList_->item(i);
        if (i == currentRow) {
            // 当前指令：蓝色边框（用浅蓝背景 + 粗体模拟）
            item->setBackground(kCurrentBg);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        } else if (i < currentRow) {
            // 已执行：绿色浅背景
            item->setBackground(kExecutedBg);
            QFont f = item->font();
            f.setBold(false);
            item->setFont(f);
        } else {
            // 未执行：默认背景
            item->setBackground(Qt::white);
            QFont f = item->font();
            f.setBold(false);
            item->setFont(f);
        }
    }
    // 滚动到当前行
    if (currentRow >= 0 && currentRow < bytecodeList_->count()) {
        bytecodeList_->scrollToItem(bytecodeList_->item(currentRow), QAbstractItemView::PositionAtCenter);
    }

    // 3. 刷新操作数栈视图（栈顶在上方）
    traceStackView_->clear();
    if (controller_->isVmInitialized()) {
        auto stack = controller_->getVmStack();
        bool isReg = controller_->isVmRegisterMode();
        // AUDIT-P2 fix: 根据 isReg 动态设置 GroupBox 标题，避免 RegisterVM 模式下
        // 标题"操作数栈"与内容"R0=..."不符导致教学误导。
        if (auto* gb = qobject_cast<QGroupBox*>(traceStackView_->parentWidget())) {
            gb->setTitle(isReg ? mlTr("寄存器窗口（R0-R31）") : mlTr("操作数栈（执行前）"));
        }
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            QString text;
            if (isReg) {
                // RegisterVM 模式：getVmStack 返回寄存器窗口，下标从 0 开始
                // rbegin 是高编号寄存器，rend 是 R0
                int regIdx = static_cast<int>(stack.size() - 1 - std::distance(stack.rbegin(), it));
                text = QString("R%1 = %2").arg(regIdx).arg(QString::fromStdString(it->toString()));
            } else {
                if (it == stack.rbegin()) {
                    text = QString::fromStdString(it->toString()) + mlTr("  ← 栈顶");
                } else {
                    text = QString::fromStdString(it->toString());
                }
            }
            traceStackView_->addItem(text);
        }
        if (stack.empty()) {
            // AUDIT-P2 fix: RegisterVM 模式下显示"无寄存器值"而非"栈为空"，
            // 与当前模式语义一致。
            traceStackView_->addItem(isReg ? mlTr("（无寄存器值）") : mlTr("（栈为空）"));
        }
    } else {
        traceStackView_->addItem(mlTr("（VM 未初始化）"));
    }

    // 4. 刷新寄存器表
    refreshRegisterTable();

    // 5. 刷新输出区（从 controller 读取输出——IdeController 暂未暴露统一输出接口，
    //    这里用 VM 的 lastError 作为兜底显示，正常输出由 vmStep 内部回调累积）
    //    注：IdeController 的 outputReady 信号由 ide.cpp 主窗口接收并显示在底部输出面板，
    //    此处仅显示追踪页自身的状态信息。

    // BUG-96 fix (P3): 复位重入守卫
    traceRefreshing_ = false;
}

/// 刷新寄存器表（如跟踪的是寄存器机模型时）。
void VmStackSandboxPanel::refreshRegisterTable() {
    if (!controller_) {
        registerTable_->setRowCount(0);
        return;
    }

    if (!controller_->isVmRegisterMode()) {
        // StackVM 模式：显示提示
        registerTable_->setRowCount(1);
        auto* nameItem = new QTableWidgetItem(mlTr("（栈式 VM）"));
        auto* valItem = new QTableWidgetItem(mlTr("无寄存器，操作数在栈顶"));
        valItem->setToolTip(mlTr("栈式 VM 使用操作数栈而非寄存器，切换到 RegisterVM 引擎可查看寄存器状态"));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEnabled);
        valItem->setFlags(valItem->flags() & ~Qt::ItemIsEnabled);
        registerTable_->setItem(0, 0, nameItem);
        registerTable_->setItem(0, 1, valItem);
        return;
    }

    // RegisterVM 模式：显示 32 个虚拟寄存器
    constexpr int kRegCount = 32;
    registerTable_->setRowCount(kRegCount);
    auto regs = controller_->getVmStack(); // RegisterVM 模式返回寄存器窗口
    for (int i = 0; i < kRegCount; ++i) {
        auto* nameItem = new QTableWidgetItem(QString("R%1").arg(i));
        nameItem->setTextAlignment(Qt::AlignCenter);
        QString valStr;
        if (i < static_cast<int>(regs.size())) {
            valStr = QString::fromStdString(regs[i].toString());
        } else {
            valStr = QStringLiteral("-");
        }
        auto* valItem = new QTableWidgetItem(valStr);
        valItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        registerTable_->setItem(i, 0, nameItem);
        registerTable_->setItem(i, 1, valItem);
    }
}
