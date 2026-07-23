// ============================================================
// ExerciseGraderPanel.cpp — 交互式练习评分教学面板实现
// ------------------------------------------------------------
// 面板内部独立完成 Lexer → Parser → 三后端执行流程：
//   1. 评分校验：Interpreter 后端运行测试用例，比对期望输出
//   2. 执行详情：Interpreter / StackVM(IR) / RegisterVM(IR) 三后端
//      串行执行学生代码，展示输出与状态
//   3. 代码风格：逐行扫描缩进/空行/行尾空格/运算符空格
//   4. 评分报告：测试通过率 60 分 + 风格 20 分 + 编译 10 分 + 运行时 10 分
// 三后端在主线程串行执行，捕获输出/状态/错误，参考
// BackendParallelPanel 的 runInterpreter / runStackVM_IR / runRegVM_IR。
// ============================================================

#include "gui/ExerciseGraderPanel.h"

#include "common/Diagnostic.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <sstream>

// ============================================================
// 预设题库（4 个题目）
// ============================================================

const std::vector<Exercise>& ExerciseGraderLibrary::exercises() {
    static const std::vector<Exercise> kExercises = {
        // 1. 两数之和
        {"两数之和",
         "<h3>题目：两数之和</h3>"
         "<p>实现一个函数 <code>add(a, b)</code>，返回两个整数之和。</p>"
         "<p><b>要求：</b>函数接受两个参数，返回它们的和。</p>",
         "使用 <code>return a + b;</code> 直接返回两数之和。",
         "fun add(a, b) {\n    return a + b;\n}\nprint(add(3, 5));",
         {{"3+5", "fun add(a, b) { return a + b; } print(add(3, 5));", "8"},
          {"10+20", "fun add(a, b) { return a + b; } print(add(10, 20));", "30"},
          {"-1+1", "fun add(a, b) { return a + b; } print(add(-1, 1));", "0"}}},
        // 2. 阶乘计算
        {"阶乘计算",
         "<h3>题目：阶乘计算</h3>"
         "<p>实现一个函数 <code>fact(n)</code>，返回 n 的阶乘（n!）。</p>"
         "<p><b>要求：</b>使用递归，当 n &le; 1 时返回 1。</p>",
         "递归终止条件 <code>if (n &lt;= 1) { return 1; }</code>。",
         "fun fact(n) {\n    if (n <= 1) {\n        return 1;\n    }\n    return n * fact(n-1);\n}\nprint(fact(5));",
         {{"fact(5)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(5));", "120"},
          {"fact(1)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(1));", "1"},
          {"fact(6)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(6));", "720"}}},
        // 3. 数组求和
        {"数组求和",
         "<h3>题目：数组求和</h3>"
         "<p>实现一个函数 <code>sum(arr)</code>，返回数组所有元素之和。</p>"
         "<p><b>要求：</b>使用 <code>for (var x in arr)</code> 遍历数组。</p>",
         "初始化累加器 <code>var s = 0;</code>，循环中 <code>s = s + x;</code>。",
         "fun sum(arr) {\n    var s = 0;\n    for (var x in arr) {\n        s = s + x;\n    }\n    return "
         "s;\n}\nprint(sum([1,2,3,4,5]));",
         {{"[1,2,3,4,5]",
           "fun sum(arr) { var s = 0; for (var x in arr) { s = s + x; } return s; } print(sum([1,2,3,4,5]));", "15"},
          {"[10,20,30]",
           "fun sum(arr) { var s = 0; for (var x in arr) { s = s + x; } return s; } print(sum([10,20,30]));", "60"},
          {"[]", "fun sum(arr) { var s = 0; for (var x in arr) { s = s + x; } return s; } print(sum([]));", "0"}}},
        // 4. 字符串反转
        {"字符串反转",
         "<h3>题目：字符串反转</h3>"
         "<p>实现一个函数 <code>reverse(s)</code>，返回字符串 s 的反转。</p>"
         "<p><b>要求：</b>使用 <code>s.length()</code> 获取长度，<code>s[i]</code> 索引字符。</p>",
         "从末尾向前遍历，<code>r = r + s[i];</code> 逐字符拼接。",
         "fun reverse(s) {\n    var r = \"\";\n    for (var i = s.length()-1; i >= 0; i = i - 1) {\n        r = r + "
         "s[i];\n    }\n    return r;\n}\nprint(reverse(\"hello\"));",
         {{"hello",
           "fun reverse(s) { var r = \"\"; for (var i = s.length()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"hello\"));",
           "olleh"},
          {"abc",
           "fun reverse(s) { var r = \"\"; for (var i = s.length()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"abc\"));",
           "cba"},
          {"a",
           "fun reverse(s) { var r = \"\"; for (var i = s.length()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"a\"));",
           "a"}}},
    };
    return kExercises;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 格式化 DiagnosticBag 中的错误条目为纯文本（分号分隔）
QString formatDiagnosticErrors(const DiagnosticBag& bag) {
    QString result;
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            result += QString::fromUtf8(d.format().c_str()) + QStringLiteral("; ");
        }
    }
    if (result.isEmpty()) {
        result = QStringLiteral("（未知错误）");
    }
    return result;
}

} // anonymous namespace

// ============================================================
// ExerciseGraderPanel 实现
// ============================================================

/// 构造面板：构建界面并填充题目，预选第一题。
ExerciseGraderPanel::ExerciseGraderPanel(QWidget* parent) : QWidget(parent) {
    buildUI();
    populateExercises();
}

/// 构建界面：顶部题目选择+描述+代码编辑，中间测试用例表+三后端执行详情，
/// 底部评分表+反馈报告+加载样例按钮。使用 QSplitter(Vertical) 划分三区。
void ExerciseGraderPanel::buildUI() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部栏：题目选择 + 提交评分按钮
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("题目：")));
    exerciseCombo_ = new QComboBox;
    exerciseCombo_->setPlaceholderText(tr("选择练习题目"));
    topBar->addWidget(exerciseCombo_, 1);
    submitBtn_ = new QPushButton(tr("提交评分"));
    topBar->addWidget(submitBtn_);
    outer->addLayout(topBar);

    // 主分割区（垂直）：顶部代码区 / 中间测试区 / 底部评分区
    auto* mainSplitter = new QSplitter(Qt::Vertical);

    // ---- 顶部区：题目描述 + 代码编辑器 ----
    auto* topWidget = new QWidget;
    auto* topLayout = new QVBoxLayout(topWidget);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(4);
    descBrowser_ = new QTextBrowser;
    descBrowser_->setMaximumHeight(100);
    descBrowser_->setOpenExternalLinks(false);
    topLayout->addWidget(descBrowser_);
    codeEdit_ = new QPlainTextEdit;
    codeEdit_->setPlaceholderText(tr("在此编写代码..."));
    topLayout->addWidget(codeEdit_);
    mainSplitter->addWidget(topWidget);

    // ---- 中间区：测试用例表 + 三后端执行详情 ----
    auto* midSplitter = new QSplitter(Qt::Vertical);
    testTable_ = new QTableWidget(0, 5);
    testTable_->setHorizontalHeaderLabels({tr("用例名"), tr("输入"), tr("期望输出"), tr("实际输出"), tr("状态")});
    testTable_->verticalHeader()->setVisible(false);
    testTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    testTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    testTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    testTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    testTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    testTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    testTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    testTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    midSplitter->addWidget(testTable_);
    execDetail_ = new QTextBrowser;
    execDetail_->setOpenExternalLinks(false);
    execDetail_->setPlaceholderText(tr("三后端执行详情（提交评分后显示）"));
    midSplitter->addWidget(execDetail_);
    midSplitter->setSizes({250, 150});
    mainSplitter->addWidget(midSplitter);

    // ---- 底部区：评分表 + 反馈报告 ----
    auto* bottomSplitter = new QSplitter(Qt::Vertical);
    gradeTable_ = new QTableWidget(0, 4);
    gradeTable_->setHorizontalHeaderLabels({tr("评分项"), tr("得分"), tr("满分"), tr("说明")});
    gradeTable_->verticalHeader()->setVisible(false);
    gradeTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gradeTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gradeTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gradeTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gradeTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    gradeTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    gradeTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    bottomSplitter->addWidget(gradeTable_);
    feedbackBrowser_ = new QTextBrowser;
    feedbackBrowser_->setOpenExternalLinks(false);
    feedbackBrowser_->setPlaceholderText(tr("评分反馈报告（提交评分后显示）"));
    bottomSplitter->addWidget(feedbackBrowser_);
    bottomSplitter->setSizes({150, 200});
    mainSplitter->addWidget(bottomSplitter);

    mainSplitter->setSizes({300, 250, 200});
    outer->addWidget(mainSplitter, 1);

    // 底部栏：加载样例按钮
    auto* bottomBar = new QHBoxLayout;
    loadSampleBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    bottomBar->addWidget(loadSampleBtn_);
    bottomBar->addStretch();
    outer->addLayout(bottomBar);

    // 信号连接
    connect(exerciseCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ExerciseGraderPanel::onExerciseSelected);
    connect(submitBtn_, &QPushButton::clicked, this, &ExerciseGraderPanel::onSubmitGrade);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &ExerciseGraderPanel::onLoadSample);
}

/// 填充题目下拉框并预选第一题。
void ExerciseGraderPanel::populateExercises() {
    const auto& exs = ExerciseGraderLibrary::exercises();
    exerciseCombo_->blockSignals(true);
    exerciseCombo_->clear();
    for (const auto& ex : exs) {
        exerciseCombo_->addItem(QString::fromUtf8(ex.title.c_str()));
    }
    exerciseCombo_->blockSignals(false);
    if (!exs.empty()) {
        onExerciseSelected(0);
    }
}

/// 选中题目：更新描述、起始代码、测试用例表，清空执行详情与评分。
void ExerciseGraderPanel::onExerciseSelected(int idx) {
    const auto& exs = ExerciseGraderLibrary::exercises();
    if (idx < 0 || idx >= static_cast<int>(exs.size())) {
        return;
    }
    const auto& ex = exs[idx];
    descBrowser_->setHtml(QString::fromUtf8(ex.description.c_str()) + QStringLiteral("<p style='color:#666;'><b>") +
                          tr("提示：") + QStringLiteral("</b>") + QString::fromUtf8(ex.hint.c_str()) +
                          QStringLiteral("</p>"));
    codeEdit_->setPlainText(QString::fromUtf8(ex.starterCode.c_str()));

    // 初始化测试用例表
    testTable_->setRowCount(static_cast<int>(ex.testCases.size()));
    for (int i = 0; i < static_cast<int>(ex.testCases.size()); ++i) {
        const auto& tc = ex.testCases[i];
        auto* nameItem = new QTableWidgetItem(QString::fromUtf8(tc.name.c_str()));
        nameItem->setTextAlignment(Qt::AlignCenter);
        testTable_->setItem(i, 0, nameItem);
        testTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(tc.code.c_str())));
        auto* expItem = new QTableWidgetItem(QString::fromUtf8(tc.expectedOutput.c_str()));
        expItem->setTextAlignment(Qt::AlignCenter);
        testTable_->setItem(i, 2, expItem);
        testTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8("（待运行）")));
        auto* statusItem = new QTableWidgetItem(QString::fromUtf8("待运行"));
        statusItem->setTextAlignment(Qt::AlignCenter);
        testTable_->setItem(i, 4, statusItem);
    }

    // 清空执行详情与评分
    execDetail_->clear();
    gradeTable_->setRowCount(0);
    feedbackBrowser_->clear();
}

/// 提交评分：运行测试用例集 + 三后端执行详情 + 风格/编译/运行时检查，
/// 生成评分表与反馈报告。
void ExerciseGraderPanel::onSubmitGrade() {
    const auto& exs = ExerciseGraderLibrary::exercises();
    int idx = exerciseCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(exs.size())) {
        return;
    }
    const auto& ex = exs[idx];
    int total = static_cast<int>(ex.testCases.size());

    // 1. 运行测试用例（Interpreter 后端校验输出）
    int passed = 0;
    QStringList passDetails;
    QStringList failDetails;
    for (int i = 0; i < total; ++i) {
        const auto& tc = ex.testCases[i];
        ExecResult result = runTestCase(tc.code);
        QString actual = result.output.trimmed();
        QString expected = QString::fromUtf8(tc.expectedOutput.c_str()).trimmed();
        bool pass = result.success && (actual == expected);

        QString actualDisplay = actual;
        if (!result.success && actual.isEmpty()) {
            actualDisplay = QString::fromUtf8("（错误）");
        }
        auto* actualItem = new QTableWidgetItem(actualDisplay);
        auto* statusItem = new QTableWidgetItem(pass ? QString::fromUtf8("✅ 通过") : QString::fromUtf8("❌ 失败"));
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (pass) {
            statusItem->setForeground(QColor(QStringLiteral("#27AE60")));
            ++passed;
            passDetails << QString::fromUtf8("%1: ✅ 期望「%2」, 实际「%3」")
                               .arg(QString::fromUtf8(tc.name.c_str()))
                               .arg(expected)
                               .arg(actual);
        } else {
            statusItem->setForeground(QColor(QStringLiteral("#C0392B")));
            QString actualForDetail = actual.isEmpty() ? QString::fromUtf8("（无输出）") : actual;
            failDetails << QString::fromUtf8("%1: ❌ 期望「%2」, 实际「%3」")
                               .arg(QString::fromUtf8(tc.name.c_str()))
                               .arg(expected)
                               .arg(actualForDetail);
        }
        testTable_->setItem(i, 3, actualItem);
        testTable_->setItem(i, 4, statusItem);
    }

    // 2. 三后端执行详情（学生代码）
    std::string studentCode = codeEdit_->toPlainText().toStdString();
    ExecResult interpR = runInterpreter(studentCode);
    ExecResult stackR = runStackVM_IR(studentCode);
    ExecResult regR = runRegVM_IR(studentCode);

    QString execHtml = QStringLiteral("<h3>三后端执行详情（学生代码）</h3>");
    auto appendBackend = [&](const QString& name, const ExecResult& r) {
        QString out = r.output.toHtmlEscaped();
        if (!r.success && !r.errorMsg.isEmpty()) {
            out += QStringLiteral("\n") + r.errorMsg.toHtmlEscaped();
        }
        execHtml += QStringLiteral("<h4>%1</h4><p><b>状态:</b> %2</p>"
                                   "<pre style='white-space:pre-wrap;'>%3</pre>")
                        .arg(name)
                        .arg(r.status.toHtmlEscaped())
                        .arg(out);
    };
    appendBackend(QString::fromUtf8("Interpreter"), interpR);
    appendBackend(QString::fromUtf8("StackVM (IR)"), stackR);
    appendBackend(QString::fromUtf8("RegisterVM (IR)"), regR);
    execDetail_->setHtml(execHtml);

    // 3. 评分
    std::vector<GradeItem> items;

    // 测试用例通过率（满分 60，每题 20 分）
    int testScore = passed * 20;
    items.push_back(
        {"测试用例通过率", testScore, 60, QString::fromUtf8("通过 %1/%2 个用例").arg(passed).arg(total).toStdString()});

    // 代码风格（满分 20）
    QString styleNote;
    int styleScore = checkStyle(studentCode, styleNote);
    items.push_back({"代码风格", styleScore, 20, styleNote.toStdString()});

    // 编译错误（满分 10）
    bool hasCompileError = interpR.compileError;
    items.push_back({"编译错误", hasCompileError ? 0 : 10, 10,
                     hasCompileError ? interpR.errorMsg.toStdString() : QString::fromUtf8("无编译错误").toStdString()});

    // 运行时错误（满分 10）
    bool hasRuntimeError = interpR.runtimeError;
    items.push_back(
        {"运行时错误", hasRuntimeError ? 0 : 10, 10,
         hasRuntimeError ? interpR.errorMsg.toStdString() : QString::fromUtf8("无运行时错误").toStdString()});

    // 4. 反馈报告
    int totalScore = 0;
    for (const auto& it : items) {
        totalScore += it.score;
    }
    QString feedback = QStringLiteral("<h2>评分报告</h2>");
    feedback += QStringLiteral("<p style='font-size:16px;'><b>总分：%1 / 100</b></p>").arg(totalScore);
    feedback += QStringLiteral("<h3>测试用例详情</h3>");
    if (!passDetails.isEmpty()) {
        feedback += QStringLiteral("<p style='color:#27AE60;'><b>通过：</b></p><ul>");
        for (const auto& d : passDetails) {
            feedback += QStringLiteral("<li>") + d + QStringLiteral("</li>");
        }
        feedback += QStringLiteral("</ul>");
    }
    if (!failDetails.isEmpty()) {
        feedback += QStringLiteral("<p style='color:#C0392B;'><b>失败：</b></p><ul>");
        for (const auto& d : failDetails) {
            feedback += QStringLiteral("<li>") + d + QStringLiteral("</li>");
        }
        feedback += QStringLiteral("</ul>");
    }
    feedback += QStringLiteral("<h3>改进建议</h3><ul>");
    if (hasCompileError) {
        feedback += QStringLiteral("<li>修复编译错误：") + interpR.errorMsg.toHtmlEscaped() + QStringLiteral("</li>");
    }
    if (hasRuntimeError) {
        feedback += QStringLiteral("<li>修复运行时错误：") + interpR.errorMsg.toHtmlEscaped() + QStringLiteral("</li>");
    }
    if (styleScore < 20) {
        feedback += QStringLiteral("<li>改进代码风格：") + styleNote + QStringLiteral("</li>");
    }
    if (passed < total) {
        feedback += QStringLiteral("<li>检查测试用例未通过的原因，对照期望输出调试代码。</li>");
    }
    if (totalScore == 100) {
        feedback += QStringLiteral("<li>表现优秀，全部通过！</li>");
    }
    feedback += QStringLiteral("</ul>");

    renderGradeReport(items, feedback);
}

/// 加载样例代码到主编辑器：emit loadSampleRequested 信号。
void ExerciseGraderPanel::onLoadSample() {
    emit loadSampleRequested(codeEdit_->toPlainText());
}

/// 运行单个测试用例（Interpreter 后端，用于评分校验输出）。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runTestCase(const std::string& src) {
    return runInterpreter(src);
}

/// 代码风格检查：逐行扫描 Tab 缩进/连续空行/行尾空格/长行/赋值运算符空格，
/// 返回得分 0-20，note 填写扣分说明。
int ExerciseGraderPanel::checkStyle(const std::string& code, QString& note) {
    if (code.empty()) {
        note = QString::fromUtf8("代码为空");
        return 0;
    }

    int score = 20;
    QStringList issues;

    // 逐行扫描
    std::stringstream ss(code);
    std::string line;
    int consecutiveBlanks = 0;
    int maxConsecutiveBlanks = 0;
    bool hasTab = false;
    int longLines = 0;
    int trailingWs = 0;

    while (std::getline(ss, line)) {
        // Tab 检查
        if (line.find('\t') != std::string::npos) {
            hasTab = true;
        }
        // 空行检查
        if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
            ++consecutiveBlanks;
            maxConsecutiveBlanks = std::max(maxConsecutiveBlanks, consecutiveBlanks);
        } else {
            consecutiveBlanks = 0;
        }
        // 行尾空格
        if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            ++trailingWs;
        }
        // 长行检查
        if (line.size() > 120) {
            ++longLines;
        }
    }

    if (hasTab) {
        score -= 5;
        issues << QString::fromUtf8("包含 Tab 缩进（建议使用 4 空格）");
    }
    if (maxConsecutiveBlanks > 1) {
        score -= 4;
        issues << QString::fromUtf8("存在连续多余空行");
    }
    if (trailingWs > 0) {
        int ded = std::min(trailingWs, 3);
        score -= ded;
        issues << QString::fromUtf8("行尾多余空格（%1 处）").arg(trailingWs);
    }
    if (longLines > 0) {
        int ded = std::min(longLines * 2, 5);
        score -= ded;
        issues << QString::fromUtf8("超过 120 列的行（%1 行）").arg(longLines);
    }

    // 运算符空格检查：简单检测 = 赋值两侧无空格（排除 ==, <=, >=, !=, +=, -=）
    int opIssues = 0;
    for (size_t i = 1; i + 1 < code.size(); ++i) {
        char prev = code[i - 1];
        char cur = code[i];
        char next = code[i + 1];
        if (cur == '=' && prev != '=' && next != '=' && prev != '!' && prev != '<' && prev != '>' && prev != '+' &&
            prev != '-' && prev != '*' && prev != '/' && prev != ' ' && prev != '\t' && prev != '\n' && next != ' ' &&
            next != '\t' && next != '\n' && next != '=') {
            ++opIssues;
        }
    }
    if (opIssues > 0) {
        int ded = std::min(opIssues, 3);
        score -= ded;
        issues << QString::fromUtf8("赋值运算符两侧缺少空格（%1 处）").arg(opIssues);
    }

    if (score < 0) {
        score = 0;
    }
    if (score > 20) {
        score = 20;
    }

    if (issues.isEmpty()) {
        note = QString::fromUtf8("代码风格良好");
    } else {
        note = issues.join(QString::fromUtf8("；"));
    }
    return score;
}

/// 渲染评分表（评分项/得分/满分/说明）与反馈报告 HTML。
void ExerciseGraderPanel::renderGradeReport(const std::vector<GradeItem>& items, const QString& feedback) {
    gradeTable_->setRowCount(static_cast<int>(items.size()));
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& it = items[i];
        auto* nameItem = new QTableWidgetItem(QString::fromUtf8(it.item.c_str()));
        auto* scoreItem = new QTableWidgetItem(QString::number(it.score));
        scoreItem->setTextAlignment(Qt::AlignCenter);
        auto* maxItem = new QTableWidgetItem(QString::number(it.maxScore));
        maxItem->setTextAlignment(Qt::AlignCenter);
        auto* noteItem = new QTableWidgetItem(QString::fromUtf8(it.note.c_str()));

        // 着色：满分绿色，零分红色，部分分橙色
        if (it.score == it.maxScore) {
            scoreItem->setForeground(QColor(QStringLiteral("#27AE60")));
        } else if (it.score == 0) {
            scoreItem->setForeground(QColor(QStringLiteral("#C0392B")));
        } else {
            scoreItem->setForeground(QColor(QStringLiteral("#E67E22")));
        }

        gradeTable_->setItem(i, 0, nameItem);
        gradeTable_->setItem(i, 1, scoreItem);
        gradeTable_->setItem(i, 2, maxItem);
        gradeTable_->setItem(i, 3, noteItem);
    }
    feedbackBrowser_->setHtml(feedback);
}

// ============================================================
// 三后端执行函数（参考 BackendParallelPanel::runInterpreter 等）
// ============================================================

/// 运行 Interpreter 树遍历后端：Lexer → Parser → Interpreter::execute(AST)。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runInterpreter(const std::string& src) {
    ExecResult r;

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + formatDiagnosticErrors(lex.getDiagnostics());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("语法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.errorMsg = QString::fromUtf8("语法错误: ") + err;
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Interpreter 执行
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });

    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        r.errorMsg = QString::fromUtf8("运行时错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.runtimeError = true;
        r.output = QString::fromUtf8(out.c_str());
        return r;
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("运行时错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.runtimeError = true;
        r.output = QString::fromUtf8(out.c_str());
        return r;
    }

    r.output = QString::fromUtf8(out.c_str());
    r.status = QString::fromUtf8("✅ 成功");
    r.success = true;
    return r;
}

/// 运行 StackVM（IR 路径）：Compiler(setUseIR(true)) → VM::execute(CompileResult)。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runStackVM_IR(const std::string& src) {
    ExecResult r;

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + formatDiagnosticErrors(lex.getDiagnostics());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("语法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.errorMsg = QString::fromUtf8("语法错误: ") + err;
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Compiler（IR 路径）
    Compiler compiler;
    compiler.setUseIR(true);
    CompileResult cr;
    try {
        cr = compiler.compile(*ast);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("编译错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        r.errorMsg = QString::fromUtf8("编译错误: ") + formatDiagnosticErrors(compiler.getDiagnostics());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // VM 执行
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });

    vm.execute(cr);
    r.output = QString::fromUtf8(out.c_str());
    if (vm.hasError()) {
        r.errorMsg = QString::fromUtf8("运行时错误: ") + QString::fromUtf8(vm.getLastError().c_str());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.runtimeError = true;
    } else {
        r.status = QString::fromUtf8("✅ 成功");
        r.success = true;
    }
    return r;
}

/// 运行 RegisterVM（IR 路径）：Compiler(setUseRegisterVM(true)) →
/// RegisterVM::execute(RegBytecodeChunk)。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runRegVM_IR(const std::string& src) {
    ExecResult r;

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        r.errorMsg = QString::fromUtf8("词法错误: ") + formatDiagnosticErrors(lex.getDiagnostics());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("语法错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.errorMsg = QString::fromUtf8("语法错误: ") + err;
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    // Compiler（寄存器 IR 路径）
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    try {
        compiler.compile(*ast);
    } catch (const std::exception& e) {
        r.errorMsg = QString::fromUtf8("编译错误: ") + QString::fromUtf8(e.what());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        r.errorMsg = QString::fromUtf8("编译错误: ") + formatDiagnosticErrors(compiler.getDiagnostics());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.compileError = true;
        return r;
    }

    const auto& regResult = compiler.getLastRegisterResult();

    // RegisterVM 执行
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });

    vm.execute(regResult);
    r.output = QString::fromUtf8(out.c_str());
    if (vm.hasError()) {
        r.errorMsg = QString::fromUtf8("运行时错误: ") + QString::fromUtf8(vm.getLastError().c_str());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
        r.runtimeError = true;
    } else {
        r.status = QString::fromUtf8("✅ 成功");
        r.success = true;
    }
    return r;
}
