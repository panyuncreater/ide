// ============================================================
// BytecodeTracePanel.cpp — 字节码执行轨迹面板实现（第三波 P0-3）
// ============================================================

#include "gui/BytecodeTracePanel.h"
#include "gui/GuidedTour.h"
#include "gui/PanelAnimator.h"
#include "gui/MarkdownRenderer.h"
#include "app/IdeController.h"
#include "interpreter/Value.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QTimer>
#include <sstream>

#include "Label.h"   // QFluentKit（CaptionLabel）

// ============================================================
// BytecodeTraceLibrary — 静态 OpCode 教学库
// ============================================================

const std::vector<OpCodeDocEntry>& BytecodeTraceLibrary::opCodeDocs() {
    static const std::vector<OpCodeDocEntry> kDocs = {
        OpCodeDocEntry{
            "OP_INT", "const",
            "nameIdx(2B)", "push 1",
            "📜 从常量池读取整数并压入栈顶。",
            "var x = 42;"
        },
        OpCodeDocEntry{
            "OP_FLOAT", "const",
            "nameIdx(2B)", "push 1",
            "📜 从常量池读取浮点数并压入栈顶。",
            "var pi = 3.14;"
        },
        OpCodeDocEntry{
            "OP_STRING", "const",
            "nameIdx(2B)", "push 1",
            "📜 从常量池读取字符串并压入栈顶。",
            "var s = \"hello\";"
        },
        OpCodeDocEntry{
            "OP_NULL", "const",
            "无", "push 1",
            "📜 压入 null 值。",
            "var n = null;"
        },
        OpCodeDocEntry{
            "OP_ADD", "arith",
            "无", "pop 2 / push 1",
            "🔢 弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。",
            "var z = x + y;"
        },
        OpCodeDocEntry{
            "OP_GET_GLOBAL", "var",
            "slot(2B)", "push 1",
            "📍 读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。",
            "print(x);"
        },
        OpCodeDocEntry{
            "OP_SET_GLOBAL", "var",
            "slot(2B)", "pop 1",
            "📍 弹出栈顶值写入全局槽位。",
            "x = 10;"
        },
        OpCodeDocEntry{
            "OP_JUMP", "control",
            "offset(2B)", "no effect",
            "🔄 无条件跳转到 offset 指定的相对位置。",
            "if (true) { print(\"yes\"); }"
        },
        OpCodeDocEntry{
            "OP_JUMP_IF_FALSE", "control",
            "offset(2B)", "pop 1",
            "🔄 弹出栈顶条件，若为 false 则跳转。",
            "if (cond) { ... }"
        },
        OpCodeDocEntry{
            "OP_LOOP", "control",
            "offset(2B)", "no effect",
            "🔄 回跳到循环入口（负偏移）。",
            "while (cond) { ... }"
        },
        OpCodeDocEntry{
            "OP_CALL", "call",
            "argCount(1B)", "pop N+1 / push 1",
            "📞 调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。",
            "result = foo(1, 2);"
        },
        OpCodeDocEntry{
            "OP_RETURN", "call",
            "无", "pop frame",
            "📞 从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。",
            "return x;"
        },
        OpCodeDocEntry{
            "OP_BUILD_ARRAY", "container",
            "count(1B)", "pop N / push 1",
            "📊 弹出栈顶 N 个元素构建 ArrayData 并压入。",
            "var arr = [1, 2, 3];"
        },
        OpCodeDocEntry{
            "OP_BUILD_DICT", "container",
            "pairCount(1B)", "pop 2N / push 1",
            "📊 弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。",
            "var d = {\"x\": 1};"
        },
        OpCodeDocEntry{
            "OP_CLOSURE", "closure",
            "nameIdx(2B) + upvalueCount(1B)", "pop N / push 1",
            "📦 创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。",
            "fun outer() { var x = 1; fun inner() { return x; } return inner; }"
        },
        OpCodeDocEntry{
            "OP_GET_UPVALUE", "closure",
            "upvalueIndex(1B)", "push 1",
            "🔗 读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。",
            "// 闭包内访问外层变量"
        },
        OpCodeDocEntry{
            "OP_CLASS_NEW", "class",
            "nameIdx(2B) + argCount(1B)", "pop N+1 / push 1",
            "📦 类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。",
            "var p = Point.new(3, 4);"
        },
        OpCodeDocEntry{
            "OP_METHOD_CALL", "class",
            "nameIdx(2B) + argCount(1B) + recvVarIdx(2B)", "pop N+1 / push 1",
            "📞 方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。",
            "p.distance();"
        },
    };
    return kDocs;
}

// ============================================================
// BytecodeTracePanel 实现
// ============================================================

BytecodeTracePanel::BytecodeTracePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageTraceBtn_   = new QPushButton(tr("执行轨迹"));
    pageLibraryBtn_ = new QPushButton(tr("OpCode 教学库"));
    pageTraceBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageTraceBtn_->setChecked(true);
    pageBar->addWidget(pageTraceBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* tracePage   = new QWidget;
    auto* libraryPage = new QWidget;
    buildTracePage(tracePage);
    buildLibraryPage(libraryPage);
    stack_->addWidget(tracePage);
    stack_->addWidget(libraryPage);
    outer->addWidget(stack_, 1);

    connect(pageTraceBtn_,   &QPushButton::clicked, [this]() { stack_->setCurrentIndex(0); pageLibraryBtn_->setChecked(false); PanelAnimator::slideInWidget(stack_->currentWidget()); });
    connect(pageLibraryBtn_, &QPushButton::clicked, [this]() { stack_->setCurrentIndex(1); pageTraceBtn_->setChecked(false); PanelAnimator::slideInWidget(stack_->currentWidget()); });

    // OPT-1: 自动捕获改为 vmStateChanged 监听器即时触发（见 setController）。
    // QTimer 降级为 2000ms 安全网，覆盖监听器未触达的边角场景。
    autoTimer_ = new QTimer(this);
    autoTimer_->setInterval(2000);
    connect(autoTimer_, &QTimer::timeout, this, &BytecodeTracePanel::onCaptureNow);

    populateDocs();
}

void BytecodeTracePanel::setController(IdeController* controller) {
    if (controller_ == controller) return;
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener([this] { onVmStateChanged(); });
    }
}

void BytecodeTracePanel::buildTracePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_  = new CaptionLabel(tr("状态：未初始化"));
    captureBtn_       = new QPushButton(tr("立即捕获"));
    autoCaptureCheck_ = new QCheckBox(tr("自动捕获（VM 暂停时）"));
    clearBtn_          = new QPushButton(tr("清空"));
    bar->addWidget(liveStatusLabel_);
    bar->addStretch();
    bar->addWidget(autoCaptureCheck_);
    bar->addWidget(captureBtn_);
    bar->addWidget(clearBtn_);
    v->addLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    traceTable_ = new QTableWidget(0, 5);
    traceTable_->setHorizontalHeaderLabels({tr("步"), tr("IP"), tr("OpCode"), tr("帧数"), tr("栈大小")});
    traceTable_->verticalHeader()->setVisible(false);
    traceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    traceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    traceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    traceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    traceTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    splitter->addWidget(traceTable_);

    stackDetail_ = new QTextBrowser;
    stackDetail_->setOpenExternalLinks(false);
    splitter->addWidget(stackDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    connect(captureBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onCaptureNow);
    connect(autoCaptureCheck_, &QCheckBox::toggled, this, &BytecodeTracePanel::onAutoCaptureToggled);
    connect(clearBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onClearTrace);
    connect(traceTable_, &QTableWidget::currentCellChanged,
        [this](int, int, int, int) { onTraceRowSelected(); });
}

void BytecodeTracePanel::buildLibraryPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    docList_   = new QListWidget;
    docDetail_ = new QTextBrowser;
    docDetail_->setOpenExternalLinks(false);
    splitter->addWidget(docList_);
    splitter->addWidget(docDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    auto* btnBar = new QHBoxLayout;
    loadCodeBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addStretch();
    btnBar->addWidget(loadCodeBtn_);
    v->addLayout(btnBar);

    connect(docList_, &QListWidget::currentRowChanged, this, &BytecodeTracePanel::onDocSelected);
    connect(loadCodeBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onLoadDocCode);
}

void BytecodeTracePanel::onCaptureNow() {
    captureCurrentState();
}

void BytecodeTracePanel::onAutoCaptureToggled(bool checked) {
    if (checked) autoTimer_->start();
    else         autoTimer_->stop();
}

void BytecodeTracePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (autoCaptureCheck_ && autoCaptureCheck_->isChecked() &&
        autoTimer_ && !autoTimer_->isActive()) {
        autoTimer_->start();
    }
}

void BytecodeTracePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

void BytecodeTracePanel::onClearTrace() {
    traceHistory_.clear();
    stepCounter_ = 0;
    traceTable_->setRowCount(0);
    stackDetail_->clear();
    liveStatusLabel_->setText(tr("状态：未运行（轨迹已清空）"));
}

void BytecodeTracePanel::captureCurrentState() {
    if (!controller_) {
        liveStatusLabel_->setText(tr("状态：未绑定 controller"));
        return;
    }
    if (!controller_->isVmInitialized()) {
        liveStatusLabel_->setText(tr("状态：VM 未初始化（启动 VM 单步以捕获轨迹）"));
        return;
    }

    TraceEntry e;
    e.step        = ++stepCounter_;
    e.ip          = controller_->getVmCurrentIP();
    e.opCodeName  = controller_->getVmCurrentOpCodeName();
    e.frameCount  = static_cast<int>(controller_->getVmFrameCount());
    e.line        = controller_->getVmCurrentLine();

    auto stack = controller_->getVmStack();
    e.stackSnapshot.reserve(stack.size());
    for (const auto& v : stack) {
        e.stackSnapshot.push_back(v.toString());
    }

    liveStatusLabel_->setText(tr("状态：已捕获 step=%1 | IP=%2 | OpCode=%3 | 帧数=%4")
        .arg(e.step).arg(e.ip)
        .arg(QString::fromUtf8(e.opCodeName.c_str()))
        .arg(e.frameCount));

    if (static_cast<int>(traceHistory_.size()) >= kMaxTraceEntries) {
        traceHistory_.erase(traceHistory_.begin());
    }
    traceHistory_.push_back(std::move(e));
    refreshTraceTable();

    // 自动选中最新条目
    int lastRow = traceTable_->rowCount() - 1;
    if (lastRow >= 0) {
        traceTable_->selectRow(lastRow);
        onTraceRowSelected();
    }
}

void BytecodeTracePanel::refreshTraceTable() {
    traceTable_->setRowCount(0);
    traceTable_->setRowCount(static_cast<int>(traceHistory_.size()));
    for (int i = 0; i < static_cast<int>(traceHistory_.size()); ++i) {
        const auto& e = traceHistory_[i];
        auto* c0 = new QTableWidgetItem(QString::number(e.step));
        auto* c1 = new QTableWidgetItem(QString::number(e.ip));
        auto* c2 = new QTableWidgetItem(QString::fromUtf8(e.opCodeName.c_str()));
        auto* c3 = new QTableWidgetItem(QString::number(e.frameCount));
        auto* c4 = new QTableWidgetItem(QString::number(e.stackSnapshot.size()));
        c0->setTextAlignment(Qt::AlignCenter);
        c1->setTextAlignment(Qt::AlignCenter);
        c3->setTextAlignment(Qt::AlignCenter);
        c4->setTextAlignment(Qt::AlignCenter);
        traceTable_->setItem(i, 0, c0);
        traceTable_->setItem(i, 1, c1);
        traceTable_->setItem(i, 2, c2);
        traceTable_->setItem(i, 3, c3);
        traceTable_->setItem(i, 4, c4);
    }
    traceTable_->scrollToBottom();
}

void BytecodeTracePanel::onTraceRowSelected() {
    int row = traceTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(traceHistory_.size())) {
        stackDetail_->clear();
        return;
    }
    const auto& e = traceHistory_[row];

    QString stackHtml;
    if (e.stackSnapshot.empty()) {
        stackHtml = tr("<p><i>（操作数栈为空）</i></p>");
    } else {
        stackHtml = tr("<h4>操作数栈（栈顶 → 栈底）</h4><ol>");
        // 倒序显示：栈顶在前
        for (auto it = e.stackSnapshot.rbegin(); it != e.stackSnapshot.rend(); ++it) {
            stackHtml += QString("<li><code>%1</code></li>")
                .arg(QString::fromUtf8(it->c_str()).toHtmlEscaped());
        }
        stackHtml += QStringLiteral("</ol>");
    }

    QString html = QString(
        "<h3>Step %1: %2</h3>"
        "<p><b>IP:</b> %3 | <b>行:</b> %4 | <b>帧数:</b> %5</p>"
        "%6"
    ).arg(e.step)
     .arg(QString::fromUtf8(e.opCodeName.c_str()))
     .arg(e.ip)
     .arg(e.line)
     .arg(e.frameCount)
     .arg(stackHtml);
    stackDetail_->setHtml(html);

    // Emit source line for editor highlighting
    if (e.line > 0) {
        emit sourceLineRequested(e.line);
    }
}

void BytecodeTracePanel::populateDocs() {
    docList_->clear();
    for (const auto& d : BytecodeTraceLibrary::opCodeDocs()) {
        docList_->addItem(QString::fromUtf8(d.opCodeName.c_str()));
    }
    if (docList_->count() > 0) {
        docList_->setCurrentRow(0);
    }
}

void BytecodeTracePanel::onDocSelected(int index) {
    showDoc(index);
}

void BytecodeTracePanel::showDoc(int index) {
    currentDocIdx_ = index;
    if (index < 0 || index >= static_cast<int>(BytecodeTraceLibrary::opCodeDocs().size())) {
        docDetail_->clear();
        return;
    }
    const auto& d = BytecodeTraceLibrary::opCodeDocs()[index];
    QString html = QString(
        "<h2>%1</h2>"
        "<p><b>分类:</b> %2</p>"
        "<h3>操作数格式</h3>"
        "<p><code>%3</code></p>"
        "<h3>栈效果</h3>"
        "<p><code>%4</code></p>"
        "<h3>语义</h3>"
        "%5"
        "<h3>样例代码</h3>"
        "<pre>%6</pre>"
    ).arg(QString::fromUtf8(d.opCodeName.c_str()))
     .arg(QString::fromUtf8(d.category.c_str()))
     .arg(QString::fromUtf8(d.operandFormat.c_str()))
     .arg(QString::fromUtf8(d.stackEffect.c_str()))
     .arg(MarkdownRenderer::markdownToHtmlFragment(d.semantics))
     .arg(QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped());
    docDetail_->setHtml(html);
}

void BytecodeTracePanel::onLoadDocCode() {
    if (currentDocIdx_ < 0 || currentDocIdx_ >= static_cast<int>(BytecodeTraceLibrary::opCodeDocs().size())) {
        return;
    }
    const auto& d = BytecodeTraceLibrary::opCodeDocs()[currentDocIdx_];
    emit loadSampleRequested(QString::fromUtf8(d.exampleCode.c_str()));
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* BytecodeTracePanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageTraceBtn_,
                  QString::fromUtf8("执行轨迹"),
                  QString::fromUtf8("这里显示每条字节码指令执行后的栈状态快照。"));
    tour->addStep(autoCaptureCheck_,
                  QString::fromUtf8("自动捕获"),
                  QString::fromUtf8("勾选后，VM 暂停时会自动捕获一条轨迹，无需手动点击。"));
    tour->addStep(traceTable_,
                  QString::fromUtf8("轨迹表"),
                  QString::fromUtf8("每行一条指令记录，包含 IP / OpCode / 栈快照。点击某行可定位到对应源码行。"));
    tour->addStep(pageLibraryBtn_,
                  QString::fromUtf8("OpCode 教学库"),
                  QString::fromUtf8("切换到 OpCode 参考库，查看每条指令的语义说明与样例代码。"));
    tour->addStep(loadCodeBtn_,
                  QString::fromUtf8("加载样例"),
                  QString::fromUtf8("点击可将当前 OpCode 的示例代码加载到主编辑器，方便直接运行观察。"));
    return tour;
}
