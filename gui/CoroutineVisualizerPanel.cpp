// ============================================================
// CoroutineVisualizerPanel.cpp — 协程/生成器可视化教学面板实现
// ------------------------------------------------------------
// 面板内部独立完成 Lexer → Parser → 三后端执行流程，验证协程/
// 生成器（fun* / yield / .next() / .done()）在 Interpreter /
// StackVM(IR) / RegisterVM(IR) 三条路径上的 yield 重放一致性。
// JIT 后端暂不支持协程，故按钮标注「运行四后端」但实际执行三后端。
//
// 三后端串行执行（主线程），捕获输出/指令数/状态/耗时/协程状态摘要，
// 并对比三后端输出一致性验证 yield 语义等价性。
// ============================================================

#include "gui/CoroutineVisualizerPanel.h"
#include "gui/GuidedTour.h"

#include "common/Diagnostic.h"
#include "compiler/Bytecode.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "gui/GuiTextUtils.h"
#include "gui/I18n.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <sstream>

// ============================================================
// 内置协程示例库（5 个）
// ============================================================

const std::vector<CoroutineSample>& CoroutineVisualizerPanel::samples() {
    static const std::vector<CoroutineSample> kSamples = {
        {"基础三 yield",
         "fun* gen() {\n    yield 1;\n    yield 2;\n    yield 3;\n}\nvar g = "
         "gen();\nprint(g.next());\nprint(g.next());\n"
         "print(g.next());\nprint(g.done());\n",
         "三个 yield 顺序返回 1,2,3，done=true"},
        {"带参数 range",
         "fun* range(start: int, end: int) {\n    var i = start;\n    while (i < end) {\n        yield i;\n        i = "
         "i + 1;\n    "
         "}\n}\nvar g = range(10, "
         "13);\nprint(g.next());\nprint(g.next());\nprint(g.next());\nprint(g.done());\nprint(g.next("
         "));\nprint(g.done());\n",
         "循环内 yield，动态 yieldCount，第4次 next 返回 null 触发 done"},
        {"闭包捕获",
         "var multiplier = 10;\nfun* gen() {\n    yield 1 * multiplier;\n    yield 2 * multiplier;\n}\nvar g = "
         "gen();\nprint(g.next());\nprint(g.next());\nprint(g.done());\n",
         "生成器闭包捕获外层变量 multiplier"},
        {"偶数生成器",
         "fun* evenNumbers(start: int, end: int) {\n    var i = start;\n    while (i < end) {\n        if (i % 2 == 0) "
         "{\n            yield i;\n        }\n        i = i + 1;\n    }\n}\nvar g = evenNumbers(1, "
         "8);\nprint(g.next());\nprint("
         "g.next());\nprint(g.next());\nprint(g.next());\nprint(g.done());\n",
         "循环+条件 yield，前3次返回 2,4,6，第4次返回 null 触发 done"},
        {"显式 return 终止",
         "fun* gen() {\n    yield 1;\n    return 99;\n    yield 2;\n}\nvar g = "
         "gen();\nprint(g.next());\nprint(g.next());\n"
         "print(g.done());\n",
         "return 后 done=true，第三个 yield 不会执行"},
    };
    return kSamples;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 格式化 DiagnosticBag 中的错误条目为 HTML（已转义）
QString formatDiagnosticErrors(const DiagnosticBag& bag) {
    QString result;
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            result += QString::fromUtf8(d.format().c_str()).toHtmlEscaped() + QStringLiteral("<br>");
        }
    }
    if (result.isEmpty()) {
        result = QStringLiteral("（未知错误）");
    }
    return result;
}

/// 描述栈式字节码 CompileResult 中的生成器 chunk（用于执行轨迹 HTML）
/// 列出 isGenerator=true 的 chunk 名 + yieldCount（动态则标注 INT_MAX）
QString describeStackGeneratorChunks(const std::map<std::string, BytecodeChunk>& chunks) {
    QStringList found;
    for (const auto& kv : chunks) {
        if (kv.second.isGenerator) {
            QString yc;
            if (kv.second.yieldCount == BytecodeChunk::kDynamicYieldCount) {
                yc = QStringLiteral("动态(INT_MAX)");
            } else {
                yc = QString::number(kv.second.yieldCount);
            }
            found << QStringLiteral("%1 (yieldCount=%2)").arg(QString::fromUtf8(kv.first.c_str()), yc);
        }
    }
    if (found.isEmpty()) {
        return QStringLiteral("未发现生成器 chunk（源码中无 fun* 声明？）");
    }
    return found.join(QStringLiteral("<br>"));
}

/// 描述寄存器式字节码 RegisterCompileResult 中的生成器 chunk
QString describeRegGeneratorChunks(const std::map<std::string, RegBytecodeChunk>& chunks) {
    QStringList found;
    for (const auto& kv : chunks) {
        if (kv.second.isGenerator) {
            QString yc;
            if (kv.second.yieldCount == RegBytecodeChunk::kDynamicYieldCount) {
                yc = QStringLiteral("动态(INT_MAX)");
            } else {
                yc = QString::number(kv.second.yieldCount);
            }
            found << QStringLiteral("%1 (yieldCount=%2)").arg(QString::fromUtf8(kv.first.c_str()), yc);
        }
    }
    if (found.isEmpty()) {
        return QStringLiteral("未发现生成器 chunk（源码中无 fun* 声明？）");
    }
    return found.join(QStringLiteral("<br>"));
}

/// 构造执行轨迹 HTML：编译阶段 + 执行阶段 + 协程摘要 + 统计信息
QString buildTraceHtml(const QString& compileStage, const QString& execStage, const QString& coroutineSummary,
                       const QString& instrCount, qint64 elapsedMs, bool hasRuntimeError) {
    QString stats = QStringLiteral("<b>指令数:</b> %1 | <b>耗时:</b> %2 ms | <b>运行时错误:</b> %3")
                        .arg(instrCount)
                        .arg(elapsedMs)
                        .arg(hasRuntimeError ? QStringLiteral("是") : QStringLiteral("否"));
    return QStringLiteral("<h3>编译阶段</h3><p>%1</p>"
                          "<h3>执行阶段</h3><pre style='white-space:pre-wrap;'>%2</pre>"
                          "<h3>协程摘要</h3><p>%3</p>"
                          "<h3>统计信息</h3><p>%4</p>")
        .arg(compileStage)
        .arg(execStage)
        .arg(coroutineSummary)
        .arg(stats);
}

} // anonymous namespace

// ============================================================
// CoroutineVisualizerPanel 实现
// ============================================================

/// 构造面板：顶部源码输入 + 运行按钮 + 样例下拉，中间结果表格 + 三后端
/// 执行轨迹，底部加载样例按钮 + 一致性提示标签。预填示例代码。
CoroutineVisualizerPanel::CoroutineVisualizerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：源码输入 + 运行按钮
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(mlTr("源码：")));
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(mlTr("输入 MiniLang 协程源码后点击运行四后端"));
    runBtn_ = new QPushButton(mlTr("运行四后端"));
    topBar->addWidget(sourceEdit_, 1);
    topBar->addWidget(runBtn_);
    outer->addLayout(topBar);

    // 样例切换栏（QComboBox 紧凑选择 5 个样例）
    auto* sampleBar = new QHBoxLayout;
    sampleBar->addWidget(new QLabel(mlTr("内置样例：")));
    sampleCombo_ = new QComboBox;
    sampleCombo_->setToolTip(mlTr("选择预设协程样例，自动填充源码到输入框"));
    for (const auto& s : samples()) {
        // 显示 "标题 - 描述（截断）"，data 保留 index
        QString label =
            QString::fromUtf8(s.title.c_str()) + QStringLiteral(" — ") + QString::fromUtf8(s.description.c_str());
        sampleCombo_->addItem(label);
    }
    sampleBar->addWidget(sampleCombo_, 1);
    outer->addLayout(sampleBar);

    // 中间：QSplitter(Vertical) 上方结果表格 + 下方三后端轨迹
    auto* splitter = new QSplitter(Qt::Vertical);
    resultTable_ = new QTableWidget(0, 5);
    resultTable_->setHorizontalHeaderLabels(
        {mlTr("后端"), mlTr("输出"), mlTr("状态"), mlTr("协程状态"), mlTr("耗时(ms)")});
    resultTable_->verticalHeader()->setVisible(false);
    resultTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    resultTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    resultTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    splitter->addWidget(resultTable_);

    // 下方：QSplitter(Horizontal) 三个 QTextBrowser 并排
    auto* traceSplitter = new QSplitter(Qt::Horizontal);
    interpTrace_ = new QTextBrowser;
    stackVmTrace_ = new QTextBrowser;
    regVmTrace_ = new QTextBrowser;
    interpTrace_->setOpenExternalLinks(false);
    stackVmTrace_->setOpenExternalLinks(false);
    regVmTrace_->setOpenExternalLinks(false);
    interpTrace_->setPlaceholderText(mlTr("Interpreter 执行轨迹（含协程摘要）"));
    stackVmTrace_->setPlaceholderText(mlTr("StackVM (IR) 执行轨迹（含生成器 chunk 信息）"));
    regVmTrace_->setPlaceholderText(mlTr("RegisterVM (IR) 执行轨迹（含生成器 chunk 信息）"));
    // 等宽字体（GuiTextUtils 工厂，跨机器回退链）
    const QFont& monoFont = GuiTextUtils::monospaceFont(10);
    interpTrace_->setFont(monoFont);
    stackVmTrace_->setFont(monoFont);
    regVmTrace_->setFont(monoFont);
    traceSplitter->addWidget(interpTrace_);
    traceSplitter->addWidget(stackVmTrace_);
    traceSplitter->addWidget(regVmTrace_);
    traceSplitter->setSizes({400, 400, 400});
    splitter->addWidget(traceSplitter);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    outer->addWidget(splitter, 1);

    // 一致性提示标签（表格下方）
    consistencyLabel_ = new QLabel(mlTr("（点击「运行四后端」后自动检查三后端协程输出一致性）"));
    consistencyLabel_->setWordWrap(true);
    outer->addWidget(consistencyLabel_);

    // 底部：加载样例按钮 + 提示
    auto* bottomBar = new QHBoxLayout;
    loadSampleBtn_ = new QPushButton(mlTr("加载样例代码到主编辑器"));
    auto* hintLabel = new QLabel(mlTr("三后端 yield 重放输出应完全一致（协程语义等价性验证）"));
    hintLabel->setStyleSheet(QStringLiteral("color:#6E6E6E;"));
    bottomBar->addWidget(loadSampleBtn_);
    bottomBar->addStretch();
    bottomBar->addWidget(hintLabel);
    outer->addLayout(bottomBar);

    // 信号连接
    connect(runBtn_, &QPushButton::clicked, this, &CoroutineVisualizerPanel::onRunAllBackends);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &CoroutineVisualizerPanel::onLoadSample);
    connect(sampleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &CoroutineVisualizerPanel::onSelectSample);

    // 预填示例代码（样例0）
    sourceEdit_->setText(QString::fromUtf8(samples()[0].code.c_str()));

    // 初始化表格为 3 行（待运行状态）
    resultTable_->setRowCount(3);
    QStringList backendNames = {QString::fromUtf8("Interpreter"), QString::fromUtf8("StackVM (IR)"),
                                QString::fromUtf8("RegisterVM (IR)")};
    for (int i = 0; i < 3; ++i) {
        auto* nameItem = new QTableWidgetItem(backendNames[i]);
        nameItem->setTextAlignment(Qt::AlignCenter);
        resultTable_->setItem(i, 0, nameItem);
        resultTable_->setItem(i, 1, new QTableWidgetItem(mlTr("（待运行）")));
        resultTable_->setItem(i, 2, new QTableWidgetItem(mlTr("待运行")));
        resultTable_->setItem(i, 3, new QTableWidgetItem(mlTr("—")));
        resultTable_->setItem(i, 4, new QTableWidgetItem(mlTr("—")));
    }
}

/// 运行三后端：串行执行 Interpreter / StackVM(IR) / RegisterVM(IR)，
/// 渲染表格行与执行轨迹，并检查协程输出一致性。
void CoroutineVisualizerPanel::onRunAllBackends() {
    std::string src = sourceEdit_->text().toStdString();
    if (src.empty()) {
        consistencyLabel_->setText(mlTr("❌ 源码为空，请输入 MiniLang 协程源码"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B;"));
        return;
    }

    // 串行执行三后端（主线程，避免引擎层非线程安全问题）
    BackendExecResult interp = runInterpreter(src);
    BackendExecResult stackvm = runStackVM_IR(src);
    BackendExecResult regvm = runRegVM_IR(src);

    // 渲染表格行
    renderResult(0, QString::fromUtf8("Interpreter"), interp);
    renderResult(1, QString::fromUtf8("StackVM (IR)"), stackvm);
    renderResult(2, QString::fromUtf8("RegisterVM (IR)"), regvm);

    // 渲染执行轨迹
    interpTrace_->setHtml(interp.traceHtml);
    stackVmTrace_->setHtml(stackvm.traceHtml);
    regVmTrace_->setHtml(regvm.traceHtml);

    // 一致性检查
    renderConsistency(interp, stackvm, regvm);
}

/// 运行 Interpreter 树遍历后端：Lexer → Parser → Interpreter::execute(AST)。
/// 指令数显示 N/A（无字节码），耗时用 QElapsedTimer 测量。
CoroutineVisualizerPanel::BackendExecResult CoroutineVisualizerPanel::runInterpreter(const std::string& src) {
    BackendExecResult r;
    r.instrCount = QStringLiteral("N/A");

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 词法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        QString err = formatDiagnosticErrors(lex.getDiagnostics());
        r.status = mlTr("❌ 词法错误");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 语法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.status = mlTr("❌ 语法错误");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Interpreter 执行
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });

    QElapsedTimer timer;
    timer.start();
    bool hasRuntimeError = false;
    QString runtimeErr;
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        hasRuntimeError = true;
        runtimeErr = QString::fromUtf8(e.what()).toHtmlEscaped();
    } catch (const std::exception& e) {
        hasRuntimeError = true;
        runtimeErr = QString::fromUtf8(e.what()).toHtmlEscaped();
    }
    r.elapsedMs = timer.elapsed();
    r.output = QString::fromUtf8(out.c_str());
    r.coroutineState = extractCoroutineState(src, r.output);

    QString compileStage = mlTr("✅ 编译成功（Interpreter 直接执行 AST，无字节码；"
                                "yield 重放模式由 visitYieldExpr 计数器实现）");
    if (hasRuntimeError) {
        r.status = mlTr("❌ 运行时错误: ") + runtimeErr;
        r.success = false;
        r.traceHtml = buildTraceHtml(compileStage,
                                     QString::fromUtf8(out.c_str()).toHtmlEscaped() +
                                         QStringLiteral("\n❌ 运行时错误: %1").arg(runtimeErr),
                                     r.coroutineState, r.instrCount, r.elapsedMs, true);
    } else {
        r.status = mlTr("✅ 成功");
        r.success = true;
        r.traceHtml = buildTraceHtml(compileStage, QString::fromUtf8(out.c_str()).toHtmlEscaped(), r.coroutineState,
                                     r.instrCount, r.elapsedMs, false);
    }
    return r;
}

/// 运行 StackVM（IR 路径）：Compiler(setUseIR(true)) → VM::execute(CompileResult)。
/// 指令数取 mainChunk.code.size()（字节码字节数），并提取生成器 chunk 信息。
CoroutineVisualizerPanel::BackendExecResult CoroutineVisualizerPanel::runStackVM_IR(const std::string& src) {
    BackendExecResult r;

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 词法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        QString err = formatDiagnosticErrors(lex.getDiagnostics());
        r.status = mlTr("❌ 词法错误");
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 语法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.status = mlTr("❌ 语法错误");
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Compiler（IR 路径）
    Compiler compiler;
    compiler.setUseIR(true);
    CompileResult cr;
    try {
        cr = compiler.compile(*ast);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 编译错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 编译错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        QString err = formatDiagnosticErrors(compiler.getDiagnostics());
        r.status = mlTr("❌ 编译错误: ") + QString::fromUtf8(compiler.getLastError().c_str()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 编译错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    r.instrCount = QString::number(static_cast<qint64>(cr.mainChunk.code.size()));

    // VM 执行
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });

    QElapsedTimer timer;
    timer.start();
    vm.execute(cr);
    r.elapsedMs = timer.elapsed();
    r.output = QString::fromUtf8(out.c_str());
    r.coroutineState = extractCoroutineState(src, r.output);

    QString genInfo = describeStackGeneratorChunks(cr.functionChunks);
    QString compileStage = QStringLiteral("✅ 编译成功（IR 路径，字节码 %1 字节）<br>"
                                          "<b>生成器 chunk:</b><br>%2")
                               .arg(r.instrCount, genInfo);
    if (vm.hasError()) {
        QString err = QString::fromUtf8(vm.getLastError().c_str()).toHtmlEscaped();
        r.status = mlTr("❌ 运行时错误: ") + err;
        r.success = false;
        r.traceHtml = buildTraceHtml(compileStage,
                                     QString::fromUtf8(out.c_str()).toHtmlEscaped() +
                                         QStringLiteral("\n❌ 运行时错误: %1").arg(err),
                                     r.coroutineState, r.instrCount, r.elapsedMs, true);
    } else {
        r.status = mlTr("✅ 成功");
        r.success = true;
        r.traceHtml = buildTraceHtml(compileStage, QString::fromUtf8(out.c_str()).toHtmlEscaped(), r.coroutineState,
                                     r.instrCount, r.elapsedMs, false);
    }
    return r;
}

/// 运行 RegisterVM（IR 路径）：Compiler(setUseRegisterVM(true)) →
/// RegisterVM::execute(RegisterCompileResult)。指令数取 mainChunk.code.size()，
/// 并提取生成器 chunk 信息（isGenerator/yieldCount）。
CoroutineVisualizerPanel::BackendExecResult CoroutineVisualizerPanel::runRegVM_IR(const std::string& src) {
    BackendExecResult r;

    // Lexer
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 词法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        QString err = formatDiagnosticErrors(lex.getDiagnostics());
        r.status = mlTr("❌ 词法错误");
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 词法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Parser
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 语法错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (!ast || parser.hasErrors()) {
        QString err = ast ? formatDiagnosticErrors(parser.getDiagnostics()) : QStringLiteral("AST 为空");
        r.status = mlTr("❌ 语法错误");
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 语法错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    // Compiler（寄存器 IR 路径）
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    try {
        compiler.compile(*ast);
    } catch (const std::exception& e) {
        r.status = mlTr("❌ 编译错误: ") + QString::fromUtf8(e.what()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 编译错误: %1").arg(QString::fromUtf8(e.what()).toHtmlEscaped()),
                                     QStringLiteral(""), r.coroutineState, r.instrCount, 0, false);
        return r;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        QString err = formatDiagnosticErrors(compiler.getDiagnostics());
        r.status = mlTr("❌ 编译错误: ") + QString::fromUtf8(compiler.getLastError().c_str()).toHtmlEscaped();
        r.instrCount = QStringLiteral("N/A");
        r.traceHtml = buildTraceHtml(QStringLiteral("❌ 编译错误:<br>%1").arg(err), QStringLiteral(""),
                                     r.coroutineState, r.instrCount, 0, false);
        return r;
    }

    const auto& regResult = compiler.getLastRegisterResult();
    r.instrCount = QString::number(static_cast<qint64>(regResult.mainChunk.code.size()));

    // RegisterVM 执行
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });

    QElapsedTimer timer;
    timer.start();
    vm.execute(regResult);
    r.elapsedMs = timer.elapsed();
    r.output = QString::fromUtf8(out.c_str());
    r.coroutineState = extractCoroutineState(src, r.output);

    QString genInfo = describeRegGeneratorChunks(regResult.functionChunks);
    QString compileStage = QStringLiteral("✅ 编译成功（寄存器 IR 路径，字节码 %1 字节）<br>"
                                          "<b>生成器 chunk:</b><br>%2")
                               .arg(r.instrCount, genInfo);
    if (vm.hasError()) {
        QString err = QString::fromUtf8(vm.getLastError().c_str()).toHtmlEscaped();
        r.status = mlTr("❌ 运行时错误: ") + err;
        r.success = false;
        r.traceHtml = buildTraceHtml(compileStage,
                                     QString::fromUtf8(out.c_str()).toHtmlEscaped() +
                                         QStringLiteral("\n❌ 运行时错误: %1").arg(err),
                                     r.coroutineState, r.instrCount, r.elapsedMs, true);
    } else {
        r.status = mlTr("✅ 成功");
        r.success = true;
        r.traceHtml = buildTraceHtml(compileStage, QString::fromUtf8(out.c_str()).toHtmlEscaped(), r.coroutineState,
                                     r.instrCount, r.elapsedMs, false);
    }
    return r;
}

/// 解析源码与输出，提取协程状态摘要：
///   - 统计源码中 .next() 与 .done() 调用次数
///   - 按 print 输出行顺序，前 nextCount 行作为 yield 返回值，
///     后 doneCount 行作为 done 返回值
///   - 摘要格式："next×N, done=<last value>"
QString CoroutineVisualizerPanel::extractCoroutineState(const std::string& src, const QString& output) {
    const std::string nextTok = ".next()";
    const std::string doneTok = ".done()";
    int nextCount = 0;
    int doneCount = 0;
    for (size_t pos = 0; (pos = src.find(nextTok, pos)) != std::string::npos; pos += nextTok.size()) {
        ++nextCount;
    }
    for (size_t pos = 0; (pos = src.find(doneTok, pos)) != std::string::npos; pos += doneTok.size()) {
        ++doneCount;
    }

    if (nextCount == 0 && doneCount == 0) {
        return mlTr("（源码未发现 .next()/.done() 调用，非协程代码）");
    }

    // 切分输出（按换行，忽略空行）
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    // 取最后一个 done 的值作为最终态
    QString lastDoneValue;
    int idx = 0;
    for (int i = 0; i < nextCount && idx < lines.size(); ++i, ++idx) {
        // 跳过 yield 返回值行
    }
    for (int i = 0; i < doneCount && idx < lines.size(); ++i, ++idx) {
        lastDoneValue = lines[idx].trimmed();
    }

    if (doneCount > 0) {
        if (lastDoneValue.isEmpty()) {
            return QStringLiteral("next×%1, done=<空>").arg(nextCount);
        }
        return QStringLiteral("next×%1, done=%2").arg(nextCount).arg(lastDoneValue);
    }
    return QStringLiteral("next×%1, done=未查询").arg(nextCount);
}

/// 渲染单个后端的表格行（后端名/输出/状态/协程状态/耗时）。
void CoroutineVisualizerPanel::renderResult(int row, const QString& backendName, const BackendExecResult& r) {
    if (row < 0 || row >= resultTable_->rowCount()) {
        return;
    }
    auto* nameItem = new QTableWidgetItem(backendName);
    nameItem->setTextAlignment(Qt::AlignCenter);
    auto* outItem = new QTableWidgetItem(r.output);
    auto* statusItem = new QTableWidgetItem(r.status);
    statusItem->setTextAlignment(Qt::AlignCenter);
    auto* coroItem = new QTableWidgetItem(r.coroutineState);
    coroItem->setTextAlignment(Qt::AlignCenter);
    auto* timeItem = new QTableWidgetItem(QString::number(r.elapsedMs));
    timeItem->setTextAlignment(Qt::AlignCenter);

    // 状态着色：成功绿色，失败红色
    if (r.success) {
        statusItem->setForeground(QColor(QStringLiteral("#27AE60")));
    } else {
        statusItem->setForeground(QColor(QStringLiteral("#C0392B")));
    }

    resultTable_->setItem(row, 0, nameItem);
    resultTable_->setItem(row, 1, outItem);
    resultTable_->setItem(row, 2, statusItem);
    resultTable_->setItem(row, 3, coroItem);
    resultTable_->setItem(row, 4, timeItem);
}

/// 渲染一致性检查标签：三后端均成功且输出完全一致则 ✅，否则 ❌。
void CoroutineVisualizerPanel::renderConsistency(const BackendExecResult& interp, const BackendExecResult& stackvm,
                                                 const BackendExecResult& regvm) {
    bool allSuccess = interp.success && stackvm.success && regvm.success;
    bool outputEqual = (interp.output == stackvm.output) && (stackvm.output == regvm.output);
    if (allSuccess && outputEqual) {
        consistencyLabel_->setText(mlTr("✅ 三后端协程输出一致，yield 重放语义等价性验证通过"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#27AE60; font-weight:bold;"));
    } else {
        consistencyLabel_->setText(mlTr("❌ 三后端协程输出不一致，存在 yield 重放语义差异！"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    }
}

/// 选中内置样例：将样例代码填入源码输入框。
void CoroutineVisualizerPanel::onSelectSample(int idx) {
    const auto& s = samples();
    if (idx < 0 || idx >= static_cast<int>(s.size())) {
        return;
    }
    sourceEdit_->setText(QString::fromUtf8(s[idx].code.c_str()));
}

/// 将当前源码输入框内容 emit loadSampleRequested，加载到主编辑器运行观察。
void CoroutineVisualizerPanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->text());
}

// ============================================================
// createGuidedTour — 新手引导（4 步）
// ============================================================

/// 构建 4 步新手引导：协程概念（yield/next）、生成器状态机、upvalue 捕获、动手实验。
GuidedTour* CoroutineVisualizerPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(runBtn_, mlTr("协程概念"),
                  mlTr("MiniLang 用 fun* / yield / .next() 实现协程：yield 暂停当前执行并返回值，"
                       ".next() 恢复执行到下一个 yield。点击「运行四后端」观察三后端输出。"));
    tour->addStep(resultTable_, mlTr("生成器状态机"),
                  mlTr("结果表的「协程状态」列展示生成器状态机摘要（next×N, done=<值>），"
                       "反映 yield 重放次数与终态。"));
    tour->addStep(stackVmTrace_, mlTr("upvalue 捕获"),
                  mlTr("下方执行轨迹展示三后端每步指令与协程状态，可观察闭包 upvalue 捕获链"
                       "如何在 yield 暂停/恢复期间保持变量存活。"));
    tour->addStep(loadSampleBtn_, mlTr("动手实验"),
                  mlTr("点击「加载样例」将当前源码送入主编辑器，配合调试器单步观察"
                       "yield 暂停点与 next 恢复点的栈帧状态。"));
    return tour;
}
