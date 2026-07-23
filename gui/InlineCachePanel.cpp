// ============================================================
// InlineCachePanel.cpp — 内联缓存可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行任何执行引擎。包含：
//   1. IC 原理与三态静态文档（monomorphic/polymorphic/megamorphic）
//      + MiniLang JIT IC 实现对照（R152/R153 类型反馈与特化）
//   2. IC 状态模拟器：输入类型序列，逐步可视化状态转换、
//      缓存内容、命中/未命中统计
// ============================================================

#include "gui/InlineCachePanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <sstream>

// ============================================================
// InlineCacheLibrary — 静态教学数据
// ============================================================

const std::vector<IcStateInfo>& InlineCacheLibrary::stateInfos() {
    static const std::vector<IcStateInfo> kStates = {
        {"Monomorphic", "单态", "1 种", "直接内联 / 直接跳转（无需查表）", "最快（1 次比较）",
         "调用点只观测到 1 种类型/方法。IC 缓存该类型的处理入口，后续调用直接跳转，"
         "无需任何查找。这是 IC 的最优状态。"},
        {"Polymorphic", "多态", "2-4 种", "线性扫描缓存列表（最多 4 项）", "较快（1-4 次比较）",
         "调用点观测到 2-4 种类型/方法。IC 缓存这些类型的处理入口列表，"
         "每次调用线性扫描列表匹配。仍比通用查找快，但有线性开销。"},
        {"Megamorphic", "超多态", "5+ 种", "放弃缓存，走通用查找（字典/虚表）", "最慢（O(n) 查找）",
         "调用点观测到 5 种以上类型/方法。IC 缓存溢出，放弃缓存机制，"
         "每次调用都走通用查找（如字典查找或虚表分发）。这是 IC 的最差状态。"},
    };
    return kStates;
}

const std::vector<MiniLangIcMapping>& InlineCacheLibrary::miniLangMappings() {
    static const std::vector<MiniLangIcMapping> kMappings = {
        {"类型反馈（Type Feedback）", "per-chunk typeFeedback_（intCount/floatCount/otherCount）", "R152",
         "JIT 为每个 chunk 的算术操作数收集类型反馈数据。这是 IC 的数据基础——"
         "通过观测历史调用类型决定特化策略，等价于经典 IC 的「观测-缓存」机制。"},
        {"单态特化（Monomorphic Specialization）", "compileChunkSpecialized（INT/FLOAT 特化）", "R152-R153",
         "当 chunk 内所有算术操作数都是同一类型（如全 INT），JIT 跳过 emitCheckInt "
         "类型检查直接走原生路径。这等价于 monomorphic IC 的「直接跳转」——"
         "因为只有一种类型，无需运行时类型分派。"},
        {"特化入口持久化", "specializedMethodEntries_ + compileAllChunks 重应用", "R153",
         "特化后的入口地址持久化到 specializedMethodEntries_，在重新编译时恢复。"
         "这保证了特化决策在多次 execute() 间稳定，避免「无状态编译器」覆盖特化入口。"},
        {"热点检测（Hotspot Detection）", "chunkCallCounts_ + hotThresholds_ + recompiledFlags_", "R150-R151",
         "JIT 为每个 chunk 维护调用计数与热点阈值。达到阈值后触发重编译（可能特化）。"
         "这是 IC 的触发机制——只有热点代码才值得 JIT 编译与特化，"
         "冷代码保持解释执行避免编译开销。"},
        {"回退保护（Fallback Guard）", "特化前校验 otherCount==0 && floatCount==0", "R152",
         "特化前必须验证类型反馈数据表明 chunk 内无其他类型操作数。"
         "若运行时遇到非预期类型，特化版本会产生语义错误。"
         "这是 IC 的「安全回退」原则——特化必须保证语义等价。"},
    };
    return kMappings;
}

std::string InlineCacheLibrary::stateToString(IcSimState s) {
    switch (s) {
    case IcSimState::Uninitialized:
        return "Uninitialized";
    case IcSimState::Monomorphic:
        return "Monomorphic";
    case IcSimState::Polymorphic:
        return "Polymorphic";
    case IcSimState::Megamorphic:
        return "Megamorphic";
    }
    return "Unknown";
}

std::string InlineCacheLibrary::stateToChinese(IcSimState s) {
    switch (s) {
    case IcSimState::Uninitialized:
        return "未初始化";
    case IcSimState::Monomorphic:
        return "单态";
    case IcSimState::Polymorphic:
        return "多态";
    case IcSimState::Megamorphic:
        return "超多态";
    }
    return "未知";
}

// ============================================================
// InlineCachePanel 构造
// ============================================================

InlineCachePanel::InlineCachePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮
    auto* pageBar = new QHBoxLayout;
    pageTheoryBtn_ = new QPushButton(tr("① IC 原理与三态"));
    pageSimulatorBtn_ = new QPushButton(tr("② IC 状态模拟器"));
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

    // 信号连接
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
    connect(loadSampleBtn, &QPushButton::clicked, this, &InlineCachePanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    populatePresets();
}

// ============================================================
// 子页 1：IC 原理与三态
// ============================================================

void InlineCachePanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    layout->addWidget(theoryBrowser_, 1);

    // 三态对比表
    layout->addWidget(new QLabel(tr("<b>IC 三态对比</b>")));
    stateTable_ = new QTableWidget(0, 5);
    stateTable_->setHorizontalHeaderLabels({tr("状态"), tr("观测类型数"), tr("分发策略"), tr("相对速度"), tr("说明")});
    stateTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    stateTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    stateTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(stateTable_, 1);

    // MiniLang IC 实现对照表
    layout->addWidget(new QLabel(tr("<b>MiniLang JIT 的 IC 实现对照</b>")));
    mappingTable_ = new QTableWidget(0, 4);
    mappingTable_->setHorizontalHeaderLabels({tr("经典 IC 概念"), tr("MiniLang 实现"), tr("版本"), tr("说明")});
    mappingTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    mappingTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    mappingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mappingTable_, 1);
}

void InlineCachePanel::populateTheory() {
    // 理论概览 HTML
    QString html =
        QStringLiteral("<h2>内联缓存（Inline Cache, IC）原理</h2>"
                       "<p>内联缓存是动态语言运行时优化方法分发/类型分派的核心技术。"
                       "其核心思想是：<b>在调用点缓存上次观测到的类型/方法，下次调用时先检查缓存，"
                       "命中则直接跳转，未命中则走通用查找并更新缓存</b>。</p>"
                       "<h3>三态状态机</h3>"
                       "<p>IC 有三个状态，随观测到的类型数量增长而退化：</p>"
                       "<ul>"
                       "<li><b>Monomorphic（单态）</b>：只见过 1 种类型 - 直接跳转，最快</li>"
                       "<li><b>Polymorphic（多态）</b>：见过 2-4 种类型 - 线性扫描缓存列表，较快</li>"
                       "<li><b>Megamorphic（超多态）</b>：见过 5+ 种类型 - 缓存溢出，走通用查找，最慢</li>"
                       "</ul>"
                       "<h3>状态转换</h3>"
                       "<p><b>Uninitialized -&gt; Monomorphic</b>：首次观测，缓存该类型<br>"
                       "<b>Monomorphic -&gt; Polymorphic</b>：观测到新类型，缓存扩展为 2 项<br>"
                       "<b>Polymorphic -&gt; Megamorphic</b>：缓存达到 5 项，放弃缓存</p>"
                       "<h3>MiniLang JIT 的 IC 实现</h3>"
                       "<p>MiniLang JIT（R138-R156）通过<b>类型反馈 + 特化重编译</b>实现了 IC 的等价机制：</p>"
                       "<ul>"
                       "<li><b>R152 类型反馈</b>：per-chunk 收集算术操作数类型（intCount/floatCount/otherCount），"
                       "等价于 IC 的「观测」阶段</li>"
                       "<li><b>R152-R153 特化重编译</b>：当 chunk 内全为同一类型时，跳过类型检查直接走原生路径，"
                       "等价于 monomorphic IC 的「直接跳转」</li>"
                       "<li><b>R150-R151 热点检测</b>：调用计数达阈值后触发重编译，等价于 IC 的「触发」机制</li>"
                       "</ul>"
                       "<p><b>注意</b>：MiniLang 的 IC 是粗粒度的（per-chunk 而非 per-call-site），"
                       "且仅支持 monomorphic 特化（无 polymorphic 缓存列表）。"
                       "这与 V8/SpiderMonkey 等工业级 JIT 的 per-call-site IC 有差异，"
                       "但核心思想一致：用历史类型信息指导代码特化。</p>"
                       "<p style='color:#666;font-size:small;'>"
                       "提示：切换到「② IC 状态模拟器」子页，输入类型序列交互式体验状态转换。</p>");
    theoryBrowser_->setHtml(html);

    // 三态对比表
    const auto& states = InlineCacheLibrary::stateInfos();
    stateTable_->setRowCount(static_cast<int>(states.size()));
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        const auto& s = states[i];
        QString displayName = QString::fromUtf8(s.name.c_str()) + QStringLiteral("（") +
                              QString::fromUtf8(s.chineseName.c_str()) + QStringLiteral("）");
        stateTable_->setItem(i, 0, new QTableWidgetItem(displayName));
        stateTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(s.threshold.c_str())));
        stateTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(s.strategy.c_str())));
        stateTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(s.speed.c_str())));
        stateTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8(s.description.c_str())));
        stateTable_->item(i, 4)->setToolTip(QString::fromUtf8(s.description.c_str()));
    }

    // MiniLang IC 实现对照表
    const auto& mappings = InlineCacheLibrary::miniLangMappings();
    mappingTable_->setRowCount(static_cast<int>(mappings.size()));
    for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
        const auto& m = mappings[i];
        mappingTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m.icConcept.c_str())));
        mappingTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(m.miniLangImpl.c_str())));
        mappingTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(m.version.c_str())));
        mappingTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(m.explanation.c_str())));
        mappingTable_->item(i, 3)->setToolTip(QString::fromUtf8(m.explanation.c_str()));
    }
}

// ============================================================
// 子页 2：IC 状态模拟器
// ============================================================

void InlineCachePanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：类型序列输入 + 控制按钮
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(tr("类型序列：")));
    typeSeqEdit_ = new QLineEdit;
    typeSeqEdit_->setPlaceholderText(tr("用逗号分隔，如 int,int,float,string,int"));
    inputBar->addWidget(typeSeqEdit_, 1);
    runBtn_ = new QPushButton(tr("运行模拟"));
    stepBtn_ = new QPushButton(tr("单步执行"));
    resetBtn_ = new QPushButton(tr("重置"));
    inputBar->addWidget(runBtn_);
    inputBar->addWidget(stepBtn_);
    inputBar->addWidget(resetBtn_);
    layout->addLayout(inputBar);

    // 预设序列
    layout->addWidget(new QLabel(tr("预设序列：")));
    presetList_ = new QListWidget;
    presetList_->setMaximumHeight(100);
    layout->addWidget(presetList_);

    // 当前状态标签
    currentStateLabel_ = new QLabel(tr("当前 IC 状态：未初始化"));
    currentStateLabel_->setStyleSheet(QStringLiteral("font-weight:bold; padding:4px;"));
    layout->addWidget(currentStateLabel_);

    // 模拟步骤表
    layout->addWidget(new QLabel(tr("<b>模拟步骤</b>")));
    simStepTable_ = new QTableWidget(0, 7);
    simStepTable_->setHorizontalHeaderLabels(
        {tr("步骤"), tr("观测类型"), tr("转换前状态"), tr("转换后状态"), tr("命中"), tr("缓存内容"), tr("说明")});
    simStepTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    simStepTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    simStepTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    simStepTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    simStepTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    simStepTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    simStepTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    simStepTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(simStepTable_, 2);

    // 汇总
    layout->addWidget(new QLabel(tr("<b>汇总</b>")));
    simSummary_ = new QTextBrowser;
    simSummary_->setMaximumHeight(120);
    layout->addWidget(simSummary_);

    // 信号连接
    connect(runBtn_, &QPushButton::clicked, this, &InlineCachePanel::onRunSimulation);
    connect(stepBtn_, &QPushButton::clicked, this, &InlineCachePanel::onStepForward);
    connect(resetBtn_, &QPushButton::clicked, this, &InlineCachePanel::onResetSimulation);
    connect(presetList_, &QListWidget::currentRowChanged, this, &InlineCachePanel::onSelectPresetSequence);
}

void InlineCachePanel::populatePresets() {
    // 预设类型序列（涵盖三种状态转换路径）
    struct Preset {
        const char* label;
        const char* sequence;
    };
    static const Preset kPresets[] = {
        {"单态稳定（int,int,int,int）—— 全程命中", "int,int,int,int"},
        {"单态->多态（int,int,float）—— 1 次未命中扩展缓存", "int,int,float"},
        {"多态渐进（int,float,string,bool）—— 4 种类型保持多态", "int,float,string,bool"},
        {"多态->超多态（int,float,string,bool,null）—— 第 5 种触发溢出", "int,float,string,bool,null"},
        {"超多态后持续（int,float,string,bool,null,int,float）—— 永远未命中", "int,float,string,bool,null,int,float"},
        {"交替类型（int,float,int,float）—— 多态命中与未命中交替", "int,float,int,float"},
    };
    presetList_->clear();
    for (const auto& p : kPresets) {
        presetList_->addItem(QString::fromUtf8(p.label));
    }
}

// ============================================================
// 模拟器逻辑
// ============================================================

void InlineCachePanel::parseTypeSequence(const std::string& text, std::vector<std::string>& out) {
    out.clear();
    std::string current;
    for (char c : text) {
        if (c == ',' || c == ',') { // 支持中英文逗号
            if (!current.empty()) {
                // trim
                size_t start = current.find_first_not_of(" \t");
                size_t end = current.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    out.push_back(current.substr(start, end - start + 1));
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        size_t start = current.find_first_not_of(" \t");
        size_t end = current.find_last_not_of(" \t");
        if (start != std::string::npos) {
            out.push_back(current.substr(start, end - start + 1));
        }
    }
}

std::string InlineCachePanel::formatCache(const std::vector<std::string>& cache) {
    if (cache.empty()) {
        return "（空）";
    }
    std::string result;
    for (size_t i = 0; i < cache.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += cache[i];
    }
    return result;
}

/// IC 状态转换核心算法（经典 IC 理论）：
/// - Uninitialized -> Monomorphic（首次观测）
/// - Monomorphic -> Polymorphic（观测到新类型）
/// - Polymorphic -> Megamorphic（缓存达 5 项）
/// - Megamorphic 保持（永远未命中）
IcSimStep InlineCachePanel::simulateOneStep(const std::string& observedType) {
    IcSimStep step;
    step.stepNumber = static_cast<int>(simSteps_.size()) + 1;
    step.observedType = observedType;
    step.stateBefore = simState_;

    // 检查是否命中缓存
    bool inCache = false;
    for (const auto& t : simCache_) {
        if (t == observedType) {
            inCache = true;
            break;
        }
    }
    step.hit = inCache;
    simTotalCalls_++;

    switch (simState_) {
    case IcSimState::Uninitialized:
        // 首次观测 -> 进入单态
        simCache_.push_back(observedType);
        simState_ = IcSimState::Monomorphic;
        step.note = "首次观测，进入单态（缓存该类型）";
        break;

    case IcSimState::Monomorphic:
        if (inCache) {
            step.note = "命中单态缓存，保持单态";
        } else {
            // 观测到新类型 -> 进入多态
            simCache_.push_back(observedType);
            simState_ = IcSimState::Polymorphic;
            step.note = "未命中，观测到新类型，进入多态（缓存扩展为 2 项）";
        }
        break;

    case IcSimState::Polymorphic:
        if (inCache) {
            step.note = "命中多态缓存，保持多态";
        } else {
            // 观测到新类型
            if (static_cast<int>(simCache_.size()) < InlineCacheLibrary::kPolymorphicMax) {
                simCache_.push_back(observedType);
                step.note = "未命中，缓存未满，添加新类型保持多态";
            } else {
                // 缓存达 4 项，第 5 种类型触发溢出 -> 进入超多态
                simCache_.clear(); // 放弃缓存
                simState_ = IcSimState::Megamorphic;
                step.note = "未命中，缓存达上限，放弃缓存进入超多态";
            }
        }
        break;

    case IcSimState::Megamorphic:
        // 超多态：永远未命中（走通用查找）
        step.note = "超多态，放弃缓存，走通用查找";
        break;
    }

    if (inCache) {
        simHits_++;
    }

    step.stateAfter = simState_;
    step.cacheContent = formatCache(simCache_);
    if (simState_ == IcSimState::Megamorphic) {
        step.cacheContent = "（溢出，已放弃）";
    }

    simSteps_.push_back(step);
    return step;
}

void InlineCachePanel::renderSimSteps() {
    simStepTable_->setRowCount(static_cast<int>(simSteps_.size()));
    for (int i = 0; i < static_cast<int>(simSteps_.size()); ++i) {
        const auto& s = simSteps_[i];
        simStepTable_->setItem(i, 0, new QTableWidgetItem(QString::number(s.stepNumber)));
        simStepTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(s.observedType.c_str())));
        simStepTable_->setItem(
            i, 2, new QTableWidgetItem(QString::fromUtf8(InlineCacheLibrary::stateToChinese(s.stateBefore).c_str())));
        simStepTable_->setItem(
            i, 3, new QTableWidgetItem(QString::fromUtf8(InlineCacheLibrary::stateToChinese(s.stateAfter).c_str())));

        auto* hitItem = new QTableWidgetItem(s.hit ? tr("命中") : tr("未命中"));
        if (s.hit) {
            hitItem->setForeground(QColor("#2E7D32"));
        } else {
            hitItem->setForeground(QColor("#C62828"));
        }
        simStepTable_->setItem(i, 4, hitItem);

        simStepTable_->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8(s.cacheContent.c_str())));
        simStepTable_->setItem(i, 6, new QTableWidgetItem(QString::fromUtf8(s.note.c_str())));
    }
}

void InlineCachePanel::renderSimSummary() {
    double hitRate = simTotalCalls_ > 0 ? (static_cast<double>(simHits_) / simTotalCalls_ * 100.0) : 0.0;
    QString stateColor;
    switch (simState_) {
    case IcSimState::Uninitialized:
        stateColor = QStringLiteral("#666");
        break;
    case IcSimState::Monomorphic:
        stateColor = QStringLiteral("#2E7D32"); // 绿
        break;
    case IcSimState::Polymorphic:
        stateColor = QStringLiteral("#F57F17"); // 橙
        break;
    case IcSimState::Megamorphic:
        stateColor = QStringLiteral("#C62828"); // 红
        break;
    }

    currentStateLabel_->setText(tr("当前 IC 状态：<span style='color:%1;font-weight:bold;'>%2</span>")
                                    .arg(stateColor)
                                    .arg(QString::fromUtf8(InlineCacheLibrary::stateToChinese(simState_).c_str())));

    QString html = QStringLiteral("<table cellpadding='4'>"
                                  "<tr><td><b>总调用次数</b></td><td>%1</td></tr>"
                                  "<tr><td><b>缓存命中</b></td><td>%2</td></tr>"
                                  "<tr><td><b>未命中</b></td><td>%3</td></tr>"
                                  "<tr><td><b>命中率</b></td><td>%4%</td></tr>"
                                  "<tr><td><b>最终状态</b></td><td>%5</td></tr>"
                                  "</table>")
                       .arg(simTotalCalls_)
                       .arg(simHits_)
                       .arg(simTotalCalls_ - simHits_)
                       .arg(QString::number(hitRate, 'f', 1))
                       .arg(QString::fromUtf8(InlineCacheLibrary::stateToChinese(simState_).c_str()));

    // 状态说明
    QString stateExplanation;
    switch (simState_) {
    case IcSimState::Uninitialized:
        stateExplanation = tr("尚未开始模拟。");
        break;
    case IcSimState::Monomorphic:
        stateExplanation = tr("单态：所有调用命中同一类型，IC 性能最优（直接跳转，无查找开销）。"
                              "MiniLang JIT 的 R152 INT/FLOAT 特化即对应此状态。");
        break;
    case IcSimState::Polymorphic:
        stateExplanation = tr("多态：观测到多种类型，IC 线性扫描缓存列表。性能介于单态与超多态之间。"
                              "MiniLang JIT 当前不支持 polymorphic IC（仅 monomorphic 特化）。");
        break;
    case IcSimState::Megamorphic:
        stateExplanation = tr("超多态：缓存溢出，放弃 IC 机制走通用查找。性能最差。"
                              "MiniLang JIT 遇到此场景会保持非特化版本（emitCheckInt 类型检查不跳过）。");
        break;
    }
    html += QStringLiteral("<p>%1</p>").arg(stateExplanation);

    simSummary_->setHtml(html);
}

// ============================================================
// 槽函数
// ============================================================

void InlineCachePanel::onRunSimulation() {
    // 重置后完整运行
    onResetSimulation();

    std::string text = typeSeqEdit_->text().toStdString();
    std::vector<std::string> types;
    parseTypeSequence(text, types);

    if (types.empty()) {
        simSummary_->setHtml(tr("<p style='color:red;'>请输入类型序列（逗号分隔，如 int,int,float）</p>"));
        return;
    }

    for (const auto& t : types) {
        simulateOneStep(t);
    }
    renderSimSteps();
    renderSimSummary();
}

void InlineCachePanel::onStepForward() {
    // 若有待执行类型，执行下一步
    if (pendingTypes_.empty()) {
        // 从输入框重新解析
        std::string text = typeSeqEdit_->text().toStdString();
        parseTypeSequence(text, pendingTypes_);
        if (pendingTypes_.empty()) {
            simSummary_->setHtml(tr("<p style='color:red;'>请输入类型序列后单步执行</p>"));
            return;
        }
    }
    std::string next = pendingTypes_.front();
    pendingTypes_.erase(pendingTypes_.begin());
    simulateOneStep(next);
    renderSimSteps();
    renderSimSummary();
}

void InlineCachePanel::onResetSimulation() {
    simState_ = IcSimState::Uninitialized;
    simCache_.clear();
    simSteps_.clear();
    pendingTypes_.clear();
    simTotalCalls_ = 0;
    simHits_ = 0;
    simStepTable_->setRowCount(0);
    renderSimSummary();
}

void InlineCachePanel::onSelectPresetSequence(int idx) {
    if (idx < 0) {
        return;
    }
    // 预设序列定义在 populatePresets 中
    static const char* kSequences[] = {
        "int,int,int,int",
        "int,int,float",
        "int,float,string,bool",
        "int,float,string,bool,null",
        "int,float,string,bool,null,int,float",
        "int,float,int,float",
    };
    if (idx < static_cast<int>(sizeof(kSequences) / sizeof(kSequences[0]))) {
        typeSeqEdit_->setText(QString::fromUtf8(kSequences[idx]));
        onResetSimulation();
    }
}

void InlineCachePanel::onLoadSample() {
    // 加载一个展示 IC 相关概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 内联缓存（IC）样例：方法分发与类型特化\n"
                                    "// MiniLang JIT 通过类型反馈实现 IC 的等价机制\n"
                                    "\n"
                                    "// 热点方法（JIT 会特化为 INT 路径）\n"
                                    "fun add(a, b) { return a + b; }\n"
                                    "\n"
                                    "// 循环调用使 add 成为热点，触发 JIT 编译 + INT 特化\n"
                                    "var i = 0;\n"
                                    "var sum = 0;\n"
                                    "while (i < 1000) {\n"
                                    "  sum = add(sum, i);\n"
                                    "  i = i + 1;\n"
                                    "}\n"
                                    "print(sum);\n");
    emit loadSampleRequested(sample);
}

void InlineCachePanel::onPageSwitch(int idx) {
    stack_->setCurrentIndex(idx);
}
