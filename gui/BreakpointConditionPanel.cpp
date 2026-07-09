// ============================================================
// BreakpointConditionPanel.cpp — 条件断点可视化面板实现（第二档 P1-2）
// ============================================================

#include "gui/BreakpointConditionPanel.h"
#include "app/IdeController.h"
#include "gui/GuidedTour.h"
#include "gui/PanelAnimator.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <sstream>

#include "Label.h"                // QFluentKit（CaptionLabel）
#include "Theme.h"                // QFluentKit（onThemeModeChanged 信号）
#include "gui/MarkdownRenderer.h" // 散文式说明统一 Markdown 渲染
#include "gui/TeachingTheme.h"

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
        {"simple-equality", "🔴 简单相等条件（i == 50）", "i == 50",
         "🔴 最基础的条件断点形式。当循环变量 i 等于 50 时暂停。"
         "条件在断点行命中时求值一次，结果为 truthy（非 0 / 非 null / 非 false）时暂停。",
         "循环执行到第 50 次迭代时暂停，其余迭代继续执行（不暂停）",
         "var i = 0;\nwhile (i < 100) {\n    i = i + 1;\n}\nprint(i);"},
        {"modular-trigger", "🎯 取模触发条件（i % 100 == 0）", "i % 100 == 0",
         "🎯 每隔 100 次迭代暂停一次。常用于在大循环中观察周期性状态，"
         "避免每次迭代都暂停导致调试效率低下。",
         "i = 0, 100, 200, 300... 时暂停，共触发 10 次（假设循环 1000 次）",
         "var i = 0;\nwhile (i < 1000) {\n    i = i + 1;\n}\nprint(i);"},
        {"string-equality", "🎯 字符串相等条件（s == \"target\"）", "s == \"target\"",
         "🎯 字符串相等比较。MiniLang 字符串相等基于值比较（非引用），"
         "条件断点在沙箱中求值时使用 Interpreter 的 EQ 实现。",
         "当变量 s 等于 \"target\" 时暂停",
         "var s = \"\";\nvar i = 0;\nwhile (i < 100) {\n    if (i == 50) s = \"target\";\n    i = i + "
         "1;\n}\nprint(s);"},
        {"null-check", "🔴 null 检查条件（x == null）", "x == null",
         "🔴 null 检查条件断点。常用于排查变量未初始化导致的运行时错误。"
         "MiniLang 中 null 是一等值，可与任何变量比较。",
         "当 x 为 null 时暂停（如未初始化或显式赋值为 null）",
         "var x = null;\nvar i = 0;\nwhile (i < 10) {\n    if (i == 5) x = 42;\n    i = i + 1;\n}\nprint(x);"},
        {"compound-condition", "⚙️ 复合布尔条件（a > 0 && b < 100）", "a > 0 && b < 100",
         "⚙️ 复合布尔表达式条件断点。MiniLang 的 and/or 短路求值（返回操作数原值而非布尔），"
         "条件断点求值时遵循相同语义：a > 0 为 falsy 时不会求值 b < 100。",
         "当 a > 0 且 b < 100 同时成立时暂停",
         "var a = 5;\nvar b = 50;\nvar i = 0;\nwhile (i < 100) {\n    b = b + 1;\n    i = i + 1;\n}\nprint(a + b);"},
        {"boolean-shortcut", "⚙️ 布尔短路条件（x && y > 0）", "x && y > 0",
         "⚙️ 利用 and 短路特性。当 x 为 truthy 时才会求值 y > 0。"
         "条件断点求值时若 x 为 falsy，整个表达式直接返回 x（短路），不计算 y。",
         "当 x truthy 且 y > 0 时暂停",
         "var x = 1;\nvar y = 10;\nvar i = 0;\nwhile (i < 100) {\n    if (i == 50) x = 0;\n    i = i + 1;\n}\nprint(x "
         "&& y);"},
        {"method-call-condition", "🎯 方法调用条件（this.value > 100）", "this.value > 100",
         "🎯 在方法内部设置条件断点，访问 this 的字段。"
         "条件求值时通过 boundInstance_ 缓存访问实例字段，与正常运行时语义一致。"
         "MiniLang 类构造使用 ClassName(args) 语法（非 .new()）。",
         "当方法被调用且 this.value > 100 时暂停",
         "class Counter {\n    var value;\n    fun init() { value = 0; }\n    fun inc() { value = value + 1; }\n}\nvar "
         "c = Counter();\nvar i = 0;\nwhile (i < 200) {\n    c.inc();\n    i = i + 1;\n}\nprint(c.value);"},
        {"exception-condition", "🛑 异常值检查（e == \"expected\")", "e == \"expected\"",
         "🛑 在 catch 块中设置条件断点，检查捕获的异常值。"
         "MiniLang 的 throw 可抛出任意值（字符串/数字/对象），catch 变量绑定该值，"
         "条件断点求值时直接比较该值，与正常运行时路径一致。",
         "当 catch 块捕获异常且异常值等于 \"expected\" 时暂停",
         "fun risky(n) {\n    if (n < 0) throw \"invalid\";\n    if (n > 100) throw \"expected\";\n    return "
         "n;\n}\nvar i = 0;\nwhile (i < 200) {\n    try {\n        risky(i);\n    } catch (e) {\n        print(e);\n   "
         " }\n    i = i + 1;\n}\nprint(\"done\");"},
    };
    return kScenarios;
}

// ============================================================
// BreakpointConditionPanel 实现
// ============================================================

BreakpointConditionPanel::BreakpointConditionPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页面切换按钮
    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_ = new QPushButton(QString::fromUtf8("实时断点"), this);
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
        PanelAnimator::slideInWidget(stack_->currentWidget());
        refreshLive();
    });
    connect(pageLibraryBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        pageLibraryBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // 填充教学场景列表
    const auto& items = BreakpointConditionLibrary::scenarios();
    for (const auto& s : items) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty())
        scenarioList_->setCurrentRow(0);

    // OPT-1: 实时断点列表轮询定时器降频 500ms→2000ms。断点变化/步进/暂停由
    // vmStateChanged 监听器（见 setController）即时触发 refreshLive，QTimer 仅作安全网。
    // 注：不在构造时自动启动，改为在 showEvent 中启动 / hideEvent 中停止
    // 避免 dock 隐藏时 QTimer 永久空转浪费 CPU（修复卡顿问题）
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(2000);
    connect(refreshTimer_, &QTimer::timeout, this, &BreakpointConditionPanel::refreshLive);
    // refreshTimer_->start() 移至 showEvent

    // 主题切换时刷新教学场景详情 HTML（populateScenarioDetail 中 <pre> 背景使用
    // TeachingTheme::surface()，需重新渲染以跟随新主题）。
    // receiver=this 保证生命周期安全，析构自动断开。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (scenarioList_ && scenarioDetail_) {
            populateScenarioDetail(scenarioList_->currentRow());
        }
    });
}

void BreakpointConditionPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 面板显示时启动轮询，首次立即刷新一次
    if (refreshTimer_ && !refreshTimer_->isActive()) {
        refreshLive();
        refreshTimer_->start();
    }
}

void BreakpointConditionPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // 面板隐藏时停止轮询，避免后台空转
    if (refreshTimer_ && refreshTimer_->isActive()) {
        refreshTimer_->stop();
    }
}

void BreakpointConditionPanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // AUDIT-P0 fix: 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

// AUDIT-P0 fix: 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调。
BreakpointConditionPanel::~BreakpointConditionPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

void BreakpointConditionPanel::buildLivePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    liveStatusLabel_ = new CaptionLabel(QString::fromUtf8("未绑定控制器"), host);
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
    layout->setSpacing(4);
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(host);
    scenarioDetail_ = new QTextBrowser(host);
    // R52-7 fix: 显式设置 setOpenExternalLinks(false)，对齐其他面板模式
    scenarioDetail_->setOpenExternalLinks(false);
    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter, 1);

    // 加载示例代码按钮：将当前场景的 sampleCode 加载到编辑器
    loadSampleBtn_ = new QPushButton(QString::fromUtf8("加载示例代码到编辑器"), host);
    loadSampleBtn_->setFixedHeight(28);
    layout->addWidget(loadSampleBtn_);
    connect(loadSampleBtn_, &QPushButton::clicked, this, [this]() {
        if (!currentSampleCode_.empty()) {
            emit loadSampleRequested(QString::fromUtf8(currentSampleCode_.c_str()));
        }
    });

    connect(scenarioList_, &QListWidget::currentRowChanged, this, &BreakpointConditionPanel::populateScenarioDetail);
}

void BreakpointConditionPanel::refreshLive() {
    if (!controller_) {
        liveStatusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }
    // R52-3 fix: 补充 Interpreter 调试 resume 期间的状态守卫。原仅检查 VM 路径，
    // 缺 Interpreter 调试路径——resume 期间 worker 线程活跃，可能并发更新
    // debugCoord_ 内部的 breakpointHitCounts_，refreshLive 读取会触发 UB。
    // 对齐 CallStackPanel / VariableInspectorPanel 的双路径守卫模式。
    if (controller_->isRunning() && controller_->isDebugRun() && !controller_->isDebugPaused()) {
        liveStatusLabel_->setText(QString::fromUtf8("状态：调试运行中（暂停后刷新）"));
        return;
    }
    // AUDIT-P2 fix: VM 运行期间读断点元数据（getBreakpointCondition/getBreakpointHitCount）
    // 可能不一致，对齐 CallStackPanel / VariableInspectorPanel 模式，运行中且未暂停时早退。
    if (controller_->isVmRunning() && !controller_->isDebugPaused()) {
        liveStatusLabel_->setText(QString::fromUtf8("状态：VM 运行中（暂停后刷新）"));
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
    liveStatusLabel_->setText(QString::fromUtf8("共 %1 个断点 | %2").arg(sortedLines.size()).arg(modeText));

    for (int i = 0; i < sortedLines.size(); ++i) {
        int line = sortedLines[i];
        std::string cond = controller_->getBreakpointCondition(line);
        int hitCount = controller_->getBreakpointHitCount(line);

        breakpointTable_->setItem(i, 0, new QTableWidgetItem(QString::number(line)));
        breakpointTable_->setItem(
            i, 1,
            new QTableWidgetItem(cond.empty() ? QString::fromUtf8("（无条件）") : QString::fromUtf8(cond.c_str())));
        breakpointTable_->setItem(i, 2, new QTableWidgetItem(QString::number(hitCount)));
        // 状态：条件断点 vs 普通断点
        QString status = cond.empty() ? QString::fromUtf8("普通断点") : QString::fromUtf8("条件断点");
        breakpointTable_->setItem(i, 3, new QTableWidgetItem(status));
    }
}

void BreakpointConditionPanel::populateScenarioDetail(int index) {
    const auto& items = BreakpointConditionLibrary::scenarios();
    if (index < 0 || index >= (int)items.size()) {
        currentSampleCode_.clear();
        return;
    }
    const auto& s = items[index];
    currentSampleCode_ = s.sampleCode; // 保存当前示例代码供加载按钮使用

    // 散文式字段（description）走 Markdown 渲染，支持 **粗体** / `code` / 列表
    QString descFragment = MarkdownRenderer::markdownToHtmlFragment(s.description);

    // AUDIT-P2 fix: HTML 转义——sampleCode 含 < > 字符（如 if (n < 0)），
    // condition 含 < >（如 a > 0 && b < 100）。未转义会导致 HTML 标签注入/渲染错误。
    auto escapeHtml = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    };

    std::ostringstream os;
    os << "<h3>" << escapeHtml(s.title) << "</h3>";
    os << "<p><b>条件表达式：</b> <code>" << escapeHtml(s.condition) << "</code></p>";
    os << "<hr>";
    os << descFragment.toStdString();
    os << "<p><b>触发行为：</b> " << escapeHtml(s.expectedBehavior) << "</p>";
    os << "<h4>示例代码：</h4>";
    os << "<pre style='background:" << TeachingTheme::surface().name().toStdString()
       << "; padding:8px; font-family:Consolas;'>" << escapeHtml(s.sampleCode) << "</pre>";
    scenarioDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* BreakpointConditionPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    // 注：只高亮「始终可见」的页切换按钮，概念性步骤用 nullptr（居中气泡）+ 示例代码。
    // 不高亮 breakpointTable_/loadSampleBtn_ 等位于 QStackedWidget 某一页的控件，
    // 避免目标页未显示时 mapTo 返回错误坐标导致气泡定位混乱。
    tour->addStep(pageLiveBtn_, QString::fromUtf8("实时断点"),
                  QString::fromUtf8("「实时断点」页在调试时显示所有断点的条件 / 命中次数 / 状态。"
                                    "每行一个断点，可查看行号 / 条件表达式 / 启用状态。"));
    tour->addStep(nullptr, QString::fromUtf8("示例代码：条件断点"),
                  QString::fromUtf8(
                      "<p>将以下代码粘贴到编辑器，在 print 行设条件断点观察命中：</p>"
                      "<pre style='background:#EEE8D5;padding:8px;border-radius:4px;font-family:Consolas,monospace;'>"
                      "for (var i = 0; i < 100; i = i + 1) {\n"
                      "    if (i % 10 == 0) {\n"
                      "        print i;\n"
                      "    }\n"
                      "}\n"
                      "</pre>"
                      "<p>在编辑器行号区点击设置断点，右键可编辑条件（如 i==50 或 i%100==0），"
                      "按 F5 调试观察条件命中行为。</p>"));
    tour->addStep(pageLibraryBtn_, QString::fromUtf8("教学场景库"),
                  QString::fromUtf8("点击「教学场景库」切换到静态教学页，查看 i==50 / i%100==0 等条件断点示例。"));
    tour->addStep(nullptr, QString::fromUtf8("开始实验"),
                  QString::fromUtf8("切换到教学场景库后，选中场景，点击「加载示例」载入编辑器，"
                                    "按 F5 调试，在编辑器行号区点击设置断点，观察命中行为。"));
    return tour;
}
