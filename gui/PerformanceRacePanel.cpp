// ============================================================
// PerformanceRacePanel.cpp — 三后端性能竞赛教学面板实现
// ------------------------------------------------------------
// ARCH-10 重构：面板不再直接 include Compiler.h/VM.h/RegisterVM.h/
// Interpreter.h/Lexer.h/Parser.h 等内部头文件，统一通过
// BackendExecutionService 中间层触发 Lexer → Parser → 三后端执行。
// 每个后端运行 N 次（1-10，由 QSpinBox 选择），记录平均/最小/最大
// 耗时，并用 HTML/CSS 绘制水平柱状图，生成性能分析报告。
// 三后端在主线程串行执行（避免引擎层非线程安全问题）。
// ============================================================

#include "gui/PerformanceRacePanel.h"

#include "common/BackendExecutionService.h" // ARCH-10: 后端执行服务中间层

#include <QApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <sstream>

// ============================================================
// PerformanceRaceLibrary — 静态教学样例库
// ============================================================

const std::vector<PerfSample>& PerformanceRaceLibrary::samples() {
    static const std::vector<PerfSample> kSamples = {
        {"基本算术", "var x = 10; var y = 20; var z = (x + y) * 2 - x / 5; print(z);",
         "变量绑定 + 四则运算 + 括号优先级，验证三后端基础算术语义与耗时基线"},
        {"递归fib", "fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print(fib(20));",
         "函数定义 + 递归调用 + 条件分支，验证调用栈一致性，体现解释器开销"},
        {"循环累加", "var sum = 0; var i = 1; while (i <= 1000) { sum = sum + i; i = i + 1; } print(sum);",
         "while 循环 + 累加赋值，验证循环字节码优化与寄存器式效率优势"},
    };
    return kSamples;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 后端柱状图颜色（Interpreter 蓝 / StackVM 绿 / RegisterVM 橙）
QString backendColor(int idx) {
    switch (idx) {
    case 0:
        return QStringLiteral("#3498DB"); // Interpreter 蓝
    case 1:
        return QStringLiteral("#27AE60"); // StackVM 绿
    case 2:
        return QStringLiteral("#E67E22"); // RegisterVM 橙
    default:
        return QStringLiteral("#888888");
    }
}

/// 格式化耗时（保留 3 位小数）
QString formatMs(double ms) {
    return QString::number(ms, 'f', 3) + QStringLiteral(" ms");
}

/// 将服务层 BackendExecResult 转换为面板内部 SingleRunResult
PerformanceRacePanel::SingleRunResult convertServiceResult(const ::BackendExecResult& sr) {
    PerformanceRacePanel::SingleRunResult r;
    r.success = sr.success;
    r.output = QString::fromUtf8(sr.output.c_str());
    r.elapsedMs = static_cast<qint64>(sr.elapsedMs);
    r.instrCount = QString::fromUtf8(sr.instrCountText().c_str());
    r.status = QString::fromUtf8(sr.statusText().c_str());
    return r;
}

} // anonymous namespace

// ============================================================
// PerformanceRacePanel 实现
// ============================================================

/// 构造面板：顶部源码输入 + 运行次数 + 竞赛按钮 + 样例切换，中间性能
/// 对比表 + 柱状图 + 分析报告，底部加载样例按钮 + 一致性标签。预填示例。
PerformanceRacePanel::PerformanceRacePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：源码输入 + 运行次数 + 竞赛按钮 + 样例切换
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("源码：")));
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(tr("输入 MiniLang 源码后点击竞赛"));
    topBar->addWidget(sourceEdit_, 1);
    topBar->addWidget(new QLabel(tr("运行次数：")));
    runCountSpin_ = new QSpinBox;
    runCountSpin_->setRange(1, 10);
    runCountSpin_->setValue(5);
    runCountSpin_->setToolTip(tr("每个后端运行次数（1-10），取平均/最小/最大耗时"));
    topBar->addWidget(runCountSpin_);
    raceBtn_ = new QPushButton(tr("开始竞赛"));
    topBar->addWidget(raceBtn_);
    outer->addLayout(topBar);

    // 样例切换按钮栏
    auto* sampleBar = new QHBoxLayout;
    sampleBar->addWidget(new QLabel(tr("内置样例：")));
    sample1Btn_ = new QPushButton(QString::fromUtf8("样例1: ") + tr("基本算术"));
    sample2Btn_ = new QPushButton(QString::fromUtf8("样例2: ") + tr("递归fib"));
    sample3Btn_ = new QPushButton(QString::fromUtf8("样例3: ") + tr("循环累加"));
    sampleBar->addWidget(sample1Btn_);
    sampleBar->addWidget(sample2Btn_);
    sampleBar->addWidget(sample3Btn_);
    sampleBar->addStretch();
    outer->addLayout(sampleBar);

    // 中间：QSplitter(Vertical) 上方性能对比表 + 中间柱状图 + 下方分析报告
    auto* splitter = new QSplitter(Qt::Vertical);

    // 上方：性能对比表（5列）
    resultTable_ = new QTableWidget(0, 5);
    resultTable_->setHorizontalHeaderLabels(
        {tr("后端"), tr("平均耗时(ms)"), tr("最小耗时(ms)"), tr("最大耗时(ms)"), tr("指令数")});
    resultTable_->verticalHeader()->setVisible(false);
    resultTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    resultTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    resultTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    splitter->addWidget(resultTable_);

    // 中间：柱状图（HTML/CSS 水平柱状图）
    barChartView_ = new QTextBrowser;
    barChartView_->setOpenExternalLinks(false);
    barChartView_->setPlaceholderText(tr("三后端平均耗时柱状图（点击「开始竞赛」后生成）"));
    splitter->addWidget(barChartView_);

    // 下方：性能分析报告
    analysisView_ = new QTextBrowser;
    analysisView_->setOpenExternalLinks(false);
    analysisView_->setPlaceholderText(tr("性能分析报告（点击「开始竞赛」后生成）"));
    splitter->addWidget(analysisView_);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 3);
    outer->addWidget(splitter, 1);

    // 一致性提示标签（splitter 下方）
    consistencyLabel_ = new QLabel(QString::fromUtf8("（点击「开始竞赛」后自动检查三后端输出一致性）"));
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
    connect(raceBtn_, &QPushButton::clicked, this, &PerformanceRacePanel::onRunRace);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &PerformanceRacePanel::onLoadSample);
    // BUG-95 fix (P3): 为 lambda connect 补齐 this 作为 context object，
    // 确保 PerformanceRacePanel 析构后自动断开连接，避免悬垂 this 捕获。
    connect(sample1Btn_, &QPushButton::clicked, this, [this]() { onSelectSample(0); });
    connect(sample2Btn_, &QPushButton::clicked, this, [this]() { onSelectSample(1); });
    connect(sample3Btn_, &QPushButton::clicked, this, [this]() { onSelectSample(2); });

    // 预填示例代码
    sourceEdit_->setText(QString::fromUtf8(PerformanceRaceLibrary::samples()[0].code.c_str()));

    // 初始化表格为 3 行（待运行状态）
    resultTable_->setRowCount(3);
    QStringList backendNames = {QString::fromUtf8("Interpreter"), QString::fromUtf8("StackVM (IR)"),
                                QString::fromUtf8("RegisterVM (IR)")};
    for (int i = 0; i < 3; ++i) {
        auto* nameItem = new QTableWidgetItem(backendNames[i]);
        nameItem->setTextAlignment(Qt::AlignCenter);
        resultTable_->setItem(i, 0, nameItem);
        for (int col = 1; col <= 3; ++col) {
            resultTable_->setItem(i, col, new QTableWidgetItem(QString::fromUtf8("—")));
        }
        resultTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8("—")));
    }
}

/// 开始竞赛：对三后端各运行 N 次，汇总平均/最小/最大耗时，渲染表格、
/// 柱状图、分析报告与一致性标签。三后端在主线程串行执行。
void PerformanceRacePanel::onRunRace() {
    std::string src = sourceEdit_->text().toStdString();
    if (src.empty()) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 源码为空，请输入 MiniLang 源码"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B;"));
        return;
    }

    int runCount = runCountSpin_->value();
    if (runCount < 1) {
        runCount = 1;
    }

    // Bug #72 fix: 执行期间禁用按钮，防止重复点击
    raceBtn_->setEnabled(false);

    // 串行执行三后端（主线程，避免引擎层非线程安全问题）
    BackendPerfResult interp =
        runBackendMultiple(src, QString::fromUtf8("Interpreter"), runCount, &PerformanceRacePanel::runInterpreterOnce);
    BackendPerfResult stackvm =
        runBackendMultiple(src, QString::fromUtf8("StackVM (IR)"), runCount, &PerformanceRacePanel::runStackVM_IR_Once);
    BackendPerfResult regvm = runBackendMultiple(src, QString::fromUtf8("RegisterVM (IR)"), runCount,
                                                 &PerformanceRacePanel::runRegVM_IR_Once);

    // 渲染表格行
    renderResultRow(0, interp);
    renderResultRow(1, stackvm);
    renderResultRow(2, regvm);

    // 渲染柱状图与分析报告
    renderBarChart(interp, stackvm, regvm);
    renderAnalysis(interp, stackvm, regvm);

    // 一致性检查
    renderConsistency(interp, stackvm, regvm);

    // Bug #72 fix: 恢复按钮
    raceBtn_->setEnabled(true);
}

/// 运行 Interpreter 树遍历后端一次。
/// ARCH-10: 通过 BackendExecutionService 触发，面板不再直接依赖 Lexer/Parser/Interpreter。
PerformanceRacePanel::SingleRunResult PerformanceRacePanel::runInterpreterOnce(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::Interpreter));
}

/// 运行 StackVM（IR 路径）一次。
/// ARCH-10: 通过 BackendExecutionService 触发，面板不再直接依赖 Compiler/VM。
PerformanceRacePanel::SingleRunResult PerformanceRacePanel::runStackVM_IR_Once(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::StackVM_IR));
}

/// 运行 RegisterVM（IR 路径）一次。
/// ARCH-10: 通过 BackendExecutionService 触发，面板不再直接依赖 Compiler/RegisterVM。
PerformanceRacePanel::SingleRunResult PerformanceRacePanel::runRegVM_IR_Once(const std::string& src) {
    return convertServiceResult(BackendExecutionService::execute(src, BackendType::RegisterVM_IR));
}

/// 多次运行汇总：调用指定单次执行函数 N 次，记录每次耗时，计算平均/最小/最大。
/// 任一次运行失败则整体失败（status 取最后一次失败状态，runs 仅记录成功次数）。
BackendPerfResult
PerformanceRacePanel::runBackendMultiple(const std::string& src, const QString& backendName, int runCount,
                                         SingleRunResult (PerformanceRacePanel::*runner)(const std::string&)) {
    BackendPerfResult result;
    result.backendName = backendName;

    if (runCount < 1) {
        runCount = 1;
    }

    SingleRunResult last;
    for (int i = 0; i < runCount; ++i) {
        SingleRunResult r = (this->*runner)(src);
        last = r;
        // Bug #72 fix: 让出 UI 事件循环，避免长时间冻结
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (r.success) {
            result.runs.push_back(static_cast<double>(r.elapsedMs));
        } else {
            // 任一次失败即整体失败，提前结束
            result.success = false;
            result.status = r.status;
            result.output = r.output;
            result.instrCount = r.instrCount;
            return result;
        }
    }

    // 计算平均/最小/最大
    if (!result.runs.empty()) {
        double sum = 0.0;
        double mn = result.runs[0];
        double mx = result.runs[0];
        for (double v : result.runs) {
            sum += v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        result.avgMs = sum / static_cast<double>(result.runs.size());
        result.minMs = mn;
        result.maxMs = mx;
        result.success = true;
        result.status = QString::fromUtf8("✅ 成功");
        result.output = last.output;
        result.instrCount = last.instrCount;
    } else {
        result.success = false;
        result.status = QString::fromUtf8("❌ 无有效运行");
        result.output = last.output;
        result.instrCount = last.instrCount;
    }
    return result;
}

/// 渲染表格行：后端名 / 平均 / 最小 / 最大 / 指令数。
void PerformanceRacePanel::renderResultRow(int row, const BackendPerfResult& r) {
    auto* nameItem = new QTableWidgetItem(r.backendName);
    nameItem->setTextAlignment(Qt::AlignCenter);

    auto* avgItem = new QTableWidgetItem(r.success ? QString::number(r.avgMs, 'f', 3) : QString::fromUtf8("—"));
    avgItem->setTextAlignment(Qt::AlignCenter);

    auto* minItem = new QTableWidgetItem(r.success ? QString::number(r.minMs, 'f', 3) : QString::fromUtf8("—"));
    minItem->setTextAlignment(Qt::AlignCenter);

    auto* maxItem = new QTableWidgetItem(r.success ? QString::number(r.maxMs, 'f', 3) : QString::fromUtf8("—"));
    maxItem->setTextAlignment(Qt::AlignCenter);

    auto* instrItem = new QTableWidgetItem(r.instrCount.isEmpty() ? QString::fromUtf8("N/A") : r.instrCount);
    instrItem->setTextAlignment(Qt::AlignCenter);

    // 失败行标红
    if (!r.success) {
        QBrush redBrush(QColor(0xC0, 0x39, 0x2B));
        avgItem->setForeground(redBrush);
        minItem->setForeground(redBrush);
        maxItem->setForeground(redBrush);
        avgItem->setText(r.status);
        minItem->setText(QString::fromUtf8("—"));
        maxItem->setText(QString::fromUtf8("—"));
    }

    resultTable_->setItem(row, 0, nameItem);
    resultTable_->setItem(row, 1, avgItem);
    resultTable_->setItem(row, 2, minItem);
    resultTable_->setItem(row, 3, maxItem);
    resultTable_->setItem(row, 4, instrItem);
}

/// 渲染柱状图：用 HTML/CSS（嵌套 table + bgcolor）绘制水平柱状图，
/// 每个后端一个柱子，长度按平均耗时比例（相对最慢后端归一化），
/// 颜色 Interpreter 蓝 / StackVM 绿 / RegisterVM 橙。
void PerformanceRacePanel::renderBarChart(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                                          const BackendPerfResult& regvm) {
    const BackendPerfResult* backends[3] = {&interp, &stackvm, &regvm};

    // 找最大平均耗时作为归一化基准（仅成功后端参与）
    double maxAvg = 0.0;
    int successCount = 0;
    for (int i = 0; i < 3; ++i) {
        if (backends[i]->success && backends[i]->avgMs > maxAvg) {
            maxAvg = backends[i]->avgMs;
        }
        if (backends[i]->success) {
            ++successCount;
        }
    }

    QString html =
        QStringLiteral("<html><body style='font-family:\"Consolas\",\"Microsoft YaHei\",monospace;padding:4px;'>"
                       "<h3 style='margin:0 0 8px 0;color:#2C3E50;'>三后端平均耗时对比（柱状图）</h3>");

    if (successCount == 0 || maxAvg <= 0.0) {
        html += QStringLiteral("<p style='color:#C0392B;'>所有后端均失败或无有效耗时数据，无法绘制柱状图。</p>");
        html += QStringLiteral("</body></html>");
        barChartView_->setHtml(html);
        return;
    }

    html += QStringLiteral("<table cellspacing='0' cellpadding='3' style='width:100%;font-family:monospace;'>");

    for (int i = 0; i < 3; ++i) {
        const BackendPerfResult* b = backends[i];
        QString color = backendColor(i);

        // 柱子宽度百分比（相对最慢后端归一化，最少 5% 保证可见）
        int widthPct = 5;
        QString valueText;
        if (b->success && b->avgMs > 0.0) {
            widthPct = static_cast<int>((b->avgMs / maxAvg) * 100.0);
            if (widthPct < 5)
                widthPct = 5;
            if (widthPct > 100)
                widthPct = 100;
            valueText = formatMs(b->avgMs);
        } else {
            widthPct = 5;
            valueText = QString::fromUtf8("失败");
        }
        int restPct = 100 - widthPct;

        html += QStringLiteral("<tr>"
                               "<td width='20%' style='font-weight:bold;color:%1;'>%2</td>"
                               "<td width='65%'>"
                               "<table cellspacing='0' cellpadding='0' width='100%' style='height:22px;'>"
                               "<tr>"
                               "<td bgcolor='%3' width='%4%' height='22'></td>"
                               "<td width='%5%' height='22'></td>"
                               "</tr>"
                               "</table>"
                               "</td>"
                               "<td width='15%' align='right' style='color:%6;'>%7</td>"
                               "</tr>")
                    .arg(color)
                    .arg(b->backendName)
                    .arg(color)
                    .arg(widthPct)
                    .arg(restPct)
                    .arg(b->success ? QStringLiteral("#555555") : QStringLiteral("#C0392B"))
                    .arg(valueText.toHtmlEscaped());
    }

    html += QStringLiteral("</table>");

    // 图例
    html += QStringLiteral("<p style='margin-top:10px;color:#6E6E6E;font-size:11px;'>"
                           "图例：<span style='color:%1;'>■</span> Interpreter "
                           "（树遍历） | <span style='color:%2;'>■</span> StackVM "
                           "（栈式字节码） | <span style='color:%3;'>■</span> RegisterVM（寄存器式）"
                           "</p>")
                .arg(backendColor(0))
                .arg(backendColor(1))
                .arg(backendColor(2));

    // 运行次数提示（取三后端中最大成功运行次数，避免某个后端失败时显示 0）
    size_t maxRuns = std::max({interp.runs.size(), stackvm.runs.size(), regvm.runs.size()});
    html += QStringLiteral("<p style='color:#6E6E6E;font-size:11px;'>注：柱长按平均耗时相对最慢后端归一化，"
                           "每个后端运行次数 = %1，仅成功后端参与归一化。</p>")
                .arg(static_cast<int>(maxRuns));

    html += QStringLiteral("</body></html>");
    barChartView_->setHtml(html);
}

/// 渲染性能分析报告：最快后端 / 加速比 / 性能差异原因 / 指令数对比。
void PerformanceRacePanel::renderAnalysis(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                                          const BackendPerfResult& regvm) {
    const BackendPerfResult* backends[3] = {&interp, &stackvm, &regvm};

    QString html =
        QStringLiteral("<html><body style='font-family:\"Consolas\",\"Microsoft YaHei\",monospace;padding:4px;'>"
                       "<h3 style='margin:0 0 8px 0;color:#2C3E50;'>性能分析报告</h3>");

    // 统计成功后端（fastest/slowest 由此循环得出，无需单独计数）
    const BackendPerfResult* fastest = nullptr;
    const BackendPerfResult* slowest = nullptr;
    for (int i = 0; i < 3; ++i) {
        if (!backends[i]->success)
            continue;
        if (!fastest || backends[i]->avgMs < fastest->avgMs)
            fastest = backends[i];
        if (!slowest || backends[i]->avgMs > slowest->avgMs)
            slowest = backends[i];
    }

    // 1. 最快后端
    html += QStringLiteral("<h4 style='color:#27AE60;margin:8px 0 4px 0;'>1. 最快后端</h4>");
    if (fastest) {
        html += QStringLiteral("<p>%1 以 <b>%2</b> 平均耗时胜出。</p>")
                    .arg(fastest->backendName)
                    .arg(formatMs(fastest->avgMs));
    } else {
        html += QStringLiteral("<p style='color:#C0392B;'>所有后端均失败，无最快后端。</p>");
    }

    // 2. 加速比
    html += QStringLiteral("<h4 style='color:#2980B9;margin:8px 0 4px 0;'>2. 加速比</h4>");
    if (fastest && slowest && fastest != slowest && fastest->avgMs > 0.0) {
        double speedup = slowest->avgMs / fastest->avgMs;
        html += QStringLiteral("<p>最快 vs 最慢：<b>%1</b>（%2） vs <b>%3</b>（%4），"
                               "加速比 <b>%5x</b>（最慢 / 最快）。</p>")
                    .arg(fastest->backendName)
                    .arg(formatMs(fastest->avgMs))
                    .arg(slowest->backendName)
                    .arg(formatMs(slowest->avgMs))
                    .arg(QString::number(speedup, 'f', 2));
    } else if (fastest && slowest && fastest == slowest) {
        html += QStringLiteral("<p>仅一个后端成功（%1），无法计算加速比。</p>").arg(fastest->backendName);
    } else {
        html += QStringLiteral("<p style='color:#C0392B;'>数据不足，无法计算加速比。</p>");
    }

    // 3. 性能差异原因分析
    html += QStringLiteral("<h4 style='color:#8E44AD;margin:8px 0 4px 0;'>3. 性能差异原因分析</h4>");
    html += QStringLiteral("<ul style='margin:4px 0;'>");
    html += QStringLiteral("<li><b>Interpreter（树遍历解释器）</b>：直接遍历 AST 节点，"
                           "每个节点需虚函数分发与指针解引用，无编译期优化，"
                           "循环与递归场景下重复解释开销显著，通常最慢。</li>");
    html += QStringLiteral("<li><b>StackVM（栈式字节码 VM）</b>：编译为紧凑字节码后由 VM 解释执行，"
                           "操作数栈模型简单但每条指令需 push/pop，"
                           "调用约定通过栈传参，中等性能。</li>");
    html += QStringLiteral("<li><b>RegisterVM（寄存器式 VM）</b>：寄存器式字节码减少指令数与栈操作，"
                           "操作数直接通过虚拟寄存器（32 个）传递，"
                           "指令更密集、分发次数更少，通常最快。</li>");
    html += QStringLiteral("</ul>");
    html += QStringLiteral("<p style='color:#6E6E6E;font-size:11px;'>注：递归 fib 场景下解释器开销主要来自 AST 重复遍历"
                           "与递归调用栈；循环累加场景下寄存器式 VM 的局部变量寄存器分配优势明显。</p>");

    // 4. 指令数对比
    html += QStringLiteral("<h4 style='color:#D35400;margin:8px 0 4px 0;'>4. 指令数对比</h4>");
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;font-family:monospace;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='40%'>后端</td>"
                           "<td width='30%'>指令数</td>"
                           "<td width='30%'>说明</td>"
                           "</tr>");
    html += QStringLiteral("<tr><td>Interpreter</td><td>%1</td><td>树遍历，无字节码</td></tr>")
                .arg(interp.instrCount.isEmpty() ? QStringLiteral("N/A") : interp.instrCount);
    html += QStringLiteral("<tr><td>StackVM (IR)</td><td>%1</td><td>栈式字节码字节数</td></tr>")
                .arg(stackvm.instrCount.isEmpty() ? QStringLiteral("N/A") : stackvm.instrCount);
    html += QStringLiteral("<tr><td>RegisterVM (IR)</td><td>%1</td><td>寄存器字节码字节数</td></tr>")
                .arg(regvm.instrCount.isEmpty() ? QStringLiteral("N/A") : regvm.instrCount);
    html += QStringLiteral("</table>");
    html += QStringLiteral(
        "<p style='color:#6E6E6E;font-size:11px;'>注：寄存器式 VM 通常指令数更少（每条指令完成更多工作），"
        "但单条指令编码可能更长；指令数对比仅作参考，实际性能受分发开销、缓存命中率等多因素影响。</p>");

    // 5. 各次运行明细
    html += QStringLiteral("<h4 style='color:#16A085;margin:8px 0 4px 0;'>5. 各次运行耗时明细</h4>");
    for (int i = 0; i < 3; ++i) {
        const BackendPerfResult* b = backends[i];
        html += QStringLiteral("<p><b>%1</b>：").arg(b->backendName);
        if (b->runs.empty()) {
            html += QStringLiteral("<span style='color:#C0392B;'>无有效运行</span></p>");
        } else {
            for (size_t j = 0; j < b->runs.size(); ++j) {
                if (j > 0)
                    html += QStringLiteral(", ");
                html +=
                    QStringLiteral("#%1=%2 ms").arg(static_cast<int>(j + 1)).arg(QString::number(b->runs[j], 'f', 3));
            }
            html += QStringLiteral("</p>");
        }
    }

    html += QStringLiteral("</body></html>");
    analysisView_->setHtml(html);
}

/// 渲染一致性检查：比较三后端输出是否完全一致。
void PerformanceRacePanel::renderConsistency(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                                             const BackendPerfResult& regvm) {
    // 至少一个后端失败时给出部分一致性提示
    bool interpOk = interp.success;
    bool stackvmOk = stackvm.success;
    bool regvmOk = regvm.success;

    if (!interpOk || !stackvmOk || !regvmOk) {
        QString failed;
        if (!interpOk)
            failed += interp.backendName + QStringLiteral(" ");
        if (!stackvmOk)
            failed += stackvm.backendName + QStringLiteral(" ");
        if (!regvmOk)
            failed += regvm.backendName + QStringLiteral(" ");
        consistencyLabel_->setText(QString::fromUtf8("⚠ 部分后端失败：") + failed.trimmed() +
                                   QStringLiteral("，无法完整验证一致性"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#D35400;"));
        return;
    }

    // 三后端均成功，比较输出
    if (interp.output == stackvm.output && stackvm.output == regvm.output) {
        consistencyLabel_->setText(
            QString::fromUtf8("✅ 三后端输出完全一致（语义等价性验证通过）：") +
            (interp.output.isEmpty() ? QString::fromUtf8("（无输出）") : interp.output.trimmed().left(80)));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#27AE60;"));
    } else {
        consistencyLabel_->setText(QString::fromUtf8("❌ 三后端输出不一致（可能存在语义 bug）——") +
                                   QString::fromUtf8("Interpreter: 「") + interp.output.trimmed().left(40) +
                                   QString::fromUtf8("」| StackVM: 「") + stackvm.output.trimmed().left(40) +
                                   QString::fromUtf8("」| RegisterVM: 「") + regvm.output.trimmed().left(40) +
                                   QString::fromUtf8("」"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B;"));
    }
}

/// 加载当前源码输入框内容到主编辑器（发出 loadSampleRequested 信号）
void PerformanceRacePanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->text());
}

/// 选择内置样例：填充源码输入框
void PerformanceRacePanel::onSelectSample(int idx) {
    const auto& s = PerformanceRaceLibrary::samples();
    if (idx < 0 || idx >= static_cast<int>(s.size()))
        return;
    sourceEdit_->setText(QString::fromUtf8(s[static_cast<size_t>(idx)].code.c_str()));
    sourceEdit_->setFocus();
}
