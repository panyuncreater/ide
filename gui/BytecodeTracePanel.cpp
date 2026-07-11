// ============================================================
// BytecodeTracePanel.cpp — 字节码执行轨迹面板实现（第三波 P0-3）
// ============================================================

#include "gui/BytecodeTracePanel.h"
#include "app/IdeController.h"
#include "gui/GuidedTour.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"
#include "interpreter/Value.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <sstream>

#include "Label.h" // QFluentKit（CaptionLabel）

// ============================================================
// BytecodeTraceLibrary — 静态 OpCode 教学库
// ============================================================

/// 返回 OpCode 教学库（静态单例）：46 条核心字节码（常量/算术/变量/控制/
/// 调用/容器/闭包/类），每条含分类、操作数格式、栈效果、语义与样例代码，
/// 供「OpCode 教学库」子页展示并支持加载样例到主编辑器。
const std::vector<OpCodeDocEntry>& BytecodeTraceLibrary::opCodeDocs() {
    static const std::vector<OpCodeDocEntry> kDocs = {
        OpCodeDocEntry{"OP_INT", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取整数并压入栈顶。", "var x = 42;"},
        OpCodeDocEntry{"OP_FLOAT", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取浮点数并压入栈顶。",
                       "var pi = 3.14;"},
        OpCodeDocEntry{"OP_STRING", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取字符串并压入栈顶。",
                       "var s = \"hello\";"},
        OpCodeDocEntry{"OP_NULL", "const", "无", "push 1", "📜 压入 null 值。", "var n = null;"},
        OpCodeDocEntry{"OP_ADD", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。",
                       "var z = x + y;"},
        OpCodeDocEntry{"OP_GET_GLOBAL", "var", "slot(2B)", "push 1",
                       "📍 读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。", "print(x);"},
        OpCodeDocEntry{"OP_SET_GLOBAL", "var", "slot(2B)", "pop 1", "📍 弹出栈顶值写入全局槽位。", "x = 10;"},
        OpCodeDocEntry{"OP_JUMP", "control", "offset(2B)", "no effect", "🔄 无条件跳转到 offset 指定的相对位置。",
                       "if (true) { print(\"yes\"); }"},
        OpCodeDocEntry{"OP_JUMP_IF_FALSE", "control", "offset(2B)", "pop 1", "🔄 弹出栈顶条件，若为 false 则跳转。",
                       "if (cond) { ... }"},
        OpCodeDocEntry{"OP_LOOP", "control", "offset(2B)", "no effect", "🔄 回跳到循环入口（负偏移）。",
                       "while (cond) { ... }"},
        OpCodeDocEntry{"OP_CALL", "call", "argCount(1B)", "pop N+1 / push 1",
                       "📞 调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。", "result = foo(1, 2);"},
        OpCodeDocEntry{"OP_RETURN", "call", "无", "pop frame",
                       "📞 从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。", "return x;"},
        OpCodeDocEntry{"OP_BUILD_ARRAY", "container", "count(1B)", "pop N / push 1",
                       "📊 弹出栈顶 N 个元素构建 ArrayData 并压入。", "var arr = [1, 2, 3];"},
        OpCodeDocEntry{"OP_BUILD_DICT", "container", "pairCount(1B)", "pop 2N / push 1",
                       "📊 弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。", "var d = {\"x\": 1};"},
        OpCodeDocEntry{
            "OP_CLOSURE", "closure", "nameIdx(2B) + upvalueCount(1B)", "pop N / push 1",
            "📦 创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。",
            "fun outer() { var x = 1; fun inner() { return x; } return inner; }"},
        OpCodeDocEntry{"OP_GET_UPVALUE", "closure", "upvalueIndex(1B)", "push 1",
                       "🔗 读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。",
                       "// 闭包内访问外层变量"},
        OpCodeDocEntry{"OP_CLASS_NEW", "class", "nameIdx(2B) + argCount(1B)", "pop N+1 / push 1",
                       "📦 类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。",
                       "var p = Point(3, 4);"},
        OpCodeDocEntry{"OP_METHOD_CALL", "class", "nameIdx(2B) + argCount(1B) + recvVarIdx(2B)", "pop N+1 / push 1",
                       "📞 方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。", "p.distance();"},
        // ---- Arithmetic ----
        OpCodeDocEntry{"OP_SUBTRACT", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值（右、左），相减后压入结果（左 - 右）。", "var z = x - y;"},
        OpCodeDocEntry{"OP_MULTIPLY", "arith", "无", "pop 2 / push 1", "🔢 弹出栈顶两个值相乘后压入结果。",
                       "var z = x * y;"},
        OpCodeDocEntry{"OP_DIVIDE", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值相除后压入结果（整数除法截断向零，三后端统一）。", "var z = x / y;"},
        OpCodeDocEntry{"OP_MODULO", "arith", "无", "pop 2 / push 1", "🔢 弹出栈顶两个值取模后压入结果。",
                       "var z = x % y;"},
        OpCodeDocEntry{"OP_NEGATE", "arith", "无", "pop 1 / push 1", "🔢 弹出栈顶值取负后压入。", "var z = -x;"},
        // ---- Boolean ----
        OpCodeDocEntry{"OP_TRUE", "const", "无", "push 1", "🟢 压入布尔常量 true。", "var b = true;"},
        OpCodeDocEntry{"OP_FALSE", "const", "无", "push 1", "🔴 压入布尔常量 false。", "var b = false;"},
        OpCodeDocEntry{"OP_NOT", "arith", "无", "pop 1 / push 1", "🔵 弹出栈顶值取逻辑非后压入布尔结果。",
                       "var b = !x;"},
        // ---- Comparison ----
        OpCodeDocEntry{"OP_EQUAL", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值进行相等比较后压入布尔结果。",
                       "var b = x == y;"},
        OpCodeDocEntry{"OP_NOT_EQUAL", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值进行不等比较后压入布尔结果。",
                       "var b = x != y;"},
        OpCodeDocEntry{"OP_LESS", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值（右、左），若左 < 右则压入 true。",
                       "var b = x < y;"},
        OpCodeDocEntry{"OP_GREATER", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 > 右则压入 true。", "var b = x > y;"},
        OpCodeDocEntry{"OP_LESS_EQUAL", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 <= 右则压入 true。", "var b = x <= y;"},
        OpCodeDocEntry{"OP_GREATER_EQUAL", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 >= 右则压入 true。", "var b = x >= y;"},
        // ---- Stack / IO ----
        OpCodeDocEntry{"OP_POP", "control", "无", "pop 1",
                       "📤 弹出栈顶值并丢弃（表达式语句副作用求值后丢弃返回值，防止栈泄漏）。", "foo();"},
        OpCodeDocEntry{"OP_PRINT", "call", "无", "pop 1", "🖨️ 弹出栈顶值并输出到标准输出（带换行）。", "print(x);"},
        // ---- Local / var ----
        OpCodeDocEntry{"OP_GET_LOCAL", "var", "slot(1B)", "push 1",
                       "📍 读取当前栈帧中局部变量槽位并压栈（编译期分配的栈相对位置）。", "// 函数内访问局部变量"},
        OpCodeDocEntry{"OP_SET_LOCAL", "var", "slot(1B)", "pop 1", "📍 弹出栈顶值写入当前栈帧的局部变量槽位。",
                       "x = 10;"},
        OpCodeDocEntry{"OP_DEFINE_GLOBAL", "var", "nameIdx(2B)", "pop 1",
                       "📍 弹出栈顶值，以 name 为键首次写入全局表（与 OP_SET_GLOBAL 的已存在赋值语义区分）。",
                       "var x = 10;"},
        // ---- Index / member ----
        OpCodeDocEntry{"OP_INDEX_GET", "container", "无", "pop 2 / push 1",
                       "🔑 弹出索引与容器，读取 container[index] 并压栈（支持数组/字典/字符串）。", "var v = arr[0];"},
        OpCodeDocEntry{"OP_INDEX_SET", "container", "无", "pop 3",
                       "🔑 弹出值、索引、容器，执行 container[index] = value（COW 写时拷贝）。", "arr[0] = 99;"},
        OpCodeDocEntry{"OP_MEMBER_GET", "container", "nameIdx(2B)", "pop 1 / push 1",
                       "🔑 弹出实例，读取字段 name 的值并压栈（通过 fieldSlotIndex 查表）。", "var v = p.x;"},
        OpCodeDocEntry{"OP_MEMBER_SET", "container", "nameIdx(2B)", "pop 2",
                       "🔑 弹出值与实例，写入字段 name（实例字段槽位赋值）。", "p.x = 10;"},
        // ---- Closure ----
        OpCodeDocEntry{"OP_SET_UPVALUE", "closure", "upvalueIndex(1B)", "pop 1",
                       "🔗 弹出栈顶值写入指定 upvalue（开放则写栈帧，已关闭则写堆）。", "// 闭包内赋值外层变量"},
        OpCodeDocEntry{"OP_CLOSE_UPVALUE", "closure", "无", "pop 1",
                       "🔗 弹出栈顶值，将仍开放的 upvalue 迁移到堆（变量逃逸出作用域时触发）。",
                       "// 闭包捕获的变量逃逸"},
        // ---- Exception ----
        OpCodeDocEntry{"OP_THROW", "control", "无", "pop 1",
                       "⚠️ 弹出栈顶值作为异常对象并抛出，沿调用栈寻找匹配的 catch。", "throw \"error\";"},
        OpCodeDocEntry{"OP_TRY_BEGIN", "control", "catchOffset(2B)", "no effect",
                       "🛡️ 注册 try 块的 catch 处理偏移，异常抛出时跳转到该处执行。", "try { ... } catch (e) { ... }"},
        // ---- Class ----
        OpCodeDocEntry{"OP_SUPER_CALL", "class", "nameIdx(2B) + argCount(1B)", "pop N+1 / push 1",
                       "📞 调用父类方法：从当前实例的父类链查找方法并调用（super 语义三后端统一）。", "super.foo();"},
    };
    return kDocs;
}

// ============================================================
// BytecodeTracePanel 实现
// ============================================================

/// 构造面板：组装顶部子页切换（执行轨迹 / OpCode 教学库）与 QStackedWidget，
/// 构建两个子页，配置 2s 自动捕获定时器（安全网，即时刷新由监听器触发），
/// 并填充教学库列表。
BytecodeTracePanel::BytecodeTracePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageTraceBtn_ = new QPushButton(tr("执行轨迹"));
    pageLibraryBtn_ = new QPushButton(tr("OpCode 教学库"));
    pageTraceBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageTraceBtn_->setChecked(true);
    pageBar->addWidget(pageTraceBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* tracePage = new QWidget;
    auto* libraryPage = new QWidget;
    buildTracePage(tracePage);
    buildLibraryPage(libraryPage);
    stack_->addWidget(tracePage);
    stack_->addWidget(libraryPage);
    outer->addWidget(stack_, 1);

    connect(pageTraceBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(0);
        pageLibraryBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageLibraryBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(1);
        pageTraceBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // OPT-1: 自动捕获改为 vmStateChanged 监听器即时触发（见 setController）。
    // QTimer 降级为 2000ms 安全网，覆盖监听器未触达的边角场景。
    autoTimer_ = new QTimer(this);
    autoTimer_->setInterval(2000);
    connect(autoTimer_, &QTimer::timeout, this, &BytecodeTracePanel::onCaptureNow);

    populateDocs();
}

/// 绑定/换绑 IdeController：注册 vmStateChanged 监听器（owner=this），
/// 换绑前先反注册旧监听器，避免 controller 持有悬垂回调。
void BytecodeTracePanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // AUDIT-P0 fix: 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        // AUDIT-P0 fix: owner=this 供析构/换绑时反注册，避免悬垂 lambda UAF。
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

// AUDIT-P0 fix: 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调。
/// 析构时反注册 vmStateChanged 监听器，避免 controller 持有悬垂 this 回调。
BytecodeTracePanel::~BytecodeTracePanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

/// 构建「执行轨迹」子页：顶部状态/立即捕获/自动捕获/清空按钮 + 垂直 splitter
/// （上方轨迹表 + 下方栈快照浏览器）。轨迹表列：步/IP/OpCode/帧数/栈大小。
void BytecodeTracePanel::buildTracePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_ = new CaptionLabel(tr("状态：未初始化"));
    captureBtn_ = new QPushButton(tr("立即捕获"));
    autoCaptureCheck_ = new QCheckBox(tr("自动捕获（VM 暂停时）"));
    clearBtn_ = new QPushButton(tr("清空"));
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
    connect(traceTable_, &QTableWidget::currentCellChanged, [this](int, int, int, int) { onTraceRowSelected(); });
}

/// 构建「OpCode 教学库」子页：左侧 OpCode 列表 + 右侧说明浏览器 + 加载样例按钮，
/// 选中项变化时通过 onDocSelected 渲染指令语义详情。
void BytecodeTracePanel::buildLibraryPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    docList_ = new QListWidget;
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

/// 立即捕获按钮回调：委托 captureCurrentState() 抓取当前 VM 单步状态。
void BytecodeTracePanel::onCaptureNow() {
    captureCurrentState();
}

/// 自动捕获复选框切换：勾选启动 2s 定时器，取消则停止（即时刷新仍由 vmStateChanged 触发）。
void BytecodeTracePanel::onAutoCaptureToggled(bool checked) {
    if (checked)
        autoTimer_->start();
    else
        autoTimer_->stop();
}

/// 面板重新可见时：若已勾选自动捕获则恢复 2s 定时器。
void BytecodeTracePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (autoCaptureCheck_ && autoCaptureCheck_->isChecked() && autoTimer_ && !autoTimer_->isActive()) {
        autoTimer_->start();
    }
}

/// 面板隐藏时停止自动捕获定时器，避免后台空转。
void BytecodeTracePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

/// 清空轨迹：重置步计数器、清空历史向量与轨迹表/栈快照浏览器，状态回未运行。
void BytecodeTracePanel::onClearTrace() {
    traceHistory_.clear();
    stepCounter_ = 0;
    // OPT-2 fix: 清空时同步重置指纹，使下一次捕获不会被误判为"状态未变"而跳过。
    lastCaptureFingerprint_.clear();
    // R51-2 fix: 同步重置滑动标志，避免下次 capture 误触发全量重建
    dequeShifted_ = false;
    traceTable_->setRowCount(0);
    stackDetail_->clear();
    // R54-14 fix: 空状态占位提示
    stackDetail_->setHtml(
        QString::fromUtf8("<div style='color:#6E6E6E; padding:8px;'><i>（轨迹已清空，捕获后将显示栈快照）</i></div>"));
    liveStatusLabel_->setText(tr("状态：未运行（轨迹已清空）"));
}

/// 抓取一次 VM 单步快照（轨迹单步/断点的数据来源）：
/// 从未初始化的 VM 直接返回占位；否则从 controller 取当前 IP、OpCode 名、
/// 栈帧数、源码行号，并通过 getVmStack() 取得操作数栈快照（每个 Value 经
/// toString 序列化，堆类型用 try/catch 包裹防异常）。快照追加到 traceHistory_
/// （超 kMaxTraceEntries 则丢弃最旧），刷新表格并自动选中最新行。
/// 该轨迹即字节码单步/断点的可视化数据源，每行对应一条指令执行后的完整状态。
void BytecodeTracePanel::captureCurrentState() {
    if (!controller_) {
        liveStatusLabel_->setText(tr("状态：未绑定 controller"));
        return;
    }
    if (!controller_->isVmInitialized()) {
        liveStatusLabel_->setText(tr("状态：VM 未初始化（启动 VM 单步以捕获轨迹）"));
        return;
    }
    // AUDIT-P2 fix: 对齐 CallStackPanel/VariableInspectorPanel/BreakpointConditionPanel/
    // MemoryModelPanel 的 isVmRunning() 守卫。VM RUN 批之间捕获轨迹会混入用户
    // 未主动请求的中间状态数据，且未来若 VmStepper 改为多线程会升级为数据竞争。
    if (controller_->isVmRunning()) {
        liveStatusLabel_->setText(tr("状态：VM 运行中（暂停后可捕获）"));
        return;
    }

    TraceEntry e;
    // OPT-2 fix: step 延迟到指纹检查通过后再赋值，避免状态未变时 stepCounter_
    // 误增导致后续捕获步号跳号（autoTimer 2s 安全网可能在 VM 状态未变时触发）。
    e.ip = controller_->getVmCurrentIP();
    e.opCodeName = controller_->getVmCurrentOpCodeName();
    e.frameCount = static_cast<int>(controller_->getVmFrameCount());
    e.line = controller_->getVmCurrentLine();

    auto stack = controller_->getVmStack();
    e.stackSnapshot.reserve(stack.size());
    // AUDIT-P2 fix: Value::toString 对堆类型（数组/字典/实例）走 toStringImpl，
    // 极端情况（循环引用、深度嵌套、bad_alloc）可能抛异常。对齐 CallStackPanel/
    // VariableInspectorPanel 的 try/catch 防护，避免异常传播到 QTimer 槽。
    for (const auto& v : stack) {
        std::string s;
        try {
            s = v.toString();
        } catch (...) {
            s = "<error>";
        }
        e.stackSnapshot.push_back(std::move(s));
    }

    // OPT-2 fix: autoTimer 2s 安全网无指纹对比会盲目累积重复轨迹（累积式 push_back）。
    // 在 e 各字段已填充后计算指纹，与上次比对——状态未变则跳过本次捕获。
    // 指纹不含 stackSnapshot 内容（避免长栈 O(n) 字符串拼接），仅用大小即可
    // 区分绝大多数"状态未变"场景；如栈大小相同但内容变化，会多捕获一条，
    // 不影响正确性，仅极小性能开销。
    std::string fingerprint = std::to_string(e.ip) + "|" + e.opCodeName + "|" + std::to_string(e.frameCount) + "|" +
                              std::to_string(e.stackSnapshot.size());
    if (fingerprint == lastCaptureFingerprint_) {
        // 状态未变，跳过本次捕获（不更新 stepCounter_，不追加历史，不刷新表格）
        return;
    }
    lastCaptureFingerprint_ = std::move(fingerprint);

    // 指纹检查通过，确认本次捕获有效，递增步号
    e.step = ++stepCounter_;

    liveStatusLabel_->setText(tr("状态：已捕获 step=%1 | IP=%2 | OpCode=%3 | 帧数=%4")
                                  .arg(e.step)
                                  .arg(e.ip)
                                  .arg(QString::fromUtf8(e.opCodeName.c_str()))
                                  .arg(e.frameCount));

    if (static_cast<int>(traceHistory_.size()) >= kMaxTraceEntries) {
        // AUDIT-P1 fix: deque pop_front O(1)（原 vector erase(begin()) O(n) 移位）
        traceHistory_.pop_front();
        // R51-2 fix: 标记 deque 已滑动，refreshTraceTable 需全量重建
        dequeShifted_ = true;
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

/// 将 traceHistory_ 渲染为轨迹表。AUDIT-P1 fix: 改为增量更新——
/// 仅追加新行 + 移除溢出行，避免每次 capture 都 setRowCount(0) 全量重建 N×5 个 item。
/// R51-2 fix: deque 容量满后 pop_front+push_back 保持 size 不变但索引偏移，
/// 增量更新无法感知，需检测 dequeShifted_ 标志后全量重建。
void BytecodeTracePanel::refreshTraceTable() {
    int histSize = static_cast<int>(traceHistory_.size());

    // R51-2 fix: deque 滑动后索引整体偏移，必须全量重建
    if (dequeShifted_) {
        traceTable_->setRowCount(0);
        dequeShifted_ = false;
    }

    int tableRows = traceTable_->rowCount();

    // 表行多于历史（清空场景或溢出移除）→ 移除多余行
    while (tableRows > histSize) {
        traceTable_->removeRow(tableRows - 1);
        --tableRows;
    }

    // 追加新行（仅新增部分）
    for (int i = tableRows; i < histSize; ++i) {
        traceTable_->insertRow(i);
    }

    // 填充新行的单元格数据（仅新行，旧行数据不变）
    for (int i = tableRows; i < histSize; ++i) {
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

/// 选中某条轨迹行时渲染其详情：倒序展示操作数栈（栈顶在前），并显示
/// IP/行号/帧数；同时 emit sourceLineRequested 高亮编辑器对应源码行。
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
            stackHtml += QString("<li><code>%1</code></li>").arg(QString::fromUtf8(it->c_str()).toHtmlEscaped());
        }
        stackHtml += QStringLiteral("</ol>");
    }

    QString html = QString("<h3>Step %1: %2</h3>"
                           "<p><b>IP:</b> %3 | <b>行:</b> %4 | <b>帧数:</b> %5</p>"
                           "%6")
                       .arg(e.step)
                       .arg(QString::fromUtf8(e.opCodeName.c_str()).toHtmlEscaped()) // R54-15 fix: HTML 转义
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

/// 用 OpCode 教学库名称填充左侧列表并默认选中首项。
void BytecodeTracePanel::populateDocs() {
    docList_->clear();
    for (const auto& d : BytecodeTraceLibrary::opCodeDocs()) {
        docList_->addItem(QString::fromUtf8(d.opCodeName.c_str()));
    }
    if (docList_->count() > 0) {
        docList_->setCurrentRow(0);
    }
}

/// 教学库列表选中项变化时委托 showDoc 渲染该 OpCode 的指令详情。
void BytecodeTracePanel::onDocSelected(int index) {
    showDoc(index);
}

/// 根据索引渲染 OpCode 文档：标题、分类、操作数格式、栈效果、Markdown 语义
/// 与样例代码（HTML 转义），写入右侧说明浏览器。
void BytecodeTracePanel::showDoc(int index) {
    currentDocIdx_ = index;
    if (index < 0 || index >= static_cast<int>(BytecodeTraceLibrary::opCodeDocs().size())) {
        docDetail_->clear();
        return;
    }
    const auto& d = BytecodeTraceLibrary::opCodeDocs()[index];
    // R54-16 fix: 统一 HTML 转义所有字段，对齐 R52 批量转义修复
    QString html = QString("<h2>%1</h2>"
                           "<p><b>分类:</b> %2</p>"
                           "<h3>操作数格式</h3>"
                           "<p><code>%3</code></p>"
                           "<h3>栈效果</h3>"
                           "<p><code>%4</code></p>"
                           "<h3>语义</h3>"
                           "%5"
                           "<h3>样例代码</h3>"
                           "<pre>%6</pre>")
                       .arg(QString::fromUtf8(d.opCodeName.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.category.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.operandFormat.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.stackEffect.c_str()).toHtmlEscaped())
                       .arg(MarkdownRenderer::markdownToHtmlFragment(d.semantics))
                       .arg(QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped());
    docDetail_->setHtml(html);
}

/// 将当前选中 OpCode 的样例代码 emit loadSampleRequested，加载到主编辑器运行观察。
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

/// 构建 5 步新手引导：高亮执行轨迹页、自动捕获、轨迹表、OpCode 教学库与加载样例按钮。
GuidedTour* BytecodeTracePanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageTraceBtn_, QString::fromUtf8("执行轨迹"),
                  QString::fromUtf8("这里显示每条字节码指令执行后的栈状态快照。"));
    tour->addStep(autoCaptureCheck_, QString::fromUtf8("自动捕获"),
                  QString::fromUtf8("勾选后，VM 暂停时会自动捕获一条轨迹，无需手动点击。"));
    tour->addStep(traceTable_, QString::fromUtf8("轨迹表"),
                  QString::fromUtf8("每行一条指令记录，包含 IP / OpCode / 栈快照。点击某行可定位到对应源码行。"));
    tour->addStep(pageLibraryBtn_, QString::fromUtf8("OpCode 教学库"),
                  QString::fromUtf8("切换到 OpCode 参考库，查看每条指令的语义说明与样例代码。"));
    tour->addStep(loadCodeBtn_, QString::fromUtf8("加载样例"),
                  QString::fromUtf8("点击可将当前 OpCode 的示例代码加载到主编辑器，方便直接运行观察。"));
    return tour;
}
