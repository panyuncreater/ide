// ============================================================
// WelcomeWizard.cpp — 首次启动欢迎向导实现（功能 1）
// ------------------------------------------------------------
// 3 步交互式导览，纯前端模拟（不调用 IdeController / 引擎层）。
// 所有用户可见文本用 mlTr() 包裹。
// ============================================================

#include "gui/WelcomeWizard.h"
#include "gui/I18n.h"
#include "gui/TeachingTheme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QTimer>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QHeaderView>
#include <QFont>
#include <QFrame>
#include <QSpacerItem>
#include <QButtonGroup>

#include "PushButton.h"   // QFluentKit（PushButton / PrimaryPushButton，全局类）
#include "Label.h"        // QFluentKit（TitleLabel / CaptionLabel，全局类）

// ============================================================
// 构造
// ============================================================

WelcomeWizard::WelcomeWizard(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(mlTr("欢迎使用 MiniLang IDE"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedSize(720, 520);

    // 预定义 Token 在 print("Hello!"); 中的字符区间
    // 索引： p(0)r(1)i(2)n(3)t(4) ((5) "(6)H(7)e(8)l(9)l(10)o(11)!(12)"(13) )(14) ;(15)
    tokenSpans_ = {
        {0, 5},     // print
        {6, 14},    // "Hello!"  （含引号，8 字符）
        {15, 16},   // ;
    };

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);

    // ---- 顶部：步骤指示器 ----
    stepIndicator_ = new QLabel(this);
    stepIndicator_->setAlignment(Qt::AlignCenter);
    stepIndicator_->setStyleSheet(QString(
        "font-size: 12px; color: %1; padding: 2px;").arg(
        TeachingTheme::textSecondary().name()));
    root->addWidget(stepIndicator_);

    // ---- 中央：3 步页面 ----
    pages_ = new QStackedWidget(this);
    root->addWidget(pages_, 1);

    buildStep1();
    buildStep2();
    buildStep3();
    buildStep4();

    // ---- 底部：导航按钮 ----
    auto* navLayout = new QHBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->addStretch(1);
    prevBtn_ = new PushButton(mlTr("← 上一步"), this);
    nextBtn_ = new PushButton(mlTr("下一步 →"), this);
    navLayout->addWidget(prevBtn_);
    navLayout->addWidget(nextBtn_);
    root->addLayout(navLayout);

    // Step 1 起步：prevBtn 隐藏，nextBtn 文本为"开始探索"语义由 buildStep1 内部按钮接管
    // 这里统一连接信号；Step1 使用自己的 [开始探索]/[跳过] 按钮，prevBtn_/nextBtn_ 仅用于 Step2/3
    connect(prevBtn_, &PushButton::clicked, this, &WelcomeWizard::onPrevStep);
    connect(nextBtn_, &PushButton::clicked, this, &WelcomeWizard::onNextStep);

    goToStep(0);
}

// ============================================================
// Step 1：欢迎页 + 角色选择
// ============================================================

void WelcomeWizard::buildStep1() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(14);
    layout->setContentsMargins(40, 20, 40, 20);

    auto* titleLabel = new TitleLabel(mlTr("👋 欢迎使用 MiniLang IDE！"), page);
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    auto* subtitleLabel = new CaptionLabel(
        mlTr("让我们用 3 分钟看看你的代码是如何运行的"), page);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setWordWrap(true);
    layout->addWidget(subtitleLabel);

    layout->addSpacing(16);

    auto* chooseLabel = new QLabel(mlTr("—— 选择你的起点 ——"), page);
    chooseLabel->setAlignment(Qt::AlignCenter);
    chooseLabel->setStyleSheet(QString(
        "color: %1; font-size: 11px;").arg(TeachingTheme::textHint().name()));
    layout->addWidget(chooseLabel);

    roleBeginner_ = new QRadioButton(
        mlTr("我完全新手，从头开始"), page);
    roleIntermediate_ = new QRadioButton(
        mlTr("我懂一点编程，想了解编译原理"), page);
    roleExpert_ = new QRadioButton(
        mlTr("我学过编译原理，想看高级功能"), page);
    roleBeginner_->setChecked(true);

    auto* roleGroup = new QButtonGroup(page);
    roleGroup->addButton(roleBeginner_);
    roleGroup->addButton(roleIntermediate_);
    roleGroup->addButton(roleExpert_);

    auto* roleLayout = new QVBoxLayout();
    roleLayout->setSpacing(8);
    roleLayout->addWidget(roleBeginner_);
    roleLayout->addWidget(roleIntermediate_);
    roleLayout->addWidget(roleExpert_);
    layout->addLayout(roleLayout);

    layout->addSpacing(20);

    auto* btnRow = new QHBoxLayout();
    btnRow->setAlignment(Qt::AlignCenter);
    btnRow->setSpacing(12);
    auto* startBtn = new PrimaryPushButton(mlTr("▶ 开始探索"), page);
    startBtn->setMinimumWidth(160);
    startBtn->setMinimumHeight(36);
    auto* skipBtn = new PushButton(mlTr("跳过"), page);
    skipBtn->setMinimumWidth(100);
    skipBtn->setMinimumHeight(36);
    btnRow->addWidget(startBtn);
    btnRow->addWidget(skipBtn);
    layout->addLayout(btnRow);

    connect(startBtn, &PushButton::clicked, this, &WelcomeWizard::onStartExplore);
    connect(skipBtn, &PushButton::clicked, this, &WelcomeWizard::onSkip);

    pages_->addWidget(page);
}

// ============================================================
// Step 2：Token 概念
// ============================================================

void WelcomeWizard::buildStep2() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* hintLabel = new QLabel(
        mlTr("① 你写的代码被切成了 3 个 Token —— 这就是「词法分析」\n"
             "点击右侧任意一行 Token，左侧代码对应字符会高亮。"), page);
    hintLabel->setWordWrap(true);
    hintLabel->setStyleSheet(QString(
        "padding: 6px; background: %1; border-left: 3px solid %2;"
        "font-size: 12px;").arg(
        TeachingTheme::surface().name(),
        TeachingTheme::primary().name()));
    layout->addWidget(hintLabel);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    // 左侧：代码（只读）
    auto* codeBox = new QGroupBox(mlTr("你写的代码"), splitter);
    auto* codeLayout = new QVBoxLayout(codeBox);
    codeLayout->setContentsMargins(6, 6, 6, 6);
    codeEdit_ = new QTextEdit(codeBox);
    codeEdit_->setReadOnly(true);
    codeEdit_->setPlainText(QStringLiteral("print(\"Hello!\");"));
    QFont monoFont = codeEdit_->font();
    monoFont.setFamily("Consolas");
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(13);
    codeEdit_->setFont(monoFont);
    codeEdit_->setStyleSheet(
        "QTextEdit { background: #1e1e1e; color: #d4d4d4; border: none; }");
    codeLayout->addWidget(codeEdit_);
    splitter->addWidget(codeBox);

    // 右侧：Token 表
    auto* tokenBox = new QGroupBox(mlTr("Token 表（词法分析结果）"), splitter);
    auto* tokenLayout = new QVBoxLayout(tokenBox);
    tokenLayout->setContentsMargins(6, 6, 6, 6);
    tokenTable_ = new QTableWidget(3, 3, tokenBox);
    tokenTable_->setHorizontalHeaderLabels(
        QStringList() << mlTr("Token") << mlTr("类型") << mlTr("说明"));
    tokenTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tokenTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tokenTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tokenTable_->verticalHeader()->setVisible(false);
    tokenTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tokenTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tokenTable_->setSelectionMode(QAbstractItemView::SingleSelection);

    // 3 行 Token 数据
    struct TokenRow { const char* lexeme; const char* type; const char* desc; };
    const TokenRow rows[] = {
        { "print",      "IDENTIFIER", "标识符：函数名" },
        { "\"Hello!\"", "STRING",     "字符串字面量（含引号整体）" },
        { ";",          "SEMICOLON",  "分号：语句结束符" },
    };
    for (int i = 0; i < 3; ++i) {
        tokenTable_->setItem(i, 0, new QTableWidgetItem(QString::fromLatin1(rows[i].lexeme)));
        tokenTable_->setItem(i, 1, new QTableWidgetItem(QString::fromLatin1(rows[i].type)));
        tokenTable_->setItem(i, 2, new QTableWidgetItem(mlTr(rows[i].desc)));
    }
    tokenTable_->setMinimumWidth(280);
    tokenLayout->addWidget(tokenTable_);
    splitter->addWidget(tokenBox);

    splitter->setSizes({360, 320});
    layout->addWidget(splitter, 1);

    // 默认高亮第一个 token
    connect(tokenTable_, &QTableWidget::cellClicked, this, &WelcomeWizard::onTokenRowClicked);

    pages_->addWidget(page);
}

// ============================================================
// Step 3：AST + 字节码 + 运行
// ============================================================

void WelcomeWizard::buildStep3() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    runHint_ = new QLabel(
        mlTr("②③④ 代码 → AST → 字节码 → 执行，得到结果！\n"
             "点击 [▶ 运行] 看虚拟机如何执行这 2 条指令。"), page);
    runHint_->setWordWrap(true);
    runHint_->setStyleSheet(QString(
        "padding: 6px; background: %1; border-left: 3px solid %2;"
        "font-size: 12px;").arg(
        TeachingTheme::surface().name(),
        TeachingTheme::warning().name()));
    layout->addWidget(runHint_);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    // 左侧：AST 简化树
    auto* astBox = new QGroupBox(mlTr("② AST（语法树）"), splitter);
    auto* astLayout = new QVBoxLayout(astBox);
    astLayout->setContentsMargins(6, 6, 6, 6);
    astTree_ = new QTreeWidget(astBox);
    astTree_->setHeaderHidden(true);
    auto* printNode = new QTreeWidgetItem(QStringList() << QStringLiteral("Print"));
    auto* strNode = new QTreeWidgetItem(printNode,
        QStringList() << QStringLiteral("StringLiteral  \"Hello!\""));
    (void)strNode;  // 已挂载到 printNode
    astTree_->addTopLevelItem(printNode);
    astTree_->expandAll();
    astTree_->setStyleSheet(
        "QTreeWidget { font-family: Consolas, monospace; font-size: 12px; }");
    astLayout->addWidget(astTree_);
    splitter->addWidget(astBox);

    // 右侧：字节码
    auto* bcBox = new QGroupBox(mlTr("③ 字节码（虚拟机指令）"), splitter);
    auto* bcLayout = new QVBoxLayout(bcBox);
    bcLayout->setContentsMargins(6, 6, 6, 6);
    bytecodeList_ = new QListWidget(bcBox);
    bytecodeList_->addItem(QStringLiteral("OP_STRING  \"Hello!\""));
    bytecodeList_->addItem(QStringLiteral("OP_PRINT"));
    bytecodeList_->setStyleSheet(
        "QListWidget { font-family: Consolas, monospace; font-size: 12px; }");
    bcLayout->addWidget(bytecodeList_);
    splitter->addWidget(bcBox);

    splitter->setSizes({340, 340});
    layout->addWidget(splitter, 1);

    // 底部：运行按钮 + 输出区
    auto* runRow = new QHBoxLayout();
    runBtn_ = new QPushButton(mlTr("▶ 运行"), page);
    runBtn_->setMinimumWidth(120);
    runBtn_->setMinimumHeight(32);
    // 运行按钮保留 QPushButton，用 TeachingTheme::success() 着色（绿色语义：执行）
    runBtn_->setStyleSheet(QString(
        "QPushButton { background: %1; color: white; border: none;"
        "  border-radius: 5px; font-size: 13px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %3; }"
        "QPushButton:disabled { background: #888; }").arg(
        TeachingTheme::success().name(),
        TeachingTheme::success().darker(112).name(),
        TeachingTheme::success().darker(122).name()));
    connect(runBtn_, &QPushButton::clicked, this, &WelcomeWizard::onRunClicked);
    runRow->addWidget(runBtn_);

    auto* outLabel = new QLabel(mlTr("④ 输出："), page);
    outLabel->setStyleSheet("font-size: 12px;");
    runRow->addWidget(outLabel);

    runOutput_ = new QTextEdit(page);
    runOutput_->setReadOnly(true);
    runOutput_->setMaximumHeight(70);
    runOutput_->setStyleSheet(
        "QTextEdit { background: #101820; color: #90ffd0;"
        "  font-family: Consolas, monospace; font-size: 13px; }");
    runRow->addWidget(runOutput_, 1);
    layout->addLayout(runRow);

    pages_->addWidget(page);
}

// ============================================================
// Step 4：学习路径推荐
// ============================================================
void WelcomeWizard::buildStep4() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(12);

    auto* titleLabel = new QLabel(mlTr("🚀 你的学习路径"), page);
    titleLabel->setStyleSheet(QString(
        "font-size: 18px; font-weight: bold; color: %1;").arg(
        TeachingTheme::primary().name()));
    layout->addWidget(titleLabel);

    auto* introLabel = new QLabel(
        mlTr("导览到此结束！接下来推荐你按以下路径系统学习 MiniLang："),
        page);
    introLabel->setWordWrap(true);
    layout->addWidget(introLabel);

    // 5 阶段学习路径缩略图
    const QStringList stages = {
        mlTr("阶段零 · 首次接触\n欢迎向导 · 代码旅程"),
        mlTr("阶段一 · 编译前端\nToken 拼图 · AST 构建器"),
        mlTr("阶段二 · 执行引擎\nVM 沙盒 · IR 变换 · 内存模型"),
        mlTr("阶段三 · 深入理解\n闭包检查 · 异常流 · 变量检查"),
        mlTr("阶段四 · 实战训练\nBug 狩猎 · 性能剖析 · 实验手册"),
    };
    const QStringList colors = {"#4CAF50", "#FFC107", "#2196F3", "#9C27B0", "#F44336"};

    auto* stagesLayout = new QHBoxLayout;
    stagesLayout->setSpacing(8);
    for (int i = 0; i < stages.size(); ++i) {
        auto* stageCard = new QFrame(page);
        stageCard->setFrameShape(QFrame::StyledPanel);
        stageCard->setStyleSheet(QString(
            "QFrame { background: %1; color: white; border-radius: 6px; "
            "padding: 8px; font-size: 11px; }").arg(colors[i]));
        stageCard->setMinimumHeight(80);
        auto* stageLayout = new QVBoxLayout(stageCard);
        stageLayout->setContentsMargins(6, 6, 6, 6);
        auto* stageLabel = new QLabel(stages[i], stageCard);
        stageLabel->setWordWrap(true);
        stageLabel->setAlignment(Qt::AlignCenter);
        stageLabel->setStyleSheet("color: white; background: transparent;");
        stageLayout->addWidget(stageLabel);
        stagesLayout->addWidget(stageCard);
    }
    layout->addLayout(stagesLayout);

    auto* tipLabel = new QLabel(
        mlTr("💡 点击下方「开始学习」打开学习路径地图，跟踪你的进度。"
             "左侧栏「学习」图标可随时打开学习中心。"),
        page);
    tipLabel->setWordWrap(true);
    tipLabel->setStyleSheet(QString(
        "color: %1; font-size: 12px; padding: 8px;").arg(
        TeachingTheme::textSecondary().name()));
    layout->addWidget(tipLabel);

    layout->addStretch(1);

    // [开始学习 ✓] 按钮（QFluentKit 主按钮，主题色由 Theme::themeColor 自动驱动）
    startLearningBtn_ = new PrimaryPushButton(mlTr("开始学习 ✓"), page);
    startLearningBtn_->setFixedWidth(200);
    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch(1);
    btnLayout->addWidget(startLearningBtn_);
    layout->addLayout(btnLayout);

    connect(startLearningBtn_, &PushButton::clicked, this, [this]() {
        completed_ = true;
        emit learningPathRequested();
        accept();
    });

    pages_->addWidget(page);
}

// ============================================================
// 导航逻辑
// ============================================================

void WelcomeWizard::goToStep(int index) {
    if (index < 0 || index >= pages_->count()) return;
    pages_->setCurrentIndex(index);
    updateStepIndicator();
    updateNavButtons();

    // 进入 Step 2 时默认高亮第一个 token
    if (index == 1) {
        tokenTable_->selectRow(0);
        highlightCodeRange(tokenSpans_[0].start, tokenSpans_[0].end);
    }
}

void WelcomeWizard::updateStepIndicator() {
    int idx = pages_->currentIndex();
    stepIndicator_->setText(
        mlTr("步骤 %1 / %3").arg(idx + 1).arg(pages_->count()));
}

void WelcomeWizard::updateNavButtons() {
    int idx = pages_->currentIndex();
    // Step 1 和 Step 4：自带按钮，隐藏通用导航按钮
    if (idx == 0 || idx == 3) {
        prevBtn_->setVisible(false);
        nextBtn_->setVisible(false);
        return;
    }
    prevBtn_->setVisible(true);
    nextBtn_->setVisible(true);

    if (idx == 1) {
        // Step 2
        prevBtn_->setText(mlTr("← 上一步"));
        nextBtn_->setText(mlTr("下一步 →"));
    } else if (idx == 2) {
        // Step 3
        prevBtn_->setText(mlTr("← 上一步"));
        nextBtn_->setText(mlTr("下一步 →"));
    }
}

void WelcomeWizard::onStartExplore() {
    // 选"我学过编译原理" → 直接跳过导览，关闭对话框
    if (roleExpert_->isChecked()) {
        completed_ = true;
        accept();
        return;
    }
    // 其他角色 → 进入 Step 2
    goToStep(1);
}

void WelcomeWizard::onSkip() {
    completed_ = true;
    reject();  // 跳过也视为完成（已看过提示），用 reject 区分"主动跳过"
}

void WelcomeWizard::onNextStep() {
    int idx = pages_->currentIndex();
    if (idx == 1) {
        goToStep(2);
    } else if (idx == 2) {
        // Step 3 → Step 4（学习路径推荐）
        goToStep(3);
    }
}

void WelcomeWizard::onPrevStep() {
    int idx = pages_->currentIndex();
    if (idx > 0) goToStep(idx - 1);
}

void WelcomeWizard::onFinish() {
    completed_ = true;
    accept();
}

// ============================================================
// Step 2：Token 行点击 → 高亮源码
// ============================================================

void WelcomeWizard::onTokenRowClicked(int row) {
    if (row < 0 || row >= tokenSpans_.size()) return;
    highlightCodeRange(tokenSpans_[row].start, tokenSpans_[row].end);
}

void WelcomeWizard::highlightCodeRange(int start, int end) {
    if (!codeEdit_) return;
    QTextCursor cursor(codeEdit_->document());
    QList<QTextEdit::ExtraSelection> selections;

    QTextEdit::ExtraSelection sel;
    sel.cursor = cursor;
    sel.cursor.setPosition(start);
    sel.cursor.setPosition(end, QTextCursor::KeepAnchor);
    sel.format.setBackground(TeachingTheme::primary());
    sel.format.setForeground(QColor("#ffffff"));
    selections.append(sel);

    codeEdit_->setExtraSelections(selections);
}

// ============================================================
// Step 3：运行按钮 → 模拟执行
// ============================================================

void WelcomeWizard::onRunClicked() {
    if (runExecuted_) {
        // 重复点击：清空后重新演示
        runOutput_->clear();
        runExecuted_ = false;
    }

    runBtn_->setEnabled(false);
    runBtn_->setText(mlTr("运行中…"));
    runOutput_->setPlainText(mlTr("(虚拟机正在执行 OP_STRING …)"));

    // 高亮第一条指令
    if (bytecodeList_->count() > 0) {
        bytecodeList_->setCurrentRow(0);
        bytecodeList_->setStyleSheet(QString(
            "QListWidget { font-family: Consolas, monospace; font-size: 12px; }"
            "QListWidget::item:selected { background: %1; color: white; }").arg(
            TeachingTheme::primary().name()));
    }

    // 600ms 后切换到第二条指令 + 输出
    QTimer::singleShot(600, this, [this]() {
        if (bytecodeList_->count() > 1) {
            bytecodeList_->setCurrentRow(1);
        }
        runOutput_->setPlainText(mlTr("(虚拟机正在执行 OP_PRINT …)"));

        // 再 600ms 后显示最终结果
        QTimer::singleShot(600, this, [this]() {
            runOutput_->setPlainText(QStringLiteral("Hello!"));
            runBtn_->setEnabled(true);
            runBtn_->setText(mlTr("▶ 再次运行"));
            runExecuted_ = true;
        });
    });
}
