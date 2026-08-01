// ============================================================
// JitVisualizerPanel.cpp — JIT 编译可视化教学面板实现
// ------------------------------------------------------------
// 实现 4 子页：编译原理概览 / 类型反馈与特化 / 热点检测 / OpCode 覆盖。
// 子页 2/3 面板内部独立创建 JITBackend 运行内置场景（参考 TestJIT.cpp
// 的 runJIT 模式），不修改 IdeController / 引擎层。
// ============================================================

#include "gui/JitVisualizerPanel.h"

#include "gui/GuiTextUtils.h" // 拓展二期：monospaceFont（字节码/汇编等宽字体）
#include "gui/I18n.h"         // mlTr
#include "gui/JitRunner.h"    // Qt↔asmjit 隔离层（不直接包含 JIT.h 以避免宏冲突）

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPlainTextEdit> // 拓展二期：子页 6 源码编辑框
#include <QSplitter>
#include <QVBoxLayout>

// ============================================================
// JitVisualizerLibrary — 静态教学数据
// ============================================================

const std::vector<JitRegisterInfo>& JitVisualizerLibrary::registerInfos() {
    static const std::vector<JitRegisterInfo> kRegs = {
        {"r12", "JitContext* ctx", "callee-saved", "跨调用保留，JIT 代码通过 r12+offset 访问上下文"},
        {"r13", "frame basePointer", "callee-saved", "指向当前函数帧第一个参数，局部变量寻址 [r13-slot*8]"},
        {"r15", "operand stack top", "callee-saved", "操作数栈顶，向低地址增长；调用辅助函数前需同步到 ctx"},
        {"r14", "临时 / 调用者帧", "callee-saved", "方法调用时保存调用者 bp"},
        {"rax", "临时寄存器", "caller-saved", "算术运算、调用参数、比较结果"},
        {"rcx", "临时寄存器", "caller-saved", "算术运算、调用参数、计数"},
        {"rdx", "临时寄存器", "caller-saved", "除法（idiv 余数在 rdx）、调用参数"},
        {"rsp", "原生栈", "—", "仅用于函数调用 shadow space（Windows x64 ABI）"},
    };
    return kRegs;
}

const std::vector<JitContextField>& JitVisualizerLibrary::contextFields() {
    static const std::vector<JitContextField> kFields = {
        {0, "outputCallback", "std::function*", "输出回调"},
        {8, "hasError", "bool*", "错误标志"},
        {16, "errorBuffer", "std::string*", "错误信息缓冲"},
        {24, "globalSlots", "int64_t*", "全局变量 slot 数组首地址（R139）"},
        {32, "frames", "JitFrame*", "帧栈数组（R141）"},
        {40, "frameCount", "size_t*", "当前帧深度指针（R141）"},
        {48, "stackTop", "int64_t*", "操作数栈顶指针（R146 邮箱模式）"},
        {56, "classInfoPtr", "unordered_map*", "类信息表（R148）"},
        {64, "pendingFieldOrderPtr", "vector<string>*", "待定字段顺序（R148）"},
        {72, "methodEntryPtr", "void*", "方法入口地址邮箱（R149）"},
        {80, "methodLocalCount", "int64_t", "方法 localCount 邮箱（R149）"},
        {88, "methodEntriesPtr", "unordered_map*", "方法入口表（R149）"},
        {96, "callerBp", "int64_t*", "调用者 r13 邮箱（R149）"},
        {104, "chunkCallCounts", "uint64_t*", "per-chunk 调用计数（R150 热点检测）"},
        {112, "hotThresholds", "uint64_t*", "per-chunk 热点阈值（R151）"},
        {120, "recompiledFlags", "uint64_t*", "per-chunk 重编译标志（R151）"},
        {128, "typeFeedback", "TypeFeedback*", "per-chunk 类型反馈（R152）"},
        {136, "backendPtr", "JITBackend*", "this 指针（R152）"},
        {144, "lastMutatedReceiverPtr", "int64_t*", "嵌套左值赋值中转邮箱（R154）"},
        {152, "funcEntriesPtr", "unordered_map*", "普通函数入口表（R155）"},
    };
    return kFields;
}

const std::vector<JitOpCodeCategory>& JitVisualizerLibrary::opCodeCategories() {
    static const std::vector<JitOpCodeCategory> kCats = {
        {"字面量/栈操作（9）",
         {"OP_INT", "OP_FLOAT", "OP_STRING", "OP_NULL", "OP_TRUE", "OP_FALSE", "OP_POP", "OP_DUP", "OP_DUP_N"}},
        {"算术（6）", {"OP_ADD", "OP_SUBTRACT", "OP_MULTIPLY", "OP_DIVIDE", "OP_MODULO", "OP_NEGATE"}},
        {"比较（7）",
         {"OP_EQUAL", "OP_NOT_EQUAL", "OP_LESS", "OP_GREATER", "OP_LESS_EQUAL", "OP_GREATER_EQUAL", "OP_NOT"}},
        {"控制流（3）", {"OP_JUMP", "OP_JUMP_IF_FALSE", "OP_LOOP"}},
        {"变量（5）", {"OP_GET_LOCAL", "OP_SET_LOCAL", "OP_GET_GLOBAL", "OP_SET_GLOBAL", "OP_DEFINE_GLOBAL"}},
        {"容器（8）",
         {"OP_BUILD_ARRAY", "OP_BUILD_DICT", "OP_BUILD_TUPLE", "OP_INDEX_GET", "OP_INDEX_SET", "OP_INDEX_SET_LOCAL",
          "OP_INDEX_SET_VAR", "OP_LEN"}},
        {"类（7）",
         {"OP_CLASS_NEW", "OP_INIT_FIELD", "OP_DEFINE_CLASS", "OP_MEMBER_GET", "OP_MEMBER_SET_VAR",
          "OP_MEMBER_SET_LOCAL", "OP_TYPE_CHECK"}},
        {"函数/方法（6）",
         {"OP_CALL", "OP_METHOD_CALL", "OP_SUPER_CALL", "OP_CLOSURE", "OP_CLOSE_UPVALUE", "OP_CALL_EXPR"}},
        {"返回/输出（2）", {"OP_RETURN", "OP_PRINT"}},
        {"嵌套赋值链（5）",
         {"OP_LOAD_MUTATED", "OP_WRITEBACK_MEMBER_VAR", "OP_WRITEBACK_INDEX_VAR", "OP_WRITEBACK_MEMBER_LOCAL",
          "OP_WRITEBACK_INDEX_LOCAL"}},
    };
    return kCats;
}

const std::vector<TypeFeedbackScenario>& JitVisualizerLibrary::typeFeedbackScenarios() {
    static const std::vector<TypeFeedbackScenario> kScenarios = {
        {"tf-int", "INT 特化（纯整数运算）",
         "方法 inc(x) 内部 x+1 全为 int，emitCheckInt 通过走原生路径，\n"
         "floatCount=0、otherCount=0 → 触发 INT 特化重编译。",
         "class Adder {\n"
         "    fun inc(x) { return x + 1; }\n"
         "}\n"
         "var a = Adder();\n"
         "var i = 0;\n"
         "var s = 0;\n"
         "while (i < 10) { s = a.inc(s); i = i + 1; }\n"
         "print(s);\n",
         "Adder.inc", "INT",
         "inc 方法调用达热点阈值后重编译。类型反馈 otherCount==0 且 floatCount==0，\n"
         "决策为 INT 特化：移除 emitCheckInt 类型守卫，直接走原生 add 指令。"},
        {"tf-float", "FLOAT 特化（含浮点运算）",
         "方法 half(x) 内部 x/2.0 含 float 操作数，emitCheckInt 失败走 genericDivide\n"
         "慢速路径并递增 floatCount → floatCount>0、otherCount=0 → FLOAT 特化。",
         "class Calc {\n"
         "    fun half(x) { return x / 2.0; }\n"
         "}\n"
         "var c = Calc();\n"
         "var i = 0;\n"
         "var r = 0;\n"
         "while (i < 10) { r = c.half(i); i = i + 1; }\n"
         "print(r);\n",
         "Calc.half", "FLOAT",
         "half 方法类型反馈 floatCount>0 且 otherCount==0，决策为 FLOAT 特化：\n"
         "emitCheckInt 改为无条件跳转 failLabel，跳过类型反馈记录，\n"
         "直接走浮点除法路径。"},
        {"tf-none", "不特化（含字符串拼接）",
         "方法 hi(n) 内部 \"hi \"+n 为 string 拼接，emitCheckInt 失败走 genericAdd\n"
         "慢速路径并递增 otherCount → otherCount>0 → 不特化，保持类型分派。",
         "class Greeter {\n"
         "    fun hi(n) { return \"hi \" + n; }\n"
         "}\n"
         "var g = Greeter();\n"
         "var i = 0;\n"
         "while (i < 10) { g.hi(\"x\"); i = i + 1; }\n"
         "print(\"done\");\n",
         "Greeter.hi", "不特化",
         "hi 方法类型反馈 otherCount>0，决策为不特化：保留 emitCheckInt 类型守卫\n"
         "与 genericXXX 慢速路径分派，无法移除运行时类型检查开销。"},
    };
    return kScenarios;
}

const std::vector<HotspotScenario>& JitVisualizerLibrary::hotspotScenarios() {
    static const std::vector<HotspotScenario> kScenarios = {
        {"hot-method", "方法热点触发重编译",
         "类方法被循环调用，调用计数达到热点阈值后触发重编译标志，\n"
         "execute() 返回后由特化重编译循环处理。",
         "class Counter {\n"
         "    fun step(x) { return x + 1; }\n"
         "}\n"
         "var c = Counter();\n"
         "var i = 0;\n"
         "while (i < 10) { i = c.step(i); }\n"
         "print(i);\n",
         "Counter.step",
         "仅 chunk 名含 '.' 的方法 chunk 默认设热点阈值（kDefaultHotThreshold=1000）。\n"
         "本场景调小阈值至 5 以加速演示。方法入口处生成的本地代码递增 chunkCallCounts，\n"
         "达阈值后置 recompiledFlags=1 并调用 jitTriggerRecompile（仅设标志，不立即重编译）。\n"
         "真正的特化重编译在 execute() 返回后的 C++ 循环中完成。"},
    };
    return kScenarios;
}

const std::vector<TierScenario>& JitVisualizerLibrary::tierScenarios() {
    static const std::vector<TierScenario> kScenarios = {
        {"tier-osr", "OSR 栈帧迁移（热循环触发）",
         "方法 chunk 内含 while 循环，循环回边计数达 OSR 阈值后，JIT 代码将当前"
         "栈帧状态（r13/r15）保存到 OsrFrameSnapshot 邮箱，调用特化重编译生成含"
         "OSR 入口点的特化版本，从 OSR 入口点恢复栈帧并继续执行（真正 OSR，非"
         "R157 的'下次 execute 生效'）。",
         "class Acc {\n"
         "    fun sum(n) {\n"
         "        var s = 0;\n"
         "        var i = 0;\n"
         "        while (i < n) { s = s + i; i = i + 1; }\n"
         "        return s;\n"
         "    }\n"
         "}\n"
         "var a = Acc();\n"
         "print(a.sum(20));\n",
         "Acc.sum", "OSR 迁移",
         "OP_LOOP 回边时 JIT 代码递增 osrLoopCountsPtr，达 osrLoopThresholdsPtr 阈值后\n"
         "置 osrRecompiledFlagsPtr=1 并调用 jitTriggerOsrRecompile。该辅助函数执行\n"
         "特化重编译（基于类型反馈生成 INT 特化版本），并在特化代码中插入 OSR 入口点\n"
         "（读取 osrSavedBp/osrSavedSp 恢复栈帧），返回 osrEntryPoint 地址供 JIT 代码\n"
         "jmp 过去继续执行。本场景调小 OSR 阈值至 5 加速演示。"},
        {"tier-lazy", "Lazy Compilation（按需编译）",
         "分层编译模式下，functionChunks 不在 execute() 入口一次性全部编译，而是"
         "标记为 lazy，首次被 OP_CALL/OP_CALL_EXPR 调用时才触发编译。本场景定义"
         "3 个函数但只调用 2 个，第 3 个函数 chunk 不会被编译（不出现在 tier 表中）。",
         "fun add(a, b) { return a + b; }\n"
         "fun sub(a, b) { return a - b; }\n"
         "fun unused(a) { return a * 2; }\n"
         "print(add(3, 4));\n"
         "print(sub(10, 7));\n",
         "", "lazy 编译",
         "setTieredCompilation(true) 启用后，execute() 仅编译 mainChunk，functionChunks\n"
         "标记 lazyCompiledChunks_。OP_CALL 调度时若目标 chunk 未编译，触发即时编译\n"
         "（jitLazyCompile 辅助函数）并填充 funcEntriesPtr。getLazyCompiledChunks()\n"
         "返回按需编译过的 chunk 名列表。本场景预期 add/sub 出现在 lazy 列表，"
         "unused 不出现。"},
        {"tier-deopt", "Deoptimization（特化失效回退）",
         "方法 chunk 先被 INT 特化（Tier 2），随后传入非 int 操作数触发类型守卫"
         "失败。JIT 代码调用 jitDeoptimize 辅助函数：生成 Tier 1 入口点（deoptEntryPoint），\n"
         "设置 deoptChunkIdx，JIT 代码 jmp 到 Tier 1 继续执行（丢失特化性能但语义正确）。\n"
         "deoptCount_ 递增，chunk 名加入 deoptimizedChunks_。",
         "class Mix {\n"
         "    fun op(x) { return x + 1; }\n"
         "}\n"
         "var m = Mix();\n"
         "var i = 0;\n"
         "while (i < 8) { m.op(i); i = i + 1; }\n"
         "m.op(1.5);\n"
         "print(\"done\");\n",
         "Mix.op", "deopt 回退",
         "INT 特化版本移除 emitCheckInt 类型守卫，直接走原生 add。若传入 float 操作数，\n"
         "原生 add 在 int64 寄存器上执行会产生错误结果（不崩溃但语义错误）。JIT 通过\n"
         "类型反馈监控：当 otherCount/floatCount 在特化后仍递增，触发 jitDeoptimize\n"
         "回退到 Tier 1（保留类型守卫的非特化版本）。本场景调小 OSR 阈值至 5 加速\n"
         "特化触发，第 9 次调用传入 1.5 触发 deopt。"},
    };
    return kScenarios;
}

// ============================================================
// JitVisualizerPanel 构造与子页构建
// ============================================================
// 注：教学面板显示时由 Ide::wrapTeachingPanel 统一包裹 TeachingPanelHeader
// （标题 + 帮助 + 学习路径 + 返回编辑器），面板自身不再创建标题栏，
// 避免双重包裹。面板内仅放置 JIT 可用性提示与子页内容。

JitVisualizerPanel::JitVisualizerPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // JIT 可用性提示
    jitAvailLabel_ = new QLabel(this);
#ifdef MINILANG_USE_JIT
    jitAvailLabel_->setText(QStringLiteral("✅ JIT 已启用（asmjit，支持 58 种 OpCode）"));
#else
    jitAvailLabel_->setText(QStringLiteral("⚠️ JIT 未启用（需 CMake -DMINILANG_USE_JIT=ON）。子页 1/4 静态内容可查看，"
                                           "子页 2/3 运行功能已禁用。"));
#endif
    jitAvailLabel_->setWordWrap(true);
    root->addWidget(jitAvailLabel_);

    // 子页切换（UX-R fix: 统一 TeachingSubPageBar 组件，替代手写 page*Btn_ + stack_；
    // 互斥选中态 / 主题色高亮 / 滑入动画由组件内置，addPage 同时创建按钮并入栈）
    subPageBar_ = new TeachingSubPageBar(this);
    root->addLayout(subPageBar_->buttonBar());
    root->addWidget(subPageBar_->stack(), 1);

    // 构建六个子页
    {
        auto* host = new QWidget();
        buildOverviewPage(host);
        subPageBar_->addPage(mlTr("① 编译原理概览"), host);
    }
    {
        auto* host = new QWidget();
        buildTypeFeedbackPage(host);
        subPageBar_->addPage(mlTr("② 类型反馈与特化"), host);
    }
    {
        auto* host = new QWidget();
        buildHotspotPage(host);
        subPageBar_->addPage(mlTr("③ 热点检测"), host);
    }
    {
        auto* host = new QWidget();
        buildOpCodePage(host);
        subPageBar_->addPage(mlTr("④ OpCode 覆盖"), host);
    }
    {
        auto* host = new QWidget();
        buildTierMetricsPage(host);
        subPageBar_->addPage(mlTr("⑤ 分层编译与 OSR"), host);
    }
    // 拓展二期：子页 6（字节码↔汇编对照）
    {
        auto* host = new QWidget();
        buildAsmComparePage(host);
        subPageBar_->addPage(mlTr("⑥ 字节码↔汇编对照"), host);
    }

    // 初始数据
    populateOverview();
    populateOpCode();
    populateTfScenarios();
    populateHotScenarios();
    populateTierScenarios();
}

void JitVisualizerPanel::buildOverviewPage(QWidget* host) {
    auto* lay = new QVBoxLayout(host);
    overviewBrowser_ = new QTextBrowser(host);
    overviewBrowser_->setOpenExternalLinks(false);
    lay->addWidget(overviewBrowser_, 1);
}

void JitVisualizerPanel::buildTypeFeedbackPage(QWidget* host) {
    auto* lay = new QVBoxLayout(host);
    auto* split = new QSplitter(Qt::Horizontal, host);
    lay->addWidget(split, 1);

    // 左：场景列表
    auto* left = new QWidget();
    auto* leftLay = new QVBoxLayout(left);
    leftLay->addWidget(new QLabel(mlTr("场景列表"), left));
    tfScenarioList_ = new QListWidget(left);
    leftLay->addWidget(tfScenarioList_, 1);
    split->addWidget(left);

    // 右：详情 + 运行 + 结果
    auto* right = new QWidget();
    auto* rightLay = new QVBoxLayout(right);

    rightLay->addWidget(new QLabel(mlTr("源码"), right));
    tfSourceView_ = new QTextBrowser(right);
    rightLay->addWidget(tfSourceView_);

    tfRunBtn_ = new QPushButton(mlTr("▶ 运行 JIT 并采集类型反馈"), right);
    rightLay->addWidget(tfRunBtn_);

    rightLay->addWidget(new QLabel(mlTr("JIT 输出"), right));
    tfOutputView_ = new QTextBrowser(right);
    tfOutputView_->setMaximumHeight(80);
    rightLay->addWidget(tfOutputView_);

    rightLay->addWidget(new QLabel(mlTr("per-chunk 类型反馈"), right));
    tfTable_ = new QTableWidget(right);
    tfTable_->setColumnCount(5);
    tfTable_->setHorizontalHeaderLabels(
        {mlTr("chunk 名"), mlTr("intCount"), mlTr("floatCount"), mlTr("otherCount"), mlTr("特化决策")});
    tfTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rightLay->addWidget(tfTable_);

    tfDecisionLabel_ = new QLabel(right);
    tfDecisionLabel_->setWordWrap(true);
    rightLay->addWidget(tfDecisionLabel_);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

#ifndef MINILANG_USE_JIT
    tfRunBtn_->setEnabled(false);
    tfRunBtn_->setText(mlTr("▶ JIT 未启用"));
#endif

    connect(tfScenarioList_, &QListWidget::currentRowChanged, this, &JitVisualizerPanel::showTfScenario);
#ifdef MINILANG_USE_JIT
    connect(tfRunBtn_, &QPushButton::clicked, this, [this]() {
        int idx = tfScenarioList_->currentRow();
        if (idx >= 0)
            runTypeFeedbackScenario(idx);
    });
#endif
}

void JitVisualizerPanel::buildHotspotPage(QWidget* host) {
    auto* lay = new QVBoxLayout(host);
    auto* split = new QSplitter(Qt::Horizontal, host);
    lay->addWidget(split, 1);

    auto* left = new QWidget();
    auto* leftLay = new QVBoxLayout(left);
    leftLay->addWidget(new QLabel(mlTr("场景列表"), left));
    hotScenarioList_ = new QListWidget(left);
    leftLay->addWidget(hotScenarioList_, 1);
    split->addWidget(left);

    auto* right = new QWidget();
    auto* rightLay = new QVBoxLayout(right);

    rightLay->addWidget(new QLabel(mlTr("源码"), right));
    hotSourceView_ = new QTextBrowser(right);
    rightLay->addWidget(hotSourceView_);

    hotRunBtn_ = new QPushButton(mlTr("▶ 运行 JIT 并采集热点统计"), right);
    rightLay->addWidget(hotRunBtn_);

    rightLay->addWidget(new QLabel(mlTr("per-chunk 调用计数"), right));
    hotCallCountTable_ = new QTableWidget(right);
    hotCallCountTable_->setColumnCount(2);
    hotCallCountTable_->setHorizontalHeaderLabels({mlTr("chunk 名"), mlTr("调用计数")});
    hotCallCountTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rightLay->addWidget(hotCallCountTable_);

    rightLay->addWidget(new QLabel(mlTr("per-chunk 重编译标志"), right));
    hotRecompileTable_ = new QTableWidget(right);
    hotRecompileTable_->setColumnCount(3);
    hotRecompileTable_->setHorizontalHeaderLabels({mlTr("chunk 名"), mlTr("recompiledFlag"), mlTr("状态")});
    hotRecompileTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rightLay->addWidget(hotRecompileTable_);

    hotNoteView_ = new QTextBrowser(right);
    hotNoteView_->setMaximumHeight(100);
    rightLay->addWidget(hotNoteView_);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

#ifndef MINILANG_USE_JIT
    hotRunBtn_->setEnabled(false);
    hotRunBtn_->setText(mlTr("▶ JIT 未启用"));
#endif

    connect(hotScenarioList_, &QListWidget::currentRowChanged, this, &JitVisualizerPanel::showHotScenario);
#ifdef MINILANG_USE_JIT
    connect(hotRunBtn_, &QPushButton::clicked, this, [this]() {
        int idx = hotScenarioList_->currentRow();
        if (idx >= 0)
            runHotspotScenario(idx);
    });
#endif
}

void JitVisualizerPanel::buildOpCodePage(QWidget* host) {
    auto* lay = new QVBoxLayout(host);
    opCodeBrowser_ = new QTextBrowser(host);
    lay->addWidget(opCodeBrowser_, 1);
}

void JitVisualizerPanel::buildTierMetricsPage(QWidget* host) {
    auto* lay = new QVBoxLayout(host);
    auto* split = new QSplitter(Qt::Horizontal, host);
    lay->addWidget(split, 1);

    // 左：场景列表
    auto* left = new QWidget();
    auto* leftLay = new QVBoxLayout(left);
    leftLay->addWidget(new QLabel(mlTr("场景列表"), left));
    tierScenarioList_ = new QListWidget(left);
    leftLay->addWidget(tierScenarioList_, 1);
    split->addWidget(left);

    // 右：详情 + 运行 + 结果
    auto* right = new QWidget();
    auto* rightLay = new QVBoxLayout(right);

    rightLay->addWidget(new QLabel(mlTr("源码"), right));
    tierSourceView_ = new QTextBrowser(right);
    rightLay->addWidget(tierSourceView_);

    tierRunBtn_ = new QPushButton(mlTr("▶ 运行分层编译并采集 tier/OSR/deopt 指标"), right);
    rightLay->addWidget(tierRunBtn_);

    rightLay->addWidget(new QLabel(mlTr("JIT 输出"), right));
    tierOutputView_ = new QTextBrowser(right);
    tierOutputView_->setMaximumHeight(80);
    rightLay->addWidget(tierOutputView_);

    rightLay->addWidget(new QLabel(mlTr("per-chunk tier 状态与 OSR/deopt 指标"), right));
    tierTable_ = new QTableWidget(right);
    tierTable_->setColumnCount(7);
    tierTable_->setHorizontalHeaderLabels({mlTr("chunk 名"), mlTr("tier"), mlTr("OSR 回边计数"), mlTr("OSR 重编译"),
                                           mlTr("OSR 迁移"), mlTr("lazy 编译"), mlTr("已 deopt")});
    tierTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rightLay->addWidget(tierTable_);

    rightLay->addWidget(new QLabel(mlTr("全局汇总"), right));
    tierSummaryView_ = new QTextBrowser(right);
    tierSummaryView_->setMaximumHeight(140);
    rightLay->addWidget(tierSummaryView_);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

#ifndef MINILANG_USE_JIT
    tierRunBtn_->setEnabled(false);
    tierRunBtn_->setText(mlTr("▶ JIT 未启用"));
#endif

    connect(tierScenarioList_, &QListWidget::currentRowChanged, this, &JitVisualizerPanel::showTierScenario);
#ifdef MINILANG_USE_JIT
    connect(tierRunBtn_, &QPushButton::clicked, this, [this]() {
        int idx = tierScenarioList_->currentRow();
        if (idx >= 0)
            runTierScenario(idx);
    });
#endif
}

// ============================================================
// 子页 1：编译原理概览
// ============================================================
void JitVisualizerPanel::populateOverview() {
    QString html;
    html += QStringLiteral("<h2>JIT 编译原理概览</h2>");
    html += QStringLiteral("<p>JIT 后端（R138-R155）基于 asmjit 将 <b>BytecodeChunk</b> 编译为 x86-64 "
                           "本地机器码，直接在 CPU 上执行。与 StackVM 的 switch dispatch loop 相比，"
                           "省去了取指-解码开销，典型加速比 <b>39-100x</b>。</p>");

    html += QStringLiteral("<h3>🔗 与 StackVM 的关系</h3>");
    html +=
        QStringLiteral("<p>JIT 是 StackVM 的<b>加速器</b>：二者共享编译管线（Lexer→Parser→Compiler→"
                       "BytecodeChunk），输入相同。JIT 不做 NaN-boxing（内部以 raw int64_t 表示 Value），"
                       "遇不支持的 OpCode 直接 <code>compileError</code> 硬失败，<b>无自动回退到 StackVM</b>。</p>");

    html += QStringLiteral("<h3>📋 寄存器分配</h3>");
    html += QStringLiteral("<table border='1' cellspacing='0' cellpadding='4'>");
    html += QStringLiteral("<tr><th>寄存器</th><th>用途</th><th>性质</th><th>备注</th></tr>");
    for (const auto& r : JitVisualizerLibrary::registerInfos()) {
        html += QStringLiteral("<tr><td><code>%1</code></td><td>%2</td><td>%3</td><td>%4</td></tr>")
                    .arg(QString::fromStdString(r.reg), QString::fromStdString(r.role), QString::fromStdString(r.kind),
                         QString::fromStdString(r.note));
    }
    html += QStringLiteral("</table>");

    html += QStringLiteral("<h3>🧱 栈帧布局（rbp 相对寻址）</h3>");
    html += QStringLiteral("<pre>[rbp]      = saved rbp\n"
                           "[rbp-8]    = saved r12\n"
                           "[rbp-16]   = saved r13\n"
                           "[rbp-24]   = saved r14\n"
                           "[rbp-32]   = saved r15\n"
                           "[rbp-32]   = operand stack base（r15 初始值，空栈）\n"
                           "...        = operand stack（8KB = 1024 值，向下增长）\n"
                           "prologue:  sub rsp, 32 + 8192</pre>");
    html += QStringLiteral("<p>方法调用时 <b>Windows x64 ABI</b> 栈对齐用 <code>sub rsp,40</code>"
                           "（R151 教训：shadow space 32B + 对齐 8B）。</p>");

    html += QStringLiteral("<h3>📬 JitContext 内存布局（20 字段，r12+offset 访问）</h3>");
    html += QStringLiteral("<table border='1' cellspacing='0' cellpadding='4'>");
    html += QStringLiteral("<tr><th>offset</th><th>字段</th><th>类型</th><th>说明</th></tr>");
    for (const auto& f : JitVisualizerLibrary::contextFields()) {
        html +=
            QStringLiteral("<tr><td>%1</td><td><code>%2</code></td><td>%3</td><td>%4</td></tr>")
                .arg(f.offset)
                .arg(QString::fromStdString(f.name), QString::fromStdString(f.type), QString::fromStdString(f.note));
    }
    html += QStringLiteral("</table>");
    html +=
        QStringLiteral("<p><b>邮箱模式</b>（R146-R155）：JIT 代码调用 C++ 辅助函数前 <code>mov [r12+48], r15</code> "
                       "同步栈顶，调用后 <code>mov r15, [r12+48]</code> 恢复。方法入口地址通过 "
                       "<code>ctx->methodEntryPtr</code>(offset 72) 邮箱返回。</p>");

    overviewBrowser_->setHtml(html);
}

// ============================================================
// 子页 4：OpCode 覆盖与回退
// ============================================================
void JitVisualizerPanel::populateOpCode() {
    QString html;
    html += QStringLiteral("<h2>OpCode 覆盖与回退</h2>");
    html += QStringLiteral("<p>JIT <code>compileChunk</code> 的 switch 共 <b>58 个 case 分支</b>，"
                           "覆盖 MiniLang 字节码全集（含 R154 嵌套左值赋值链、R155 闭包值调用）。</p>");

    html += QStringLiteral("<h3>📂 支持的 OpCode 分类</h3>");
    for (const auto& cat : JitVisualizerLibrary::opCodeCategories()) {
        html += QStringLiteral("<h4>%1</h4><p>").arg(QString::fromStdString(cat.category));
        for (size_t i = 0; i < cat.opcodes.size(); ++i) {
            if (i > 0)
                html += QStringLiteral(", ");
            html += QStringLiteral("<code>%1</code>").arg(QString::fromStdString(cat.opcodes[i]));
        }
        html += QStringLiteral("</p>");
    }

    html += QStringLiteral("<h3>⚠️ 不支持 OpCode 的回退行为</h3>");
    html += QStringLiteral("<p><code>default</code> 分支直接 <code>compileError(\"JIT 阶段 2b 不支持的 OpCode: \" "
                           "+ opCodeName(op))</code> 并返回 <code>nullptr</code>，<code>execute()</code> 据此返回 "
                           "<code>JitResult::CompileError</code>。<b>没有自动回退到 StackVM 的机制</b>——"
                           "调用方必须自行捕获 CompileError 并切换后端。</p>");

    html += QStringLiteral("<h3>🔧 类型守卫与慢速路径</h3>");
    html += QStringLiteral("<p>算术/比较指令统一通过 <code>emitCheckInt</code> lambda 做 INT 快速路径分派："
                           "操作数均为 int 时走原生指令（零开销）；失败则调用 C++ 辅助函数"
                           "（<code>jitAdd</code>/<code>jitDivide</code> 等）处理任意类型，"
                           "并在慢速路径入口调用 <code>emitRecordTypeFeedback</code> 递增类型反馈计数。</p>");

    opCodeBrowser_->setHtml(html);
}

// ============================================================
// 子页 2：类型反馈与特化
// ============================================================
void JitVisualizerPanel::populateTfScenarios() {
    tfScenarioList_->clear();
    for (const auto& s : JitVisualizerLibrary::typeFeedbackScenarios()) {
        tfScenarioList_->addItem(QString::fromStdString(s.title));
    }
    if (tfScenarioList_->count() > 0)
        tfScenarioList_->setCurrentRow(0);
}

void JitVisualizerPanel::showTfScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::typeFeedbackScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];
    tfSourceView_->setPlainText(QString::fromStdString(s.sourceCode));
    tfOutputView_->clear();
    tfTable_->setRowCount(0);
    tfDecisionLabel_->setText(
        QStringLiteral("<b>预期特化：%1</b><br>%2")
            .arg(QString::fromStdString(s.expectedSpecialization), QString::fromStdString(s.explanation)));
}

void JitVisualizerPanel::runTypeFeedbackScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::typeFeedbackScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];

    tfOutputView_->clear();
    tfTable_->setRowCount(0);
    tfDecisionLabel_->setText(QStringLiteral("运行中..."));

    // 通过 JitRunner 隔离层运行（避免 Qt↔asmjit 头文件冲突）
    auto r = JitRunner::runTypeFeedback(s.sourceCode, s.methodChunkName, 5);
    if (!r.ok) {
        tfOutputView_->setPlainText(QString::fromStdString(r.error));
        tfDecisionLabel_->setText(QStringLiteral("❌ JIT 运行失败：%1").arg(QString::fromStdString(r.error)));
        return;
    }

    tfOutputView_->setPlainText(QString::fromStdString(r.output));

    tfTable_->setRowCount((int)r.rows.size());
    for (int i = 0; i < (int)r.rows.size(); ++i) {
        const auto& row = r.rows[i];
        tfTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.chunkName)));
        tfTable_->setItem(i, 1, new QTableWidgetItem(QString::number(row.intCount)));
        tfTable_->setItem(i, 2, new QTableWidgetItem(QString::number(row.floatCount)));
        tfTable_->setItem(i, 3, new QTableWidgetItem(QString::number(row.otherCount)));
        tfTable_->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(row.decision)));
    }

    QString summary = QStringLiteral("<b>实际特化结果：</b>");
    if (r.specializedChunks.empty()) {
        summary += QStringLiteral("无 chunk 被特化");
    } else {
        for (const auto& name : r.specializedChunks) {
            summary += QStringLiteral("<code>%1</code> ").arg(QString::fromStdString(name));
        }
    }
    summary += QStringLiteral("<br><b>预期：</b>%1").arg(QString::fromStdString(s.expectedSpecialization));
    tfDecisionLabel_->setText(summary);
}

// ============================================================
// 子页 3：热点检测
// ============================================================
void JitVisualizerPanel::populateHotScenarios() {
    hotScenarioList_->clear();
    for (const auto& s : JitVisualizerLibrary::hotspotScenarios()) {
        hotScenarioList_->addItem(QString::fromStdString(s.title));
    }
    if (hotScenarioList_->count() > 0)
        hotScenarioList_->setCurrentRow(0);
}

void JitVisualizerPanel::showHotScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::hotspotScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];
    hotSourceView_->setPlainText(QString::fromStdString(s.sourceCode));
    hotCallCountTable_->setRowCount(0);
    hotRecompileTable_->setRowCount(0);
    hotNoteView_->setPlainText(QString::fromStdString(s.explanation));
}

void JitVisualizerPanel::runHotspotScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::hotspotScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];

    hotCallCountTable_->setRowCount(0);
    hotRecompileTable_->setRowCount(0);

    // 通过 JitRunner 隔离层运行（避免 Qt↔asmjit 头文件冲突）
    auto r = JitRunner::runHotspot(s.sourceCode, s.methodChunkName, 5);
    if (!r.ok) {
        hotNoteView_->setPlainText(QStringLiteral("❌ JIT 执行失败：%1").arg(QString::fromStdString(r.error)));
        return;
    }

    hotCallCountTable_->setRowCount((int)r.rows.size());
    hotRecompileTable_->setRowCount((int)r.rows.size());
    for (int i = 0; i < (int)r.rows.size(); ++i) {
        const auto& row = r.rows[i];
        hotCallCountTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.chunkName)));
        hotCallCountTable_->setItem(i, 1, new QTableWidgetItem(QString::number(row.callCount)));
        QString status = row.recompiledFlag ? QStringLiteral("🔥 已触发重编译") : QStringLiteral("— 未触发");
        hotRecompileTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.chunkName)));
        hotRecompileTable_->setItem(i, 1, new QTableWidgetItem(QString::number(row.recompiledFlag)));
        hotRecompileTable_->setItem(i, 2, new QTableWidgetItem(status));
    }

    hotNoteView_->setPlainText(QString::fromStdString(s.explanation) + QStringLiteral("\n\nJIT 输出：") +
                               QString::fromStdString(r.output));
}

// ============================================================
// 子页 5：分层编译与 OSR（R160）
// ============================================================
void JitVisualizerPanel::populateTierScenarios() {
    tierScenarioList_->clear();
    for (const auto& s : JitVisualizerLibrary::tierScenarios()) {
        tierScenarioList_->addItem(QString::fromStdString(s.title));
    }
    if (tierScenarioList_->count() > 0)
        tierScenarioList_->setCurrentRow(0);
}

void JitVisualizerPanel::showTierScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::tierScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];
    tierSourceView_->setPlainText(QString::fromStdString(s.sourceCode));
    tierOutputView_->clear();
    tierTable_->setRowCount(0);
    tierSummaryView_->setHtml(
        QStringLiteral("<b>预期亮点：%1</b><br>%2")
            .arg(QString::fromStdString(s.expectedHighlights), QString::fromStdString(s.explanation)));
}

void JitVisualizerPanel::runTierScenario(int index) {
    const auto& scenarios = JitVisualizerLibrary::tierScenarios();
    if (index < 0 || index >= (int)scenarios.size())
        return;
    const auto& s = scenarios[index];

    tierOutputView_->clear();
    tierTable_->setRowCount(0);
    tierSummaryView_->setPlainText(QStringLiteral("运行中..."));

    // 通过 JitRunner 隔离层运行分层编译场景（OSR 阈值调小至 5 加速演示）
    auto r = JitRunner::runTieredCompilation(s.sourceCode, s.osrChunkName, 5);
    if (!r.ok) {
        tierOutputView_->setPlainText(QString::fromStdString(r.error));
        tierSummaryView_->setPlainText(QStringLiteral("❌ JIT 运行失败：%1").arg(QString::fromStdString(r.error)));
        return;
    }

    tierOutputView_->setPlainText(QString::fromStdString(r.output));

    // 填充 per-chunk tier 状态表
    tierTable_->setRowCount((int)r.rows.size());
    for (int i = 0; i < (int)r.rows.size(); ++i) {
        const auto& row = r.rows[i];
        tierTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.chunkName)));
        tierTable_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(row.tier)));
        tierTable_->setItem(i, 2, new QTableWidgetItem(QString::number(row.osrLoopCount)));
        tierTable_->setItem(i, 3, new QTableWidgetItem(QString::number(row.osrRecompiled)));
        tierTable_->setItem(i, 4,
                            new QTableWidgetItem(row.osrMigrated ? QStringLiteral("✅ 是") : QStringLiteral("—")));
        tierTable_->setItem(i, 5,
                            new QTableWidgetItem(row.lazyCompiled ? QStringLiteral("✅ 是") : QStringLiteral("—")));
        tierTable_->setItem(i, 6, new QTableWidgetItem(row.deoptimized ? QStringLiteral("⚠️ 是") : QStringLiteral("—")));
    }

    // 填充全局汇总
    QString summary;
    summary += QStringLiteral("<b>分层编译状态：</b>%1<br>")
                   .arg(r.tieredEnabled ? QStringLiteral("✅ 已启用") : QStringLiteral("⚠️ 未启用"));
    summary += QStringLiteral("<b>总 deopt 次数：</b>%1<br>").arg(r.totalDeoptCount);

    auto formatList = [](const std::vector<std::string>& v) -> QString {
        if (v.empty())
            return QStringLiteral("（无）");
        QString s;
        for (const auto& name : v)
            s += QStringLiteral("<code>%1</code> ").arg(QString::fromStdString(name));
        return s;
    };
    summary += QStringLiteral("<b>lazy 编译 chunk：</b>%1<br>").arg(formatList(r.lazyCompiledChunks));
    summary += QStringLiteral("<b>OSR 迁移 chunk：</b>%1<br>").arg(formatList(r.osrMigratedChunks));
    summary += QStringLiteral("<b>deopt chunk：</b>%1<br>").arg(formatList(r.deoptimizedChunks));
    summary += QStringLiteral("<br><b>预期亮点：</b>%1").arg(QString::fromStdString(s.expectedHighlights));

    tierSummaryView_->setHtml(summary);
}

// ============================================================
// 拓展二期·子页 6：字节码↔汇编对照
// ------------------------------------------------------------
// 左栏字节码反汇编（BytecodeChunk::disassemble），右栏 JIT 发射的
// x86-64 汇编（JITBackend::setAsmCapture 挂 asmjit StringLogger）。
// 源码可编辑，预填整数算术示例（JIT 全覆盖的 OpCode 子集）。
// ============================================================

void JitVisualizerPanel::buildAsmComparePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    layout->addWidget(new QLabel(mlTr("<b>字节码↔汇编对照</b>：编辑源码后点击「编译并对照」，"
                                      "左栏展示栈式字节码反汇编，右栏展示 JIT 真实发射的 x86-64 汇编"
                                      "（含机器码字节，asmjit StringLogger 捕获）。"),
                                 host));

    asmSrcEdit_ = new QPlainTextEdit(host);
    asmSrcEdit_->setFont(GuiTextUtils::monospaceFont());
    asmSrcEdit_->setMaximumHeight(120);
    asmSrcEdit_->setPlainText(QStringLiteral("fun add(a, b) {\n    return a + b;\n}\nprint(add(3, 5));\n"));
    layout->addWidget(asmSrcEdit_);

    auto* btnBar = new QHBoxLayout();
    asmRunBtn_ = new QPushButton(mlTr("编译并对照"), host);
#ifndef MINILANG_USE_JIT
    asmRunBtn_->setEnabled(false);
#endif
    btnBar->addWidget(asmRunBtn_);
    asmStatusLabel_ = new QLabel(host);
    btnBar->addWidget(asmStatusLabel_, 1);
    layout->addLayout(btnBar);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    auto* leftWrap = new QWidget(host);
    auto* leftLayout = new QVBoxLayout(leftWrap);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel(mlTr("字节码反汇编：")));
    asmBytecodeView_ = new QTextBrowser(host);
    asmBytecodeView_->setFont(GuiTextUtils::monospaceFont());
    leftLayout->addWidget(asmBytecodeView_, 1);
    auto* rightWrap = new QWidget(host);
    auto* rightLayout = new QVBoxLayout(rightWrap);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel(mlTr("JIT 发射的 x86-64 汇编：")));
    asmAsmView_ = new QTextBrowser(host);
    asmAsmView_->setFont(GuiTextUtils::monospaceFont());
    rightLayout->addWidget(asmAsmView_, 1);
    splitter->addWidget(leftWrap);
    splitter->addWidget(rightWrap);
    splitter->setSizes({300, 400});
    layout->addWidget(splitter, 1);

    connect(asmRunBtn_, &QPushButton::clicked, this, &JitVisualizerPanel::runAsmCompare);
}

void JitVisualizerPanel::runAsmCompare() {
    asmStatusLabel_->setText(mlTr("编译中..."));
    auto r = JitRunner::runAsmDump(asmSrcEdit_->toPlainText().toStdString());
    if (!r.ok) {
        // 字节码反汇编可能已成功（仅 JIT 阶段失败），仍展示左栏
        asmBytecodeView_->setPlainText(QString::fromStdString(r.bytecodeText));
        asmAsmView_->setPlainText(QString::fromStdString(r.error));
        asmStatusLabel_->setText(mlTr("❌ 失败：%1").arg(QString::fromStdString(r.error)));
        return;
    }
    asmBytecodeView_->setPlainText(QString::fromStdString(r.bytecodeText));
    asmAsmView_->setPlainText(QString::fromStdString(r.asmText));
    asmStatusLabel_->setText(mlTr("✅ 成功（输出：%1）").arg(QString::fromStdString(r.output).trimmed()));
}
