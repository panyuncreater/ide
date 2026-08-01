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

#include "common/BackendExecutionService.h" // ARCH-10: 后端执行服务中间层

#include <QApplication>
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
#include <cctype>
#include <sstream>
#include <utility>

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
          {"-1+1", "fun add(a, b) { return a + b; } print(add(-1, 1));", "0"},
          // 拓展二期：隐藏用例（大数+负数组合，防硬编码骗分）
          {"隐藏用例", "fun add(a, b) { return a + b; } print(add(123456, -456));", "123000", true}}},
        // 2. 阶乘计算
        {"阶乘计算",
         "<h3>题目：阶乘计算</h3>"
         "<p>实现一个函数 <code>fact(n)</code>，返回 n 的阶乘（n!）。</p>"
         "<p><b>要求：</b>使用递归，当 n &le; 1 时返回 1。</p>",
         "递归终止条件 <code>if (n &lt;= 1) { return 1; }</code>。",
         "fun fact(n) {\n    if (n <= 1) {\n        return 1;\n    }\n    return n * fact(n-1);\n}\nprint(fact(5));",
         {{"fact(5)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(5));", "120"},
          {"fact(1)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(1));", "1"},
          {"fact(6)", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(6));", "720"},
          // 拓展二期：隐藏用例（边界 n=0）
          {"隐藏用例", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(0));", "1", true}}},
        // 3. 数组求和
        // 拓展二期修复（题库自校验测试发现）：MiniLang 无 for-in 语法，
        // 原题面/用例的 「for (var x in arr)」 全部语法错误，学生按题目写必然 0 分。
        // 改为索引遍历 + arr.len()（与引擎实际支持的语法一致）。
        {"数组求和",
         "<h3>题目：数组求和</h3>"
         "<p>实现一个函数 <code>sum(arr)</code>，返回数组所有元素之和。</p>"
         "<p><b>要求：</b>使用 <code>arr.len()</code> 获取长度，索引遍历数组。</p>",
         "初始化累加器 <code>var s = 0;</code>，循环中 <code>s = s + arr[i];</code>。",
         "fun sum(arr) {\n    var s = 0;\n    for (var i = 0; i < arr.len(); i = i + 1) {\n        s = s + "
         "arr[i];\n    }\n    return s;\n}\nprint(sum([1,2,3,4,5]));",
         {{"[1,2,3,4,5]",
           "fun sum(arr) { var s = 0; for (var i = 0; i < arr.len(); i = i + 1) { s = s + arr[i]; } return s; } "
           "print(sum([1,2,3,4,5]));",
           "15"},
          {"[10,20,30]",
           "fun sum(arr) { var s = 0; for (var i = 0; i < arr.len(); i = i + 1) { s = s + arr[i]; } return s; } "
           "print(sum([10,20,30]));",
           "60"},
          {"[]",
           "fun sum(arr) { var s = 0; for (var i = 0; i < arr.len(); i = i + 1) { s = s + arr[i]; } return s; } "
           "print(sum([]));",
           "0"},
          // 拓展二期：隐藏用例（含负数与重复元素）
          {"隐藏用例",
           "fun sum(arr) { var s = 0; for (var i = 0; i < arr.len(); i = i + 1) { s = s + arr[i]; } return s; } "
           "print(sum([-5,5,7,7,-14]));",
           "0", true}}},
        // 4. 字符串反转
        // 拓展二期修复（题库自校验测试发现）：字符串方法是 len() 而非 length()，
        // 原题面/用例的 s.length() 全部运行时错误。
        {"字符串反转",
         "<h3>题目：字符串反转</h3>"
         "<p>实现一个函数 <code>reverse(s)</code>，返回字符串 s 的反转。</p>"
         "<p><b>要求：</b>使用 <code>s.len()</code> 获取长度，<code>s[i]</code> 索引字符。</p>",
         "从末尾向前遍历，<code>r = r + s[i];</code> 逐字符拼接。",
         "fun reverse(s) {\n    var r = \"\";\n    for (var i = s.len()-1; i >= 0; i = i - 1) {\n        r = r + "
         "s[i];\n    }\n    return r;\n}\nprint(reverse(\"hello\"));",
         {{"hello",
           "fun reverse(s) { var r = \"\"; for (var i = s.len()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"hello\"));",
           "olleh"},
          {"abc",
           "fun reverse(s) { var r = \"\"; for (var i = s.len()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"abc\"));",
           "cba"},
          {"a",
           "fun reverse(s) { var r = \"\"; for (var i = s.len()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"a\"));",
           "a"},
          // 拓展二期：隐藏用例（回文串，反转后相同——硬编码 "olleh" 骗不过）
          {"隐藏用例",
           "fun reverse(s) { var r = \"\"; for (var i = s.len()-1; i >= 0; i = i - 1) { r = r + s[i]; } return r; } "
           "print(reverse(\"racecar\"));",
           "racecar", true}}},
    };
    return kExercises;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

// ARCH-10: 原 formatDiagnosticErrors 函数已随 runInterpreter/runStackVM_IR/
// runRegVM_IR 重构移除——错误信息现在由 BackendExecutionService 直接通过
// BackendExecResult.errorMsg 提供，无需面板自行格式化 DiagnosticBag。

namespace {

/// 判断 src[pos..] 是否以关键字 kw 开头且其前后为单词边界（非标识符字符）。
bool matchKeywordAt(const std::string& src, size_t pos, const std::string& kw) {
    if (pos + kw.size() > src.size())
        return false;
    if (src.compare(pos, kw.size(), kw) != 0)
        return false;
    if (pos > 0) {
        char pc = src[pos - 1];
        if (std::isalnum(static_cast<unsigned char>(pc)) || pc == '_')
            return false;
    }
    size_t after = pos + kw.size();
    if (after < src.size()) {
        char nc = src[after];
        if (std::isalnum(static_cast<unsigned char>(nc)) || nc == '_')
            return false;
    }
    return true;
}

/// 把源码按顶层结构切成「函数定义」与「其余顶层语句（调用）」两部分。
///   - 顶层 `fun 名(...) {...}`（含可选结尾 ;）归入 first（defs）
///   - 其余顶层语句（如 print(...);）归入 second（rest / 调用）
/// 花括号与字符串内容均被跳过，不误计嵌套深度。
/// 用途：学生代码提供函数实现，测试用例参考代码提供调用；把两者组合后运行，
/// 才能真正以学生实现判分（原实现直接跑参考代码 tc.code，与学生代码无关，恒满分）。
std::pair<std::string, std::string> splitDefsAndRest(const std::string& src) {
    std::string defs;
    std::string rest;
    size_t i = 0;
    const size_t n = src.size();
    while (i < n) {
        char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (matchKeywordAt(src, i, "fun")) {
            const size_t start = i;
            size_t j = i;
            bool inStr = false;
            char strCh = 0;
            // 推进到函数体起始 '{'
            while (j < n) {
                char cj = src[j];
                if (inStr) {
                    if (cj == '\\') {
                        j += 2;
                        continue;
                    }
                    if (cj == strCh)
                        inStr = false;
                    ++j;
                    continue;
                }
                if (cj == '"' || cj == '\'') {
                    inStr = true;
                    strCh = cj;
                    ++j;
                    continue;
                }
                if (cj == '{')
                    break;
                ++j;
            }
            if (j >= n) { // 无函数体（异常）：剩余全归 rest
                rest.append(src, start, n - start);
                i = n;
                break;
            }
            // 从 '{' 做花括号匹配
            int depth = 0;
            inStr = false;
            strCh = 0;
            while (j < n) {
                char cj = src[j];
                if (inStr) {
                    if (cj == '\\') {
                        j += 2;
                        continue;
                    }
                    if (cj == strCh)
                        inStr = false;
                    ++j;
                    continue;
                }
                if (cj == '"' || cj == '\'') {
                    inStr = true;
                    strCh = cj;
                    ++j;
                    continue;
                }
                if (cj == '{') {
                    ++depth;
                } else if (cj == '}') {
                    --depth;
                    if (depth == 0) {
                        ++j;
                        break;
                    }
                }
                ++j;
            }
            // 吞掉紧随的可选 ';' 与前导空白
            while (j < n && std::isspace(static_cast<unsigned char>(src[j])))
                ++j;
            if (j < n && src[j] == ';')
                ++j;
            defs.append(src, start, j - start);
            defs.push_back('\n');
            i = j;
        } else {
            // 顶层语句：推进到 depth 0 的分号（尊重字符串与括号）
            const size_t start = i;
            int depth = 0;
            bool inStr = false;
            char strCh = 0;
            size_t j = i;
            while (j < n) {
                char cj = src[j];
                if (inStr) {
                    if (cj == '\\') {
                        j += 2;
                        continue;
                    }
                    if (cj == strCh)
                        inStr = false;
                    ++j;
                    continue;
                }
                if (cj == '"' || cj == '\'') {
                    inStr = true;
                    strCh = cj;
                    ++j;
                    continue;
                }
                if (cj == '{' || cj == '[' || cj == '(') {
                    ++depth;
                } else if (cj == '}' || cj == ']' || cj == ')') {
                    --depth;
                } else if (cj == ';' && depth == 0) {
                    ++j;
                    break;
                }
                ++j;
            }
            rest.append(src, start, j - start);
            rest.push_back('\n');
            i = j;
        }
    }
    return {defs, rest};
}

} // namespace

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

    // 初始化测试用例表（拓展二期：隐藏用例不展示输入与期望输出）
    testTable_->setRowCount(static_cast<int>(ex.testCases.size()));
    for (int i = 0; i < static_cast<int>(ex.testCases.size()); ++i) {
        const auto& tc = ex.testCases[i];
        auto* nameItem = new QTableWidgetItem(tc.hidden ? QString::fromUtf8("🔒 ") + QString::fromUtf8(tc.name.c_str())
                                                        : QString::fromUtf8(tc.name.c_str()));
        nameItem->setTextAlignment(Qt::AlignCenter);
        testTable_->setItem(i, 0, nameItem);
        testTable_->setItem(
            i, 1, new QTableWidgetItem(tc.hidden ? QString::fromUtf8("（隐藏）") : QString::fromUtf8(tc.code.c_str())));
        auto* expItem = new QTableWidgetItem(tc.hidden ? QString::fromUtf8("（隐藏）")
                                                       : QString::fromUtf8(tc.expectedOutput.c_str()));
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

    // 防重入：串行执行多用例 + 三后端期间 processEvents 不排除信号/定时器，
    // 快速双击「提交评分」会触发重复评分与结果错乱。
    if (grading_)
        return;
    grading_ = true;
    // UX：评分期间禁用按钮 + 文案提示，避免误认为无响应
    const QString submitOrigText = submitBtn_ ? submitBtn_->text() : QString();
    if (submitBtn_) {
        submitBtn_->setEnabled(false);
        submitBtn_->setText(tr("评分中\xE2\x80\xA6"));
    }
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // 评分准确性修复：以学生代码判分——提取学生的函数定义，与每个测试用例的
    // 调用组合后运行。原实现直接运行参考代码 tc.code（内嵌参考解），与学生代码无
    // 关，导致测试项恒为满分（无论学生写什么）。
    const std::string studentCode = codeEdit_->toPlainText().toStdString();
    const std::string studentDefs = splitDefsAndRest(studentCode).first;

    // 1. 运行测试用例（Interpreter 后端校验输出）
    int passed = 0;
    QStringList passDetails;
    QStringList failDetails;
    for (int i = 0; i < total; ++i) {
        const auto& tc = ex.testCases[i];
        // 提取参考用例的调用部分，与学生的函数定义组合
        const std::string invocation = splitDefsAndRest(tc.code).second;
        std::string program = studentDefs;
        program += "\n";
        program += invocation;
        ExecResult result = runTestCase(program);
        QString actual = result.output.trimmed();
        QString expected = QString::fromUtf8(tc.expectedOutput.c_str()).trimmed();
        bool pass = result.success && (actual == expected);

        QString actualDisplay = actual;
        if (!result.success && actual.isEmpty()) {
            actualDisplay = QString::fromUtf8("（错误）");
        }
        // 拓展二期：隐藏用例通过时实际输出列也隐藏（实际==期望，展示即泄漏）；
        // 失败时展示学生自己代码的输出，不构成期望值泄漏
        if (tc.hidden && pass) {
            actualDisplay = QString::fromUtf8("（隐藏）");
        }
        auto* actualItem = new QTableWidgetItem(actualDisplay);
        auto* statusItem = new QTableWidgetItem(pass ? QString::fromUtf8("✅ 通过") : QString::fromUtf8("❌ 失败"));
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (pass) {
            statusItem->setForeground(QColor(QStringLiteral("#27AE60")));
            ++passed;
            // 拓展二期：隐藏用例通过时也不泄漏期望/实际值
            if (tc.hidden) {
                passDetails << QString::fromUtf8("%1: ✅ 隐藏用例通过").arg(QString::fromUtf8(tc.name.c_str()));
            } else {
                passDetails << QString::fromUtf8("%1: ✅ 期望「%2」, 实际「%3」")
                                   .arg(QString::fromUtf8(tc.name.c_str()))
                                   .arg(expected)
                                   .arg(actual);
            }
        } else {
            statusItem->setForeground(QColor(QStringLiteral("#C0392B")));
            QString actualForDetail = actual.isEmpty() ? QString::fromUtf8("（无输出）") : actual;
            // 拓展二期：隐藏用例失败时不泄漏期望值（防针对性硬编码）
            if (tc.hidden) {
                failDetails << QString::fromUtf8("%1: ❌ 输出与隐藏用例期望不符（期望值不公开）")
                                   .arg(QString::fromUtf8(tc.name.c_str()));
            } else {
                failDetails << QString::fromUtf8("%1: ❌ 期望「%2」, 实际「%3」")
                                   .arg(QString::fromUtf8(tc.name.c_str()))
                                   .arg(expected)
                                   .arg(actualForDetail);
            }
        }
        testTable_->setItem(i, 3, actualItem);
        testTable_->setItem(i, 4, statusItem);
        // UX：多用例串行执行期间让出事件循环，保持界面响应
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    // 2. 三后端执行详情（学生代码）
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

    // 测试用例通过率（满分 60，拓展二期：按比例计分——用例数从 3 个增到
    // 3 公开 + 1 隐藏，原「每例 20 分」硬编码不再适用）
    int testScore = total > 0 ? (passed * 60) / total : 0;
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
    // 评分准确性修复：编译未通过时运行时无从检测，不应白送 10 分。
    bool hasRuntimeError = interpR.runtimeError;
    int runtimeScore;
    std::string runtimeNote;
    if (hasCompileError) {
        runtimeScore = 0;
        runtimeNote = QString::fromUtf8("编译未通过，运行时未检测").toStdString();
    } else if (hasRuntimeError) {
        runtimeScore = 0;
        runtimeNote = interpR.errorMsg.toStdString();
    } else {
        runtimeScore = 10;
        runtimeNote = QString::fromUtf8("无运行时错误").toStdString();
    }
    items.push_back({"运行时错误", runtimeScore, 10, runtimeNote});

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

    // 恢复按钮与重入守卫
    if (submitBtn_) {
        submitBtn_->setText(submitOrigText);
        submitBtn_->setEnabled(true);
    }
    grading_ = false;
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
// 三后端执行函数（ARCH-10 重构：通过 BackendExecutionService 触发）
// ============================================================
// 面板不再直接依赖 Lexer/Parser/Compiler/VM/RegisterVM/Interpreter 等
// 内部头文件，统一通过 BackendExecutionService 中间层触发执行流程。
// 服务返回的 BackendExecResult 包含 success/output/errorPrefix/errorMsg，
// 这里转换为面板内部使用的 ExecResult 结构（含 compileError/runtimeError 标志）。

namespace {

/// 将服务层 BackendExecResult 转换为面板内部 ExecResult
ExerciseGraderPanel::ExecResult convertServiceResult(const ::BackendExecResult& sr) {
    ExerciseGraderPanel::ExecResult r;
    r.output = QString::fromUtf8(sr.output.c_str());
    if (sr.success) {
        r.status = QString::fromUtf8("✅ 成功");
        r.success = true;
    } else {
        // 根据 errorPrefix 判定错误阶段
        bool isCompileStage =
            (sr.errorPrefix == "词法错误" || sr.errorPrefix == "语法错误" || sr.errorPrefix == "编译错误");
        r.compileError = isCompileStage;
        r.runtimeError = !isCompileStage; // "运行时错误"
        r.errorMsg = QString::fromUtf8(sr.errorPrefix.c_str()) + QString::fromUtf8(": ") +
                     QString::fromUtf8(sr.errorMsg.c_str());
        r.status = QString::fromUtf8("❌ ") + r.errorMsg;
    }
    return r;
}

} // namespace

/// 运行 Interpreter 树遍历后端。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runInterpreter(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::Interpreter));
}

/// 运行 StackVM（IR 路径）。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runStackVM_IR(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::StackVM_IR));
}

/// 运行 RegisterVM（IR 路径）。
ExerciseGraderPanel::ExecResult ExerciseGraderPanel::runRegVM_IR(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::RegisterVM_IR));
}
