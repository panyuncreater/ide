// ============================================================
// IRTransformPanel.cpp — IR 变换过程可视化面板实现（第二波 P1-1）
// ============================================================
//
// P3-20 拆分（2026-07-25）：
// 静态教学场景库已迁移到独立编译单元，本文件仅保留面板 UI 与交互逻辑：
//   - IRTransformLibrary（lowering/optimization 场景）→ gui/IRTransformLibrary.cpp
//   - IROptReplayLibrary（逐步优化回放场景）        → gui/IROptReplayLibrary.cpp
// 与 BugHuntLibrary.cpp / MemoryModelLibrary.cpp 拆分模式一致。

#include "gui/IRTransformPanel.h"
#include "app/IdeController.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "gui/GuiTextUtils.h" // R75: monospaceFont() 跨机器字体回退链
#include "gui/I18n.h"         // mlTr() 国际化
#include "gui/PanelAnimator.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QApplication>
#include <QCheckBox> // 拓展二期：预测模式开关
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QSpinBox> // 拓展二期：预测指令数输入
#include <QSplitter>
#include <QVBoxLayout>
#include <sstream>

#include "Label.h"      // QFluentKit（CaptionLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）
#include "Theme.h"      // QFluentKit（onThemeModeChanged 信号）
#include "gui/TeachingTheme.h"

// --- Clickable IR/bytecode HTML helper ---
// Converts plain text where lines may end with "; line N" into HTML
// with those lines wrapped in <a href="#LINE_N"> anchors.
namespace {
/// 将纯文本 IR 转换为可点击 HTML：识别行尾 "; line N" 注释，把该行
/// 包裹为 <a href="#LINE_N"> 锚点，便于在浏览器中点击跳转到源码对应行。
static QString irTextToClickableHtml(const QString& plainText) {
    QStringList lines = plainText.split("\n");
    QString html;
    html += "<pre style=\"font-family:'Cascadia Code','Cascadia Mono','Consolas','JetBrains Mono','Source Code "
            "Pro','Menlo','DejaVu Sans Mono','Courier New',monospace;\">";
    static const QRegularExpression lineCommentRe(";\\s*line\\s+(\\d+)\\s*$");
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        auto m = lineCommentRe.match(line);
        if (m.hasMatch()) {
            QString lineNum = m.captured(1);
            QString escaped = line.toHtmlEscaped();
            html +=
                "<a href=\"#LINE_" + lineNum + "\" style=\"color:inherit;text-decoration:none;\">" + escaped + "</a>";
        } else {
            html += line.toHtmlEscaped();
        }
        if (i < lines.size() - 1)
            html += "\n";
    }
    html += "</pre>";
    return html;
}
} // anonymous namespace
// --- End clickable IR HTML helper ---

// ============================================================
// IRTransformPanel 实现
// ============================================================

/// 构造面板：组装顶部 4 个子页切换按钮（lowering / 优化对比 / 当前源码 IR /
/// 逐步回放）与 QStackedWidget，构建各子页并连接切换信号、主题刷新与列表填充。
IRTransformPanel::IRTransformPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLoweringBtn_ = new QPushButton(tr("AST → IR lowering"), this);
    pageOptimizeBtn_ = new QPushButton(tr("优化 pass 对比"), this);
    pageCurrentBtn_ = new QPushButton(tr("当前源码 IR"), this);
    pageReplayBtn_ = new QPushButton(tr("逐步优化回放"), this);
    pageLoweringBtn_->setCheckable(true);
    pageOptimizeBtn_->setCheckable(true);
    pageCurrentBtn_->setCheckable(true);
    pageReplayBtn_->setCheckable(true);
    pageBar->addWidget(pageLoweringBtn_);
    pageBar->addWidget(pageOptimizeBtn_);
    pageBar->addWidget(pageCurrentBtn_);
    pageBar->addWidget(pageReplayBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* p1 = new QWidget(this);
    auto* p2 = new QWidget(this);
    auto* p3 = new QWidget(this);
    auto* p4 = new QWidget(this);
    buildLoweringPage(p1);
    buildOptimizePage(p2);
    buildCurrentPage(p3);
    buildReplayPage(p4);
    stack_->addWidget(p1);
    stack_->addWidget(p2);
    stack_->addWidget(p3);
    stack_->addWidget(p4);

    pageLoweringBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageLoweringBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageLoweringBtn_->setChecked(true);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageOptimizeBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(true);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageCurrentBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(true);
        pageReplayBtn_->setChecked(false);
        populateCurrentIR();
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageReplayBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(3);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    populateLoweringList();
    populateOptList();
    populateReplayList();

    // 主题切换时刷新当前页 HTML（populateLoweringDetail 中 <pre> 背景使用
    // TeachingTheme::surface()，需重新渲染以跟随新主题）。
    // receiver=this 保证生命周期安全，析构自动断开。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (!stack_)
            return;
        switch (stack_->currentIndex()) {
        case 0: // AST → IR lowering 页（HTML 含主题色 <pre> 背景）
            if (loweringList_)
                populateLoweringDetail(loweringList_->currentRow());
            break;
        case 1: // 优化 pass 对比页（setPlainText，无主题依赖，仍刷新以防未来扩展）
            if (optList_)
                populateOptDetail(optList_->currentRow());
            break;
        case 2: // 当前源码 IR 页（setPlainText，无主题依赖）
            populateCurrentIR();
            break;
        case 3: // 逐步优化回放页（setPlainText，无主题依赖）
            if (replayList_ && replayStepsList_)
                populateReplayStep(replayList_->currentRow(), replayStepsList_->currentRow());
            break;
        default:
            break;
        }
    });
}

/// 构建「AST → IR lowering」子页：左侧产生式/节点列表 + 右侧说明浏览器，
/// 选中项变化时通过 populateLoweringDetail 渲染 HTML。
void IRTransformPanel::buildLoweringPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    loweringList_ = new QListWidget(host);
    loweringDetail_ = new QTextBrowser(host);
    splitter->addWidget(loweringList_);
    splitter->addWidget(loweringDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter);

    connect(loweringList_, &QListWidget::currentRowChanged, this, &IRTransformPanel::populateLoweringDetail);
}

/// 构建「优化 pass 对比」子页：左侧优化 pass 列表 + 右侧优化前/后 IR 浏览器
/// 及摘要标签，选中项变化时通过 populateOptDetail 显示指令数变化。
void IRTransformPanel::buildOptimizePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    optList_ = new QListWidget(host);
    optSummary_ = new QLabel(tr("请选择优化 pass"), host);

    auto* beforeWrap = new QWidget(host);
    auto* beforeLayout = new QVBoxLayout(beforeWrap);
    beforeLayout->setContentsMargins(0, 0, 0, 0);
    beforeLayout->addWidget(new QLabel(tr("优化前 IR：")));
    optBefore_ = new QTextBrowser(host);
    beforeLayout->addWidget(optBefore_, 1);

    auto* afterWrap = new QWidget(host);
    auto* afterLayout = new QVBoxLayout(afterWrap);
    afterLayout->setContentsMargins(0, 0, 0, 0);
    afterLayout->addWidget(new QLabel(tr("优化后 IR：")));
    optAfter_ = new QTextBrowser(host);
    afterLayout->addWidget(optAfter_, 1);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(optList_);
    splitter->addWidget(beforeWrap);
    splitter->addWidget(afterWrap);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({150, 250, 250});

    // 拓展二期·教学：IR 优化预测练习模式控件行。
    // 开启后隐藏优化后 IR 与指令数变化，学生先预测优化后指令数
    // 再提交揭示（答案数据直接取自 IROptimizationExample.instrAfter）。
    auto* predictBar = new QHBoxLayout();
    predictCheck_ = new QCheckBox(tr("🎯 预测模式（先猜优化后指令数，再揭示答案）"), host);
    predictBar->addWidget(predictCheck_);
    predictBar->addWidget(new QLabel(tr("预测优化后指令数:"), host));
    predictSpin_ = new QSpinBox(host);
    predictSpin_->setRange(0, 999);
    predictSpin_->setEnabled(false);
    predictBar->addWidget(predictSpin_);
    predictSubmitBtn_ = new QPushButton(tr("提交预测"), host);
    predictSubmitBtn_->setEnabled(false);
    predictBar->addWidget(predictSubmitBtn_);
    predictResultLabel_ = new QLabel(host);
    predictResultLabel_->setWordWrap(true);
    predictBar->addWidget(predictResultLabel_, 1);
    layout->addLayout(predictBar);

    layout->addWidget(optSummary_);
    layout->addWidget(splitter, 1);

    connect(optList_, &QListWidget::currentRowChanged, this, &IRTransformPanel::populateOptDetail);
    connect(predictCheck_, &QCheckBox::toggled, this, [this](bool on) {
        predictMode_ = on;
        predictSpin_->setEnabled(on);
        predictSubmitBtn_->setEnabled(on);
        predictResultLabel_->clear();
        // 切换模式后重新渲染当前场景（隐藏/揭示优化后 IR）
        populateOptDetail(optList_->currentRow());
    });
    connect(predictSubmitBtn_, &QPushButton::clicked, this, &IRTransformPanel::onPredictSubmit);
}

/// 拓展二期：提交预测——揭示优化后 IR，对照 instrAfter 判定预测。
void IRTransformPanel::onPredictSubmit() {
    int index = optList_->currentRow();
    const auto& items = IRTransformLibrary::optimizationExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& e = items[index];

    // 揭示答案
    optAfter_->setPlainText(QString::fromUtf8(e.irAfter.c_str()));
    optSummary_->setText(tr("%1 — %2 → %3 条指令（减少 %4 条）")
                             .arg(QString::fromUtf8(e.title.c_str()))
                             .arg(e.instrBefore)
                             .arg(e.instrAfter)
                             .arg(e.instrBefore - e.instrAfter));

    int guess = predictSpin_->value();
    if (guess == e.instrAfter) {
        predictResultLabel_->setText(tr("✅ 预测正确！优化后确为 %1 条指令").arg(e.instrAfter));
    } else {
        predictResultLabel_->setText(
            tr("❌ 预测 %1 条，实际 %2 条——对照右侧揭示的优化后 IR 看看哪些指令被消除了").arg(guess).arg(e.instrAfter));
    }
}

/// 构建「当前源码 IR」子页：刷新按钮 + 状态标签 + 可点击 IR 浏览器。
/// 浏览器将 "; line N" 渲染为锚点，点击 emit sourceLineRequested 高亮对应源码行。
void IRTransformPanel::buildCurrentPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout;
    refreshBtn_ = new PrimaryPushButton(tr("重新生成 IR"), host);
    currentStatusLabel_ = new CaptionLabel(tr("尚未生成"), host);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch();
    toolbar->addWidget(currentStatusLabel_);
    layout->addLayout(toolbar);

    currentIrBrowser_ = new QTextBrowser(host);
    currentIrBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    currentIrBrowser_->setOpenLinks(false);
    currentIrBrowser_->setOpenExternalLinks(false);
    layout->addWidget(currentIrBrowser_, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, [this]() { populateCurrentIR(); });

    // Click-to-highlight: extract source line from anchor and emit signal
    connect(currentIrBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });
}

/// 当面板当前停在「当前源码 IR」子页（index==2）时，重新生成并刷新 IR 显示。
void IRTransformPanel::reloadCurrentIR() {
    if (stack_->currentIndex() == 2) {
        populateCurrentIR();
    }
}

/// 用 lowering 场景库标题填充左侧列表并默认选中首项。
void IRTransformPanel::populateLoweringList() {
    loweringList_->clear();
    const auto& items = IRTransformLibrary::loweringExamples();
    for (const auto& e : items) {
        loweringList_->addItem(QString::fromUtf8(e.title.c_str()));
    }
    if (!items.empty())
        loweringList_->setCurrentRow(0);
}

/// 根据选中索引渲染 lowering 详情：标题 / AST 节点 / 源码 / 说明 +
/// 原始 IR（<pre> 背景用 TeachingTheme::surface() 以跟随主题）。
// R51-8 fix: 所有库文本字段经 toHtmlEscaped 转义，防止 < > & 等字符破坏 HTML 结构
void IRTransformPanel::populateLoweringDetail(int index) {
    const auto& items = IRTransformLibrary::loweringExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& e = items[index];

    // R51-8 fix: 转义库文本中的 HTML 特殊字符
    auto esc = [](const std::string& s) -> std::string {
        return QString::fromUtf8(s.c_str()).toHtmlEscaped().toStdString();
    };

    std::ostringstream os;
    os << "<h3>" << esc(e.title) << "</h3>";
    os << "<p><b>" << tr("AST 节点：").toStdString() << "</b> <code>" << esc(e.astSummary) << "</code></p>";
    os << "<p><b>" << tr("源码：").toStdString() << "</b> <code>" << esc(e.sourceCode) << "</code></p>";
    os << "<hr>";
    os << "<p>" << esc(e.description) << "</p>";
    os << "<h4>" << tr("Lowering 后的 IR：").toStdString() << "</h4>";
    os << "<pre style='background:" << TeachingTheme::surface().name().toStdString()
       << "; padding:8px; font-family:\"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source "
          "Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace;'>"
       << esc(e.irBefore) << "</pre>";
    loweringDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 用优化场景库的「pass 名 + 标题」填充左侧列表并默认选中首项。
void IRTransformPanel::populateOptList() {
    optList_->clear();
    const auto& items = IRTransformLibrary::optimizationExamples();
    for (const auto& e : items) {
        QString text = QString::fromUtf8("[%1] %2")
                           .arg(QString::fromUtf8(e.passName.c_str()))
                           .arg(QString::fromUtf8(e.title.c_str()));
        optList_->addItem(text);
    }
    if (!items.empty())
        optList_->setCurrentRow(0);
}

/// 根据选中索引渲染优化对比：摘要标签（标题 + 指令数变化）+ 优化前/后 IR 文本。
/// 拓展二期：预测模式下隐藏优化后 IR 与指令数（由 onPredictSubmit 揭示）。
void IRTransformPanel::populateOptDetail(int index) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& e = items[index];

    optBefore_->setPlainText(QString::fromUtf8(e.irBefore.c_str()));
    if (predictMode_) {
        // 预测模式：遮盖答案，提示学生先思考
        optSummary_->setText(tr("%1 — 优化前 %2 条指令，优化后？条（预测后提交揭示）")
                                 .arg(QString::fromUtf8(e.title.c_str()))
                                 .arg(e.instrBefore));
        optAfter_->setPlainText(tr("（预测模式：先在上方输入你预测的优化后指令数，\n"
                                   "点击「提交预测」后揭示优化后 IR 与答案）"));
        predictResultLabel_->clear();
        return;
    }
    optSummary_->setText(tr("%1 — %2 → %3 条指令（减少 %4 条）")
                             .arg(QString::fromUtf8(e.title.c_str()))
                             .arg(e.instrBefore)
                             .arg(e.instrAfter)
                             .arg(e.instrBefore - e.instrAfter));

    optAfter_->setPlainText(QString::fromUtf8(e.irAfter.c_str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 从 controller 取 AST，用 AstIRBuilder 生成 IRModule，经 IRToString 转文本后
/// 渲染为可点击 HTML，并显示基本块/指令数。异常时在状态栏报告。
void IRTransformPanel::populateCurrentIR() {
    if (!controller_) {
        currentStatusLabel_->setText(tr("未绑定控制器"));
        currentIrBrowser_->setPlainText(tr("（未绑定控制器）"));
        return;
    }
    Block* ast = controller_->astRoot();
    if (!ast) {
        currentStatusLabel_->setText(tr("请先在主编辑器中输入并编译代码"));
        currentIrBrowser_->setPlainText(tr("（无 AST）"));
        return;
    }

    try {
        AstIRBuilder builder;
        builder.build(*ast);
        IRModule* mod = builder.getModule();
        if (!mod || !mod->mainFunction) {
            currentStatusLabel_->setText(tr("IR 生成失败"));
            return;
        }
        std::string irText = IRToString(*mod->mainFunction);
        currentIrBrowser_->setHtml(irTextToClickableHtml(QString::fromUtf8(irText.c_str())));
        int instrCount = 0;
        for (const auto& blk : mod->mainFunction->blocks) {
            instrCount += (int)blk.instructions.size();
        }
        currentStatusLabel_->setText(
            tr("已生成 IR：%1 基本块 / %2 条指令").arg(mod->mainFunction->blocks.size()).arg(instrCount));
    } catch (const std::exception& e) {
        currentStatusLabel_->setText(tr("IR 生成异常"));
        currentIrBrowser_->setPlainText(QString::fromUtf8(e.what()));
    }
}

// ============================================================
// 第 4 子页：逐步优化回放
// ============================================================
//
// 布局：左侧场景列表 + 中间步骤列表 + 右上 IR 快照 + 右下决策列表
// 使用 QSplitter 嵌套布局：水平 splitter（场景 | 步骤 | 右侧垂直 splitter）

/// 构建「逐步优化回放」子页：左侧场景列表 + 中间 pass 步骤列表 +
/// 右上 IR 快照浏览器 + 右下决策解释列表（嵌套水平/垂直 splitter）。
/// 连接场景/步骤切换与锚点点击（跳转源码行）。
void IRTransformPanel::buildReplayPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    replayStatusLabel_ = new QLabel(tr("请选择场景"), host);

    // 左侧：场景列表
    auto* scenarioWrap = new QWidget(host);
    auto* scenarioLayout = new QVBoxLayout(scenarioWrap);
    scenarioLayout->setContentsMargins(0, 0, 0, 0);
    scenarioLayout->addWidget(new QLabel(tr("场景列表："), scenarioWrap));
    replayList_ = new QListWidget(scenarioWrap);
    scenarioLayout->addWidget(replayList_, 1);

    // 中间：步骤列表
    auto* stepsWrap = new QWidget(host);
    auto* stepsLayout = new QVBoxLayout(stepsWrap);
    stepsLayout->setContentsMargins(0, 0, 0, 0);
    stepsLayout->addWidget(new QLabel(tr("Pass 步骤："), stepsWrap));
    replayStepsList_ = new QListWidget(stepsWrap);
    stepsLayout->addWidget(replayStepsList_, 1);

    // 右上：IR 快照
    auto* irWrap = new QWidget(host);
    auto* irLayout = new QVBoxLayout(irWrap);
    irLayout->setContentsMargins(0, 0, 0, 0);
    irLayout->addWidget(new QLabel(tr("IR 快照："), irWrap));
    replayIrBrowser_ = new QTextBrowser(irWrap);
    replayIrBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    replayIrBrowser_->setOpenLinks(false);
    replayIrBrowser_->setOpenExternalLinks(false);
    irLayout->addWidget(replayIrBrowser_, 1);

    // 右下：决策列表
    auto* decisionsWrap = new QWidget(host);
    auto* decisionsLayout = new QVBoxLayout(decisionsWrap);
    decisionsLayout->setContentsMargins(0, 0, 0, 0);
    decisionsLayout->addWidget(new QLabel(tr("决策解释："), decisionsWrap));
    replayDecisionsList_ = new QListWidget(decisionsWrap);
    decisionsLayout->addWidget(replayDecisionsList_, 1);

    // 右侧垂直 splitter（IR 上 | 决策 下）
    auto* rightSplitter = new QSplitter(Qt::Vertical, host);
    rightSplitter->addWidget(irWrap);
    rightSplitter->addWidget(decisionsWrap);
    rightSplitter->setStretchFactor(0, 1);
    rightSplitter->setStretchFactor(1, 1);
    rightSplitter->setSizes({250, 150});

    // 主水平 splitter（场景 | 步骤 | 右侧 splitter）
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(scenarioWrap);
    splitter->addWidget(stepsWrap);
    splitter->addWidget(rightSplitter);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 2);
    splitter->setSizes({150, 150, 350});

    layout->addWidget(replayStatusLabel_);
    layout->addWidget(splitter, 1);

    // 场景列表切换 → 刷新步骤列表
    connect(replayList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (row >= (int)scenarios.size())
            return;
        replayStepsList_->clear();
        const auto& steps = scenarios[row].second;
        for (const auto& s : steps) {
            QString text = tr("[%1] %2 (修改 %3 条)")
                               .arg(QString::fromUtf8(s.passName.c_str()))
                               .arg(QString::fromUtf8(s.passRound.c_str()))
                               .arg(s.modifiedCount);
            replayStepsList_->addItem(text);
        }
        if (!steps.empty()) {
            replayStepsList_->setCurrentRow(0);
        }
        replayStatusLabel_->setText(
            tr("场景：%1 — %2 个步骤").arg(QString::fromUtf8(scenarios[row].first.c_str())).arg(steps.size()));
        // 注：移除 fadeInWidget —— QListWidget 刷新无需动画，
        // QGraphicsOpacityEffect 会导致连续切换时 opacity 卡 0 内容空白。
    });

    // Click-to-highlight from replay IR browser
    connect(replayIrBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });

    // 步骤列表切换 → 刷新 IR 快照 + 决策列表
    connect(replayStepsList_, &QListWidget::currentRowChanged, this, [this](int stepRow) {
        int scenRow = replayList_->currentRow();
        if (scenRow < 0 || stepRow < 0)
            return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (scenRow >= (int)scenarios.size())
            return;
        const auto& steps = scenarios[scenRow].second;
        if (stepRow >= (int)steps.size())
            return;
        populateReplayStep(scenRow, stepRow);
    });
}

/// 用回放场景库的场景名填充左侧列表并默认选中首项。
void IRTransformPanel::populateReplayList() {
    if (!replayList_)
        return;
    replayList_->clear();
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        replayList_->addItem(QString::fromUtf8(s.first.c_str()));
    }
    if (!scenarios.empty()) {
        replayList_->setCurrentRow(0);
    }
}

/// 根据场景与步骤索引渲染回放详情：IR 快照（可点击）+ 决策解释列表 +
/// 状态标签（场景 / pass 名 / 轮次 / 指令数 / 修改数）。
void IRTransformPanel::populateReplayStep(int scenarioIdx, int stepIdx) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    if (scenarioIdx < 0 || scenarioIdx >= (int)scenarios.size())
        return;
    const auto& steps = scenarios[scenarioIdx].second;
    if (stepIdx < 0 || stepIdx >= (int)steps.size())
        return;
    const auto& s = steps[stepIdx];

    // IR 快照
    replayIrBrowser_->setHtml(irTextToClickableHtml(QString::fromUtf8(s.irSnapshot.c_str())));

    // 决策列表
    replayDecisionsList_->clear();
    for (const auto& d : s.decisions) {
        replayDecisionsList_->addItem(QString::fromUtf8(d.c_str()));
    }

    // 状态标签
    replayStatusLabel_->setText(tr("场景：%1 — %2 / %3 — %4 条指令（修改 %5 条）")
                                    .arg(QString::fromUtf8(scenarios[scenarioIdx].first.c_str()))
                                    .arg(QString::fromUtf8(s.passName.c_str()))
                                    .arg(QString::fromUtf8(s.passRound.c_str()))
                                    .arg(s.instrCount)
                                    .arg(s.modifiedCount));

    // 注：移除 fadeInWidget —— QTextBrowser/QListWidget 内容刷新无需动画，
    // QGraphicsOpacityEffect 会导致连续切换步骤时 opacity 卡 0 内容空白。
}
