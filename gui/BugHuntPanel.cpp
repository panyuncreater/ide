#include "gui/BugHuntPanel.h"
#include "gui/GuidedTour.h"
#include "gui/I18n.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QButtonGroup>
#include <QLineEdit>
#include <sstream>
#include <chrono>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（StrongBodyLabel）

// 注：BugHuntVariantLibrary::variants() 实现已拆分到独立文件
// gui/BugHuntVariantLibrary.cpp 中，避免测试目标编译时引入 IdeController.h
// 依赖（IdeController 依赖 app/ 下多个文件）。
// ============================================================

BugHuntPanel::BugHuntPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);
    mainLayout->setSpacing(2);

    // 顶部按钮栏（紧凑化：按钮固定高度 28px，减少垂直占用）
    auto* btnBar = new QHBoxLayout;
    btnBar->setSpacing(4);
    runBtn_ = new PrimaryPushButton(mlTr("运行验证"), this);
    tripleVerifyBtn_ = new QPushButton(mlTr("三后端对比"), this);
    hintBtn_ = new QPushButton(mlTr("下一提示"), this);
    answerBtn_ = new QPushButton(mlTr("查看答案"), this);
    loadBtn_ = new QPushButton(mlTr("加载到主编辑器"), this);
    variantBtn_ = new QPushButton(mlTr("变体模式"), this);
    variantBtn_->setCheckable(true);
    statusLabel_ = new StrongBodyLabel(mlTr("请选择题目"), this);
    variantStatusLabel_ = new QLabel(QString::fromUtf8(""), this);
    for (auto* btn : {runBtn_, tripleVerifyBtn_, hintBtn_, answerBtn_, loadBtn_, variantBtn_})
        btn->setFixedHeight(28);
    btnBar->addWidget(runBtn_);
    btnBar->addWidget(tripleVerifyBtn_);
    btnBar->addWidget(hintBtn_);
    btnBar->addWidget(answerBtn_);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(variantStatusLabel_);
    btnBar->addWidget(statusLabel_);
    btnBar->addWidget(variantBtn_);
    mainLayout->addLayout(btnBar);

    // 难度筛选栏（紧凑化：间距 4px，按钮固定高度 26px）
    auto* diffBar = new QHBoxLayout;
    diffBar->setSpacing(4);
    diffBar->addWidget(new QLabel(mlTr("难度："), this));
    diffAllBtn_          = new QPushButton(mlTr("全部"), this);
    diffBeginnerBtn_     = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门"), this);
    diffIntermediateBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶"), this);
    diffExpertBtn_       = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家"), this);
    for (auto* btn : {diffAllBtn_, diffBeginnerBtn_, diffIntermediateBtn_, diffExpertBtn_}) {
        btn->setCheckable(true);
        btn->setFixedHeight(26);
    }
    auto* diffGroup = new QButtonGroup(this);
    diffGroup->setExclusive(true);
    diffGroup->addButton(diffAllBtn_,          -1);
    diffGroup->addButton(diffBeginnerBtn_,      0);
    diffGroup->addButton(diffIntermediateBtn_,  1);
    diffGroup->addButton(diffExpertBtn_,        2);
    diffBar->addWidget(diffAllBtn_);
    diffBar->addWidget(diffBeginnerBtn_);
    diffBar->addWidget(diffIntermediateBtn_);
    diffBar->addWidget(diffExpertBtn_);
    diffBar->addStretch();
    mainLayout->addLayout(diffBar);
    // 默认选中"入门级"
    diffBeginnerBtn_->setChecked(true);
    currentDifficultyFilter_ = 0;
    connect(diffGroup, &QButtonGroup::idClicked,
            this, &BugHuntPanel::onDifficultyChanged);

    // issue 5: 芯片栏替代大目录列表（参考 TokenPuzzlePanel 关卡芯片）
    // 水平排列的 Bug 芯片 + 右侧搜索框，紧凑一行，释放垂直空间给描述与代码区
    auto* chipRow = new QHBoxLayout;
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(6);
    // 芯片栏容器（含 Bug 芯片 + 变体芯片，互斥显示）
    chipBar_ = new QWidget(this);
    chipBar_->setObjectName("bugHuntChipBar");
    auto* chipLayout = new QHBoxLayout(chipBar_);
    chipLayout->setContentsMargins(0, 0, 0, 0);
    chipLayout->setSpacing(6);
    chipLayout->setAlignment(Qt::AlignLeft);
    // Bug 芯片与变体芯片在 populateBugChips/populateVariantChips 中动态创建
    chipRow->addWidget(chipBar_, 1);
    // 搜索框（右侧）
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(mlTr("搜索题目..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setFixedHeight(26);
    searchEdit_->setFixedWidth(180);
    chipRow->addWidget(searchEdit_);
    mainLayout->addLayout(chipRow);

    // issue 5: 主体改为 2 栏 splitter（描述 | 代码+输出），移除原题目列表与变体列表两栏
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(2);

    // === 调试流程指引（问题 4） ===
    // 教学目标：引导用户走「预测 → 观察 → 修复 → 验证」四步调试闭环，
    // 而非仅展示问题代码和错误原因。每步完成后打勾，增强可操作性。
    debugStepsLabel_ = new QLabel(this);
    debugStepsLabel_->setStyleSheet(
        "QLabel { background: #EEE8D5; border: 1px solid #93A1A1;"
        "  border-radius: 4px; padding: 6px 8px; font-size: 12px; }");
    debugStepsLabel_->setWordWrap(true);
    debugStepsLabel_->setTextFormat(Qt::RichText);
    rightLayout->addWidget(debugStepsLabel_);

    // 预测输入行
    auto* predLayout = new QHBoxLayout;
    predLayout->setSpacing(4);
    predLayout->addWidget(new QLabel(mlTr("我的预测："), this));
    predictionEdit_ = new QLineEdit(this);
    predictionEdit_->setPlaceholderText(mlTr("先预测程序输出或行为，再运行验证..."));
    predictionEdit_->setFixedHeight(26);
    submitPredictionBtn_ = new QPushButton(mlTr("提交预测"), this);
    submitPredictionBtn_->setFixedHeight(26);
    predLayout->addWidget(predictionEdit_, 1);
    predLayout->addWidget(submitPredictionBtn_);
    rightLayout->addLayout(predLayout);

    codeEditor_ = new QTextEdit(this);
    outputEdit_ = new QTextEdit(this);
    outputEdit_->setReadOnly(true);
    rightLayout->addWidget(new QLabel(mlTr("代码：")), 0);
    rightLayout->addWidget(codeEditor_, 2);
    rightLayout->addWidget(new QLabel(mlTr("运行结果：")), 0);
    rightLayout->addWidget(outputEdit_, 1);

    mainSplitter_ = splitter;
    splitter->addWidget(descBrowser_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 4);
    splitter->setSizes({420, 580});
    mainLayout->addWidget(splitter, 1);

    // issue 5: Solarized 风格 QSS（Bug 芯片按钮，三态：默认/当前/隐藏）
    // 动态属性 [current='true'] 在 refreshBugChips() 中通过 setProperty + polish() 触发
    setStyleSheet(QString::fromUtf8(
        "QPushButton#bugChip { background: #EEE8D5; border: 1px solid #93A1A1; "
        "border-radius: 4px; font-size: 11px; padding: 2px 8px; }"
        "QPushButton#bugChip:hover { border-color: #268BD2; background: #E5F3FB; }"
        "QPushButton#bugChip[current='true'] { background: #268BD2; color: white; "
        "border-color: #1E6FA3; font-weight: bold; }"
        "QPushButton#bugChip[diff='0'] { border-color: #859900; }"
        "QPushButton#bugChip[diff='1'] { border-color: #B58900; }"
        "QPushButton#bugChip[diff='2'] { border-color: #CB4B16; }"
    ));

    // 搜索框 textChanged → 过滤芯片可见性（按题目标题/id 小写包含匹配）
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        QString filter = text.toLower();
        const auto& items = BugHuntLibrary::items();
        for (int i = 0; i < bugChips_.size() && i < (int)items.size(); ++i) {
            if (!bugChips_[i]) continue;
            QString haystack = QString::fromUtf8(items[i].title.c_str()).toLower()
                             + " " + QString::fromUtf8(items[i].id.c_str()).toLower()
                             + " " + QString::fromUtf8(items[i].category.c_str()).toLower();
            bool match = filter.isEmpty() || haystack.contains(filter);
            bugChips_[i]->setVisible(match);
        }
    });

    populateBugChips();
    populateVariantChips();
    // 默认显示 Bug 芯片，隐藏变体芯片
    for (auto* c : variantChips_) c->hide();

    connect(runBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onRunVerify);
    connect(tripleVerifyBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onTripleVerify);
    connect(hintBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowHint);
    connect(answerBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowAnswer);
    connect(variantBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onToggleVariantMode);
    // 调试流程指引（问题 4）
    connect(submitPredictionBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onSubmitPrediction);
    connect(codeEditor_, &QTextEdit::textChanged,
            this, &BugHuntPanel::onCodeModified);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (variantMode_) {
            if (currentVariantIndex_ < 0) return;
            const auto& variants = BugHuntVariantLibrary::variants();
            emit loadSampleRequested(QString::fromUtf8(variants[currentVariantIndex_].sourceCode.c_str()));
            statusLabel_->setText(mlTr("已请求加载变体到主编辑器"));
            return;
        }
        if (currentItemIndex_ < 0) return;
        const auto& items = BugHuntLibrary::items();
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sourceCode.c_str()));
        statusLabel_->setText(mlTr("已请求加载到主编辑器"));
    });
}

void BugHuntPanel::populateBugChips() {
    // issue 5: 构建 Bug 芯片栏（每个芯片对应 BugHuntLibrary::items() 一项）
    // 清理旧芯片
    for (auto* c : bugChips_) {
        if (c) c->deleteLater();
    }
    bugChips_.clear();

    const auto& items = BugHuntLibrary::items();
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        auto* chip = new QPushButton(chipBar_);
        chip->setObjectName("bugChip");
        chip->setFixedHeight(28);
        // 芯片文本：编号 + 严重性（紧凑）
        QString text = QString::fromUtf8("%1\n%2")
            .arg(QString::fromUtf8(it.id.c_str()))
            .arg(QString::fromUtf8(it.severity.c_str()));
        chip->setText(text);
        chip->setProperty("diff", static_cast<int>(it.difficulty));
        // tooltip 显示完整标题与类别
        QString tip = QString::fromUtf8("%1 — %2\n类别: %3")
            .arg(QString::fromUtf8(it.id.c_str()))
            .arg(QString::fromUtf8(it.title.c_str()))
            .arg(QString::fromUtf8(it.category.c_str()));
        chip->setToolTip(tip);
        chip->setCursor(Qt::PointingHandCursor);
        const int itemIdx = i;
        connect(chip, &QPushButton::clicked, this, [this, itemIdx]() {
            onItemSelected(itemIdx);
        });
        if (auto* lay = qobject_cast<QHBoxLayout*>(chipBar_->layout())) {
            lay->addWidget(chip);
        }
        bugChips_.append(chip);
    }
    refreshBugChips();
}

void BugHuntPanel::refreshBugChips() {
    // issue 5: 刷新芯片状态（current 高亮 + 难度筛选可见性）
    const auto& items = BugHuntLibrary::items();
    int firstVisible = -1;
    for (int i = 0; i < bugChips_.size() && i < (int)items.size(); ++i) {
        QPushButton* chip = bugChips_[i];
        if (!chip) continue;
        int diffId = static_cast<int>(items[i].difficulty);
        bool visibleByDiff = (currentDifficultyFilter_ == -1 || diffId == currentDifficultyFilter_);
        chip->setVisible(visibleByDiff);
        chip->setProperty("current", (i == currentItemIndex_));
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
        if (visibleByDiff && firstVisible < 0) firstVisible = i;
    }
    // 若当前题目被筛选隐藏，自动选中第一个可见芯片
    if (currentItemIndex_ < 0 && firstVisible >= 0) {
        onItemSelected(firstVisible);
    } else if (currentItemIndex_ >= 0) {
        bool curVisible = (currentItemIndex_ < bugChips_.size()) && bugChips_[currentItemIndex_]
                          && bugChips_[currentItemIndex_]->isVisible();
        if (!curVisible && firstVisible >= 0) {
            onItemSelected(firstVisible);
        }
    }
}

void BugHuntPanel::populateVariantChips() {
    // issue 5: 构建变体芯片栏（每个芯片对应 BugHuntVariantLibrary::variants() 一项）
    for (auto* c : variantChips_) {
        if (c) c->deleteLater();
    }
    variantChips_.clear();

    const auto& variants = BugHuntVariantLibrary::variants();
    for (int i = 0; i < (int)variants.size(); ++i) {
        const auto& v = variants[i];
        auto* chip = new QPushButton(chipBar_);
        chip->setObjectName("bugChip");  // 复用 bugChip QSS
        chip->setFixedHeight(28);
        QString text = QString::fromUtf8("V%1\n%2")
            .arg(i + 1)
            .arg(QString::fromUtf8(v.parentId.c_str()));
        chip->setText(text);
        chip->setToolTip(QString::fromUtf8(v.title.c_str()));
        chip->setCursor(Qt::PointingHandCursor);
        const int varIdx = i;
        connect(chip, &QPushButton::clicked, this, [this, varIdx]() {
            onVariantSelected(varIdx);
        });
        if (auto* lay = qobject_cast<QHBoxLayout*>(chipBar_->layout())) {
            lay->addWidget(chip);
        }
        variantChips_.append(chip);
    }
    refreshVariantChips();
}

void BugHuntPanel::refreshVariantChips() {
    for (int i = 0; i < variantChips_.size(); ++i) {
        QPushButton* chip = variantChips_[i];
        if (!chip) continue;
        chip->setProperty("current", (i == currentVariantIndex_));
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
}

void BugHuntPanel::onItemSelected(int itemIndex) {
    // issue 5: 芯片点击 → 直接传入 items() 索引
    if (itemIndex < 0) {
        currentItemIndex_ = -1;
        return;
    }
    currentItemIndex_ = itemIndex;
    hintLevel_ = 0;
    showCurrentItem();
    refreshBugChips();
}

void BugHuntPanel::showCurrentItem() {
    const auto& items = BugHuntLibrary::items();
    if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& it = items[currentItemIndex_];
    // 难度标签文本（功能 9）
    QString diffLabel;
    if (it.difficulty == BugHuntDifficulty::BEGINNER) {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门级");  // 🟢
    } else if (it.difficulty == BugHuntDifficulty::INTERMEDIATE) {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶级");  // 🟡
    } else {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家级");  // 🔴
    }
    QString html = QString(
        "<html><body>"
        "<h2>[%1] %2</h2>"
        "<p><b>难度:</b> %3 &nbsp;&nbsp; <b>严重性:</b> %4</p>"
        "<p><b>类别:</b> %5</p>"
        "<p><b>背景:</b></p><p>%6</p>"
        "<p><b>期望行为:</b></p><p>%7</p>"
        "<p><b>Bug 行为:</b></p><p>%8</p>"
        "</body></html>")
        .arg(QString::fromUtf8(it.id.c_str()))
        .arg(QString::fromUtf8(it.title.c_str()))
        .arg(diffLabel)
        .arg(QString::fromUtf8(it.severity.c_str()))
        .arg(QString::fromUtf8(it.category.c_str()))
        .arg(QString::fromUtf8(it.background.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(it.expectedBehavior.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(it.buggyBehavior.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(it.sourceCode.c_str()));
    outputEdit_->clear();
    statusLabel_->setText(mlTr("当前题目：%1").arg(
        QString::fromUtf8(it.id.c_str())));
    // 重置调试流程状态
    originalCode_ = QString::fromUtf8(it.sourceCode.c_str());
    stepPredicted_ = false;
    stepObserved_ = false;
    stepAttemptedFix_ = false;
    stepVerified_ = false;
    if (predictionEdit_) predictionEdit_->clear();
    refreshDebugSteps();
}

void BugHuntPanel::onDifficultyChanged(int id) {
    // id: -1 = 全部，0 = BEGINNER，1 = INTERMEDIATE，2 = EXPERT
    currentDifficultyFilter_ = id;
    refreshBugChips();  // issue 5: 刷新芯片可见性
    // 状态栏给出筛选反馈
    QString label;
    if (id == -1)      label = mlTr("难度筛选：全部");
    else if (id == 0)  label = mlTr("难度筛选：入门级");
    else if (id == 1)  label = mlTr("难度筛选：进阶级");
    else               label = mlTr("难度筛选：专家级");
    statusLabel_->setText(label);
}

void BugHuntPanel::onRunVerify() {
    if (currentItemIndex_ < 0) {
        statusLabel_->setText(QString::fromUtf8("请先选择题目"));
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    std::ostringstream out;
    // 运行 Interpreter 路径（最宽容，便于教学观察行为）
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            const auto& diags = parser.getDiagnostics();
            out << "[Parser 错误]\n" << diags.summary() << "\n";
            outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
            return;
        }
        Interpreter interp;
        interp.setOutputCallback([&out](const std::string& s) {
            out << s << "\n";
        });
        interp.execute(*ast);
        out << "\n[Interpreter 路径运行完成]";
    } catch (const std::exception& e) {
        out << "\n[Interpreter 异常] " << e.what();
    }

    // 也可选运行 StackVM 路径观察差异
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!parser.hasErrors()) {
            Compiler compiler;
            auto result = compiler.compile(*ast);
            if (!compiler.getDiagnostics().hasErrors()) {
                VM vm;
                vm.setOutputCallback([&out](const std::string& s) {
                    out << s << "\n";
                });
                auto vmres = vm.execute(result);
                out << "\n[StackVM 路径: " << (int)vmres << "]";
            }
        }
    } catch (const std::exception& e) {
        out << "\n[StackVM 异常] " << e.what();
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("验证完成 — 对比期望/Bug 行为"));
    // 调试流程：标记「观察」步骤完成
    if (!stepObserved_) {
        stepObserved_ = true;
        refreshDebugSteps();
    }
}

void BugHuntPanel::onShowHint() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    if (hintLevel_ >= (int)items[currentItemIndex_].hints.size()) {
        statusLabel_->setText(QString::fromUtf8("已无更多提示"));
        return;
    }
    QString hint = QString::fromUtf8(items[currentItemIndex_].hints[hintLevel_].c_str());
    hintLevel_++;
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 提示 %1 ===\n%2").arg(hintLevel_).arg(hint);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("已显示提示 %1/%2").arg(hintLevel_).arg(
        (int)items[currentItemIndex_].hints.size()));
}

void BugHuntPanel::onShowAnswer() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 答案 ===\n%1").arg(
        QString::fromUtf8(items[currentItemIndex_].explanation.c_str()));
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("答案已显示"));
}

// ============================================================
// 调试流程指引（问题 4）：预测 → 观察 → 修复 → 验证 闭环
// ============================================================

void BugHuntPanel::refreshDebugSteps() {
    if (!debugStepsLabel_) return;
    // 4 步检查清单，完成打 ✅ 未完成打 ⬜
    auto check = [](bool done) -> QString {
        return done ? QString::fromUtf8("\xe2\x9c\x85")
                    : QString::fromUtf8("\xe2\xac\x9c");
    };
    QString html = QString::fromUtf8(
        "<b>调试流程：</b> %1 预测 → %2 观察 → %3 修复 → %4 验证"
        "<br><span style='color:#586E75;font-size:11px;'>"
        "教学目标：通过「预测-观察-修复-验证」四步闭环培养系统化调试能力。"
        "先在下方输入框写下你对代码行为的预测，运行后对比差异，"
        "修改代码尝试修复，最后用「三后端对比」验证修复效果。"
        "</span>")
        .arg(check(stepPredicted_))
        .arg(check(stepObserved_))
        .arg(check(stepAttemptedFix_))
        .arg(check(stepVerified_));
    debugStepsLabel_->setText(html);
}

void BugHuntPanel::onSubmitPrediction() {
    if (currentItemIndex_ < 0 && !variantMode_) {
        statusLabel_->setText(QString::fromUtf8("请先选择题目"));
        return;
    }
    QString pred = predictionEdit_->text().trimmed();
    if (pred.isEmpty()) {
        statusLabel_->setText(QString::fromUtf8("请输入你的预测后再提交"));
        return;
    }
    stepPredicted_ = true;
    refreshDebugSteps();
    // 将预测记录到输出区
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 我的预测 ===\n%1").arg(pred);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("预测已记录 — 现在点击「运行验证」观察实际行为"));
}

void BugHuntPanel::onCodeModified() {
    // 检测用户是否修改了代码（与原始代码不同）
    if (originalCode_.isEmpty()) return;
    if (codeEditor_->toPlainText() != originalCode_) {
        if (!stepAttemptedFix_) {
            stepAttemptedFix_ = true;
            refreshDebugSteps();
        }
    }
}

// ============================================================
// 三后端对比验证 + 变体模式实现
// ============================================================

namespace {
// 单后端运行结果（供 onTripleVerify 使用）
struct BackendRunResult {
    std::string output;        // 标准输出
    std::string exceptionName; // 异常名（空 = 无异常）
    long long micros = 0;      // 耗时（微秒）
};

// Interpreter 路径：树遍历解释器
BackendRunResult runInterpreter(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Interpreter interp;
            interp.setOutputCallback([&r](const std::string& s) {
                r.output += s;
                r.output += "\n";
            });
            interp.execute(*ast);
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}

// StackVM 路径：栈式字节码虚拟机
BackendRunResult runStackVM(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Compiler compiler;
            auto result = compiler.compile(*ast);
            if (compiler.getDiagnostics().hasErrors()) {
                r.exceptionName = "CompileError: " + compiler.getLastError();
            } else {
                VM vm;
                vm.setOutputCallback([&r](const std::string& s) {
                    r.output += s;
                    r.output += "\n";
                });
                vm.execute(result);
                if (vm.hasError()) {
                    r.exceptionName = vm.getLastError();
                }
            }
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}

// RegisterVM 路径：寄存器式虚拟机
BackendRunResult runRegisterVM(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Compiler compiler;
            compiler.setUseRegisterVM(true);
            compiler.compile(*ast);
            if (compiler.getDiagnostics().hasErrors()) {
                r.exceptionName = "CompileError: " + compiler.getLastError();
            } else {
                RegisterVM vm;
                vm.setOutputCallback([&r](const std::string& s) {
                    r.output += s;
                    r.output += "\n";
                });
                vm.execute(compiler.getLastRegisterResult());
                if (vm.hasError()) {
                    r.exceptionName = vm.getLastError();
                }
            }
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}
} // namespace

void BugHuntPanel::onTripleVerify() {
    // 变体模式下验证当前变体；标准模式下验证当前题目
    if (variantMode_) {
        if (currentVariantIndex_ < 0) {
            statusLabel_->setText(QString::fromUtf8("请先选择变体"));
            return;
        }
    } else {
        if (currentItemIndex_ < 0) {
            statusLabel_->setText(QString::fromUtf8("请先选择题目"));
            return;
        }
    }

    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    // 三条路径独立执行
    auto ir = runInterpreter(source);
    auto sv = runStackVM(source);
    auto rv = runRegisterVM(source);

    // 一致性判定
    bool outputConsistent = (ir.output == sv.output) && (sv.output == rv.output);
    bool exceptionConsistent =
        !ir.exceptionName.empty() &&
        !sv.exceptionName.empty() &&
        !rv.exceptionName.empty() &&
        ir.exceptionName == sv.exceptionName &&
        sv.exceptionName == rv.exceptionName;

    std::ostringstream out;
    out << "============= 三后端对比验证 =============\n";
    out << "[Interpreter] 输出: " << (ir.output.empty() ? "（无）" : ir.output);
    out << "              异常: " << (ir.exceptionName.empty() ? "无" : ir.exceptionName) << "\n";
    out << "              耗时: " << ir.micros << " μs\n\n";
    out << "[StackVM]     输出: " << (sv.output.empty() ? "（无）" : sv.output);
    out << "              异常: " << (sv.exceptionName.empty() ? "无" : sv.exceptionName) << "\n";
    out << "              耗时: " << sv.micros << " μs\n\n";
    out << "[RegisterVM]  输出: " << (rv.output.empty() ? "（无）" : rv.output);
    out << "              异常: " << (rv.exceptionName.empty() ? "无" : rv.exceptionName) << "\n";
    out << "              耗时: " << rv.micros << " μs\n\n";
    out << "============= 一致性结论 =============\n";
    out << "三后端输出 [" << (outputConsistent ? "一致" : "不一致") << "]\n";
    out << "异常行为 [" << (exceptionConsistent ? "一致" : "不一致") << "]\n";

    // 教学注解
    out << "教学注解: ";
    if (outputConsistent && ir.exceptionName.empty() && sv.exceptionName.empty() && rv.exceptionName.empty()) {
        out << "三后端输出完全一致且无异常——该代码路径在三套引擎上语义等价。";
    } else if (outputConsistent && exceptionConsistent) {
        out << "三后端均抛出相同异常且输出一致——异常路径语义等价，关注异常本身是否为预期行为。";
    } else if (!outputConsistent) {
        out << "三后端输出不一致——存在语义差异，需逐行对比 Interpreter/StackVM/RegisterVM 实现。";
        if (!ir.exceptionName.empty() && sv.exceptionName.empty() && rv.exceptionName.empty()) {
            out << "（Interpreter 抛异常但 VM 路径未抛——可能 Interpreter 更严格的检查）";
        } else if (ir.exceptionName.empty() && (!sv.exceptionName.empty() || !rv.exceptionName.empty())) {
            out << "（VM 路径抛异常但 Interpreter 未抛——可能 VM 编译/执行有缺陷）";
        } else if (!sv.exceptionName.empty() && rv.exceptionName.empty() && ir.exceptionName.empty()) {
            out << "（仅 StackVM 抛异常——栈式 VM 路径存在 bug）";
        } else if (!rv.exceptionName.empty() && sv.exceptionName.empty() && ir.exceptionName.empty()) {
            out << "（仅 RegisterVM 抛异常——寄存器式 VM 路径存在 bug）";
        }
    } else if (!exceptionConsistent) {
        out << "异常行为不一致——三后端对异常的处理存在差异（异常类型/消息/是否抛出）。";
    }
    out << "\n";

    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("三后端对比验证完成"));
    // 调试流程：标记「验证」步骤完成
    if (!stepVerified_) {
        stepVerified_ = true;
        refreshDebugSteps();
    }
    if (variantMode_) {
        variantStatusLabel_->setText(QString::fromUtf8("变体验证完成 — 输出%1")
            .arg(outputConsistent ? QString::fromUtf8("一致") : QString::fromUtf8("不一致")));
    }

    // P0-B fix: 三后端输出一致且均无异常 → 视为"狩猎"成功，发射 challengeSolved。
    // 学员修复 Bug 后三后端应产生一致输出且无异常——这与原 Bug 代码三后端不一致
    // 或抛异常的状态形成对照，构成判分依据。仅标准模式（非变体）发射，因变体本身
    // 是探索性挑战而非可机检修复。
    if (!variantMode_ && currentItemIndex_ >= 0) {
        const auto& items = BugHuntLibrary::items();
        if (currentItemIndex_ < (int)items.size()) {
            bool allClean = ir.exceptionName.empty() &&
                            sv.exceptionName.empty() &&
                            rv.exceptionName.empty();
            if (outputConsistent && allClean) {
                int diffInt = static_cast<int>(items[currentItemIndex_].difficulty);
                emit challengeSolved(diffInt);
                out << "\n✅ 狩猎成功！本档 Bug 狩猎活动已标记完成。\n";
                // 重新刷新输出（追加了成功提示）
                outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
            }
        }
    }
}

void BugHuntPanel::onToggleVariantMode() {
    variantMode_ = variantBtn_->isChecked();
    if (variantMode_) {
        // issue 5: 切换到变体模式 — 隐藏 Bug 芯片与难度筛选，显示变体芯片
        for (auto* c : bugChips_) if (c) c->hide();
        for (auto* c : variantChips_) if (c) c->show();
        hintBtn_->hide();
        answerBtn_->hide();
        diffAllBtn_->hide();
        diffBeginnerBtn_->hide();
        diffIntermediateBtn_->hide();
        diffExpertBtn_->hide();
        variantBtn_->setText(mlTr("返回标准"));
        variantStatusLabel_->setText(mlTr("变体模式 — 选择变体后点击三后端对比"));
        if (variantChips_.size() > 0 && currentVariantIndex_ < 0) {
            onVariantSelected(0);
        } else if (currentVariantIndex_ >= 0) {
            showCurrentVariant();
            refreshVariantChips();
        }
    } else {
        // issue 5: 切换回标准模式 — 隐藏变体芯片，恢复 Bug 芯片与难度筛选
        for (auto* c : variantChips_) if (c) c->hide();
        refreshBugChips();  // 恢复 Bug 芯片可见性（受难度筛选控制）
        hintBtn_->show();
        answerBtn_->show();
        diffAllBtn_->show();
        diffBeginnerBtn_->show();
        diffIntermediateBtn_->show();
        diffExpertBtn_->show();
        variantBtn_->setText(mlTr("变体模式"));
        variantStatusLabel_->clear();
        if (currentItemIndex_ >= 0) {
            showCurrentItem();
        }
    }
    statusLabel_->setText(variantMode_
        ? mlTr("变体模式")
        : mlTr("标准模式"));
}

void BugHuntPanel::onVariantSelected(int variantIndex) {
    // issue 5: 芯片点击 → 直接传入 variants() 索引
    currentVariantIndex_ = variantIndex;
    showCurrentVariant();
    refreshVariantChips();
}

void BugHuntPanel::showCurrentVariant() {
    const auto& variants = BugHuntVariantLibrary::variants();
    if (currentVariantIndex_ < 0 || currentVariantIndex_ >= (int)variants.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& v = variants[currentVariantIndex_];
    QString html = QString(
        "<html><body>"
        "<h2>%1</h2>"
        "<p><b>父题:</b> %2</p>"
        "<p><b>挑战目标:</b></p><p>%3</p>"
        "<p><b>说明:</b></p><p>%4</p>"
        "<p><b>期望行为:</b></p><p>%5</p>"
        "<p><b>提示:</b></p><p>%6</p>"
        "</body></html>")
        .arg(QString::fromUtf8(v.title.c_str()))
        .arg(QString::fromUtf8(v.parentId.c_str()))
        .arg(QString::fromUtf8(v.challengeGoal.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(v.description.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(v.expectedBehavior.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(v.hint.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(v.sourceCode.c_str()));
    outputEdit_->clear();
    variantStatusLabel_->setText(QString::fromUtf8("当前变体：%1").arg(
        QString::fromUtf8(v.id.c_str())));
    // 重置调试流程状态
    originalCode_ = QString::fromUtf8(v.sourceCode.c_str());
    stepPredicted_ = false;
    stepObserved_ = false;
    stepAttemptedFix_ = false;
    stepVerified_ = false;
    if (predictionEdit_) predictionEdit_->clear();
    refreshDebugSteps();
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* BugHuntPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(chipBar_,
                  QString::fromUtf8("题目芯片栏"),
                  QString::fromUtf8("这里水平排列所有 Bug 题目芯片，点击编号即可选择题目。可搭配右侧搜索框按关键字筛选。芯片边框颜色对应难度（绿/黄/红）。"));
    tour->addStep(diffBeginnerBtn_,
                  QString::fromUtf8("难度筛选"),
                  QString::fromUtf8("入门 / 进阶 / 专家三档，建议从「入门」开始，逐步挑战更高难度。"));
    tour->addStep(codeEditor_,
                  QString::fromUtf8("代码编辑器"),
                  QString::fromUtf8("这里展示有 Bug 的代码，可直接修改后点击「运行验证」查看输出。"));
    tour->addStep(tripleVerifyBtn_,
                  QString::fromUtf8("三后端对比"),
                  QString::fromUtf8("同时运行 Interpreter / StackVM / RegisterVM 三条路径，把输出并排对比，快速定位不一致的 Bug。"));
    tour->addStep(hintBtn_,
                  QString::fromUtf8("递进提示"),
                  QString::fromUtf8("卡住时点击获取逐步提示，多次点击可查看更多线索，最后可查看完整答案与根因分析。"));
    return tour;
}
