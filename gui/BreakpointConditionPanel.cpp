// ============================================================
// BreakpointConditionPanel.cpp — 条件断点可视化面板实现（第二档 P1-2）
// ============================================================

#include "gui/BreakpointConditionPanel.h"
#include "app/IdeController.h"
#include "gui/PanelAnimator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <sstream>
#include <algorithm>

// ============================================================
// BreakpointConditionLibrary — 静态教学场景库
// ============================================================
// 8 个典型条件断点场景，覆盖：
//   - 简单变量比较（==/</>/!=）
//   - 字符串相等
//   - null 检查
//   - 复合布尔表达式
//   - 取模条件（每隔 N 次触发）
//   - 方法调用条件
//   - 异常对象检查
//   - 闭包变量检查
//
// 帮助学习者理解条件断点的求值机制：
//   - 条件在沙箱中求值（独立 Interpreter 实例）
//   - 求值结果为 truthy 时暂停，falsy 时继续
//   - 命中次数累计（即使条件 falsy 也计数）

const std::vector<BreakpointScenario>& BreakpointConditionLibrary::scenarios() {
    static const std::vector<BreakpointScenario> kScenarios = {
        {
            "simple-equality",
            "简单相等条件（i == 50）",
            "i == 50",
            "最基础的条件断点形式。当循环变量 i 等于 50 时暂停。"
            "条件在断点行命中时求值一次，结果为 truthy（非 0 / 非 null / 非 false）时暂停。",
            "循环执行到第 50 次迭代时暂停，其余迭代继续执行（不暂停）",
            "var i = 0;\nwhile (i < 100) {\n    i = i + 1;\n}\nprint i;"
        },
        {
            "modular-trigger",
            "取模触发条件（i % 100 == 0）",
            "i % 100 == 0",
            "每隔 100 次迭代暂停一次。常用于在大循环中观察周期性状态，"
            "避免每次迭代都暂停导致调试效率低下。",
            "i = 0, 100, 200, 300... 时暂停，共触发 10 次（假设循环 1000 次）",
            "var i = 0;\nwhile (i < 1000) {\n    i = i + 1;\n}\nprint i;"
        },
        {
            "string-equality",
            "字符串相等条件（s == \"target\"）",
            "s == \"target\"",
            "字符串相等比较。MiniLang 字符串相等基于值比较（非引用），"
            "条件断点在沙箱中求值时使用 Interpreter 的 EQ 实现。",
            "当变量 s 等于 \"target\" 时暂停",
            "var s = \"\";\nvar i = 0;\nwhile (i < 100) {\n    if (i == 50) s = \"target\";\n    i = i + 1;\n}\nprint s;"
        },
        {
            "null-check",
            "null 检查条件（x == null）",
            "x == null",
            "null 检查条件断点。常用于排查变量未初始化导致的运行时错误。"
            "MiniLang 中 null 是一等值，可与任何变量比较。",
            "当 x 为 null 时暂停（如未初始化或显式赋值为 null）",
            "var x = null;\nvar i = 0;\nwhile (i < 10) {\n    if (i == 5) x = 42;\n    i = i + 1;\n}\nprint x;"
        },
        {
            "compound-condition",
            "复合布尔条件（a > 0 && b < 100）",
            "a > 0 && b < 100",
            "复合布尔表达式条件断点。MiniLang 的 and/or 短路求值（返回操作数原值而非布尔），"
            "条件断点求值时遵循相同语义：a > 0 为 falsy 时不会求值 b < 100。",
            "当 a > 0 且 b < 100 同时成立时暂停",
            "var a = 5;\nvar b = 50;\nvar i = 0;\nwhile (i < 100) {\n    b = b + 1;\n    i = i + 1;\n}\nprint a + b;"
        },
        {
            "boolean-shortcut",
            "布尔短路条件（x && y > 0）",
            "x && y > 0",
            "利用 and 短路特性。当 x 为 truthy 时才会求值 y > 0。"
            "条件断点求值时若 x 为 falsy，整个表达式直接返回 x（短路），不计算 y。",
            "当 x truthy 且 y > 0 时暂停",
            "var x = 1;\nvar y = 10;\nvar i = 0;\nwhile (i < 100) {\n    if (i == 50) x = 0;\n    i = i + 1;\n}\nprint x && y;"
        },
        {
            "method-call-condition",
            "方法调用条件（this.value > 100）",
            "this.value > 100",
            "在方法内部设置条件断点，访问 this 的字段。"
            "条件求值时通过 boundInstance_ 缓存访问实例字段，与正常运行时语义一致。",
            "当方法被调用且 this.value > 100 时暂停",
            "class Counter {\n    var value;\n    fun new() { value = 0; }\n    fun inc() { value = value + 1; }\n}\nvar c = Counter.new();\nvar i = 0;\nwhile (i < 200) {\n    c.inc();\n    i = i + 1;\n}\nprint c.value;"
        },
        {
            "exception-condition",
            "异常对象检查（e.message == \"expected\")",
            "e.message == \"expected\"",
            "在 catch 块中设置条件断点，检查异常对象字段。"
            "MiniLang 异常通过 InstanceData 表示，字段访问通过 OP_GET_FIELD 完成，"
            "条件断点求值时与正常运行时路径一致。",
            "当 catch 块捕获异常且异常 message 字段等于 \"expected\" 时暂停",
            "fun risky(n) {\n    if (n < 0) throw \"invalid\";\n    if (n > 100) throw \"expected\";\n    return n;\n}\nvar i = 0;\nwhile (i < 200) {\n    try {\n        risky(i);\n    } catch (e) {\n        print e;\n    }\n    i = i + 1;\n}\nprint \"done\";"
        },
    };
    return kScenarios;
}

// ============================================================
// BreakpointConditionPanel 实现
// ============================================================

BreakpointConditionPanel::BreakpointConditionPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页面切换按钮
    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_    = new QPushButton(QString::fromUtf8("实时断点"), this);
    pageLibraryBtn_ = new QPushButton(QString::fromUtf8("教学场景库"), this);
    pageLiveBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageBar->addWidget(pageLiveBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* p1 = new QWidget(this);
    auto* p2 = new QWidget(this);
    buildLivePage(p1);
    buildLibraryPage(p2);
    stack_->addWidget(p1);
    stack_->addWidget(p2);

    pageLiveBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageLiveBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageLiveBtn_->setChecked(true);
        pageLibraryBtn_->setChecked(false);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
        refreshLive();
    });
    connect(pageLibraryBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        pageLibraryBtn_->setChecked(true);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });

    // 填充教学场景列表
    const auto& items = BreakpointConditionLibrary::scenarios();
    for (const auto& s : items) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty()) scenarioList_->setCurrentRow(0);

    // 启动实时断点列表轮询定时器（500ms）
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, &BreakpointConditionPanel::refreshLive);
    refreshTimer_->start();
}

void BreakpointConditionPanel::buildLivePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    liveStatusLabel_ = new QLabel(QString::fromUtf8("未绑定控制器"), host);
    layout->addWidget(liveStatusLabel_);

    // 4 列：行号 / 条件表达式 / 命中次数 / 状态
    breakpointTable_ = new QTableWidget(0, 4, host);
    breakpointTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    breakpointTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("行号"),
        QString::fromUtf8("条件表达式"),
        QString::fromUtf8("命中次数"),
        QString::fromUtf8("状态"),
    });
    breakpointTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    breakpointTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    breakpointTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    breakpointTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(breakpointTable_, 1);
}

void BreakpointConditionPanel::buildLibraryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(host);
    scenarioDetail_ = new QTextBrowser(host);
    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter);

    connect(scenarioList_, &QListWidget::currentRowChanged,
            this, &BreakpointConditionPanel::populateScenarioDetail);
}

void BreakpointConditionPanel::refreshLive() {
    if (!controller_) {
        liveStatusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }

    QSet<int> breakpoints = controller_->getBreakpoints();
    if (breakpoints.isEmpty()) {
        liveStatusLabel_->setText(QString::fromUtf8("无断点"));
        breakpointTable_->setRowCount(0);
        return;
    }

    // 按行号排序展示
    QList<int> sortedLines = breakpoints.values();
    std::sort(sortedLines.begin(), sortedLines.end());
    breakpointTable_->setRowCount(sortedLines.size());

    bool isRunning = controller_->isRunning();
    bool isDebugRun = controller_->isDebugRun();
    bool isVmRunning = controller_->isVmRunning();
    QString modeText;
    if (isVmRunning) {
        modeText = controller_->isVmRegisterMode() ? QString::fromUtf8("RegisterVM 调试中")
                                                   : QString::fromUtf8("StackVM 调试中");
    } else if (isRunning && isDebugRun) {
        modeText = QString::fromUtf8("Interpreter 调试中");
    } else {
        modeText = QString::fromUtf8("未运行");
    }
    liveStatusLabel_->setText(QString::fromUtf8("共 %1 个断点 | %2")
        .arg(sortedLines.size()).arg(modeText));

    for (int i = 0; i < sortedLines.size(); ++i) {
        int line = sortedLines[i];
        std::string cond = controller_->getBreakpointCondition(line);
        int hitCount = controller_->getBreakpointHitCount(line);

        breakpointTable_->setItem(i, 0, new QTableWidgetItem(QString::number(line)));
        breakpointTable_->setItem(i, 1, new QTableWidgetItem(
            cond.empty() ? QString::fromUtf8("（无条件）") : QString::fromUtf8(cond.c_str())));
        breakpointTable_->setItem(i, 2, new QTableWidgetItem(QString::number(hitCount)));
        // 状态：条件断点 vs 普通断点
        QString status = cond.empty() ? QString::fromUtf8("普通断点")
                                       : QString::fromUtf8("条件断点");
        breakpointTable_->setItem(i, 3, new QTableWidgetItem(status));
    }
}

void BreakpointConditionPanel::populateScenarioDetail(int index) {
    const auto& items = BreakpointConditionLibrary::scenarios();
    if (index < 0 || index >= (int)items.size()) return;
    const auto& s = items[index];

    std::ostringstream os;
    os << "<h3>" << s.title << "</h3>";
    os << "<p><b>条件表达式：</b> <code>" << s.condition << "</code></p>";
    os << "<hr>";
    os << "<p>" << s.description << "</p>";
    os << "<p><b>触发行为：</b> " << s.expectedBehavior << "</p>";
    os << "<h4>示例代码：</h4>";
    os << "<pre style='background:#f5f5f5; padding:8px; font-family:Consolas;'>" << s.sampleCode << "</pre>";
    scenarioDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
    PanelAnimator::fadeInWidget(scenarioDetail_);
}
