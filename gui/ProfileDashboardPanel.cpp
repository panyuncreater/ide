// ============================================================
// ProfileDashboardPanel.cpp — 性能剖析仪表盘实现（第二波 P1-2）
// ============================================================

#include "gui/ProfileDashboardPanel.h"
#include "app/IdeController.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/GcManager.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPaintEvent>
#include <QPainter>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <sstream>

#include "Label.h"      // QFluentKit（CaptionLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

#ifdef MINILANG_HAVE_QTCHARTS
// C2: QtCharts 头文件 — 仅在编译时启用 MINILANG_USE_QTCHARTS 时引入
#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QChartView>
#include <QtCharts/QValueAxis>
QT_CHARTS_USE_NAMESPACE
#endif

// ============================================================
// ProfileLibrary — 性能场景库
// ============================================================

/// 返回预设的性能测试场景列表（静态数据）。
const std::vector<ProfileScenario>& ProfileLibrary::scenarios() {
    static const std::vector<ProfileScenario> kScenarios = {
        {"fib-recursion", "斐波那契递归（fib(15)）",
         "递归型 fib(15)。栈式 VM 与 RegisterVM 在密集函数调用场景下都显著快于 Interpreter（解释器每个 AST "
         "节点都需要虚函数分发）。"
         "RegisterVM 通常略快于 StackVM（寄存器消除 push/pop 内存往返），但差距小于 Interpreter vs VM。",
         "fun fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }\nprint(fib(15));", "arithmetic", 3},
        {"loop-sum", "循环求和（1 到 100000）",
         "纯算术循环。栈式 VM 与 RegisterVM 在热路径上优势最明显——单条 OP_ADD 比访问者模式 dispatch 快 5-10 倍。"
         "RegisterVM 通过虚拟寄存器避免每次运算 push/pop，进一步降低内存带宽占用。",
         "var sum = 0;\nvar i = 1;\nwhile (i <= 100000) { sum = sum + i; i = i + 1; }\nprint(sum);", "loop", 3},
        {"string-concat", "字符串拼接循环（1000 次）",
         "字符串 + 拼接。每次拼接会构造新 StringData（不可变语义），三后端性能相近（瓶颈在堆分配而非指令分发）。"
         "GC 在此场景频繁触发，sweep 开销可能拉低三后端共同基线。",
         "var s = \"\";\nvar i = 0;\nwhile (i < 1000) { s = s + \"x\"; i = i + 1; }\nprint(s.len());", "string", 3},
        {"class-instantiation", "类实例化循环（10000 次）",
         "Point 类构造 + 字段赋值循环。InstanceData 分配 + GcManager::registerTracked 是主要开销，"
         "三后端差距较小（解释器仍稍慢，因为 MethodCall 的 Visitor 分发）。",
         "class Point { var x; var y; fun init(px, py) { x = px; y = py; } }\nvar i = 0;\nwhile (i < 10000) { var p = "
         "Point(i, i); i = i + 1; }\nprint(\"done\");",
         "class", 3},
        {"dict-access", "字典访问循环（2000 次）",
         "字典键值读写循环。DictData 使用 unordered_map，每次访问涉及哈希计算，"
         "三后端性能相近（瓶颈在 hash 而非指令分发），Interpreter 略慢。",
         "var d = {};\nvar i = 0;\nwhile (i < 2000) { d[\"k\" + i] = i * 2; i = i + 1; }\nprint(d.len());", "loop", 3},
    };
    return kScenarios;
}

// ============================================================
// OpCodeProfileLibrary — OpCode 性能文档静态库（P1-1 配套）
// ============================================================
// 与 BytecodeTracePanel 的 OpCode 教学库互补：
// - BytecodeTraceLibrary 聚焦"指令语义与栈效应"
// - OpCodeProfileLibrary 聚焦"性能特征与热点分析"
// 帮助学习者理解为何某些指令是热点、不同后端的指令密度差异

/// 返回各 opcode 的性能说明文档列表（静态数据）。
const std::vector<OpCodePerfDoc>& OpCodeProfileLibrary::docs() {
    static const std::vector<OpCodePerfDoc> kDocs = {
        {"OP_ADD", "arith",
         "整数加法。在循环场景下是绝对热点——单条 OP_ADD 比解释器 Visitor dispatch 快 5-10 倍。"
         "RegisterVM 通过虚拟寄存器避免 push/pop 内存往返，进一步降低开销。",
         "var s = 0; var i = 1; while (i <= 100000) { s = s + i; i = i + 1; }"},
        {"OP_SUBTRACT", "arith", "整数减法。性能特征与 OP_ADD 一致，但热度通常较低（循环场景下递增多于递减）。",
         "var d = 100; while (d > 0) { d = d - 1; }"},
        {"OP_GET_LOCAL", "var",
         "读取局部变量槽位。在 StackVM 中是热点——每次变量引用都触发一次栈读取。"
         "RegisterVM 通过虚拟寄存器直接访问，无需 OP_GET_LOCAL 指令（指令密度显著降低）。",
         "var x = 1; var y = 2; var z = x + y;"},
        {"OP_SET_LOCAL", "var",
         "写入局部变量槽位。在循环体内频繁触发（如 i = i + 1）。"
         "与 OP_GET_LOCAL 配对出现，是 StackVM 指令密度的典型代表。",
         "var i = 0; while (i < 100) { i = i + 1; }"},
        {"OP_GET_GLOBAL", "var",
         "读取全局变量。比 OP_GET_LOCAL 稍慢（需查 hash 表）。"
         "在密集全局变量引用场景下可能成为热点。",
         "var g = 42; fun f() { return g; } f();"},
        {"OP_JUMP_IF_FALSE", "control",
         "条件跳转。在 while/if 场景下每次迭代触发。"
         "分支预测失败的代价高于指令本身，但 MiniLang VM 无分支预测（解释执行）。",
         "var i = 0; while (i < 100) { i = i + 1; }"},
        {"OP_CALL", "call",
         "函数调用。开销最大——涉及帧栈分配、参数传递、返回地址保存。"
         "fib(20) 场景下 OP_CALL 触发 ~21891 次，是 RegisterVM 相对 StackVM 优势最明显的指令。",
         "fun add(a, b) { return a + b; } add(1, 2);"},
        {"OP_RETURN", "call", "函数返回。与 OP_CALL 配对，开销同样较大（帧栈回收、返回值传递）。",
         "fun f() { return 42; } f();"},
        {"OP_BUILD_ARRAY", "container",
         "构造数组。涉及堆分配 + GcManager::registerTracked。"
         "在大数组构造场景下是热点，且 GC 压力大。",
         "var a = [1, 2, 3, 4, 5];"},
        {"OP_CLOSURE", "call",
         "构造闭包。涉及 ClosureData 分配 + upvalue 捕获。"
         "在闭包循环场景下热点明显，开销高于普通函数调用。",
         "fun makeCounter() { var c = 0; fun counter() { c = c + 1; return c; } return counter; }"},
        {"OP_GET_UPVALUE", "call",
         "读取闭包捕获变量。比 OP_GET_LOCAL 稍慢（需通过 upvalue 链间接访问）。"
         "在密集闭包调用场景下与 OP_CLOSURE 配对出现。",
         "fun makeCounter() { var c = 0; fun counter() { c = c + 1; return c; } return counter; } var f = "
         "makeCounter(); f();"},
        {"OP_METHOD_CALL", "call",
         "方法调用。比 OP_CALL 更昂贵——涉及方法查找（method resolution）。"
         "在类实例方法密集调用场景下是热点。",
         "class P { fun m() { return 1; } } var p = P(); p.m();"},
    };
    return kDocs;
}

// ============================================================
// ProfileDashboardPanel 实现
// ============================================================

/// 构造性能基准面板：初始化场景选择器、图表区与状态动画。
ProfileDashboardPanel::ProfileDashboardPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runProfileBtn_ = new PrimaryPushButton(QString::fromUtf8("运行剖析"), this);
    statusLabel_ = new CaptionLabel(QString::fromUtf8("请选择场景"), this);
    btnBar->addWidget(runProfileBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    mainLayout->addLayout(btnBar);

    // 主体：左场景 + 右图表区
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    scenarioList_ = new QListWidget(this);
    scenarioDesc_ = new QTextBrowser(this);

    auto* leftWrap = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftWrap);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(2);
    // scenarioList_ 自然高度（6 项），scenarioDesc_ 拉伸填满左下角剩余空间，
    // 避免描述区下方留出一小截空白。
    leftLayout->addWidget(scenarioList_, 0);
    leftLayout->addWidget(scenarioDesc_, 1);
    splitter->addWidget(leftWrap);

    auto* rightWrap = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightWrap);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // P1-1: 右侧改为 QTabWidget 切换"时间对比" / "指令计数"两个视图
    auto* rightTab = new QTabWidget(this);

    // ---- Tab 1: 时间对比（原有内容）----
    auto* timingTab = new QWidget(this);
    auto* timingLayout = new QVBoxLayout(timingTab);
    timingLayout->setContentsMargins(0, 0, 0, 0);
    // R111: 柱状图维度切换（耗时/指令数/内存）
    auto* metricLayout = new QHBoxLayout();
    metricLayout->addWidget(new QLabel(QString::fromUtf8("柱状图维度：")), 0);
    metricCombo_ = new QComboBox(this);
    metricCombo_->addItem(QString::fromUtf8("耗时 (μs)"), static_cast<int>(MetricDimension::Time));
    metricCombo_->addItem(QString::fromUtf8("指令数"), static_cast<int>(MetricDimension::Instructions));
    metricCombo_->addItem(QString::fromUtf8("内存 (GC tracked 峰值)"), static_cast<int>(MetricDimension::Memory));
    connect(metricCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ProfileDashboardPanel::onMetricChanged);
    metricLayout->addWidget(metricCombo_, 0);
    metricLayout->addStretch(1);
    timingLayout->addLayout(metricLayout, 0);
    timingLayout->addWidget(new QLabel(QString::fromUtf8("三后端综合性能对比：")), 0);
    // R111: 表格扩展为 6 列（后端 / 平均 / 标准差 / 比值 / 指令数 / 内存峰值）
    resultTable_ = new QTableWidget(0, 6, this);
    resultTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("后端"),
        QString::fromUtf8("平均 (μs)"),
        QString::fromUtf8("标准差 (μs)"),
        QString::fromUtf8("比值"),
        QString::fromUtf8("指令数"),
        QString::fromUtf8("内存峰值"),
    });
    resultTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    resultTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    resultTable_->setFixedHeight(120);
    timingLayout->addWidget(resultTable_, 0);

#ifdef MINILANG_HAVE_QTCHARTS
    // C2: QtCharts 模式 — 创建 QChartView 并加入布局（替代 paintEvent）
    chart_ = new QChart();
    chart_->setTitle(QString::fromUtf8("三后端平均时间对比 (μs)"));
    chart_->legend()->setVisible(true);
    chart_->legend()->setAlignment(Qt::AlignBottom);
    chartView_ = new QChartView(chart_, this);
    chartView_->setRenderHint(QPainter::Antialiasing);
    chartView_->setMinimumHeight(220);
    timingLayout->addWidget(chartView_, 1);
#else
    // QPainter 自绘柱状图模式（默认，零额外依赖）
    // 通过 setMinimumHeight 给 paintEvent 留出空间
    setMinimumHeight(360);
#endif

    timingLayout->addWidget(new QLabel(QString::fromUtf8("性能分析：")), 0);
    analysisView_ = new QTextBrowser(this);
    timingLayout->addWidget(analysisView_, 1);
    rightTab->addTab(timingTab, QString::fromUtf8("时间对比"));

    // ---- Tab 2: 指令计数（P1-1 新增）----
    auto* opCodeTab = new QWidget(this);
    auto* opCodeLayout = new QVBoxLayout(opCodeTab);
    opCodeLayout->setContentsMargins(0, 0, 0, 0);
    opCodeLayout->addWidget(new QLabel(QString::fromUtf8("StackVM 热点 OpCode Top 10：")), 0);
    stackVmOpTable_ = new QTableWidget(0, 3, this);
    stackVmOpTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stackVmOpTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("OpCode"),
        QString::fromUtf8("执行次数"),
        QString::fromUtf8("占比"),
    });
    stackVmOpTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    stackVmOpTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    stackVmOpTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    stackVmOpTable_->setMaximumHeight(180);
    opCodeLayout->addWidget(stackVmOpTable_, 0);

    opCodeLayout->addWidget(new QLabel(QString::fromUtf8("RegisterVM 热点 OpCode Top 10：")), 0);
    registerVmOpTable_ = new QTableWidget(0, 3, this);
    registerVmOpTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    registerVmOpTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("OpCode"),
        QString::fromUtf8("执行次数"),
        QString::fromUtf8("占比"),
    });
    registerVmOpTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    registerVmOpTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    registerVmOpTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    registerVmOpTable_->setMaximumHeight(180);
    opCodeLayout->addWidget(registerVmOpTable_, 0);

    opCodeLayout->addWidget(new QLabel(QString::fromUtf8("OpCode 性能文档：")), 0);
    opCodeDocView_ = new QTextBrowser(this);
    opCodeLayout->addWidget(opCodeDocView_, 1);
    // 默认显示第一个 OpCode 文档
    if (!OpCodeProfileLibrary::docs().empty()) {
        const auto& d = OpCodeProfileLibrary::docs()[0];
        std::ostringstream os;
        os << "<b>" << d.opCode << "</b> <i>[" << d.category << "]</i><hr>";
        os << "<p>" << d.perfNote << "</p>";
        os << "<p><b>示例：</b> <code>" << QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped().toStdString()
           << "</code></p>";
        opCodeDocView_->setHtml(QString::fromUtf8(os.str().c_str()));
    }
    rightTab->addTab(opCodeTab, QString::fromUtf8("指令计数"));

    rightLayout->addWidget(rightTab);

    splitter->addWidget(rightWrap);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});
    mainLayout->addWidget(splitter, 1);

    // 填充场景列表
    const auto& items = ProfileLibrary::scenarios();
    for (const auto& s : items) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty())
        scenarioList_->setCurrentRow(0);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto& items = ProfileLibrary::scenarios();
        if (row < 0 || row >= (int)items.size())
            return;
        const auto& s = items[row];
        std::ostringstream os;
        os << "<b>" << s.title << "</b><br>";
        os << "<i>[" << s.category << "]</i> " << s.iterations << " 次迭代<br><hr>";
        os << "<p>" << s.description << "</p>";
        scenarioDesc_->setHtml(QString::fromUtf8(os.str().c_str()));
    });

    connect(runProfileBtn_, &QPushButton::clicked, this, [this]() {
        int idx = scenarioList_->currentRow();
        if (idx < 0) {
            statusLabel_->setText(QString::fromUtf8("请先选择场景"));
            return;
        }
        runProfile(idx);
    });

    if (!items.empty()) {
        // 触发一次 scenarioDesc 填充
        emit scenarioList_->currentRowChanged(0);
    }
}

// ---- 后端测量 ----

/// 单次用解释器执行被测代码并返回耗时（秒）。
std::pair<double, size_t> ProfileDashboardPanel::measureInterpreterOnce(Block& ast) {
    // R111: 测量前清空 GcManager tracked 列表，确保峰值反映本后端执行
    GcManager::instance().reset();
    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    auto t0 = std::chrono::high_resolution_clock::now();
    interp.execute(ast);
    auto t1 = std::chrono::high_resolution_clock::now();
    size_t peakTracked = GcManager::instance().trackedCount();
    return {std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), peakTracked};
}

// ============================================================
// P1-1: 真实 instrumentation — 通过 stepCallback 累加 opcode 计数
// ============================================================
// 设计要点：
//   - 复用 VM/RegisterVM 已有的 stepCallback_ 机制（无需引擎层改造）
//   - ProfileDashboardPanel 直接 new VM/RegisterVM，绕过 VmStepper 的禁用逻辑
//   - stepCallback 签名：void(const VMStepInfo&) / void(const RegVMStepInfo&)
//   - 性能开销：每条指令一次 std::function 调用（~20-50ns），对剖析场景可接受
//   - R111: 返回 std::tuple<double, std::array<uint64_t, 256>, size_t>：时间 + opcode 计数 + GC tracked 峰值

std::tuple<double, std::array<uint64_t, 256>, size_t> ProfileDashboardPanel::measureStackVMWithProfile(Block& ast) {
    // R111: 测量前清空 GcManager tracked 列表
    GcManager::instance().reset();
    Compiler compiler;
    auto result = compiler.compile(ast);
    if (compiler.getDiagnostics().hasErrors()) {
        throw std::runtime_error("Compile error: " + compiler.getDiagnostics().summary());
    }
    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    std::array<uint64_t, 256> counts{};
    vm.setStepCallback([&counts](const VMStepInfo& info) { counts[static_cast<uint8_t>(info.opcode)]++; });
    vm.setStepCallbackEnabled(true);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto vmres = vm.execute(result);
    auto t1 = std::chrono::high_resolution_clock::now();
    vm.setStepCallbackEnabled(false);
    if (vmres != VMResult::VM_OK) {
        throw std::runtime_error("VM runtime error");
    }
    size_t peakTracked = GcManager::instance().trackedCount();
    return {std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), counts, peakTracked};
}

std::tuple<double, std::array<uint64_t, 256>, size_t> ProfileDashboardPanel::measureRegisterVMWithProfile(Block& ast) {
    // R111: 测量前清空 GcManager tracked 列表
    GcManager::instance().reset();
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    auto regResult = compiler.compileViaRegisterIR(ast);
    if (compiler.getDiagnostics().hasErrors()) {
        throw std::runtime_error("Compile error: " + compiler.getDiagnostics().summary());
    }
    RegisterVM vm;
    vm.setOutputCallback([](const std::string&) {});
    std::array<uint64_t, 256> counts{};
    vm.setStepCallback([&counts](const RegVMStepInfo& info) { counts[static_cast<uint8_t>(info.opcode)]++; });
    vm.setStepCallbackEnabled(true);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto vmres = vm.execute(regResult);
    auto t1 = std::chrono::high_resolution_clock::now();
    vm.setStepCallbackEnabled(false);
    if (vmres != VMResult::VM_OK) {
        throw std::runtime_error("RegisterVM runtime error");
    }
    size_t peakTracked = GcManager::instance().trackedCount();
    return {std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), counts, peakTracked};
}

ProfileDashboardPanel::BackendTiming
ProfileDashboardPanel::measureBackend(const std::string& name, std::function<std::pair<double, size_t>(Block&)> measure,
                                      Block& ast, int iterations) {
    BackendTiming t;
    t.name = name;
    std::vector<double> samples;
    samples.reserve(iterations);
    size_t maxTracked = 0;
    try {
        for (int i = 0; i < iterations; ++i) {
            auto [us, tracked] = measure(ast);
            samples.push_back(us);
            if (tracked > maxTracked)
                maxTracked = tracked;
        }
        t.avgMicros = mean(samples);
        t.stddevMicros = stddev(samples);
        t.peakTrackedCount = maxTracked;
        t.success = true;
    } catch (const std::exception& e) {
        t.success = false;
        t.errorMessage = e.what();
    }
    return t;
}

// ============================================================
// 问题 6: "运行中"状态动画 — 橙色背景 + 循环圆点，避免误认为卡死
// ============================================================

/// 启动状态区「运行中」动画（点号循环）以提示正在测速。
void ProfileDashboardPanel::startStatusAnimation(const QString& base) {
    statusRunningBase_ = base;
    statusAnimDots_ = 0;
    if (!statusAnimTimer_) {
        statusAnimTimer_ = new QTimer(this);
        connect(statusAnimTimer_, &QTimer::timeout, this, [this]() {
            statusAnimDots_ = (statusAnimDots_ + 1) % 4;
            QString dots(statusAnimDots_, '.');
            statusLabel_->setText(statusRunningBase_ + dots);
        });
    }
    statusAnimTimer_->start(400); // 400ms 切换一次
    statusLabel_->setText(base + ".");
}

/// 停止状态动画并恢复静态文案。
void ProfileDashboardPanel::stopStatusAnimation() {
    if (statusAnimTimer_) {
        statusAnimTimer_->stop();
    }
}

/// 对指定场景在三后端上分别测速并收集计时结果。
void ProfileDashboardPanel::runProfile(int scenarioIndex) {
    const auto& items = ProfileLibrary::scenarios();
    if (scenarioIndex < 0 || scenarioIndex >= (int)items.size())
        return;
    // AUDIT-P1 fix: 重入守卫——processEvents(ExcludeUserInputEvents) 期间定时器/信号槽
    // 可能触发间接调用 runProfile，导致 lastResults_ 被覆盖、状态混乱。
    if (profiling_)
        return;
    profiling_ = true;
    const auto& scenario = items[scenarioIndex];

    runProfileBtn_->setEnabled(false);
    // 问题 6: 醒目的"运行中"状态 — 橙色背景 + 动画圆点 + 进度
    startStatusAnimation(QString::fromUtf8("运行中 [1/3] Interpreter"));
    statusLabel_->setStyleSheet("QLabel { background: #CB4B16; color: white; border-radius: 4px;"
                                "  padding: 4px 12px; font-weight: bold; }");
    // BUG-GUI-AUDIT-1 fix attempt: Qt 6 已移除通用 ExcludeTimers flag，保持 ExcludeUserInputEvents。
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    // ROUND-76 fix: ExcludeUserInputEvents 不排除 QCloseEvent！
    // 若用户在 processEvents 期间关闭窗口，closeEvent 已在此同步执行并 setClosing(true)。
    // 必须立即退出，避免后续访问正在关闭的 widget。
    if (closing_) {
        stopStatusAnimation();
        profiling_ = false;
        return;
    }

    // 先 lex + parse 源码
    Lexer lexer;
    auto tokens = lexer.scan(scenario.sourceCode);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.getDiagnostics().hasErrors()) {
        stopStatusAnimation();
        statusLabel_->setStyleSheet("");
        statusLabel_->setText(
            QString::fromUtf8("解析失败：%1").arg(QString::fromUtf8(parser.getDiagnostics().summary().c_str())));
        runProfileBtn_->setEnabled(true);
        profiling_ = false; // AUDIT-P1 fix: 错误返回点恢复守卫
        return;
    }

    // 多次测量三后端
    std::vector<BackendTiming> results;
    results.push_back(measureBackend(
        "Interpreter", [this](Block& a) { return measureInterpreterOnce(a); }, *ast, scenario.iterations));

    // 问题 7: 在后端之间处理事件，避免长时间阻塞 UI
    statusRunningBase_ = QString::fromUtf8("运行中 [2/3] StackVM");
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    // ROUND-76 fix: 检查 closing_ 避免在窗口关闭后继续执行
    if (closing_) {
        stopStatusAnimation();
        profiling_ = false;
        return;
    }

    // P1-1: StackVM / RegisterVM 走 measureXxxWithProfile，同时累加 opcode 计数
    // R77 fix: 指令计数取 N 次平均（与时间统计 mean(samples) 一致），否则显示值为
    // 单次实际值的 N 倍（iterations=3 时 OP_CALL 等热点被放大 3 倍），与 OpCode
    // 性能文档库的参考量级（如"fib(20) OP_CALL ~21891 次"）矛盾。
    // R111: 同时累加 GC tracked 峰值（取 max）用于内存维度
    std::array<uint64_t, 256> stackVmCounts{};
    std::array<uint64_t, 256> registerVmCounts{};
    {
        BackendTiming t;
        t.name = "StackVM";
        std::vector<double> samples;
        samples.reserve(scenario.iterations);
        size_t maxTracked = 0;
        try {
            for (int i = 0; i < scenario.iterations; ++i) {
                auto [us, counts, tracked] = measureStackVMWithProfile(*ast);
                samples.push_back(us);
                for (size_t j = 0; j < 256; ++j)
                    stackVmCounts[j] += counts[j];
                if (tracked > maxTracked)
                    maxTracked = tracked;
            }
            // 取平均：指令计数是确定值，取平均消除潜在非确定性，与 avgMicros 口径一致
            if (scenario.iterations > 0) {
                for (size_t j = 0; j < 256; ++j)
                    stackVmCounts[j] /= static_cast<uint64_t>(scenario.iterations);
            }
            t.avgMicros = mean(samples);
            t.stddevMicros = stddev(samples);
            t.peakTrackedCount = maxTracked;
            // R111: 计算指令总数（sum of opcode counts）
            uint64_t total = 0;
            for (size_t j = 0; j < 256; ++j)
                total += stackVmCounts[j];
            t.totalInstructions = total;
            t.success = true;
        } catch (const std::exception& e) {
            t.success = false;
            t.errorMessage = e.what();
        }
        results.push_back(t);
    }

    statusRunningBase_ = QString::fromUtf8("运行中 [3/3] RegisterVM");
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    // ROUND-76 fix: 检查 closing_ 避免在窗口关闭后继续执行
    if (closing_) {
        stopStatusAnimation();
        profiling_ = false;
        return;
    }

    {
        BackendTiming t;
        t.name = "RegisterVM";
        std::vector<double> samples;
        samples.reserve(scenario.iterations);
        size_t maxTracked = 0;
        try {
            for (int i = 0; i < scenario.iterations; ++i) {
                auto [us, counts, tracked] = measureRegisterVMWithProfile(*ast);
                samples.push_back(us);
                for (size_t j = 0; j < 256; ++j)
                    registerVmCounts[j] += counts[j];
                if (tracked > maxTracked)
                    maxTracked = tracked;
            }
            // R77 fix: 取平均，与 StackVM 口径一致
            if (scenario.iterations > 0) {
                for (size_t j = 0; j < 256; ++j)
                    registerVmCounts[j] /= static_cast<uint64_t>(scenario.iterations);
            }
            t.avgMicros = mean(samples);
            t.stddevMicros = stddev(samples);
            t.peakTrackedCount = maxTracked;
            // R111: 计算指令总数
            uint64_t total = 0;
            for (size_t j = 0; j < 256; ++j)
                total += registerVmCounts[j];
            t.totalInstructions = total;
            t.success = true;
        } catch (const std::exception& e) {
            t.success = false;
            t.errorMessage = e.what();
        }
        results.push_back(t);
    }

    lastResults_ = results;

    // P1-1: 聚合 opcode 计数为 Top N 热点列表（按计数降序）
    auto aggregateTop10 = [](const std::array<uint64_t, 256>& counts,
                             bool isRegisterVm) -> std::vector<OpCodeProfileEntry> {
        std::vector<OpCodeProfileEntry> all;
        uint64_t total = 0;
        for (size_t i = 0; i < 256; ++i) {
            if (counts[i] > 0) {
                std::string name;
                if (isRegisterVm) {
                    name = regOpName(static_cast<RegOp>(i));
                } else {
                    name = opCodeName(static_cast<OpCode>(i));
                }
                all.push_back({name, counts[i], 0.0});
                total += counts[i];
            }
        }
        std::sort(all.begin(), all.end(),
                  [](const OpCodeProfileEntry& a, const OpCodeProfileEntry& b) { return a.count > b.count; });
        if (all.size() > 10)
            all.resize(10);
        if (total > 0) {
            for (auto& e : all)
                e.ratio = static_cast<double>(e.count) / static_cast<double>(total);
        }
        return all;
    };
    lastStackVMOpProfile_ = aggregateTop10(stackVmCounts, false);
    lastRegisterVMOpProfile_ = aggregateTop10(registerVmCounts, true);

    renderResults(results, scenario);
    renderOpCodeProfile(lastStackVMOpProfile_, lastRegisterVMOpProfile_);
#ifdef MINILANG_HAVE_QTCHARTS
    renderChart(results); // C2: QtCharts 模式刷新柱状图
#else
    update(); // QPainter 模式触发 paintEvent 重绘柱状图
#endif
    runProfileBtn_->setEnabled(true);
    stopStatusAnimation();
    statusLabel_->setStyleSheet("");
    statusLabel_->setText(QString::fromUtf8("剖析完成"));
    profiling_ = false; // AUDIT-P1 fix: 正常结束点恢复守卫
}

/// 将测速结果渲染为后端对比柱状图与概览文本。
void ProfileDashboardPanel::renderResults(const std::vector<BackendTiming>& results, const ProfileScenario& scenario) {
    resultTable_->setRowCount((int)results.size());
    double minAvg = std::numeric_limits<double>::max();
    for (const auto& r : results) {
        if (r.success && r.avgMicros < minAvg)
            minAvg = r.avgMicros;
    }
    for (int i = 0; i < (int)results.size(); ++i) {
        const auto& r = results[i];
        resultTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(r.name.c_str())));
        if (r.success) {
            resultTable_->setItem(i, 1, new QTableWidgetItem(QString::number(r.avgMicros, 'f', 1)));
            resultTable_->setItem(i, 2, new QTableWidgetItem(QString::number(r.stddevMicros, 'f', 1)));
            double ratio = (minAvg > 0) ? r.avgMicros / minAvg : 0.0;
            QString ratioText =
                (ratio > 1.001) ? QString::fromUtf8("%1x 慢").arg(ratio, 0, 'f', 2) : QString::fromUtf8("最快");
            resultTable_->setItem(i, 3, new QTableWidgetItem(ratioText));
            // R111: 指令数（Interpreter 无指令概念，显示 "—")
            QString instrText =
                (r.totalInstructions > 0) ? QString::number(r.totalInstructions) : QString::fromUtf8("—");
            resultTable_->setItem(i, 4, new QTableWidgetItem(instrText));
            // R111: 内存峰值（GC tracked 节点数）
            resultTable_->setItem(i, 5,
                                  new QTableWidgetItem(QString::number(static_cast<qulonglong>(r.peakTrackedCount))));
        } else {
            // 问题 7: 显示失败原因而非仅"失败"，帮助诊断后端兼容性问题
            QString errText = QString::fromUtf8("失败: ") + QString::fromUtf8(r.errorMessage.c_str()).left(60);
            auto* errItem = new QTableWidgetItem(errText);
            errItem->setToolTip(QString::fromUtf8(r.errorMessage.c_str()));
            errItem->setForeground(QColor("#CC0000"));
            resultTable_->setItem(i, 1, errItem);
            resultTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8("—")));
            resultTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8("—")));
            resultTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8("—")));
            resultTable_->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8("—")));
        }
    }
    analysisView_->setHtml(buildAnalysis(results, scenario));
}

// P1-1: 渲染指令计数表格（StackVM / RegisterVM Top 10 热点 opcode）
/// 渲染逐 opcode 的性能明细表（各后端耗时）。
void ProfileDashboardPanel::renderOpCodeProfile(const std::vector<OpCodeProfileEntry>& stackVmProfile,
                                                const std::vector<OpCodeProfileEntry>& registerVmProfile) {
    // StackVM 表格
    stackVmOpTable_->setRowCount((int)stackVmProfile.size());
    for (int i = 0; i < (int)stackVmProfile.size(); ++i) {
        const auto& e = stackVmProfile[i];
        stackVmOpTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(e.opCodeName.c_str())));
        stackVmOpTable_->setItem(i, 1, new QTableWidgetItem(QString::number(e.count)));
        stackVmOpTable_->setItem(i, 2, new QTableWidgetItem(QString::number(e.ratio * 100.0, 'f', 2) + "%"));
    }
    // RegisterVM 表格
    registerVmOpTable_->setRowCount((int)registerVmProfile.size());
    for (int i = 0; i < (int)registerVmProfile.size(); ++i) {
        const auto& e = registerVmProfile[i];
        registerVmOpTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(e.opCodeName.c_str())));
        registerVmOpTable_->setItem(i, 1, new QTableWidgetItem(QString::number(e.count)));
        registerVmOpTable_->setItem(i, 2, new QTableWidgetItem(QString::number(e.ratio * 100.0, 'f', 2) + "%"));
    }
}

/// 根据测速结果生成中文性能分析结论文本。
QString ProfileDashboardPanel::buildAnalysis(const std::vector<BackendTiming>& results,
                                             const ProfileScenario& scenario) {
    std::ostringstream os;
    os << "<h3>" << scenario.title << "</h3>";
    os << "<p><i>[" << scenario.category << "]</i> " << scenario.iterations << " 次迭代平均</p>";
    os << "<p>" << scenario.description << "</p>";
    os << "<hr>";

    // 找出最快后端
    const BackendTiming* fastest = nullptr;
    for (const auto& r : results) {
        if (r.success && (!fastest || r.avgMicros < fastest->avgMicros)) {
            fastest = &r;
        }
    }
    if (!fastest) {
        os << "<p style='color:#cc0000;'><b>所有后端均失败</b></p>";
    } else {
        os << "<p><b>最快后端：</b>" << fastest->name << " (" << fastest->avgMicros << " μs)</p>";
        // 计算与最慢的比值
        const BackendTiming* slowest = nullptr;
        for (const auto& r : results) {
            if (r.success && (!slowest || r.avgMicros > slowest->avgMicros)) {
                slowest = &r;
            }
        }
        if (slowest && slowest != fastest) {
            double speedup = slowest->avgMicros / fastest->avgMicros;
            os << "<p><b>最慢后端：</b>" << slowest->name << " (" << slowest->avgMicros << " μs)</p>";
            os << "<p><b>加速比：</b>" << speedup << "x</p>";
        }
    }
    // 问题 7: 列出失败后端及其错误原因
    for (const auto& r : results) {
        if (!r.success) {
            // R52-7 fix: errorMessage 来自 e.what()，可能含 < > & 字符（如 "expected <expression>"）需转义
            os << "<p style='color:#cc0000;'><b>" << r.name << " 失败：</b>"
               << QString::fromUtf8(r.errorMessage.c_str()).toHtmlEscaped().toStdString() << "</p>";
        }
    }
    os << "<hr>";
    os << "<p><small>注：标准差反映多次运行的稳定性。"
       << "若标准差 > 10% 平均值，可能是 GC 周期或系统调度影响。</small></p>";
    return QString::fromUtf8(os.str().c_str());
}

// ============================================================
// R111: 柱状图维度切换 — 耗时 / 指令数 / 内存
// ============================================================
// 设计要点：
//   - currentMetric_ 决定柱状图归一化与标签数值
//   - Interpreter 无指令概念（totalInstructions=0），Instructions 维度下显示空柱
//     并保留 zero-based 的归一化基准（避免除零）
//   - 切换维度时 QPainter 模式触发 update()，QtCharts 模式触发 renderChart()

/// R111: 从 BackendTiming 提取当前维度的数值（统一为 double 用于归一化与绘制）
double ProfileDashboardPanel::getMetricValue(const BackendTiming& r, MetricDimension metric) {
    switch (metric) {
    case MetricDimension::Time:
        return r.avgMicros;
    case MetricDimension::Instructions:
        return static_cast<double>(r.totalInstructions);
    case MetricDimension::Memory:
        return static_cast<double>(r.peakTrackedCount);
    }
    return 0.0;
}

/// R111: 当前维度的 Y 轴标题与数值格式
QString ProfileDashboardPanel::metricAxisTitle(MetricDimension metric) {
    switch (metric) {
    case MetricDimension::Time:
        return QString::fromUtf8("平均时间 (μs)");
    case MetricDimension::Instructions:
        return QString::fromUtf8("指令数");
    case MetricDimension::Memory:
        return QString::fromUtf8("GC tracked 峰值");
    }
    return {};
}

/// R111: 当前维度的柱顶数值标签文本
QString ProfileDashboardPanel::metricLabel(const BackendTiming& r, MetricDimension metric) {
    if (!r.success)
        return {};
    switch (metric) {
    case MetricDimension::Time:
        return QString::number(r.avgMicros, 'f', 0);
    case MetricDimension::Instructions:
        return QString::number(r.totalInstructions);
    case MetricDimension::Memory:
        return QString::number(static_cast<qulonglong>(r.peakTrackedCount));
    }
    return {};
}

/// R111: 维度切换槽 — 更新 currentMetric_ 并触发重绘
void ProfileDashboardPanel::onMetricChanged(int index) {
    if (index < 0 || index >= metricCombo_->count())
        return;
    int data = metricCombo_->itemData(index).toInt();
    currentMetric_ = static_cast<MetricDimension>(data);
#ifdef MINILANG_HAVE_QTCHARTS
    renderChart(lastResults_);
#else
    update();
#endif
}

/// 重写绘制事件，绘制自定义图表背景/网格。
void ProfileDashboardPanel::paintEvent(QPaintEvent* event) {
#ifdef MINILANG_HAVE_QTCHARTS
    // C2: QtCharts 模式下柱状图由 QChartView 渲染，paintEvent 仅转发基类
    QWidget::paintEvent(event);
    return;
#else
    if (lastResults_.empty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 柱状图绘制区域（resultTable_ 下方）
    // AUDIT-P2 fix: 原实现硬编码 QRect(220, 200, width()-240, 200)，x=220 假设
    // scenarioList_ 宽度固定，y=200 假设布局高度固定，height=200 不随窗口缩放。
    // splitter 拖动或窗口缩放时柱状图位置错位。改为基于 resultTable_ 实际几何
    // + widget 边界动态计算，确保柱状图始终在 resultTable_ 下方且不超出 widget。
    const int margin = 20;
    const int tableBottom = resultTable_ ? resultTable_->geometry().bottom() : 0;
    const int chartTop = tableBottom + margin;
    const int chartHeight = height() - chartTop - margin;
    QRect chartRect(margin, chartTop, width() - 2 * margin, chartHeight);
    if (chartRect.height() < 50) {
        // 空间不足时不绘制柱状图（避免负高度/重叠）
        return;
    }
    p.setPen(QColor(200, 200, 200));
    p.drawRect(chartRect);

    // R111: 维度标题
    p.setPen(QColor(80, 80, 80));
    p.drawText(chartRect.left(), chartRect.top() - 5, metricAxisTitle(currentMetric_));

    // R111: 找最大值用于归一化（根据 currentMetric_ 选择字段）
    double maxVal = 0;
    for (const auto& r : lastResults_) {
        if (r.success) {
            double v = getMetricValue(r, currentMetric_);
            if (v > maxVal)
                maxVal = v;
        }
    }
    if (maxVal <= 0)
        return;

    // 三柱
    const QColor colors[3] = {
        QColor(102, 153, 204), // Interpreter 蓝
        QColor(204, 153, 102), // StackVM 橙
        QColor(153, 204, 102), // RegisterVM 绿
    };
    int barWidth = chartRect.width() / 4;
    for (int i = 0; i < (int)lastResults_.size(); ++i) {
        const auto& r = lastResults_[i];
        double v = r.success ? getMetricValue(r, currentMetric_) : 0.0;
        int barHeight = (int)(v / maxVal * (chartRect.height() - 30));
        QRect bar(chartRect.left() + barWidth / 2 + i * barWidth, chartRect.bottom() - barHeight - 20, barWidth,
                  barHeight);
        p.setBrush(QBrush(colors[i % 3]));
        p.drawRect(bar);
        p.setPen(QColor(0, 0, 0));
        p.drawText(bar.left(), chartRect.bottom() - 5, QString::fromUtf8(r.name.c_str()));
        if (r.success) {
            p.drawText(bar.left(), bar.top() - 5, metricLabel(r, currentMetric_));
        }
    }
#endif
}

#ifdef MINILANG_HAVE_QTCHARTS
// C2: QtCharts 模式柱状图渲染 — 用 QBarSeries + QBarSet 替代 QPainter 自绘
// 视觉优势：自带 hover tooltip / legend / 入场动画 / 抗锯齿
/// 在面板内绘制后端计时对比柱状图。
void ProfileDashboardPanel::renderChart(const std::vector<BackendTiming>& results) {
    if (!chart_)
        return;
    // 清空旧数据
    chart_->removeAllSeries();
    for (const auto& axis : chart_->axes()) {
        chart_->removeAxis(axis);
    }

    auto* series = new QBarSeries(this);
    QStringList categories;
    // R111: 根据 currentMetric_ 选择维度
    double maxVal = 0;
    for (const auto& r : results) {
        if (r.success) {
            double v = getMetricValue(r, currentMetric_);
            if (v > maxVal)
                maxVal = v;
        }
    }

    // 三色柱（与 QPainter 模式保持一致：蓝/橙/绿）
    static const QColor kColors[3] = {
        QColor(102, 153, 204), // Interpreter
        QColor(204, 153, 102), // StackVM
        QColor(153, 204, 102), // RegisterVM
    };

    for (int i = 0; i < (int)results.size(); ++i) {
        const auto& r = results[i];
        auto* barSet = new QBarSet(QString::fromUtf8(r.name.c_str()), this);
        barSet->setColor(kColors[i % 3]);
        if (r.success) {
            *barSet << getMetricValue(r, currentMetric_);
        } else {
            *barSet << 0; // 失败柱以 0 高度显示
        }
        series->append(barSet);
        categories << QString::fromUtf8(r.name.c_str());
    }
    chart_->addSeries(series);

    // X 轴（后端分类）
    auto* axisX = new QBarCategoryAxis(this);
    axisX->append(categories);
    chart_->addAxis(axisX, Qt::AlignBottom);
    series->attachAxis(axisX);

    // R111: Y 轴标题随维度切换
    auto* axisY = new QValueAxis(this);
    axisY->setRange(0, maxVal * 1.1);
    axisY->setTitleText(metricAxisTitle(currentMetric_));
    axisY->setLabelFormat("%.0f");
    chart_->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisY);
}
#endif

// ---- 统计工具 ----

/// 计算多次测速样本的算术平均值。
double ProfileDashboardPanel::mean(const std::vector<double>& xs) {
    if (xs.empty())
        return 0.0;
    double s = 0;
    for (double x : xs)
        s += x;
    return s / xs.size();
}

/// 计算样本标准差，衡量后端耗时稳定性。
double ProfileDashboardPanel::stddev(const std::vector<double>& xs) {
    if (xs.size() < 2)
        return 0.0;
    double m = mean(xs);
    double sq = 0;
    for (double x : xs)
        sq += (x - m) * (x - m);
    return std::sqrt(sq / (xs.size() - 1));
}
