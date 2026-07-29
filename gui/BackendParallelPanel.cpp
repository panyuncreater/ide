// ============================================================
// BackendParallelPanel.cpp — 三后端并行可视化教学面板实现
// ------------------------------------------------------------
// ARCH-10 重构：面板不再直接 include Compiler.h/VM.h/RegisterVM.h/
// Interpreter.h/Lexer.h/Parser.h 等内部头文件，统一通过
// BackendExecutionService 中间层触发 Lexer → Parser → 三后端执行。
// 面板仅负责结果渲染与一致性比对，编译/执行细节由服务封装。
// ============================================================

#include "gui/BackendParallelPanel.h"

#include "common/BackendExecutionService.h" // ARCH-10: 后端执行服务中间层

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// ============================================================
// 内置示例库（3 个）
// ============================================================

const std::vector<BackendParallelSample>& BackendParallelPanel::samples() {
    static const std::vector<BackendParallelSample> kSamples = {
        {"基本算术", "var x = 10; var y = 20; print(x + y);", "整数加法与变量绑定，验证三后端基础算术语义一致"},
        {"容器操作", "var arr = [1, 2, 3]; arr[0] = 99; print(arr[0]);",
         "数组构建 + 索引赋值 + 索引读取，验证 COW 容器写时拷贝"},
        {"递归调用", "fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print(fib(10));",
         "函数定义 + 递归调用 + 闭包返回值，验证调用栈一致性"},
    };
    return kSamples;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 构造执行轨迹 HTML：编译阶段 + 执行阶段 + 统计信息
QString buildTraceHtml(const QString& compileStage, const QString& execStage, const QString& instrCount,
                       qint64 elapsedMs, bool hasRuntimeError) {
    QString stats = QStringLiteral("<b>指令数:</b> %1 | <b>耗时:</b> %2 ms | <b>运行时错误:</b> %3")
                        .arg(instrCount)
                        .arg(elapsedMs)
                        .arg(hasRuntimeError ? QStringLiteral("是") : QStringLiteral("否"));
    return QStringLiteral("<h3>编译阶段</h3><p>%1</p>"
                          "<h3>执行阶段</h3><pre style='white-space:pre-wrap;'>%2</pre>"
                          "<h3>统计信息</h3><p>%3</p>")
        .arg(compileStage)
        .arg(execStage)
        .arg(stats);
}

/// 将服务层 BackendExecResult 转换为面板内部结构（含 traceHtml 渲染）
BackendParallelPanel::BackendExecResult convertServiceResult(const ::BackendExecResult& sr) {
    BackendParallelPanel::BackendExecResult r;
    r.success = sr.success;
    r.output = QString::fromUtf8(sr.output.c_str());
    r.elapsedMs = static_cast<qint64>(sr.elapsedMs);
    r.instrCount = QString::fromUtf8(sr.instrCountText().c_str());

    // 状态文本
    r.status = QString::fromUtf8(sr.statusText().c_str());

    // 编译阶段描述
    QString compileStage = QString::fromUtf8(sr.compileStageText().c_str());

    // 执行阶段 HTML：输出 + 运行时错误（若有）
    QString execStage = QString::fromUtf8(sr.output.c_str()).toHtmlEscaped();
    bool hasRuntimeError = (!sr.success && sr.errorPrefix == "运行时错误");
    if (hasRuntimeError) {
        QString err = QString::fromUtf8(sr.errorMsg.c_str()).toHtmlEscaped();
        execStage += QStringLiteral("\n❌ 运行时错误: %1").arg(err);
    } else if (!sr.success) {
        // 词法/语法/编译错误：execStage 留空，compileStage 已含错误信息
        execStage = QStringLiteral("");
    }
    r.traceHtml = buildTraceHtml(compileStage, execStage, r.instrCount, r.elapsedMs, hasRuntimeError);
    return r;
}

} // anonymous namespace

// ============================================================
// BackendParallelPanel 实现
// ============================================================

/// 构造面板：顶部源码输入 + 运行按钮 + 样例切换，中间结果表格 + 三后端
/// 执行轨迹，底部加载样例按钮 + 一致性提示标签。预填示例代码。
BackendParallelPanel::BackendParallelPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：源码输入 + 运行按钮 + 样例切换
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("源码：")));
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(tr("输入 MiniLang 源码后点击运行三后端"));
    runBtn_ = new QPushButton(tr("运行三后端"));
    topBar->addWidget(sourceEdit_, 1);
    topBar->addWidget(runBtn_);
    outer->addLayout(topBar);

    // 样例切换按钮栏
    auto* sampleBar = new QHBoxLayout;
    sampleBar->addWidget(new QLabel(tr("内置样例：")));
    sample1Btn_ = new QPushButton(QString::fromUtf8("样例1: ") + tr("基本算术"));
    sample2Btn_ = new QPushButton(QString::fromUtf8("样例2: ") + tr("容器操作"));
    sample3Btn_ = new QPushButton(QString::fromUtf8("样例3: ") + tr("递归调用"));
    sampleBar->addWidget(sample1Btn_);
    sampleBar->addWidget(sample2Btn_);
    sampleBar->addWidget(sample3Btn_);
    sampleBar->addStretch();
    outer->addLayout(sampleBar);

    // 中间：QSplitter(Vertical) 上方结果表格 + 下方三后端轨迹
    auto* splitter = new QSplitter(Qt::Vertical);
    resultTable_ = new QTableWidget(0, 5);
    resultTable_->setHorizontalHeaderLabels({tr("后端"), tr("输出"), tr("指令数"), tr("状态"), tr("耗时(ms)")});
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
    interpTrace_->setPlaceholderText(tr("Interpreter 执行轨迹"));
    stackVmTrace_->setPlaceholderText(tr("StackVM (IR) 执行轨迹"));
    regVmTrace_->setPlaceholderText(tr("RegisterVM (IR) 执行轨迹"));
    traceSplitter->addWidget(interpTrace_);
    traceSplitter->addWidget(stackVmTrace_);
    traceSplitter->addWidget(regVmTrace_);
    traceSplitter->setSizes({400, 400, 400});
    splitter->addWidget(traceSplitter);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    outer->addWidget(splitter, 1);

    // 一致性提示标签（表格下方）
    consistencyLabel_ = new QLabel(QString::fromUtf8("（点击「运行三后端」后自动检查三后端输出一致性）"));
    consistencyLabel_->setWordWrap(true);
    outer->addWidget(consistencyLabel_);

    // 底部：加载样例按钮 + 提示
    auto* bottomBar = new QHBoxLayout;
    loadSampleBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    auto* hintLabel = new QLabel(QString::fromUtf8("三后端输出应完全一致（语义等价性验证）"));
    hintLabel->setStyleSheet(QStringLiteral("color:#6E6E6E;"));
    bottomBar->addWidget(loadSampleBtn_);
    bottomBar->addStretch();
    bottomBar->addWidget(hintLabel);
    outer->addLayout(bottomBar);

    // 信号连接
    connect(runBtn_, &QPushButton::clicked, this, &BackendParallelPanel::onRunAllBackends);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &BackendParallelPanel::onLoadSample);
    connect(sample1Btn_, &QPushButton::clicked, [this]() { onSelectSample(0); });
    connect(sample2Btn_, &QPushButton::clicked, [this]() { onSelectSample(1); });
    connect(sample3Btn_, &QPushButton::clicked, [this]() { onSelectSample(2); });

    // 预填示例代码
    sourceEdit_->setText(QString::fromUtf8(samples()[0].code.c_str()));

    // 初始化表格为 3 行（待运行状态）
    resultTable_->setRowCount(3);
    QStringList backendNames = {QString::fromUtf8("Interpreter"), QString::fromUtf8("StackVM (IR)"),
                                QString::fromUtf8("RegisterVM (IR)")};
    for (int i = 0; i < 3; ++i) {
        auto* nameItem = new QTableWidgetItem(backendNames[i]);
        nameItem->setTextAlignment(Qt::AlignCenter);
        resultTable_->setItem(i, 0, nameItem);
        resultTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8("（待运行）")));
        resultTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8("—")));
        resultTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8("待运行")));
        resultTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8("—")));
    }
}

/// 运行三后端：通过 BackendExecutionService 串行执行 Interpreter /
/// StackVM(IR) / RegisterVM(IR)，渲染表格行与执行轨迹，并检查输出一致性。
void BackendParallelPanel::onRunAllBackends() {
    std::string src = sourceEdit_->text().toStdString();
    if (src.empty()) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 源码为空，请输入 MiniLang 源码"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B;"));
        return;
    }

    // ARCH-10: 通过服务中间层触发三后端执行（主线程串行，避免引擎层非线程安全问题）
    BackendExecResult interp = convertServiceResult(BackendExecutionService::execute(src, BackendType::Interpreter));
    BackendExecResult stackvm = convertServiceResult(BackendExecutionService::execute(src, BackendType::StackVM_IR));
    BackendExecResult regvm = convertServiceResult(BackendExecutionService::execute(src, BackendType::RegisterVM_IR));

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

/// 渲染单个后端的表格行（后端名/输出/指令数/状态/耗时）。
void BackendParallelPanel::renderResult(int row, const QString& backendName, const BackendExecResult& r) {
    if (row < 0 || row >= resultTable_->rowCount()) {
        return;
    }
    auto* nameItem = new QTableWidgetItem(backendName);
    nameItem->setTextAlignment(Qt::AlignCenter);
    auto* outItem = new QTableWidgetItem(r.output);
    auto* instrItem = new QTableWidgetItem(r.instrCount);
    instrItem->setTextAlignment(Qt::AlignCenter);
    auto* statusItem = new QTableWidgetItem(r.status);
    statusItem->setTextAlignment(Qt::AlignCenter);
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
    resultTable_->setItem(row, 2, instrItem);
    resultTable_->setItem(row, 3, statusItem);
    resultTable_->setItem(row, 4, timeItem);
}

/// 渲染一致性检查标签：三后端均成功且输出完全一致则 ✅，否则 ❌。
void BackendParallelPanel::renderConsistency(const BackendExecResult& interp, const BackendExecResult& stackvm,
                                             const BackendExecResult& regvm) {
    bool allSuccess = interp.success && stackvm.success && regvm.success;
    bool outputEqual = (interp.output == stackvm.output) && (stackvm.output == regvm.output);
    if (allSuccess && outputEqual) {
        consistencyLabel_->setText(QString::fromUtf8("✅ 三后端输出一致，语义等价性验证通过"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#27AE60; font-weight:bold;"));
    } else {
        consistencyLabel_->setText(QString::fromUtf8("❌ 三后端输出不一致，存在语义差异！"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    }
}

/// 选中内置样例：将样例代码填入源码输入框。
void BackendParallelPanel::onSelectSample(int idx) {
    const auto& s = samples();
    if (idx < 0 || idx >= static_cast<int>(s.size())) {
        return;
    }
    sourceEdit_->setText(QString::fromUtf8(s[idx].code.c_str()));
}

/// 将当前源码输入框内容 emit loadSampleRequested，加载到主编辑器运行观察。
void BackendParallelPanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->text());
}
