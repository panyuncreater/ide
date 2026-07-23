// ============================================================
// LintExplorerPanel.cpp — 静态分析探索器教学面板实现
// ------------------------------------------------------------
// 实现 2 子页：Lint 规则说明 / 交互式 Lint 分析。
// 子页 2 调用 cli/lint_core.h 的 minilang_lint::lintSource 三步管线
// （Lexer → Parser → LintPass），将诊断结果填入表格并支持选中查看
// 规则说明与修复建议。
// ============================================================

#include "gui/LintExplorerPanel.h"
#include "gui/GuiTextUtils.h"
#include "gui/GuidedTour.h"
#include "gui/I18n.h"
#include "gui/TeachingSubPageBar.h"

#include "cli/lint_core.h"
#include "common/Diagnostic.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

/// HTML 转义辅助：std::string → 已转义的 QString
QString htmlEsc(const std::string& s) {
    return QString::fromUtf8(s.c_str()).toHtmlEscaped();
}

} // anonymous namespace

// ============================================================
// LintRuleLibrary — 8 条 Lint 规则静态文档
// ============================================================
// 顺序与 minilang::lint::LintRule 枚举严格一致：
//   UnusedVariable / UnusedFunction / UnusedParameter /
//   AssignmentInCondition / DeadCodeAfterReturn / EmptyBlock /
//   CyclomaticComplexity / NamingConvention
//
// 注意：DeadCodeAfterReturn 的规则码为 "lint-dead-code"
// （与 lint/LintPass.cpp 中 ruleCode() 实现保持一致，确保
// onDiagnosticSelected 中 findByCode(d.code) 能正确匹配）。
const std::vector<LintRuleInfo>& LintRuleLibrary::rules() {
    static const std::vector<LintRuleInfo> kRules = {
        {"lint-unused-variable", "UnusedVariable", "未使用变量", "🔇", "变量声明后未引用（退出作用域时检测）",
         "未使用的变量 '{name}'", "var unused = 1;\nvar used = 2;\nprint(used);\n",
         "移除未使用的变量声明，或在变量名前加 _ 前缀表示有意未使用"},
        {"lint-unused-function", "UnusedFunction", "未使用函数", "🔇",
         "函数定义后未调用（main/init/test 约定入口跳过）", "未使用的函数 '{name}'", "fun foo() { return 1; }\n",
         "移除未使用的函数，或调用它"},
        {"lint-unused-parameter", "UnusedParameter", "未使用参数", "🔇", "函数参数未使用", "未使用的参数 '{name}'",
         "fun foo(a) { return 1; }\nprint(foo(1));\n", "移除未使用的参数，或在参数名前加 _ 前缀"},
        {"lint-assignment-in-condition", "AssignmentInCondition", "条件中赋值", "⚠️",
         "if/while 条件中含赋值（可能误将 == 写成 =）", "条件中含赋值 '{name}='，可能误写 ==",
         "var x = 0;\nif (x = 1) { print(x); }\n", "将 = 改为 == 进行比较，或显式分开赋值与判断"},
        {"lint-dead-code", "DeadCodeAfterReturn", "return 后死代码", "💀", "return/break/continue 之后的语句",
         "return 后的不可达代码", "fun foo() {\n    return 1;\n    print(2);\n}\nprint(foo());\n",
         "移除 return 之后的不可达代码"},
        {"lint-empty-block", "EmptyBlock", "空块", "📭", "空块 {}（函数体或语句块为空）", "空代码块",
         "fun foo() {}\nprint(foo());\n", "补充代码块内容，或添加 // intentional empty 注释"},
        {"lint-cyclomatic-complexity", "CyclomaticComplexity", "圈复杂度过高", "🌀", "函数圈复杂度超过阈值（默认 10）",
         "函数 '{name}' 圈复杂度 {n} 超过阈值 {max}",
         "fun complex(a,b,c){\n    if(a>0){return 1;}\n    if(b>0){return 2;}\n    // ... 11 个 if ...\n}\n",
         "拆分函数为多个小函数，提取子逻辑，减少嵌套"},
        {"lint-naming-convention", "NamingConvention", "命名规范", "📝",
         "变量/函数非 camelCase，类非 PascalCase（全大写常量跳过）", "命名不符合规范: '{name}'",
         "var bad_name = 1;\nprint(bad_name);\n", "变量/函数用 camelCase（如 myVar），类用 PascalCase（如 MyClass）"},
    };
    return kRules;
}

const LintRuleInfo* LintRuleLibrary::findByCode(const std::string& code) {
    for (const auto& r : rules()) {
        if (r.code == code) {
            return &r;
        }
    }
    return nullptr;
}

// ============================================================
// LintSampleLibrary — 5 个预设样例
// ============================================================

const std::vector<LintSample>& LintSampleLibrary::samples() {
    static const std::vector<LintSample> kSamples = {
        {"未使用变量", "var x = 10;\nvar y = 20;\nprint(y);\n", "触发 UnusedVariable（x 未使用）"},
        {"未使用函数", "fun helper() { print(\"helper\"); }\nfun main() { print(\"main\"); }\nmain();\n",
         "触发 UnusedFunction（helper 未调用）"},
        {"条件中赋值", "var x = 0;\nif (x = 5) {\n    print(x);\n}\n", "触发 AssignmentInCondition"},
        {"return 后死代码", "fun f() {\n    return 1;\n    print(\"unreachable\");\n}\nf();\n",
         "触发 DeadCodeAfterReturn"},
        {"高圈复杂度",
         "fun complex(a, b, c) {\n"
         "    if (a > 0) { return 1; }\n"
         "    if (b > 0) { return 2; }\n"
         "    if (c > 0) { return 3; }\n"
         "    if (a > 1) { return 4; }\n"
         "    if (b > 1) { return 5; }\n"
         "    if (c > 1) { return 6; }\n"
         "    if (a > 2) { return 7; }\n"
         "    if (b > 2) { return 8; }\n"
         "    if (c > 2) { return 9; }\n"
         "    if (a > 3) { return 10; }\n"
         "    if (b > 3) { return 11; }\n"
         "    return 0;\n"
         "}\n"
         "print(complex(1, 2, 3));\n",
         "触发 CyclomaticComplexity（复杂度 12 > 阈值 10）"},
    };
    return kSamples;
}

// ============================================================
// LintExplorerPanel 实现
// ============================================================

/// 构造面板：组装顶部子页切换（Lint 规则说明 / 交互式 Lint 分析）+
/// QStackedWidget，构建两个子页，预填首个样例代码。
LintExplorerPanel::LintExplorerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 子页切换按钮栏（统一组件 TeachingSubPageBar，文案带 ① ② 前缀与其他面板一致）
    subPageBar_ = new TeachingSubPageBar(this);
    auto* rulesPage = new QWidget;
    auto* interactivePage = new QWidget;
    buildRulesPage(rulesPage);
    buildInteractivePage(interactivePage);
    subPageBar_->addPage(mlTr("① Lint 规则说明"), rulesPage);
    subPageBar_->addPage(mlTr("② 交互式 Lint 分析"), interactivePage);
    pageRulesBtn_ = subPageBar_->buttonAt(0);
    pageInteractiveBtn_ = subPageBar_->buttonAt(1);
    stack_ = subPageBar_->stack();
    outer->addLayout(subPageBar_->buttonBar());
    outer->addWidget(stack_, 1);
    // 恢复上次子页选择
    subPageBar_->restoreFromSettings(QStringLiteral("teachingPanel/lint-explorer/subPage"));

    connect(subPageBar_, &TeachingSubPageBar::currentChanged, this,
            [this](int) { subPageBar_->saveToSettings(QStringLiteral("teachingPanel/lint-explorer/subPage")); });

    populateRules();

    // 预填第一个样例到源码编辑器
    if (!LintSampleLibrary::samples().empty()) {
        sourceEdit_->setPlainText(QString::fromUtf8(LintSampleLibrary::samples().front().code.c_str()));
    }

    // 诊断详情占位提示
    diagDetail_->setHtml(QString::fromUtf8("<div style='color:#6E6E6E; padding:8px;'><i>（点击「运行 Lint」后，"
                                           "选中上方表格的某行查看该诊断的规则说明与修复建议）</i></div>"));
}

/// 构建「Lint 规则说明」子页：水平 splitter（左侧规则列表 + 右侧规则详情），
/// 底部附 DefaultVisitor 模式说明。
void LintExplorerPanel::buildRulesPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    ruleList_ = new QListWidget;
    ruleDetail_ = new QTextBrowser;
    ruleDetail_->setOpenExternalLinks(false);
    splitter->addWidget(ruleList_);
    splitter->addWidget(ruleDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    auto* hintLabel = new QLabel(mlTr("Lint 规则基于 DefaultVisitor 模式，仅分析 AST 不执行代码"));
    hintLabel->setStyleSheet(QStringLiteral("color:#6E6E6E; padding:2px;"));
    v->addWidget(hintLabel);

    connect(ruleList_, &QListWidget::currentRowChanged, this, &LintExplorerPanel::onRuleSelected);
}

/// 构建「交互式 Lint 分析」子页：控制栏（运行/规则开关/阈值/样例按钮）+
/// 源码编辑器 + 垂直 splitter（诊断表格 + 诊断详情）+ 底部统计栏。
void LintExplorerPanel::buildInteractivePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // 控制栏 1：运行按钮 + 8 个规则开关 + 圈复杂度阈值
    auto* ctrlBar1 = new QHBoxLayout;
    runLintBtn_ = new QPushButton(mlTr("运行 Lint"));
    ctrlBar1->addWidget(runLintBtn_);
    ctrlBar1->addSpacing(8);

    ruleChecks_.clear();
    const auto& rules = LintRuleLibrary::rules();
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        auto* cb = new QCheckBox(QString::fromUtf8(r.emoji.c_str()) + QStringLiteral(" ") +
                                 QString::fromUtf8(r.title.c_str()));
        cb->setChecked(true);
        cb->setToolTip(QString::fromUtf8(r.code.c_str()));
        ctrlBar1->addWidget(cb);
        ruleChecks_.push_back(cb);
    }

    ctrlBar1->addSpacing(8);
    ctrlBar1->addWidget(new QLabel(mlTr("圈复杂度阈值:")));
    complexitySpin_ = new QSpinBox;
    complexitySpin_->setRange(1, 100);
    complexitySpin_->setValue(10);
    ctrlBar1->addWidget(complexitySpin_);
    ctrlBar1->addStretch();
    v->addLayout(ctrlBar1);

    // 控制栏 2：5 个预设样例按钮
    auto* ctrlBar2 = new QHBoxLayout;
    ctrlBar2->addWidget(new QLabel(mlTr("预设样例:")));
    sampleButtons_.clear();
    const auto& samples = LintSampleLibrary::samples();
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& s = samples[i];
        auto* btn = new QPushButton(QString::fromUtf8(s.title.c_str()));
        btn->setToolTip(QString::fromUtf8(s.description.c_str()));
        ctrlBar2->addWidget(btn);
        sampleButtons_.push_back(btn);
        int idx = static_cast<int>(i);
        connect(btn, &QPushButton::clicked, this, [this, idx]() { onSampleClicked(idx); });
    }
    ctrlBar2->addStretch();
    v->addLayout(ctrlBar2);

    // 源码编辑器（多行）
    sourceEdit_ = new QTextEdit;
    sourceEdit_->setFont(GuiTextUtils::monospaceFont(10));
    sourceEdit_->setPlaceholderText(mlTr("输入 MiniLang 源码后点击「运行 Lint」"));
    v->addWidget(sourceEdit_, 2);

    // 中间：垂直 splitter（诊断表格 + 诊断详情）
    auto* splitter = new QSplitter(Qt::Vertical);
    diagTable_ = new QTableWidget(0, 4);
    diagTable_->setHorizontalHeaderLabels({mlTr("行"), mlTr("列"), mlTr("级别"), mlTr("消息")});
    diagTable_->verticalHeader()->setVisible(false);
    diagTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    diagTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    diagTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    diagTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    diagTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    splitter->addWidget(diagTable_);

    diagDetail_ = new QTextBrowser;
    diagDetail_->setOpenExternalLinks(false);
    splitter->addWidget(diagDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 3);

    // 底部：统计 + 加载样例到主编辑器按钮
    auto* bottomBar = new QHBoxLayout;
    statsLabel_ = new QLabel(mlTr("共 0 条警告，0 条错误"));
    bottomBar->addWidget(statsLabel_);
    bottomBar->addStretch();
    loadSampleBtn_ = new QPushButton(mlTr("加载样例到主编辑器"));
    bottomBar->addWidget(loadSampleBtn_);
    v->addLayout(bottomBar);

    connect(runLintBtn_, &QPushButton::clicked, this, &LintExplorerPanel::onRunLint);
    connect(diagTable_, &QTableWidget::currentCellChanged, [this](int, int, int, int) { onDiagnosticSelected(); });
    connect(loadSampleBtn_, &QPushButton::clicked, this, &LintExplorerPanel::onLoadSample);
}

/// 填充规则列表（emoji + 中文标题），默认选中首项。
void LintExplorerPanel::populateRules() {
    ruleList_->clear();
    for (const auto& rule : LintRuleLibrary::rules()) {
        ruleList_->addItem(QString::fromUtf8(rule.emoji.c_str()) + QStringLiteral(" ") +
                           QString::fromUtf8(rule.title.c_str()));
    }
    if (ruleList_->count() > 0) {
        ruleList_->setCurrentRow(0);
    }
}

/// 选中规则列表某行时，渲染该规则的完整文档（HTML 转义）。
void LintExplorerPanel::onRuleSelected(int row) {
    showRule(row);
}

/// 渲染规则文档：标题 / 规则码 / 触发条件 / 消息模板 / 示例代码 / 修复建议。
void LintExplorerPanel::showRule(int index) {
    if (index < 0 || index >= static_cast<int>(LintRuleLibrary::rules().size())) {
        ruleDetail_->clear();
        return;
    }
    const auto& r = LintRuleLibrary::rules()[index];
    QString html = QString::fromUtf8("<h2>%1 %2</h2>"
                                     "<p><b>规则码:</b> <code>%3</code> | <b>规则类名:</b> <code>%4</code></p>"
                                     "<h3>触发条件</h3>"
                                     "<p>%5</p>"
                                     "<h3>错误消息模板</h3>"
                                     "<p><code>%6</code></p>"
                                     "<h3>示例代码</h3>"
                                     "<pre>%7</pre>"
                                     "<h3>修复建议</h3>"
                                     "<p>%8</p>")
                       .arg(htmlEsc(r.emoji))
                       .arg(htmlEsc(r.title))
                       .arg(htmlEsc(r.code))
                       .arg(htmlEsc(r.name))
                       .arg(htmlEsc(r.trigger))
                       .arg(htmlEsc(r.message))
                       .arg(htmlEsc(r.example))
                       .arg(htmlEsc(r.fix));
    ruleDetail_->setHtml(html);
}

/// 点击预设样例按钮：将样例载入源码编辑器并立即分析，提供即时反馈。
void LintExplorerPanel::onSampleClicked(int index) {
    if (index < 0 || index >= static_cast<int>(LintSampleLibrary::samples().size())) {
        return;
    }
    const auto& s = LintSampleLibrary::samples()[index];
    sourceEdit_->setPlainText(QString::fromUtf8(s.code.c_str()));
    onRunLint();
}

/// 运行 Lint：构建 LintOptions（规则开关 + 圈复杂度阈值），调用
/// minilang_lint::lintSource 三步管线，将诊断填入表格并更新统计。
/// 词法/语法错误在详情区提示。
void LintExplorerPanel::onRunLint() {
    diagTable_->setRowCount(0);
    diagDetail_->clear();
    lastDiagnostics_.clear();
    statsLabel_->setText(mlTr("共 0 条警告，0 条错误"));

    QString raw = sourceEdit_->toPlainText();
    if (raw.trimmed().isEmpty()) {
        // 空源码陷阱：空源码经 Parser 产生空 Block，会触发 EmptyBlock 警告。
        // 用户清空后给出提示而非运行分析。
        diagDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>源码为空</b>，"
                                               "请输入 MiniLang 源码或点击预设样例按钮。<br>"
                                               "<span style='color:#6E6E6E;'>(注意：空源码经 Parser 会产生空 Block，"
                                               "可能触发 EmptyBlock 警告)</span></div>"));
        return;
    }
    std::string source = raw.toStdString();

    // 构建 LintOptions：未勾选的规则加入 disabledRules
    minilang::lint::LintOptions opts;
    opts.maxCyclomaticComplexity = complexitySpin_->value();
    const size_t ruleCount = static_cast<size_t>(minilang::lint::LintRule::Count);
    for (size_t i = 0; i < ruleChecks_.size() && i < ruleCount; ++i) {
        if (!ruleChecks_[i]->isChecked()) {
            opts.disabledRules.insert(static_cast<minilang::lint::LintRule>(i));
        }
    }

    minilang_lint::LintSourceResult result = minilang_lint::lintSource(source, opts);

    if (!result.ok) {
        // 词法 / 语法错误阶段，分析中止
        QString phase = result.errorPhase == "lex" ? mlTr("词法错误") : mlTr("语法错误");
        QString errs = QString::fromUtf8(result.errorMessage.c_str()).toHtmlEscaped();
        diagDetail_->setHtml(
            QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>%1:</b> %2</div>").arg(phase, errs));
        statsLabel_->setText(mlTr("分析失败：%1").arg(phase));
        return;
    }

    // 填充诊断表格
    const auto& diags = result.lint.diagnostics.all();
    lastDiagnostics_ = diags; // 拷贝副本供选中查看

    int warns = 0, errs = 0, infos = 0;
    for (const auto& d : diags) {
        int row = diagTable_->rowCount();
        diagTable_->insertRow(row);
        auto* cLine = new QTableWidgetItem(QString::number(d.line));
        auto* cCol = new QTableWidgetItem(QString::number(d.column));
        auto* cLevel = new QTableWidgetItem(QString::fromUtf8(d.levelString().c_str()));
        auto* cMsg = new QTableWidgetItem(QString::fromUtf8(d.message.c_str()));
        cLine->setTextAlignment(Qt::AlignCenter);
        cCol->setTextAlignment(Qt::AlignCenter);
        cLevel->setTextAlignment(Qt::AlignCenter);
        diagTable_->setItem(row, 0, cLine);
        diagTable_->setItem(row, 1, cCol);
        diagTable_->setItem(row, 2, cLevel);
        diagTable_->setItem(row, 3, cMsg);

        if (d.isError())
            ++errs;
        else if (d.isWarning())
            ++warns;
        else
            ++infos;
    }

    // 统计：警告 + 错误（+ 提示，若有）
    QString statText = mlTr("共 %1 条警告，%2 条错误").arg(warns).arg(errs);
    if (infos > 0) {
        statText += mlTr("，%1 条提示").arg(infos);
    }
    statsLabel_->setText(statText);

    if (!diags.empty()) {
        diagTable_->selectRow(0);
    } else {
        diagDetail_->setHtml(QString::fromUtf8("<div style='color:#27AE60; padding:8px;'><b>未触发任何 Lint 诊断</b>，"
                                               "当前源码符合所有启用的规则。</div>"));
    }
}

/// 选中诊断表格某行时，根据诊断规则码查找规则文档，渲染诊断详情
/// （规则说明 + 触发条件 + 修复建议）。未知规则码仅展示诊断本身。
void LintExplorerPanel::onDiagnosticSelected() {
    int row = diagTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(lastDiagnostics_.size())) {
        return;
    }
    const auto& d = lastDiagnostics_[row];
    const LintRuleInfo* info = LintRuleLibrary::findByCode(d.code);

    QString html;
    if (info) {
        html = QString::fromUtf8("<h3>%1 %2</h3>"
                                 "<p><b>规则码:</b> <code>%3</code> | <b>级别:</b> %4 | "
                                 "<b>位置:</b> 行 %5, 列 %6</p>"
                                 "<h4>诊断消息</h4>"
                                 "<p>%7</p>"
                                 "<h4>触发条件</h4>"
                                 "<p>%8</p>"
                                 "<h4>修复建议</h4>"
                                 "<p>%9</p>")
                   .arg(htmlEsc(info->emoji))
                   .arg(htmlEsc(info->title))
                   .arg(htmlEsc(info->code))
                   .arg(QString::fromUtf8(d.levelString().c_str()))
                   .arg(d.line)
                   .arg(d.column)
                   .arg(htmlEsc(d.message))
                   .arg(htmlEsc(info->trigger))
                   .arg(htmlEsc(info->fix));
    } else {
        html = QString::fromUtf8("<h3>诊断详情</h3>"
                                 "<p><b>规则码:</b> <code>%1</code> | <b>级别:</b> %2 | "
                                 "<b>位置:</b> 行 %3, 列 %4</p>"
                                 "<h4>诊断消息</h4>"
                                 "<p>%5</p>")
                   .arg(htmlEsc(d.code))
                   .arg(QString::fromUtf8(d.levelString().c_str()))
                   .arg(d.line)
                   .arg(d.column)
                   .arg(htmlEsc(d.message));
    }
    diagDetail_->setHtml(html);
}

/// 将当前源码编辑器内容 emit loadSampleRequested，加载到主编辑器运行观察。
void LintExplorerPanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->toPlainText());
}

// ============================================================
// createGuidedTour — 新手引导（4 步）
// ============================================================

/// 构建 4 步新手引导：Lint 规则页按钮 / 规则列表 / 交互式分析页按钮 / 运行按钮。
GuidedTour* LintExplorerPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageRulesBtn_, mlTr("Lint 规则说明"),
                  mlTr("点击「Lint 规则说明」查看 8 条静态分析规则（未使用变量、复杂度、命名等），"
                       "每条规则含触发条件、示例代码与修复建议。"));
    tour->addStep(ruleList_, mlTr("规则列表"),
                  mlTr("规则列表展示 8 条规则的 emoji 与标题，选中某条规则可在右侧查看"
                       "完整的规则码、触发条件、错误消息模板与示例代码。"));
    tour->addStep(pageInteractiveBtn_, mlTr("交互式 Lint 分析"),
                  mlTr("点击「交互式 Lint 分析」切到子页 2：在源码编辑器中输入代码，"
                       "可独立开关每条规则、调整圈复杂度阈值，或点击预设样例快速体验。"));
    tour->addStep(runLintBtn_, mlTr("运行 Lint"),
                  mlTr("点击「运行 Lint」后，诊断表格列出所有触发项；选中某行可在"
                       "详情区查看该规则的修复建议，据此改进代码。"));
    return tour;
}
