// ============================================================
// LoopUnrollingPanel.cpp — 循环展开可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行任何执行引擎。包含：
//   1. 循环展开原理静态文档（展开因子/完全展开/部分展开/代价）
//      + 展开前后对比表（N=100 示例）
//      + MiniLang 循环展开实现对照（R154 提到 JIT 暂不支持）
//   2. 循环展开模拟器：输入循环次数与展开因子，
//      对比不同展开因子（1/2/4/8/16）的性能收益，
//      展开后伪代码预览，推荐最优展开因子
// ============================================================

#include "gui/LoopUnrollingPanel.h"

#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>
#include <sstream>

// ============================================================
// LoopUnrollingLibrary — 静态教学数据与模拟算法
// ============================================================

const std::vector<UnrollCompareRow>& LoopUnrollingLibrary::compareRows() {
    // 以 N=100、循环体指令数 bodyInstrCount=5 为示例
    static const std::vector<UnrollCompareRow> kRows = {
        {"迭代次数", "100", "50", "25", "13", "展开后每次迭代执行 K 次循环体，迭代次数 = ceil(N/K)"},
        {"跳转次数", "100", "50", "25", "13", "回跳指令（OP_LOOP）数量随展开因子减少"},
        {"条件检查次数", "100", "50", "25", "13", "条件分支判断（OP_JUMP_IF_FALSE）数量减少"},
        {"循环控制指令数", "200", "100", "50", "26", "LOOP + JUMP_IF_FALSE 各计 1 条，共 2 * ceil(N/K)"},
        {"循环体指令数", "500", "500", "500", "500", "循环体指令数不变（N * bodyInstrCount）"},
        {"总指令数", "700", "600", "550", "526", "控制开销减少，循环体不变，总指令下降"},
        {"加速比", "1.00x", "1.17x", "1.27x", "1.33x", "相对原始循环的加速，收益递减"},
    };
    return kRows;
}

const std::vector<MiniLangUnrollMapping>& LoopUnrollingLibrary::miniLangMappings() {
    static const std::vector<MiniLangUnrollMapping> kMappings = {
        {"循环展开（Loop Unrolling）", "不支持自动展开",
         "MiniLang 编译器无循环展开优化 pass，所有循环保持原始结构执行。"
         "教学面板通过模拟器展示展开原理与性能收益。"},
        {"展开因子（Unroll Factor）", "不适用",
         "MiniLang 无展开因子配置。经典编译器（如 GCC -funroll-loops）"
         "可指定展开因子，MiniLang 作为教学语言不提供此优化。"},
        {"循环控制指令", "OP_LOOP + OP_JUMP_IF_FALSE",
         "栈式 VM 通过这两个 OpCode 实现 while 循环："
         "OP_LOOP 回跳到循环起始，OP_JUMP_IF_FALSE 条件不满足时跳出。"
         "展开可减少这两条指令的执行次数。"},
        {"JIT 支持", "不支持（R154 提到）",
         "R154 记录 JIT 暂不支持循环展开，当前 JIT 仅支持类型反馈特化"
         "（R152/R153 的 INT/FLOAT 特化路径）。循环展开是未来可能的优化方向。"},
        {"教学价值", "高",
         "理解循环展开原理有助于学习者掌握编译器循环优化、"
         "指令级并行（ILP）、寄存器压力与代码膨胀的权衡。"
         "这是经典优化技术的基础概念。"},
    };
    return kMappings;
}

UnrollSimResult LoopUnrollingLibrary::simulate(int loopCount, int factor, int bodyInstrCount) {
    UnrollSimResult r;
    r.factor = factor;
    // 边界保护
    if (loopCount <= 0 || factor <= 0 || bodyInstrCount < 0) {
        r.iterations = 0;
        r.jumps = 0;
        r.controlInstrs = 0;
        r.bodyInstrs = 0;
        r.totalInstrs = 0;
        r.speedup = 0.0;
        return r;
    }
    // 展开后迭代次数 = ceil(N/K)
    r.iterations = (loopCount + factor - 1) / factor;
    r.jumps = r.iterations;
    // 循环控制指令 = LOOP + JUMP_IF_FALSE，每次迭代 2 条
    r.controlInstrs = r.iterations * 2;
    // 循环体指令 = N * bodyInstrCount（展开不改变循环体总指令数）
    r.bodyInstrs = loopCount * bodyInstrCount;
    r.totalInstrs = r.controlInstrs + r.bodyInstrs;
    // 原始总指令 = N * (2 + bodyInstrCount)
    int originalTotal = loopCount * (2 + bodyInstrCount);
    r.speedup = static_cast<double>(originalTotal) / static_cast<double>(r.totalInstrs);
    return r;
}

std::string LoopUnrollingLibrary::generateUnrolledCode(int loopCount, int factor, const std::string& body) {
    std::ostringstream oss;
    // 边界保护
    if (loopCount <= 0 || factor <= 0) {
        oss << "// 无效参数：loopCount=" << loopCount << " factor=" << factor << "\n";
        return oss.str();
    }
    int iters = (loopCount + factor - 1) / factor;
    int remainder = loopCount % factor;
    oss << "// 展开因子 K=" << factor << "，循环次数 N=" << loopCount << "\n";
    oss << "// 展开后迭代次数 = ceil(" << loopCount << "/" << factor << ") = " << iters << "\n";
    if (remainder != 0) {
        oss << "// 注意：N 不被 K 整除，尾部剩余 " << remainder << " 次迭代需单独处理\n";
        oss << "// （此处省略尾部处理，实际编译器需补足剩余循环或守卫分支）\n";
    }
    oss << "var i = 0;\n";
    oss << "var sum = 0;\n";
    oss << "while (i < " << loopCount << ") {\n";
    for (int k = 0; k < factor; ++k) {
        oss << "    " << body << " i = i + 1;  // 第 " << (k + 1) << " 次循环体\n";
    }
    oss << "}\n";
    oss << "print(sum);\n";
    return oss.str();
}

// ============================================================
// LoopUnrollingPanel 构造
// ============================================================

LoopUnrollingPanel::LoopUnrollingPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮
    auto* pageBar = new QHBoxLayout;
    pageTheoryBtn_ = new QPushButton(tr("① 循环展开原理"));
    pageSimulatorBtn_ = new QPushButton(tr("② 循环展开模拟器"));
    pageTheoryBtn_->setCheckable(true);
    pageSimulatorBtn_->setCheckable(true);
    pageTheoryBtn_->setChecked(true);
    pageBar->addWidget(pageTheoryBtn_);
    pageBar->addWidget(pageSimulatorBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    // 子页堆栈
    stack_ = new QStackedWidget;
    auto* theoryPage = new QWidget;
    auto* simulatorPage = new QWidget;
    buildTheoryPage(theoryPage);
    buildSimulatorPage(simulatorPage);
    stack_->addWidget(theoryPage);
    stack_->addWidget(simulatorPage);
    outer->addWidget(stack_, 1);

    // 底部：加载样例按钮
    auto* bottomBar = new QHBoxLayout;
    auto* loadSampleBtn = new QPushButton(tr("加载样例到主编辑器"));
    bottomBar->addStretch();
    bottomBar->addWidget(loadSampleBtn);
    outer->addLayout(bottomBar);

    // 信号连接：子页切换
    connect(pageTheoryBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(0);
            pageSimulatorBtn_->setChecked(false);
        }
    });
    connect(pageSimulatorBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(1);
            pageTheoryBtn_->setChecked(false);
        }
    });
    connect(loadSampleBtn, &QPushButton::clicked, this, &LoopUnrollingPanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    // 初始模拟（默认值）
    onRunSimulation();
}

// ============================================================
// 子页 1：循环展开原理
// ============================================================

void LoopUnrollingPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    layout->addWidget(theoryBrowser_, 2);

    // 展开前后对比表
    layout->addWidget(new QLabel(tr("<b>展开前后对比（示例 N=100，循环体 5 条指令）</b>")));
    compareTable_ = new QTableWidget(0, 6);
    compareTable_->setHorizontalHeaderLabels(
        {tr("指标"), tr("原始"), tr("展开 K=2"), tr("展开 K=4"), tr("展开 K=8"), tr("说明")});
    compareTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    compareTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    compareTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    compareTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    compareTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    compareTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    compareTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(compareTable_, 1);

    // MiniLang 循环展开实现对照表
    layout->addWidget(new QLabel(tr("<b>MiniLang 循环展开实现对照</b>")));
    mappingTable_ = new QTableWidget(0, 3);
    mappingTable_->setHorizontalHeaderLabels({tr("概念"), tr("MiniLang 状态"), tr("说明")});
    mappingTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    mappingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mappingTable_, 1);
}

void LoopUnrollingPanel::populateTheory() {
    // 理论概览 HTML
    QString html =
        QStringLiteral("<h2>循环展开（Loop Unrolling）原理</h2>"
                       "<p>循环展开是编译器最经典的循环优化技术之一。其核心思想是："
                       "<b>将循环体复制 K 份放入单次迭代中，减少循环控制开销</b>，"
                       "从而提升执行效率。</p>"
                       "<h3>为什么能加速</h3>"
                       "<ul>"
                       "<li><b>减少循环控制开销</b>：每次迭代都需要执行条件检查与跳转指令"
                       "（OP_LOOP + OP_JUMP_IF_FALSE），展开后这些指令执行次数减少</li>"
                       "<li><b>改善指令流水线</b>：减少分支指令意味着减少流水线停顿与分支预测失败</li>"
                       "<li><b>增加指令级并行（ILP）</b>：展开后多个循环体操作可并行调度，"
                       "提升处理器功能单元利用率</li>"
                       "</ul>"
                       "<h3>展开因子（Unroll Factor）</h3>"
                       "<p>展开因子 K 表示每次迭代执行原循环体的 K 份副本。展开后："
                       "迭代次数 = ceil(N/K)，循环控制指令 = 2 * ceil(N/K)，"
                       "循环体指令 = N * bodyInstrCount（不变）。</p>"
                       "<h3>完全展开 vs 部分展开</h3>"
                       "<ul>"
                       "<li><b>完全展开</b>：K = N，循环完全消除，所有迭代顺序展开"
                       "（适用于迭代次数编译期已知且较小的场景）</li>"
                       "<li><b>部分展开</b>：K &lt; N，保留循环结构但减少迭代次数"
                       "（最常见，需处理 N 不被 K 整除的尾部）</li>"
                       "</ul>"
                       "<h3>代价</h3>"
                       "<ul>"
                       "<li><b>代码膨胀</b>：展开因子越大，代码体积越大，可能影响指令缓存命中率</li>"
                       "<li><b>寄存器压力</b>：展开后同时活跃的变量增多，可能超出可用寄存器数"
                       "导致溢出到栈</li>"
                       "<li><b>收益递减</b>：展开因子从 1 到 2 收益最大，后续边际收益递减</li>"
                       "</ul>"
                       "<h3>MiniLang 的状态</h3>"
                       "<p>MiniLang 编译器与 JIT <b>均不支持自动循环展开</b>（R154 提到 JIT 暂不支持）。"
                       "栈式 VM 的循环控制通过 OP_LOOP + OP_JUMP_IF_FALSE 实现，"
                       "寄存器式 VM 通过类似机制。教学面板通过模拟器展示展开原理与性能收益。</p>"
                       "<p style='color:#666;font-size:small;'>"
                       "提示：切换到「② 循环展开模拟器」子页，输入循环次数与展开因子交互式体验性能收益。</p>");
    theoryBrowser_->setHtml(html);

    // 展开前后对比表
    const auto& rows = LoopUnrollingLibrary::compareRows();
    compareTable_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[i];
        compareTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(r.metric.c_str())));
        compareTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(r.original.c_str())));
        compareTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(r.unroll2.c_str())));
        compareTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(r.unroll4.c_str())));
        compareTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8(r.unroll8.c_str())));
        compareTable_->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8(r.note.c_str())));
        compareTable_->item(i, 5)->setToolTip(QString::fromUtf8(r.note.c_str()));
    }

    // MiniLang 实现对照表
    const auto& mappings = LoopUnrollingLibrary::miniLangMappings();
    mappingTable_->setRowCount(static_cast<int>(mappings.size()));
    for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
        const auto& m = mappings[i];
        mappingTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m.conceptName.c_str())));
        mappingTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(m.miniLangState.c_str())));
        mappingTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(m.explanation.c_str())));
        mappingTable_->item(i, 2)->setToolTip(QString::fromUtf8(m.explanation.c_str()));
    }
}

// ============================================================
// 子页 2：循环展开模拟器
// ============================================================

void LoopUnrollingPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：输入控件
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(tr("循环次数：")));
    loopCountEdit_ = new QLineEdit;
    loopCountEdit_->setText(QStringLiteral("100"));
    loopCountEdit_->setPlaceholderText(tr("输入循环次数，如 100"));
    inputBar->addWidget(loopCountEdit_, 1);

    inputBar->addWidget(new QLabel(tr("展开因子：")));
    factorSpin_ = new QSpinBox;
    factorSpin_->setRange(1, 16);
    factorSpin_->setValue(4);
    factorSpin_->setSingleStep(1);
    inputBar->addWidget(factorSpin_);

    runBtn_ = new QPushButton(tr("运行模拟"));
    inputBar->addWidget(runBtn_);
    layout->addLayout(inputBar);

    // 预设按钮
    auto* presetBar = new QHBoxLayout;
    presetBtn1_ = new QPushButton(tr("简单累加 100"));
    presetBtn2_ = new QPushButton(tr("嵌套循环 100"));
    presetBtn3_ = new QPushButton(tr("大循环 10000"));
    presetBtn4_ = new QPushButton(tr("小循环 4"));
    presetBar->addWidget(presetBtn1_);
    presetBar->addWidget(presetBtn2_);
    presetBar->addWidget(presetBtn3_);
    presetBar->addWidget(presetBtn4_);
    presetBar->addStretch();
    layout->addLayout(presetBar);

    // 中间：垂直分割器
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：展开因子对比表
    simCompareTable_ = new QTableWidget(0, 6);
    simCompareTable_->setHorizontalHeaderLabels(
        {tr("展开因子"), tr("迭代次数"), tr("跳转次数"), tr("循环控制指令"), tr("总指令数"), tr("加速比")});
    simCompareTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    simCompareTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    simCompareTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    simCompareTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    simCompareTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    simCompareTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    simCompareTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vSplitter->addWidget(simCompareTable_);

    // 下方：水平分割器（代码预览 + 汇总）
    auto* hSplitter = new QSplitter(Qt::Horizontal);
    codePreview_ = new QTextBrowser;
    codePreview_->setPlaceholderText(tr("展开后伪代码预览"));
    hSplitter->addWidget(codePreview_);

    simSummary_ = new QTextBrowser;
    simSummary_->setPlaceholderText(tr("汇总与推荐"));
    hSplitter->addWidget(simSummary_);

    hSplitter->setStretchFactor(0, 1);
    hSplitter->setStretchFactor(1, 1);
    vSplitter->addWidget(hSplitter);
    vSplitter->setStretchFactor(0, 1);
    vSplitter->setStretchFactor(1, 1);
    layout->addWidget(vSplitter, 1);

    // 信号连接
    connect(runBtn_, &QPushButton::clicked, this, &LoopUnrollingPanel::onRunSimulation);
    connect(presetBtn1_, &QPushButton::clicked, this, [this]() { onSelectPreset(0); });
    connect(presetBtn2_, &QPushButton::clicked, this, [this]() { onSelectPreset(1); });
    connect(presetBtn3_, &QPushButton::clicked, this, [this]() { onSelectPreset(2); });
    connect(presetBtn4_, &QPushButton::clicked, this, [this]() { onSelectPreset(3); });
}

// ============================================================
// 模拟器逻辑
// ============================================================

void LoopUnrollingPanel::renderSimulation(int loopCount) {
    // 对每个展开因子（1/2/4/8/16）生成一行
    static const int kFactors[] = {1, 2, 4, 8, 16};
    const int factorCount = static_cast<int>(sizeof(kFactors) / sizeof(kFactors[0]));
    simCompareTable_->setRowCount(factorCount);

    for (int i = 0; i < factorCount; ++i) {
        int f = kFactors[i];
        UnrollSimResult r = LoopUnrollingLibrary::simulate(loopCount, f);
        simCompareTable_->setItem(i, 0, new QTableWidgetItem(QStringLiteral("K=%1").arg(f)));
        simCompareTable_->setItem(i, 1, new QTableWidgetItem(QString::number(r.iterations)));
        simCompareTable_->setItem(i, 2, new QTableWidgetItem(QString::number(r.jumps)));
        simCompareTable_->setItem(i, 3, new QTableWidgetItem(QString::number(r.controlInstrs)));
        simCompareTable_->setItem(i, 4, new QTableWidgetItem(QString::number(r.totalInstrs)));

        // 加速比着色：绿色表示有加速
        auto* speedupItem = new QTableWidgetItem(QString::number(r.speedup, 'f', 2) + QStringLiteral("x"));
        if (f == 1) {
            speedupItem->setForeground(QColor("#666666")); // 基线灰色
        } else if (r.speedup >= 1.2) {
            speedupItem->setForeground(QColor("#2E7D32")); // 绿色
        } else if (r.speedup >= 1.1) {
            speedupItem->setForeground(QColor("#F57F17")); // 橙色
        } else {
            speedupItem->setForeground(QColor("#666666")); // 灰色
        }
        simCompareTable_->setItem(i, 5, speedupItem);
    }
}

void LoopUnrollingPanel::renderUnrolledCode(int loopCount, int factor) {
    std::string code = LoopUnrollingLibrary::generateUnrolledCode(loopCount, factor);
    // 用等宽字体显示伪代码
    QString html = QStringLiteral("<pre style='font-family:Consolas,monospace; font-size:13px;'>") +
                   QString::fromUtf8(code.c_str()).toHtmlEscaped() + QStringLiteral("</pre>");
    codePreview_->setHtml(html);
}

void LoopUnrollingPanel::renderSummary(int loopCount) {
    // 找出最优展开因子（最高加速比）
    static const int kFactors[] = {1, 2, 4, 8, 16};
    const int factorCount = static_cast<int>(sizeof(kFactors) / sizeof(kFactors[0]));
    int bestFactor = 1;
    double bestSpeedup = 1.0;
    int bestTotalInstrs = loopCount * (2 + LoopUnrollingLibrary::kDefaultBodyInstrCount);

    for (int i = 0; i < factorCount; ++i) {
        UnrollSimResult r = LoopUnrollingLibrary::simulate(loopCount, kFactors[i]);
        if (r.speedup > bestSpeedup) {
            bestSpeedup = r.speedup;
            bestFactor = kFactors[i];
            bestTotalInstrs = r.totalInstrs;
        }
    }

    // 当前展开因子
    int currentFactor = factorSpin_->value();
    UnrollSimResult current = LoopUnrollingLibrary::simulate(loopCount, currentFactor);

    QString html = QStringLiteral("<h3>性能分析</h3>");
    html += QStringLiteral("<table cellpadding='4'>"
                           "<tr><td><b>循环次数 N</b></td><td>%1</td></tr>"
                           "<tr><td><b>当前展开因子 K</b></td><td>%2</td></tr>"
                           "<tr><td><b>当前迭代次数</b></td><td>%3</td></tr>"
                           "<tr><td><b>当前总指令数</b></td><td>%4</td></tr>"
                           "<tr><td><b>当前加速比</b></td><td>%5x</td></tr>"
                           "</table>")
                .arg(loopCount)
                .arg(currentFactor)
                .arg(current.iterations)
                .arg(current.totalInstrs)
                .arg(QString::number(current.speedup, 'f', 2));

    // 最优推荐
    html += QStringLiteral("<h3>最优展开因子推荐</h3>");
    if (bestFactor == 1) {
        html += QStringLiteral("<p>当循环次数较少时，展开收益有限，"
                               "控制开销占比低，<b>不建议展开</b>。</p>");
    } else {
        int originalTotal = loopCount * (2 + LoopUnrollingLibrary::kDefaultBodyInstrCount);
        int savedInstrs = originalTotal - bestTotalInstrs;
        html += QStringLiteral("<p>推荐展开因子 <b style='color:#2E7D32;'>K=%1</b>，"
                               "加速比 <b>%2x</b>，可节省 <b>%3</b> 条指令。</p>")
                    .arg(bestFactor)
                    .arg(QString::number(bestSpeedup, 'f', 2))
                    .arg(savedInstrs);
    }

    // 收益递减分析
    html += QStringLiteral("<h3>收益递减分析</h3>");
    html += QStringLiteral("<p>展开因子从 1 增加到 2 时收益最大（控制开销减半），"
                           "继续增大展开因子边际收益递减。"
                           "实际编译器需权衡代码膨胀与指令缓存命中率。</p>");

    // 代价提示
    html += QStringLiteral("<h3>展开代价</h3>");
    html += QStringLiteral("<ul>"
                           "<li><b>代码膨胀</b>：展开因子 K 使循环体代码量增加约 K 倍，"
                           "可能降低指令缓存命中率</li>"
                           "<li><b>寄存器压力</b>：展开后同时活跃的变量增多，"
                           "可能超出可用寄存器导致溢出</li>"
                           "<li><b>尾部处理</b>：N 不被 K 整除时需额外处理剩余迭代</li>"
                           "</ul>");

    // MiniLang 状态提示
    html += QStringLiteral("<p style='color:#666;font-size:small;'>"
                           "注：MiniLang 编译器与 JIT 均不支持自动循环展开（R154），"
                           "本面板为纯教学模拟。实际性能收益取决于处理器微架构与编译器实现。</p>");

    simSummary_->setHtml(html);
}

// ============================================================
// 槽函数
// ============================================================

void LoopUnrollingPanel::onRunSimulation() {
    // 解析循环次数
    bool ok = false;
    int loopCount = loopCountEdit_->text().trimmed().toInt(&ok);
    if (!ok || loopCount <= 0) {
        simSummary_->setHtml(tr("<p style='color:red;'>请输入正整数循环次数（如 100）</p>"));
        simCompareTable_->setRowCount(0);
        codePreview_->clear();
        return;
    }

    // 限制最大值避免表格过大
    if (loopCount > 1000000) {
        loopCount = 1000000;
        loopCountEdit_->setText(QString::number(loopCount));
    }

    renderSimulation(loopCount);
    renderUnrolledCode(loopCount, factorSpin_->value());
    renderSummary(loopCount);
}

void LoopUnrollingPanel::onSelectPreset(int idx) {
    // 预设：设置循环次数并运行模拟
    static const struct {
        const char* label;
        int loopCount;
    } kPresets[] = {
        {"简单累加 100", 100},
        {"嵌套循环 100", 100},
        {"大循环 10000", 10000},
        {"小循环 4", 4},
    };
    if (idx < 0 || idx >= static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]))) {
        return;
    }
    loopCountEdit_->setText(QString::number(kPresets[idx].loopCount));
    onRunSimulation();
}

void LoopUnrollingPanel::onLoadSample() {
    // 加载展示循环展开概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 循环展开（Loop Unrolling）样例\n"
                                    "// MiniLang 不支持自动循环展开，但可手动展开对比性能\n"
                                    "\n"
                                    "// 原始循环（K=1）\n"
                                    "var sum1 = 0;\n"
                                    "var i = 0;\n"
                                    "while (i < 100) {\n"
                                    "  sum1 = sum1 + i;\n"
                                    "  i = i + 1;\n"
                                    "}\n"
                                    "print(sum1);\n"
                                    "\n"
                                    "// 手动展开 K=4\n"
                                    "var sum2 = 0;\n"
                                    "var j = 0;\n"
                                    "while (j < 100) {\n"
                                    "  sum2 = sum2 + j; j = j + 1;\n"
                                    "  sum2 = sum2 + j; j = j + 1;\n"
                                    "  sum2 = sum2 + j; j = j + 1;\n"
                                    "  sum2 = sum2 + j; j = j + 1;\n"
                                    "}\n"
                                    "print(sum2);\n"
                                    "\n"
                                    "// 两段代码结果相同，展开版减少循环控制指令\n"
                                    "// 注：MiniLang JIT（R154）暂不支持自动循环展开\n");
    emit loadSampleRequested(sample);
}

void LoopUnrollingPanel::onPageSwitch(int idx) {
    stack_->setCurrentIndex(idx);
}
