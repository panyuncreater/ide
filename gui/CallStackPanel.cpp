// ============================================================
// CallStackPanel.cpp — 调用栈可视化面板实现（第三波 P0-1）
// ============================================================

#include "gui/CallStackPanel.h"
#include "gui/GuidedTour.h"
#include "gui/PanelAnimator.h"
#include "gui/MarkdownRenderer.h"
#include "app/IdeController.h"
#include "interpreter/Value.h"
#include "debug/DebugTypes.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QApplication>
#include <sstream>

#include "Label.h"   // QFluentKit（CaptionLabel）

// ============================================================
// CallStackLibrary — 静态教学场景库
// ============================================================

const std::vector<CallStackScenario>& CallStackLibrary::scenarios() {
    static const std::vector<CallStackScenario> kScenarios = {
        CallStackScenario{
            "simple-call",
            "📞 简单函数调用",
            "📞 顶层调用 foo()，栈只有 2 帧：main + foo。",
            "fun foo(n) { return n + 1; }\nprint(foo(10));\n",
            {"<main>", "foo"},
            "💡 最基础的调用栈形态：main 调用 foo，foo 返回后栈帧弹出。"
        },
        CallStackScenario{
            "recursion",
            "🔄 递归调用（fib）",
            "🔄 fib(5) 递归展开，每层调用产生一个新栈帧，最大深度等于入参。",
            "fun fib(n) {\n  if (n < 2) return n;\n  return fib(n-1) + fib(n-2);\n}\nprint(fib(5));\n",
            {"<main>", "fib(5)", "fib(4)", "fib(3)", "fib(2)", "fib(1)"},
            "💡 递归调用栈：每一帧持有独立的 n 值，递归返回时帧逐层弹出。栈深度等于递归深度，过深会触发 MAX_RECURSION_DEPTH=256 保护。"
        },
        CallStackScenario{
            "closure-capture",
            "📦 闭包 upvalue 捕获",
            "📦 makeCounter 返回闭包，闭包帧捕获外层 count 变量。",
            "fun makeCounter() {\n  var count = 0;\n  fun inc() { count = count + 1; return count; }\n  return inc;\n}\nvar c = makeCounter();\nprint(c());\nprint(c());\n",
            {"<main>", "makeCounter", "<closure>"},
            "💡 闭包调用栈：闭包帧不持有 count 的本地副本，而是通过 upvalue 引用外层 makeCounter 帧的 count。makeCounter 返回后帧已弹出，闭包通过 Upvalue 对象保持对 count 的引用（堆分配）。"
        },
        CallStackScenario{
            "method-dispatch",
            "🎯 类方法分派",
            "📞 Point.new() 构造 + p.distance() 方法调用。",
            "class Point {\n  var x; var y;\n  init(x, y) { this.x = x; this.y = y; }\n  fun distance() { return (x*x + y*y) ^ 0.5; }\n}\nvar p = Point.new(3, 4);\nprint(p.distance());\n",
            {"<main>", "Point.init", "Point.distance"},
            "💡 方法调用栈：distance 帧的 this 隐式绑定到 p 实例，可通过 this 访问字段 x/y。方法查找经过 ClassInfo::methodCache_ 缓存加速。"
        },
        CallStackScenario{
            "try-catch",
            "⚠️ 异常处理 try/catch",
            "🛡️ try 块内 throw 后栈迅速回退到 catch 帧。",
            "fun risky() { throw \"boom\"; }\ntry {\n  risky();\n} catch (e) {\n  print(\"caught: \" + e);\n}\n",
            {"<main>", "try-block", "risky", "<catch>"},
            "💡 异常传播：throw 时 VM 沿调用栈向上查找 try 块，沿途弹出栈帧（risky 帧被销毁），最终落到 catch 帧。注意 risky 的局部变量在异常后不可访问。"
        },
        CallStackScenario{
            "mutual-recursion",
            "🔄 相互递归 isEven/isOdd",
            "🔁 isEven 与 isOdd 互相调用直到 n=0/1，栈呈交替增长。",
            "fun isEven(n) { if (n == 0) return true; return isOdd(n-1); }\nfun isOdd(n) { if (n == 0) return false; return isEven(n-1); }\nprint(isEven(10));\n",
            {"<main>", "isEven(10)", "isOdd(9)", "isEven(8)", "isOdd(7)", "..."},
            "💡 相互递归：栈帧交替出现 isEven / isOdd，每帧 n 递减。注意 isOdd 在 isEven 之后定义但能被调用（前向引用通过 preScanModuleGlobals 修复）。"
        },
    };
    return kScenarios;
}

// ============================================================
// CallStackPanel 实现
// ============================================================

CallStackPanel::CallStackPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部子页切换
    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_    = new QPushButton(tr("实时调用栈"));
    pageLibraryBtn_ = new QPushButton(tr("教学场景库"));
    pageLiveBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageLiveBtn_->setChecked(true);
    pageBar->addWidget(pageLiveBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* livePage    = new QWidget;
    auto* libraryPage = new QWidget;
    buildLivePage(livePage);
    buildLibraryPage(libraryPage);
    stack_->addWidget(livePage);
    stack_->addWidget(libraryPage);
    outer->addWidget(stack_, 1);

    connect(pageLiveBtn_,    &QPushButton::clicked, [this]() { stack_->setCurrentIndex(0); pageLibraryBtn_->setChecked(false); PanelAnimator::slideInWidget(stack_->currentWidget()); });
    connect(pageLibraryBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        // 切到教学场景库时确保有选中项 —— 首次进入若 scenarioList_ 无选中，
        // 显式调用 showScenario(0) 让详情区立即有内容。
        if (scenarioList_->count() > 0 && scenarioList_->currentRow() < 0) {
            scenarioList_->setCurrentRow(0);
        }
        if (currentScenarioIdx_ < 0 && scenarioList_->count() > 0) {
            showScenario(0);
        }
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    autoTimer_ = new QTimer(this);
    // OPT-1: 500ms→2000ms。状态变更由 vmStateChanged 监听器即时触发刷新，
    // QTimer 降级为安全网（覆盖监听器未触达的边角场景），2s 间隔足以兜底且省 CPU。
    autoTimer_->setInterval(2000);
    connect(autoTimer_, &QTimer::timeout, this, &CallStackPanel::onRefresh);

    populateScenarios();
}

void CallStackPanel::setController(IdeController* controller) {
    if (controller_ == controller) return;
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener([this] { onVmStateChanged(); });
    }
}

void CallStackPanel::buildLivePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_ = new CaptionLabel(tr("状态：未初始化"));
    refreshBtn_       = new QPushButton(tr("刷新"));
    autoRefreshCheck_ = new QCheckBox(tr("自动刷新 (2s)"));
    bar->addWidget(liveStatusLabel_);
    bar->addStretch();
    bar->addWidget(autoRefreshCheck_);
    bar->addWidget(refreshBtn_);
    v->addLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    stackTree_   = new QTreeWidget;
    stackTree_->setHeaderLabels({tr("栈帧"), tr("行号"), tr("深度")});
    stackTree_->header()->setStretchLastSection(false);
    stackTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    stackTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    stackTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    splitter->addWidget(stackTree_);

    frameDetail_ = new QTextBrowser;
    frameDetail_->setOpenExternalLinks(false);
    splitter->addWidget(frameDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, &CallStackPanel::onRefresh);
    connect(autoRefreshCheck_, &QCheckBox::toggled, this, &CallStackPanel::onAutoRefreshToggled);
    connect(stackTree_, &QTreeWidget::currentItemChanged, [this](QTreeWidgetItem*, QTreeWidgetItem*) { onFrameSelected(); });
}

void CallStackPanel::buildLibraryPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    scenarioList_ = new QListWidget;
    scenarioDetail_ = new QTextBrowser;
    scenarioDetail_->setOpenExternalLinks(false);
    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    auto* btnBar = new QHBoxLayout;
    loadCodeBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addStretch();
    btnBar->addWidget(loadCodeBtn_);
    v->addLayout(btnBar);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, &CallStackPanel::onScenarioSelected);
    connect(loadCodeBtn_, &QPushButton::clicked, this, &CallStackPanel::onLoadScenarioCode);
}

void CallStackPanel::onRefresh() {
    refreshLive();
}

void CallStackPanel::onAutoRefreshToggled(bool checked) {
    if (checked) autoTimer_->start();
    else         autoTimer_->stop();
}

void CallStackPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 面板重新可见时，若用户已勾选自动刷新则恢复 QTimer
    if (autoRefreshCheck_ && autoRefreshCheck_->isChecked() &&
        autoTimer_ && !autoTimer_->isActive()) {
        refreshLive();
        autoTimer_->start();
    }
}

void CallStackPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // 面板隐藏时停止轮询，避免后台空转
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

void CallStackPanel::refreshLive() {
    stackTree_->clear();
    if (!controller_) {
        liveStatusLabel_->setText(tr("状态：未绑定 controller"));
        return;
    }

    std::vector<CallStackEntry> entries;
    QString modeLabel;
    if (controller_->isVmInitialized()) {
        entries    = controller_->getVmCallStack();
        modeLabel  = controller_->getUseRegisterVM() ? tr("RegisterVM") : tr("StackVM");
    } else if (controller_->isRunning() && controller_->isDebugRun()) {
        entries    = controller_->getDebugCallStack();
        modeLabel  = tr("Interpreter (debug)");
    } else {
        liveStatusLabel_->setText(tr("状态：未运行（启动调试或 VM 单步以查看调用栈）"));
        frameDetail_->clear();
        return;
    }

    liveStatusLabel_->setText(tr("状态：%1 | 帧数：%2")
        .arg(modeLabel).arg(entries.size()));

    if (entries.empty()) {
        frameDetail_->setPlainText(tr("（调用栈为空）"));
        return;
    }

    for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
        const auto& e = entries[i];
        auto* frame = new QTreeWidgetItem();
        frame->setText(0, QString::fromUtf8(e.functionName.c_str()));
        frame->setText(1, QString::number(e.line));
        frame->setText(2, QString::number(e.depth));
        // 子节点：本地变量
        for (const auto& [name, val] : e.locals) {
            auto* localItem = new QTreeWidgetItem(frame);
            localItem->setText(0, QString::fromUtf8(name.c_str()));
            localItem->setText(1, QString::fromUtf8(val.toString().c_str()));
            localItem->setText(2, QString::fromUtf8(val.typeName().c_str()));
        }
        stackTree_->addTopLevelItem(frame);
    }
    stackTree_->resizeColumnToContents(0);
}

void CallStackPanel::onFrameSelected() {
    auto* cur = stackTree_->currentItem();
    if (!cur) { frameDetail_->clear(); return; }

    // 顶层节点为帧；子节点为变量
    if (cur->parent() == nullptr) {
        QString name = cur->text(0);
        QString line = cur->text(1);
        QString depth = cur->text(2);
        QString html = QString(
            "<h3>栈帧: %1</h3>"
            "<p><b>调用深度:</b> %2</p>"
            "<p><b>当前行:</b> %3</p>"
            "<p><b>本地变量数:</b> %4</p>"
        ).arg(name).arg(depth).arg(line).arg(cur->childCount());
        frameDetail_->setHtml(html);
    } else {
        // 选中变量子节点
        QString varName = cur->text(0);
        QString varVal  = cur->text(1);
        QString varType = cur->text(2);
        QString html = QString(
            "<h3>变量: %1</h3>"
            "<p><b>类型:</b> %2</p>"
            "<p><b>值:</b> %3</p>"
        ).arg(varName).arg(varType).arg(varVal.toHtmlEscaped());
        frameDetail_->setHtml(html);
    }
}

void CallStackPanel::populateScenarios() {
    scenarioList_->clear();
    for (const auto& s : CallStackLibrary::scenarios()) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (scenarioList_->count() > 0) {
        scenarioList_->setCurrentRow(0);
    }
}

void CallStackPanel::onScenarioSelected(int index) {
    showScenario(index);
}

void CallStackPanel::showScenario(int index) {
    currentScenarioIdx_ = index;
    if (index < 0 || index >= static_cast<int>(CallStackLibrary::scenarios().size())) {
        scenarioDetail_->clear();
        return;
    }
    const auto& s = CallStackLibrary::scenarios()[index];
    QString framesHtml;
    for (size_t i = 0; i < s.expectedFrames.size(); ++i) {
        framesHtml += QString("<tr><td>%1</td><td><code>%2</code></td></tr>")
            .arg(i).arg(QString::fromUtf8(s.expectedFrames[i].c_str()).toHtmlEscaped());
    }
    QString html = QString(
        "<h2>%1</h2>"
        "<p><b>ID:</b> <code>%2</code></p>"
        "%3"
        "<h3>期望栈帧序列</h3>"
        "<table border='1' cellspacing='0' cellpadding='4'>"
        "<tr><th>深度</th><th>帧名</th></tr>"
        "%4"
        "</table>"
        "<h3>源码</h3>"
        "<pre>%5</pre>"
        "<h3>教学注解</h3>"
        "%6"
    ).arg(QString::fromUtf8(s.title.c_str()))
     .arg(QString::fromUtf8(s.id.c_str()))
     .arg(MarkdownRenderer::markdownToHtmlFragment(s.description))
     .arg(framesHtml)
     .arg(QString::fromUtf8(s.sourceCode.c_str()).toHtmlEscaped())
     .arg(MarkdownRenderer::markdownToHtmlFragment(s.teachingNote));
    scenarioDetail_->setHtml(html);
}

void CallStackPanel::onLoadScenarioCode() {
    if (currentScenarioIdx_ < 0 || currentScenarioIdx_ >= static_cast<int>(CallStackLibrary::scenarios().size())) {
        return;
    }
    const auto& s = CallStackLibrary::scenarios()[currentScenarioIdx_];
    emit loadSampleRequested(QString::fromUtf8(s.sourceCode.c_str()));
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* CallStackPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    // 注：只高亮「始终可见」的页切换按钮，概念性步骤用 nullptr（居中气泡）+ 示例代码。
    // 不高亮 stackTree_/loadCodeBtn_ 等位于 QStackedWidget 某一页的控件，
    // 避免目标页未显示时 mapTo 返回错误坐标导致气泡定位混乱。
    tour->addStep(pageLiveBtn_,
                  QString::fromUtf8("实时调用栈"),
                  QString::fromUtf8("「实时调用栈」页在调试时显示函数调用的层次结构，每层是一个栈帧。"
                                    "勾选「自动刷新」每 2 秒刷新栈帧，展开节点可查看函数名 / 行号 / 局部变量。"));
    tour->addStep(nullptr,
                  QString::fromUtf8("示例代码：递归调用栈"),
                  QString::fromUtf8(
                      "<p>将以下代码粘贴到编辑器，按 F5 调试，在调用栈中观察递归层次：</p>"
                      "<pre style='background:#EEE8D5;padding:8px;border-radius:4px;font-family:Consolas,monospace;'>"
                      "fun fib(n) {\n"
                      "    if (n < 2) {\n"
                      "        return n;\n"
                      "    }\n"
                      "    return fib(n - 1) + fib(n - 2);\n"
                      "}\n"
                      "print fib(5);\n"
                      "</pre>"
                      "<p>在 fib 函数内设断点，每次命中可看到调用栈深度变化：fib(5) → fib(4) → fib(3) → ...</p>"));
    tour->addStep(pageLibraryBtn_,
                  QString::fromUtf8("教学场景库"),
                  QString::fromUtf8("点击「教学场景库」切换到静态教学页，查看递归 / 闭包 / 方法分派等典型调用栈形态。"));
    tour->addStep(nullptr,
                  QString::fromUtf8("开始实验"),
                  QString::fromUtf8("切换到教学场景库后，选中任一场景，点击「加载场景代码」载入编辑器，"
                                    "按 F5 调试即可观察对应的调用栈形态。"));
    return tour;
}
