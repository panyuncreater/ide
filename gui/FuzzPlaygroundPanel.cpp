// ============================================================
// FuzzPlaygroundPanel.cpp — 模糊测试游乐场教学面板实现
// ------------------------------------------------------------
// 复用 cli/fuzz_core.h 的公开 API：
//   - fuzzThreeAgree(source)：单次三后端差分（Interpreter/StackVM/RegisterVM）
//   - runFuzzBatch(opts)：批量生成/变异执行 + 汇总（崩溃/分歧用例）
//   - formatSummaryText(summary)：文本格式汇总（用于统计报告）
//
// 单次模式：sourceEdit → fuzzThreeAgree → 渲染 3 行表格 + 详情
// 批量模式：opts(seed/iter/mode) → runFuzzBatch → 渲染 1 行汇总
//           + 分歧用例表（最多 20 个）+ 详情
// 三后端在主线程串行执行（教学面板可接受阻塞调用，避免线程复杂化）。
// ============================================================

#include "gui/FuzzPlaygroundPanel.h"
#include "gui/GuidedTour.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

#include "gui/GuiTextUtils.h"
#include "gui/I18n.h"

// ============================================================
// 内置示例库（5 个）
// ============================================================

const std::vector<FuzzSample>& FuzzPlaygroundPanel::samples() {
    static const std::vector<FuzzSample> kSamples = {
        {"基本算术", "var a = 1 + 2;\nvar b = a * 3;\nprint(b);\n", "基本整数算术运算"},
        {"字符串拼接", "var s = \"hello\" + \" \" + \"world\";\nprint(s);\n", "字符串拼接"},
        {"递归fibonacci",
         "fun fib(n: int) {\n    if (n < 2) { return n; }\n    return fib(n-1) + fib(n-2);\n}\nprint(fib(10));\n",
         "递归函数调用"},
        {"数组操作", "var arr = [1, 2, 3];\narr.push(4);\nprint(arr.len());\nprint(arr[0]);\n",
         "数组push/index/length"},
        {"类与继承",
         "class Animal {\n    var name: string;\n    fun speak() { return \"...\"; }\n}\nclass Dog : Animal "
         "{\n    fun speak() { return \"Woof\"; }\n}\nvar d = Dog();\nprint(d.speak());\n",
         "类字段/方法/继承/super"},
    };
    return kSamples;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 三后端颜色（Interpreter 蓝 / StackVM 绿 / RegisterVM 橙）
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

/// 判断输出是否为解析失败（<parse-fail>）
bool isParseFailure(const std::string& s) {
    return s == "<parse-fail>";
}

/// 判断输出是否为编译失败（包含 <compile: 标记）
bool isCompileFailure(const std::string& s) {
    return s.find("<compile:") != std::string::npos;
}

/// 判断输出是否为运行时错误（包含 <runtime: 标记）
bool isRuntimeError(const std::string& s) {
    return s.find("<runtime:") != std::string::npos;
}

} // anonymous namespace

// ============================================================
// FuzzPlaygroundPanel 实现
// ============================================================

/// 构造面板：顶部源码编辑器 + 控制板，中间 QTabWidget + 详情视图，
/// 底部一致性标签 + 加载到主编辑器按钮。预填示例代码。
FuzzPlaygroundPanel::FuzzPlaygroundPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // ==== 顶部：源码编辑器（多行）====
    sourceEdit_ = new QTextEdit;
    sourceEdit_->setFont(GuiTextUtils::monospaceFont(10));
    sourceEdit_->setPlaceholderText(mlTr("输入 MiniLang 源码后点击运行"));
    sourceEdit_->setMinimumHeight(80);
    sourceEdit_->setMaximumHeight(160);
    outer->addWidget(sourceEdit_);

    // ==== 顶部：模式切换 + 运行 + 种子 + 迭代次数 + 加载样例 ====
    auto* ctrlBar = new QHBoxLayout;
    ctrlBar->addWidget(new QLabel(mlTr("模式：")));
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(QString::fromUtf8("单次差分"));
    modeCombo_->addItem(QString::fromUtf8("批量生成"));
    modeCombo_->addItem(QString::fromUtf8("批量变异"));
    modeCombo_->setToolTip(mlTr("单次差分：对当前源码立即三后端比对；批量生成/变异：基于种子生成 N 段程序后统计"));
    ctrlBar->addWidget(modeCombo_);

    ctrlBar->addWidget(new QLabel(mlTr("种子：")));
    seedSpin_ = new QSpinBox;
    seedSpin_->setRange(0, 999999);
    seedSpin_->setValue(42);
    seedSpin_->setToolTip(mlTr("随机种子（0 = 时间派生），批量模式推荐固定值以复现）"));
    ctrlBar->addWidget(seedSpin_);

    ctrlBar->addWidget(new QLabel(mlTr("迭代次数：")));
    iterSpin_ = new QSpinBox;
    iterSpin_->setRange(1, 200);
    iterSpin_->setValue(20);
    iterSpin_->setEnabled(false); // 单次模式下禁用
    iterSpin_->setToolTip(mlTr("批量模式迭代次数（1-200），值越大越可能发现问题但 UI 卡顿越久"));
    ctrlBar->addWidget(iterSpin_);

    runBtn_ = new QPushButton(mlTr("运行"));
    ctrlBar->addWidget(runBtn_);

    loadSampleBtn_ = new QPushButton(mlTr("加载样例"));
    loadSampleBtn_->setToolTip(mlTr("循环加载下一个内置样例（共 5 个）"));
    ctrlBar->addWidget(loadSampleBtn_);

    ctrlBar->addStretch();
    outer->addLayout(ctrlBar);

    // ==== 中间：QSplitter(Vertical) ====
    auto* splitter = new QSplitter(Qt::Vertical);

    // 上方：QTabWidget（3 个标签页）
    tabWidget_ = new QTabWidget;

    // Tab1 差分结果
    diffTable_ = new QTableWidget(0, 4);
    diffTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diffTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    diffTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    diffTable_->verticalHeader()->setVisible(false);
    diffTable_->horizontalHeader()->setStretchLastSection(true);
    setupDiffTableForSingle();
    tabWidget_->addTab(diffTable_, mlTr("差分结果"));

    // Tab2 分歧详情
    disagreeTable_ = new QTableWidget(0, 3);
    disagreeTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    disagreeTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    disagreeTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    disagreeTable_->verticalHeader()->setVisible(false);
    disagreeTable_->horizontalHeader()->setStretchLastSection(true);
    setupDisagreeTable();
    tabWidget_->addTab(disagreeTable_, mlTr("分歧详情"));

    // Tab3 统计报告
    reportView_ = new QTextBrowser;
    reportView_->setOpenExternalLinks(false);
    reportView_->setPlaceholderText(mlTr("点击「运行」后生成统计报告（差分摘要 + 错误归一化说明 + 教学解释）"));
    tabWidget_->addTab(reportView_, mlTr("统计报告"));

    splitter->addWidget(tabWidget_);

    // 下方：选中项详情
    detailView_ = new QTextBrowser;
    detailView_->setOpenExternalLinks(false);
    detailView_->setFont(GuiTextUtils::monospaceFont(10));
    detailView_->setPlaceholderText(
        mlTr("选中差分结果行（单次模式）或分歧行（批量模式）后展示完整源码+三后端输出对比"));
    splitter->addWidget(detailView_);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    // ==== 底部：一致性提示 + 加载到主编辑器 ====
    consistencyLabel_ = new QLabel(QString::fromUtf8("（点击「运行」后自动检查三后端一致性）"));
    consistencyLabel_->setWordWrap(true);
    outer->addWidget(consistencyLabel_);

    auto* bottomBar = new QHBoxLayout;
    loadToMainBtn_ = new QPushButton(mlTr("加载源码到主编辑器"));
    bottomBar->addWidget(loadToMainBtn_);
    bottomBar->addStretch();
    auto* hintLabel = new QLabel(QString::fromUtf8("三后端输出应完全一致（语义等价性验证）"));
    hintLabel->setStyleSheet(QStringLiteral("color:#6E6E6E;"));
    bottomBar->addWidget(hintLabel);
    outer->addLayout(bottomBar);

    // ==== 信号连接 ====
    connect(runBtn_, &QPushButton::clicked, this, &FuzzPlaygroundPanel::onRun);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &FuzzPlaygroundPanel::onLoadNextSample);
    connect(loadToMainBtn_, &QPushButton::clicked, this, &FuzzPlaygroundPanel::onLoadSampleToMain);
    connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FuzzPlaygroundPanel::onModeChanged);
    connect(diffTable_, &QTableWidget::cellClicked, this, &FuzzPlaygroundPanel::onDiffRowSelected);
    connect(disagreeTable_, &QTableWidget::cellClicked, this, &FuzzPlaygroundPanel::onDisagreeRowSelected);

    // 预填示例代码
    sourceEdit_->setPlainText(QString::fromUtf8(samples()[0].code.c_str()));
}

/// 运行：根据模式分发到单次差分或批量生成/变异。
void FuzzPlaygroundPanel::onRun() {
    QString mode = modeCombo_->currentText();
    if (mode == QString::fromUtf8("单次差分")) {
        std::string src = sourceEdit_->toPlainText().toStdString();
        if (src.empty()) {
            consistencyLabel_->setText(QString::fromUtf8("❌ 源码为空，请输入 MiniLang 源码"));
            consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B;"));
            return;
        }
        // 单次差分：阻塞调用 fuzzThreeAgree
        minilang_fuzz::FuzzResult r = minilang_fuzz::fuzzThreeAgree(src);
        lastSingleResult_ = r;
        hasSingleResult_ = true;

        setupDiffTableForSingle();
        renderSingleMode(r);
        renderReportSingle(r);
        renderConsistencySingle(r);
        clearDetail();
        tabWidget_->setCurrentIndex(0); // 切换到差分结果 Tab
    } else {
        // 批量生成 / 批量变异
        minilang_fuzz::FuzzOptions opts;
        opts.seed = static_cast<uint64_t>(seedSpin_->value());
        opts.iterations = iterSpin_->value();
        opts.mode = (mode == QString::fromUtf8("批量变异")) ? minilang_fuzz::FuzzMode::Mutate
                                                            : minilang_fuzz::FuzzMode::Generate;
        opts.quiet = true;
        opts.dumpCrashes = false;

        // 批量执行（阻塞调用，可能耗时）
        minilang_fuzz::FuzzSummary s = minilang_fuzz::runFuzzBatch(opts);
        lastBatchSummary_ = s;
        hasBatchResult_ = true;

        setupDiffTableForBatch();
        renderBatchMode(s, mode);
        renderDisagreeTable(s);
        renderReportBatch(s, mode);
        renderConsistencyBatch(s);
        clearDetail();
        // 有分歧用例时切换到分歧详情 Tab，否则停留在差分结果 Tab
        tabWidget_->setCurrentIndex(s.disagreementCases.empty() ? 0 : 1);
    }
}

/// 模式切换：单次差分时禁用迭代次数 SpinBox。
void FuzzPlaygroundPanel::onModeChanged(int idx) {
    // idx 0 = 单次差分（禁用迭代），1/2 = 批量模式（启用迭代）
    iterSpin_->setEnabled(idx != 0);
}

/// 循环加载下一个内置样例到源码编辑器。
void FuzzPlaygroundPanel::onLoadNextSample() {
    const auto& s = samples();
    if (s.empty())
        return;
    currentSampleIdx_ = (currentSampleIdx_ + 1) % static_cast<int>(s.size());
    sourceEdit_->setPlainText(QString::fromUtf8(s[static_cast<size_t>(currentSampleIdx_)].code.c_str()));
}

/// 将当前源码编辑器内容 emit loadSampleRequested，加载到主编辑器运行观测。
void FuzzPlaygroundPanel::onLoadSampleToMain() {
    emit loadSampleRequested(sourceEdit_->toPlainText());
}

/// 差分结果表行选中：单次模式下展示完整源码+三后端输出对比详情。
void FuzzPlaygroundPanel::onDiffRowSelected(int row, int col) {
    Q_UNUSED(row);
    Q_UNUSED(col);
    if (!isBatchMode() && hasSingleResult_) {
        showSingleDetail();
    }
}

/// 分歧详情表行选中：批量模式下展示该分歧用例的源码+三后端输出对比详情。
void FuzzPlaygroundPanel::onDisagreeRowSelected(int row, int col) {
    Q_UNUSED(col);
    if (row < 0)
        return;
    showBatchDisagreeDetail(row);
}

// ============================================================
// 表格设置（列数与表头切换）
// ============================================================

/// 设置差分结果表为单次模式（3 行 4 列：后端/输出/状态/耗时）。
void FuzzPlaygroundPanel::setupDiffTableForSingle() {
    diffTable_->clear();
    diffTable_->setColumnCount(4);
    diffTable_->setRowCount(3);
    diffTable_->setHorizontalHeaderLabels({mlTr("后端"), mlTr("输出"), mlTr("状态"), mlTr("耗时(ms)")});
    diffTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    diffTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    diffTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    diffTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    QStringList backendNames = {QString::fromUtf8("Interpreter"), QString::fromUtf8("StackVM"),
                                QString::fromUtf8("RegisterVM")};
    for (int i = 0; i < 3; ++i) {
        auto* nameItem = new QTableWidgetItem(backendNames[i]);
        nameItem->setTextAlignment(Qt::AlignCenter);
        nameItem->setForeground(QColor(backendColor(i)));
        diffTable_->setItem(i, 0, nameItem);
        diffTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8("（待运行）")));
        diffTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8("待运行")));
        diffTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8("—")));
    }
}

/// 设置差分结果表为批量模式（1 行 6 列：总次数/一致/分歧/崩溃/解析失败/运行时错误）。
void FuzzPlaygroundPanel::setupDiffTableForBatch() {
    diffTable_->clear();
    diffTable_->setColumnCount(6);
    diffTable_->setRowCount(1);
    diffTable_->setHorizontalHeaderLabels(
        {mlTr("总次数"), mlTr("一致"), mlTr("分歧"), mlTr("崩溃"), mlTr("解析失败"), mlTr("运行时错误")});
    for (int i = 0; i < 6; ++i) {
        diffTable_->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Stretch);
    }
    // 占位数据（renderBatchMode 会覆盖）
    for (int i = 0; i < 6; ++i) {
        diffTable_->setItem(0, i, new QTableWidgetItem(QString::fromUtf8("—")));
    }
}

/// 设置分歧详情表（3 列：序号/源码片段/分歧类型），清空原有数据。
void FuzzPlaygroundPanel::setupDisagreeTable() {
    disagreeTable_->clear();
    disagreeTable_->setColumnCount(3);
    disagreeTable_->setRowCount(0);
    disagreeTable_->setHorizontalHeaderLabels({mlTr("序号"), mlTr("源码片段"), mlTr("分歧类型")});
    disagreeTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    disagreeTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    disagreeTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
}

// ============================================================
// 渲染
// ============================================================

/// 渲染单次模式差分结果表：3 行 Interpreter/StackVM/RegisterVM 的输出/状态/耗时。
/// durationMs 为三后端串行总耗时（仅显示在第 1 行，其余为"—"）。
void FuzzPlaygroundPanel::renderSingleMode(const minilang_fuzz::FuzzResult& r) {
    struct RowInfo {
        QString name;
        std::string output;
    };
    RowInfo rows[3] = {{QString::fromUtf8("Interpreter"), r.interpOutput},
                       {QString::fromUtf8("StackVM"), r.stackvmOutput},
                       {QString::fromUtf8("RegisterVM"), r.regvmOutput}};

    for (int i = 0; i < 3; ++i) {
        // 后端名
        auto* nameItem = new QTableWidgetItem(rows[i].name);
        nameItem->setTextAlignment(Qt::AlignCenter);
        nameItem->setForeground(QColor(backendColor(i)));
        diffTable_->setItem(i, 0, nameItem);

        // 输出（HTML 转义 + 单行化）
        auto* outItem = new QTableWidgetItem(summarizeOutput(rows[i].output, 200));
        diffTable_->setItem(i, 1, outItem);

        // 状态
        QString status = statusFromOutput(rows[i].output);
        auto* statusItem = new QTableWidgetItem(status);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (status.startsWith(QString::fromUtf8("✅"))) {
            statusItem->setForeground(QColor(QStringLiteral("#27AE60")));
        } else {
            statusItem->setForeground(QColor(QStringLiteral("#C0392B")));
        }
        diffTable_->setItem(i, 2, statusItem);

        // 耗时（仅第 1 行显示总耗时，其余为"—"）
        QString timeText = (i == 0) ? QString::number(static_cast<qint64>(r.durationMs)) : QString::fromUtf8("—");
        auto* timeItem = new QTableWidgetItem(timeText);
        timeItem->setTextAlignment(Qt::AlignCenter);
        if (i == 0) {
            timeItem->setToolTip(mlTr("三后端串行总耗时（fuzzThreeAgree 内部计时）"));
        }
        diffTable_->setItem(i, 3, timeItem);
    }
}

/// 渲染批量模式差分结果表：1 行汇总（总次数/一致/分歧/崩溃/解析失败/运行时错误）。
void FuzzPlaygroundPanel::renderBatchMode(const minilang_fuzz::FuzzSummary& s, const QString& modeName) {
    Q_UNUSED(modeName);
    auto setCell = [this](int col, int value, const QColor& color) {
        auto* item = new QTableWidgetItem(QString::number(value));
        item->setTextAlignment(Qt::AlignCenter);
        item->setFont(GuiTextUtils::monospaceFont(10, true));
        item->setForeground(color);
        diffTable_->setItem(0, col, item);
    };

    setCell(0, s.totalRuns, QColor(QStringLiteral("#2C3E50")));
    setCell(1, s.agreements, QColor(QStringLiteral("#27AE60")));
    setCell(2, s.disagreements,
            s.disagreements > 0 ? QColor(QStringLiteral("#C0392B")) : QColor(QStringLiteral("#27AE60")));
    setCell(3, s.crashes, s.crashes > 0 ? QColor(QStringLiteral("#C0392B")) : QColor(QStringLiteral("#27AE60")));
    setCell(4, s.parseFailures, QColor(QStringLiteral("#D35400")));
    setCell(5, s.runtimeErrors, QColor(QStringLiteral("#2980B9")));
}

/// 渲染分歧详情表：从 s.disagreementCases 取前 20 个，3 列（序号/源码片段/分歧类型）。
void FuzzPlaygroundPanel::renderDisagreeTable(const minilang_fuzz::FuzzSummary& s) {
    disagreeTable_->setRowCount(0);
    int shown = std::min<int>(20, static_cast<int>(s.disagreementCases.size()));
    if (shown <= 0) {
        disagreeTable_->setRowCount(1);
        auto* emptyItem = new QTableWidgetItem(QString::fromUtf8("（无分歧用例）"));
        emptyItem->setTextAlignment(Qt::AlignCenter);
        emptyItem->setForeground(QColor(QStringLiteral("#6E6E6E")));
        disagreeTable_->setItem(0, 0, emptyItem);
        disagreeTable_->setSpan(0, 0, 1, 3);
        return;
    }
    disagreeTable_->setRowCount(shown);
    for (int i = 0; i < shown; ++i) {
        const auto& d = s.disagreementCases[static_cast<size_t>(i)];

        auto* idxItem = new QTableWidgetItem(QString::number(i + 1));
        idxItem->setTextAlignment(Qt::AlignCenter);

        // 源码片段：取前 80 字符，换行替换为空格
        QString snippet = escapeHtml(d.source);
        snippet.replace(QLatin1Char('\n'), QStringLiteral(" "));
        snippet.replace(QLatin1Char('\r'), QString());
        if (snippet.length() > 80)
            snippet = snippet.left(80) + QStringLiteral("…");
        auto* srcItem = new QTableWidgetItem(snippet);

        auto* typeItem = new QTableWidgetItem(escapeHtml(d.errorMessage));
        typeItem->setTextAlignment(Qt::AlignCenter);
        typeItem->setForeground(QColor(QStringLiteral("#C0392B")));

        disagreeTable_->setItem(i, 0, idxItem);
        disagreeTable_->setItem(i, 1, srcItem);
        disagreeTable_->setItem(i, 2, typeItem);
    }
}

/// 渲染单次模式统计报告 HTML：差分摘要 + 错误归一化说明 + 教学解释。
void FuzzPlaygroundPanel::renderReportSingle(const minilang_fuzz::FuzzResult& r) {
    QString html =
        QStringLiteral("<html><body style='font-family:%1;padding:4px;'>").arg(GuiTextUtils::uiFontFamilyQss());

    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:#2C3E50;'>单次差分摘要</h3>");
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='30%'>项</td>"
                           "<td width='70%'>值</td>"
                           "</tr>");
    html += QStringLiteral("<tr><td>源码长度</td><td>%1 字节</td></tr>").arg(static_cast<int>(r.source.size()));
    html +=
        QStringLiteral("<tr><td>总耗时</td><td>%1 ms（三后端串行）</td></tr>").arg(static_cast<qint64>(r.durationMs));
    html += QStringLiteral("<tr><td>崩溃检测</td><td>%1</td></tr>")
                .arg(r.crashDetected ? QString::fromUtf8("<span style='color:#C0392B;'>是</span>")
                                     : QString::fromUtf8("<span style='color:#27AE60;'>否</span>"));
    html += QStringLiteral("<tr><td>三后端分歧</td><td>%1</td></tr>")
                .arg(r.disagreement ? QString::fromUtf8("<span style='color:#C0392B;'>是：") +
                                          escapeHtml(r.errorMessage) + QStringLiteral("</span>")
                                    : QString::fromUtf8("<span style='color:#27AE60;'>否</span>"));
    html += QStringLiteral("<tr><td>错误阶段</td><td>%1</td></tr>")
                .arg(r.errorPhase.empty() ? QString::fromUtf8("（无错误）") : escapeHtml(r.errorPhase));
    html += QStringLiteral("</table>");

    // 三后端输出对比
    html += QStringLiteral("<h4 style='color:#2980B9;margin:12px 0 4px 0;'>三后端输出对比</h4>");
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='20%'>后端</td>"
                           "<td width='80%'>输出（前 200 字符）</td>"
                           "</tr>");
    html += QStringLiteral("<tr><td style='color:%1;'>Interpreter</td>"
                           "<td><pre style='margin:0;white-space:pre-wrap;'>%2</pre></td></tr>")
                .arg(backendColor(0))
                .arg(summarizeOutput(r.interpOutput, 200));
    html += QStringLiteral("<tr><td style='color:%1;'>StackVM</td>"
                           "<td><pre style='margin:0;white-space:pre-wrap;'>%2</pre></td></tr>")
                .arg(backendColor(1))
                .arg(summarizeOutput(r.stackvmOutput, 200));
    html += QStringLiteral("<tr><td style='color:%1;'>RegisterVM</td>"
                           "<td><pre style='margin:0;white-space:pre-wrap;'>%2</pre></td></tr>")
                .arg(backendColor(2))
                .arg(summarizeOutput(r.regvmOutput, 200));
    html += QStringLiteral("</table>");

    // 错误归一化说明（教学）
    html += QStringLiteral("<h4 style='color:#8E44AD;margin:12px 0 4px 0;'>错误阶段归一化说明</h4>");
    html += QStringLiteral("<p style='color:#6E6E6E;'>某些语义错误（如变量重定义）在 Interpreter 中是<b>运行时错误</b>"
                           "（runtimeError 抛异常），在 VM 中是<b>编译时错误</b>（Compiler::error 记录后阻止执行）。"
                           "二者错误消息相同但阶段不同，<b>不应计为三后端分歧</b>。"
                           "fuzz_core 内部用 <code>normalizeErrorPhase()</code> 将 <code>&lt;compile:msg&gt;</code> 与 "
                           "<code>&lt;runtime:msg&gt;</code> 统一为 <code>&lt;error:msg&gt;</code>，"
                           "同时去掉编译器诊断前缀 <code>[编译器] 错误 (行 N):</code>，"
                           "使同一语义错误在不同阶段不被计为分歧。</p>");
    html += QStringLiteral(
                "<p style='font-family:%1;background:#F8F9FA;padding:6px;border-left:3px solid #8E44AD;'>"
                "示例：<br>"
                "• Interpreter 运行时错误：<code>&lt;runtime:变量 'b' 已在当前作用域中定义&gt;</code><br>"
                "• VM 编译时错误：<code>&lt;compile:[编译器] 错误 (行 1): 变量 'b' 已在当前作用域中定义&gt;</code><br>"
                "• 归一化后：<code>&lt;error:变量 'b' 已在当前作用域中定义&gt;</code><br>"
                "二者阶段不同但语义相同，不计为分歧。"
                "</p>")
                .arg(GuiTextUtils::monoFontFamilyQss());

    // 教学解释
    html += QStringLiteral("<h4 style='color:#16A085;margin:12px 0 4px 0;'>教学解释</h4>");
    html +=
        QStringLiteral("<ul style='margin:4px 0;'>"
                       "<li><b>三后端差分测试</b>：对同一份源码分别用 Interpreter / StackVM / RegisterVM "
                       "执行，比较输出是否完全一致。任何不一致都暗示某个后端存在语义 bug。</li>"
                       "<li><b>错误标记格式</b>：<code>&lt;parse-fail&gt;</code>（解析失败）、"
                       "<code>&lt;compile:msg&gt;</code>（编译错误）、<code>&lt;runtime:msg&gt;</code>（运行时错误）。"
                       "正常输出为 print 内容原文。</li>"
                       "<li><b>本面板与 BackendParallelPanel 的区别</b>：后者手动调用三后端执行并展示"
                       "执行轨迹；本面板直接复用 fuzz_core 的差分逻辑（含错误归一化），更贴近真实模糊测试流程。</li>"
                       "</ul>");

    html += QStringLiteral("</body></html>");
    reportView_->setHtml(html);
}

/// 渲染批量模式统计报告 HTML：汇总数据 + formatSummaryText 输出 + 错误归一化说明 + 教学解释。
void FuzzPlaygroundPanel::renderReportBatch(const minilang_fuzz::FuzzSummary& s, const QString& modeName) {
    QString html =
        QStringLiteral("<html><body style='font-family:%1;padding:4px;'>").arg(GuiTextUtils::uiFontFamilyQss());

    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:#2C3E50;'>批量%1统计报告</h3>").arg(modeName);
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='30%'>项</td>"
                           "<td width='70%'>值</td>"
                           "</tr>");
    html += QStringLiteral("<tr><td>种子</td><td>0x%1</td></tr>").arg(static_cast<qulonglong>(s.seed), 0, 16);
    html += QStringLiteral("<tr><td>总执行次数</td><td>%1</td></tr>").arg(s.totalRuns);
    html += QStringLiteral("<tr><td>三后端一致</td><td><span style='color:#27AE60;font-weight:bold;'>%1</span> "
                           "(%2%)</td></tr>")
                .arg(s.agreements)
                .arg(s.totalRuns > 0 ? (s.agreements * 100 / s.totalRuns) : 0);
    html += QStringLiteral("<tr><td>三后端分歧</td><td><span style='color:%1;font-weight:bold;'>%2</span> "
                           "(%3%)</td></tr>")
                .arg(s.disagreements > 0 ? QStringLiteral("#C0392B") : QStringLiteral("#27AE60"))
                .arg(s.disagreements)
                .arg(s.totalRuns > 0 ? (s.disagreements * 100 / s.totalRuns) : 0);
    html += QStringLiteral("<tr><td>崩溃</td><td><span style='color:%1;font-weight:bold;'>%2</span></td></tr>")
                .arg(s.crashes > 0 ? QStringLiteral("#C0392B") : QStringLiteral("#27AE60"))
                .arg(s.crashes);
    html += QStringLiteral("<tr><td>解析失败（三后端一致）</td><td>%1</td></tr>").arg(s.parseFailures);
    html += QStringLiteral("<tr><td>运行时错误（三后端一致）</td><td>%1</td></tr>").arg(s.runtimeErrors);
    html += QStringLiteral("<tr><td>总耗时</td><td>%1 ms（平均 %2 ms/次）</td></tr>")
                .arg(static_cast<qint64>(s.totalDurationMs))
                .arg(s.totalRuns > 0 ? static_cast<qint64>(s.totalDurationMs / s.totalRuns) : 0);
    html += QStringLiteral("<tr><td>分歧用例（已记录/已展示）</td><td>%1 / %2</td></tr>")
                .arg(static_cast<int>(s.disagreementCases.size()))
                .arg(std::min<int>(20, static_cast<int>(s.disagreementCases.size())));
    html += QStringLiteral("</table>");

    // formatSummaryText 原文（教学：展示 CLI 工具的输出格式）
    html += QStringLiteral("<h4 style='color:#2980B9;margin:12px 0 4px 0;'>formatSummaryText 输出（CLI "
                           "兼容格式）</h4>");
    std::string summaryText = minilang_fuzz::formatSummaryText(s);
    html += QStringLiteral("<pre style='font-family:%1;background:#F8F9FA;padding:6px;"
                           "white-space:pre-wrap;border-left:3px solid #2980B9;'>%2</pre>")
                .arg(GuiTextUtils::monoFontFamilyQss())
                .arg(escapeHtml(summaryText));

    // 错误归一化说明（教学）
    html += QStringLiteral("<h4 style='color:#8E44AD;margin:12px 0 4px 0;'>错误阶段归一化说明</h4>");
    html += QStringLiteral("<p style='color:#6E6E6E;'>某些语义错误（如变量重定义）在 Interpreter "
                           "中是<b>运行时错误</b>"
                           "（runtimeError 抛异常），在 VM 中是<b>编译时错误</b>（Compiler::error 记录后阻止执行）。"
                           "二者错误消息相同但阶段不同，<b>不应计为三后端分歧</b>。"
                           "fuzz_core 内部用 <code>normalizeErrorPhase()</code> 将 <code>&lt;compile:msg&gt;</code> 与 "
                           "<code>&lt;runtime:msg&gt;</code> 统一为 <code>&lt;error:msg&gt;</code>，"
                           "同时去掉编译器诊断前缀 <code>[编译器] 错误 (行 N):</code>，"
                           "使同一语义错误在不同阶段不被计为分歧。</p>");
    html += QStringLiteral("<p style='font-family:%1;background:#F8F9FA;padding:6px;border-left:3px solid #8E44AD;'>"
                           "示例：<br>"
                           "• Interpreter 运行时错误：<code>&lt;runtime:变量 'b' "
                           "已在当前作用域中定义&gt;</code><br>"
                           "• VM 编译时错误：<code>&lt;compile:[编译器] 错误 (行 1): 变量 'b' "
                           "已在当前作用域中定义&gt;</code><br>"
                           "• 归一化后：<code>&lt;error:变量 'b' 已在当前作用域中定义&gt;</code><br>"
                           "二者阶段不同但语义相同，不计为分歧。"
                           "</p>")
                .arg(GuiTextUtils::monoFontFamilyQss());

    // 教学解释
    html += QStringLiteral("<h4 style='color:#16A085;margin:12px 0 4px 0;'>教学解释</h4>");
    html += QStringLiteral("<ul style='margin:4px 0;'>"
                           "<li><b>批量生成模式</b>：基于种子的模板化随机程序生成，覆盖算术/控制流/函数/递归/"
                           "闭包/字符串/数组/类/错误路径/逻辑等类别，验证三后端在多种语法结构下的语义等价性。</li>"
                           "<li><b>批量变异模式</b>：对种子语料做字节级变异（翻转/插入/删除/替换），"
                           "用于发现 Lexer/Parser 的边界 bug（如未转义字符、超长标识符、嵌套深度等）。</li>"
                           "<li><b>固定种子的意义</b>：相同种子 + 相同迭代次数 = 完全可复现的测试集，"
                           "便于回归验证与 bug 定位。种子 0 表示由时间派生（不可复现）。</li>"
                           "<li><b>分歧用例展示</b>：disagreementCases 最多记录 50 个，本面板 UI 仅展示前 20 个。"
                           "点击分歧行可在下方详情区查看完整源码与三后端输出对比。</li>"
                           "<li><b>已知限制</b>：批量执行在主线程串行运行（阻塞 UI），迭代次数上限 200 以避免卡页过久。"
                           "如需大规模模糊测试请使用 minilang-fuzz CLI 工具。</li>"
                           "</ul>");

    html += QStringLiteral("</body></html>");
    reportView_->setHtml(html);
}

/// 渲染单次模式一致性标签。
void FuzzPlaygroundPanel::renderConsistencySingle(const minilang_fuzz::FuzzResult& r) {
    if (r.crashDetected) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 检测到崩溃（未捕获异常/abort）"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    } else if (r.disagreement) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 发现 1 个分歧：") + QString::fromUtf8(r.errorMessage.c_str()));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    } else {
        consistencyLabel_->setText(QString::fromUtf8("✅ 三后端一致 1 次"));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#27AE60; font-weight:bold;"));
    }
}

/// 渲染批量模式一致性标签。
void FuzzPlaygroundPanel::renderConsistencyBatch(const minilang_fuzz::FuzzSummary& s) {
    if (s.crashes > 0) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 发现 %1 次崩溃 + %2 个分歧（共 %3 次）")
                                       .arg(s.crashes)
                                       .arg(s.disagreements)
                                       .arg(s.totalRuns));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    } else if (s.disagreements > 0) {
        consistencyLabel_->setText(QString::fromUtf8("❌ 发现 %1 个分歧（共 %2 次执行，一致 %3 次）")
                                       .arg(s.disagreements)
                                       .arg(s.totalRuns)
                                       .arg(s.agreements));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#C0392B; font-weight:bold;"));
    } else {
        consistencyLabel_->setText(
            QString::fromUtf8("✅ 三后端一致 %1 次（共 %2 次执行）").arg(s.agreements).arg(s.totalRuns));
        consistencyLabel_->setStyleSheet(QStringLiteral("color:#27AE60; font-weight:bold;"));
    }
}

// ============================================================
// 详情视图
// ============================================================

/// 展示单次模式详情：完整源码 + 三后端输出对比 HTML。
void FuzzPlaygroundPanel::showSingleDetail() {
    if (!hasSingleResult_)
        return;
    const auto& r = lastSingleResult_;
    QString source = escapeHtml(r.source);

    QString html =
        QStringLiteral("<html><body style='font-family:%1;padding:4px;'>").arg(GuiTextUtils::uiFontFamilyQss());
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:#2C3E50;'>源码</h3>");
    html += QStringLiteral("<pre style='font-family:%1;background:#F8F9FA;padding:6px;"
                           "white-space:pre-wrap;border-left:3px solid #2C3E50;'>%2</pre>")
                .arg(GuiTextUtils::monoFontFamilyQss())
                .arg(source);

    html += QStringLiteral("<h3 style='margin:12px 0 8px 0;color:#2C3E50;'>三后端输出对比</h3>");
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='20%'>后端</td>"
                           "<td width='60%'>输出</td>"
                           "<td width='20%'>状态</td>"
                           "</tr>");

    struct RowInfo {
        QString name;
        std::string output;
    };
    RowInfo rows[3] = {{QString::fromUtf8("Interpreter"), r.interpOutput},
                       {QString::fromUtf8("StackVM"), r.stackvmOutput},
                       {QString::fromUtf8("RegisterVM"), r.regvmOutput}};
    for (int i = 0; i < 3; ++i) {
        QString status = statusFromOutput(rows[i].output);
        QString statusColor =
            status.startsWith(QString::fromUtf8("✅")) ? QStringLiteral("#27AE60") : QStringLiteral("#C0392B");
        html += QStringLiteral("<tr>"
                               "<td style='color:%1;font-weight:bold;'>%2</td>"
                               "<td><pre style='margin:0;white-space:pre-wrap;font-family:%3;'>%4</pre></td>"
                               "<td style='color:%5;text-align:center;'>%6</td>"
                               "</tr>")
                    .arg(backendColor(i))
                    .arg(rows[i].name)
                    .arg(GuiTextUtils::monoFontFamilyQss())
                    .arg(escapeHtml(rows[i].output))
                    .arg(statusColor)
                    .arg(status);
    }
    html += QStringLiteral("</table>");

    if (r.disagreement || r.crashDetected) {
        html += QStringLiteral("<h4 style='color:#C0392B;margin:12px 0 4px 0;'>诊断</h4>");
        if (r.crashDetected) {
            html += QStringLiteral("<p style='color:#C0392B;'>⚠ 检测到崩溃（未捕获异常/abort）。</p>");
        }
        if (r.disagreement) {
            html +=
                QStringLiteral("<p style='color:#C0392B;'>⚠ 三后端输出不一致：%1</p>").arg(escapeHtml(r.errorMessage));
        }
        if (!r.errorPhase.empty()) {
            html += QStringLiteral("<p style='color:#D35400;'>错误阶段：%1</p>").arg(escapeHtml(r.errorPhase));
        }
    } else {
        html += QStringLiteral("<p style='color:#27AE60;margin:12px 0 0 0;'>✅ 三后端输出完全一致，"
                               "语义等价性验证通过。</p>");
    }

    html += QStringLiteral("</body></html>");
    detailView_->setHtml(html);
}

/// 展示批量模式分歧用例详情：该用例的完整源码 + 三后端输出对比 HTML。
void FuzzPlaygroundPanel::showBatchDisagreeDetail(int idx) {
    if (!hasBatchResult_)
        return;
    const auto& cases = lastBatchSummary_.disagreementCases;
    if (idx < 0 || idx >= static_cast<int>(cases.size()))
        return;
    const auto& d = cases[static_cast<size_t>(idx)];

    QString source = escapeHtml(d.source);
    QString html =
        QStringLiteral("<html><body style='font-family:%1;padding:4px;'>").arg(GuiTextUtils::uiFontFamilyQss());
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:#2C3E50;'>分歧用例 #%1</h3>").arg(idx + 1);
    html += QStringLiteral("<p style='color:#C0392B;'>分歧类型：%1</p>").arg(escapeHtml(d.errorMessage));

    html += QStringLiteral("<h4 style='margin:12px 0 4px 0;color:#2C3E50;'>源码</h4>");
    html += QStringLiteral("<pre style='font-family:%1;background:#F8F9FA;padding:6px;"
                           "white-space:pre-wrap;border-left:3px solid #C0392B;'>%2</pre>")
                .arg(GuiTextUtils::monoFontFamilyQss())
                .arg(source);

    html += QStringLiteral("<h4 style='margin:12px 0 4px 0;color:#2C3E50;'>三后端输出对比</h4>");
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' style='width:100%;'>"
                           "<tr style='background:#ECF0F1;font-weight:bold;'>"
                           "<td width='20%'>后端</td>"
                           "<td width='60%'>输出</td>"
                           "<td width='20%'>状态</td>"
                           "</tr>");

    struct RowInfo {
        QString name;
        std::string output;
    };
    RowInfo rows[3] = {{QString::fromUtf8("Interpreter"), d.interpOutput},
                       {QString::fromUtf8("StackVM"), d.stackvmOutput},
                       {QString::fromUtf8("RegisterVM"), d.regvmOutput}};
    for (int i = 0; i < 3; ++i) {
        QString status = statusFromOutput(rows[i].output);
        QString statusColor =
            status.startsWith(QString::fromUtf8("✅")) ? QStringLiteral("#27AE60") : QStringLiteral("#C0392B");
        html += QStringLiteral("<tr>"
                               "<td style='color:%1;font-weight:bold;'>%2</td>"
                               "<td><pre style='margin:0;white-space:pre-wrap;font-family:%3;'>%4</pre></td>"
                               "<td style='color:%5;text-align:center;'>%6</td>"
                               "</tr>")
                    .arg(backendColor(i))
                    .arg(rows[i].name)
                    .arg(GuiTextUtils::monoFontFamilyQss())
                    .arg(escapeHtml(rows[i].output))
                    .arg(statusColor)
                    .arg(status);
    }
    html += QStringLiteral("</table>");

    html += QStringLiteral("<p style='color:#6E6E6E;font-size:11px;margin-top:12px;'>"
                           "提示：fuzz_core 内部已用 normalizeErrorPhase() "
                           "归一化错误阶段后再比较，"
                           "若仍被标记为分歧，说明三后端在归一化后的输出仍存在真实差异，"
                           "可能指示某个后端的语义 bug，建议复制源码到主编辑器单步调试。"
                           "</p>");

    html += QStringLiteral("</body></html>");
    detailView_->setHtml(html);
}

/// 清空详情视图。
void FuzzPlaygroundPanel::clearDetail() {
    detailView_->setHtml(QString());
    detailView_->setPlaceholderText(
        mlTr("选中差分结果行（单次模式）或分歧行（批量模式）后展示完整源码+三后端输出对比"));
}

// ============================================================
// 辅助函数
// ============================================================

/// 当前是否为批量模式（非"单次差分"）。
bool FuzzPlaygroundPanel::isBatchMode() const {
    return modeCombo_->currentText() != QString::fromUtf8("单次差分");
}

/// 根据 fuzz 输出判断后端状态文本。
/// <parse-fail> → 解析失败，<compile:...> → 编译错误，<runtime:...> → 运行时错误；其他 → 成功。
QString FuzzPlaygroundPanel::statusFromOutput(const std::string& output) {
    if (isParseFailure(output)) {
        return QString::fromUtf8("❌ 解析失败");
    }
    if (isCompileFailure(output)) {
        return QString::fromUtf8("❌ 编译错误");
    }
    if (isRuntimeError(output)) {
        return QString::fromUtf8("❌ 运行时错误");
    }
    return QString::fromUtf8("✅ 成功");
}

/// HTML 转义 std::string → QString。
QString FuzzPlaygroundPanel::escapeHtml(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size())).toHtmlEscaped();
}

/// 摘要输出：HTML 转义 + 换行单行化 + 截断。
QString FuzzPlaygroundPanel::summarizeOutput(const std::string& output, int maxLen) {
    QString q = escapeHtml(output);
    q.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    q.replace(QLatin1Char('\r'), QString());
    if (q.length() > maxLen) {
        q = q.left(maxLen) + QStringLiteral("…");
    }
    return q;
}

// ============================================================
// createGuidedTour — 新手引导（4 步）
// ============================================================

/// 构建 4 步新手引导：源码编辑器、模式选择、运行按钮、差分结果表。
GuidedTour* FuzzPlaygroundPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(sourceEdit_, mlTr("源码编辑器"),
                  mlTr("在此输入或粘贴 MiniLang 源码。面板预填了一个基本算术样例，"
                       "可直接点击运行查看三后端差分结果。"));
    tour->addStep(modeCombo_, mlTr("模式选择"),
                  mlTr("用此下拉框切换工作模式：单次差分对当前源码立即三后端比对；"
                       "批量生成/变异基于种子随机生成或变异 N 段程序后统计。"));
    tour->addStep(runBtn_, mlTr("运行"),
                  mlTr("点击「运行」执行当前模式。单次模式下表格列出三后端的输出/状态/耗时；"
                       "批量模式下表格列出汇总统计，分歧用例在「分歧详情」标签页查看。"));
    tour->addStep(diffTable_, mlTr("差分结果表"),
                  mlTr("「差分结果」标签页展示三后端（Interpreter/StackVM/RegisterVM）的"
                       "输出、状态与耗时对比，一眼看出是否一致。选中行可在下方查看完整源码与输出对比。"));
    return tour;
}
