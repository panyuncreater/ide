// ============================================================
// GcVisualizerPanel.cpp — GC 垃圾回收可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行 MiniLang 执行引擎。包含：
//   1. GC 原理静态文档（mark-sweep-finalize 三阶段 + 三种 GcMode 对比
//      + 容器类型注册表 + 引用计数与 COW 关系）
//   2. 交互式 GC 模拟器：4 个预设场景，纯 C++ 操作 Value + GcManager 单例，
//      实时展示 tracked / marked / collected 统计变化
//
// 注意：GcManager 是进程级单例，模拟会影响全局状态。
// 每次模拟前后均调用 reset() + setGcMode(RefCountWithCycleGc) 恢复干净状态。
// ============================================================

#include "gui/GcVisualizerPanel.h"
#include "app/IdeController.h" // AUDIT-R5 R7 fix (BUG-2): 运行期状态查询（isRunning/isVmRunning）
#include "gui/GuidedTour.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include "common/MemoryInspectionAPI.h" // ARCH-10: GC 统计/控制只读 API（替代直接依赖 interpreter/GcManager.h）
#include "gui/GuiTextUtils.h"
#include "gui/I18n.h"
#include "gui/TeachingSubPageBar.h"
#include "gui/TeachingTheme.h" // UX-R2 fix: 警告标签样式迁移到语义色
#include "interpreter/Value.h" // Value 用于构造模拟场景的数组（语言运行时值类型，不可避免）

// ============================================================
// 匿名命名空间 — 辅助函数
// ============================================================

namespace {

/// 将 GcPhase 枚举转为中文显示文本
QString phaseToString(GcPhase p) {
    switch (p) {
    case GcPhase::Idle:
        return QStringLiteral("Idle（空闲）");
    case GcPhase::Marking:
        return QStringLiteral("Marking（标记）");
    case GcPhase::Sweeping:
        return QStringLiteral("Sweeping（清扫）");
    case GcPhase::Finalizing:
        return QStringLiteral("Finalizing（收尾）");
    }
    return QStringLiteral("Unknown");
}

/// 将 GcMode 枚举转为显示文本
QString modeToString(GcMode m) {
    switch (m) {
    case GcMode::RefCountOnly:
        return QStringLiteral("RefCountOnly");
    case GcMode::RefCountWithCycleGc:
        return QStringLiteral("RefCountWithCycleGc");
    case GcMode::GcOnly:
        return QStringLiteral("GcOnly");
    }
    return QStringLiteral("Unknown");
}

/// HTML 转义（避免代码片段中的 < > & 破坏 HTML 结构）
QString htmlEscape(const std::string& s) {
    return QString::fromUtf8(s.c_str()).toHtmlEscaped();
}

/// ARCH-10: 一次性获取 GC 统计快照的便捷包装。
/// 替代 GcManager::instance().xxx() 直接调用，消除面板对 interpreter/GcManager.h 的依赖。
/// 注意：每次调用都会获取 GcManager 内部锁，仅用于 UI 刷新或步骤采样（非热路径）。
GcStatsSnapshot gcStats() {
    return MemoryInspectionAPI::getGcStats();
}

} // namespace

// ============================================================
// GcVisualizerLibrary — 静态教学数据
// ============================================================

const std::vector<GcPhaseRow>& GcVisualizerLibrary::phaseRows() {
    static const std::vector<GcPhaseRow> kRows = {
        {"Idle", "currentPhase_ = Idle", "空闲状态等待 GC 触发", "无操作"},
        {"Marking", "currentPhase_ = Marking", "从根集出发标记所有可达容器节点", "traverse roots → mark alive"},
        {"Sweeping", "currentPhase_ = Sweeping", "遍历 tracked_ 清理不可达的孤岛",
         "refCount==0 && !marked → finalize+free"},
        {"Finalizing", "currentPhase_ = Finalizing", "调用析构钩子清理子引用", "onDestroyed → removeFromAliveSet"},
    };
    return kRows;
}

const std::vector<GcModeRow>& GcVisualizerLibrary::modeRows() {
    static const std::vector<GcModeRow> kRows = {
        {"RefCountOnly", "是", "否", "不 sweep，仅靠 refCount 归零释放"},
        {"RefCountWithCycleGc", "是", "是", "refCount + 周期性 collectCycle 检测孤岛"},
        {"GcOnly", "弱化", "是", "完全依赖 GC 标记清除"},
    };
    return kRows;
}

const std::vector<GcContainerTypeRow>& GcVisualizerLibrary::containerTypeRows() {
    static const std::vector<GcContainerTypeRow> kRows = {
        {"ArrayData", "是", "数组元素可形成循环引用"},
        {"DictData", "是", "字典值可形成循环引用"},
        {"InstanceData", "是", "类字段可形成循环引用"},
        {"TupleData", "是", "元组元素可形成循环引用"},
        {"EnumVariantData", "是", "variant 关联值可形成循环引用"},
        {"ClosureData", "否", "已用 weak_ptr 打破循环，无需 tracked"},
    };
    return kRows;
}

const std::vector<GcScenarioInfo>& GcVisualizerLibrary::scenarios() {
    static const std::vector<GcScenarioInfo> kScenarios = {
        {"简单孤岛",
         "创建两个互相引用的数组，断开外部引用后 GC 回收。"
         "引用计数无法处理循环，需 collectCycle 触发 mark-sweep。",
         "Value a = [];\n"
         "Value b = [];\n"
         "a.push(b);   // a → b\n"
         "b.push(a);   // b → a（循环）\n"
         "// a, b 离开作用域后形成循环孤岛\n"
         "collectCycle([]);  // 空根集\n"
         "// 预期：lastCollectedCount > 0"},
        {"多层循环", "3 个容器形成 A→B→C→A 循环，GC 标记清除回收整条链。",
         "Value a = [];\n"
         "Value b = [];\n"
         "Value c = [];\n"
         "a.push(b);   // A → B\n"
         "b.push(c);   // B → C\n"
         "c.push(a);   // C → A（三层循环）\n"
         "collectCycle([]);\n"
         "// 预期：lastCollectedCount > 0"},
        {"可达保留",
         "创建循环引用但仍有外部根引用，GC 不回收。"
         "展示 mark 阶段从 roots 出发标记可达容器的能力。",
         "Value arr = [];\n"
         "arr.push(arr);  // 自循环\n"
         "roots = [arr.gcRootPtr()];\n"
         "collectCycle(roots);  // arr 作为根标记可达\n"
         "// 预期：lastCollectedCount == 0, trackedCount >= 1"},
        {"增量 GC",
         "多次分配 + GC，观察 trackedCount 变化趋势。"
         "验证 Finalizing 阶段重建 tracked_ 不会无限增长。",
         "for (i = 0; i < 5; i++) {\n"
         "    Value a = [];\n"
         "    a.push(a);  // 自循环\n"
         "    collectCycle([]);  // 每轮回收\n"
         "}\n"
         "// 预期：trackedCount 稳定不增长"},
    };
    return kScenarios;
}

// ============================================================
// GcVisualizerPanel 构造
// ============================================================

GcVisualizerPanel::GcVisualizerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮（统一组件 TeachingSubPageBar）
    subPageBar_ = new TeachingSubPageBar(this);
    auto* theoryPage = new QWidget;
    auto* simulatorPage = new QWidget;
    buildTheoryPage(theoryPage);
    buildSimulatorPage(simulatorPage);
    subPageBar_->addPage(mlTr("① GC 原理与三阶段流程"), theoryPage);
    subPageBar_->addPage(mlTr("② 交互式 GC 模拟器"), simulatorPage);
    pageTheoryBtn_ = subPageBar_->buttonAt(0);
    pageSimulatorBtn_ = subPageBar_->buttonAt(1);
    stack_ = subPageBar_->stack();
    outer->addLayout(subPageBar_->buttonBar());
    outer->addWidget(stack_, 1);
    // 恢复上次子页选择
    subPageBar_->restoreFromSettings(QStringLiteral("teachingPanel/gc-visualizer/subPage"));

    // 底部：加载样例按钮
    auto* bottomBar = new QHBoxLayout;
    auto* loadSampleBtn = new QPushButton(mlTr("加载样例到主编辑器"));
    bottomBar->addStretch();
    bottomBar->addWidget(loadSampleBtn);
    outer->addLayout(bottomBar);

    // 信号连接：子页切换持久化 + 模拟交互
    connect(subPageBar_, &TeachingSubPageBar::currentChanged, this,
            [this](int) { subPageBar_->saveToSettings(QStringLiteral("teachingPanel/gc-visualizer/subPage")); });
    connect(loadSampleBtn, &QPushButton::clicked, this, &GcVisualizerPanel::onLoadSample);
    connect(runBtn_, &QPushButton::clicked, this, &GcVisualizerPanel::onRunSimulation);
    connect(resetBtn_, &QPushButton::clicked, this, &GcVisualizerPanel::onResetGcManager);

    // 初始数据填充
    populateTheory();
    populatePresets();
    refreshStats();
}

// ============================================================
// 子页 1：GC 原理与三阶段流程
// ============================================================

void GcVisualizerPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概念
    theoryBrowser_ = new QTextBrowser;
    theoryBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    layout->addWidget(theoryBrowser_, 2);

    // GC 阶段表
    layout->addWidget(new QLabel(mlTr("<b>GC 三阶段（mark-sweep-finalize）</b>")));
    phaseTable_ = new QTableWidget(0, 4);
    phaseTable_->setHorizontalHeaderLabels({mlTr("阶段"), mlTr("状态字段"), mlTr("作用"), mlTr("关键操作")});
    phaseTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    phaseTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    phaseTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    phaseTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    phaseTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(phaseTable_, 2);

    // GcMode 对比表
    layout->addWidget(new QLabel(mlTr("<b>三种 GcMode 对比</b>")));
    modeTable_ = new QTableWidget(0, 4);
    modeTable_->setHorizontalHeaderLabels({mlTr("模式"), mlTr("引用计数"), mlTr("循环检测"), mlTr("sweep 行为")});
    modeTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    modeTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    modeTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    modeTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    modeTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(modeTable_, 2);

    // 容器类型注册表
    layout->addWidget(new QLabel(mlTr("<b>容器类型注册表（registerTracked）</b>")));
    containerTable_ = new QTableWidget(0, 3);
    containerTable_->setHorizontalHeaderLabels({mlTr("类型"), mlTr("是否 tracked"), mlTr("原因")});
    containerTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    containerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    containerTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    containerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(containerTable_, 1);
}

void GcVisualizerPanel::populateTheory() {
    // 理论概念 HTML
    QString html = QStringLiteral("<h2>GcManager 垃圾回收原理</h2>"
                                  "<p>MiniLang 的堆类型通过侵入式 <b>RefCounted</b> 基类管理引用计数，"
                                  "但引用计数<b>无法回收循环引用</b>（如 a.push(a) 形成的自环）。"
                                  "<b>GcManager</b> 实现轻量级同步<b>mark-sweep</b>，周期性扫描所有 tracked 容器节点，"
                                  "标记从根集可达的对象，释放不可达的循环孤岛。</p>"

                                  "<h3>一、mark-sweep-finalize 三阶段</h3>"
                                  "<p><b>GcManager::collectCycle(roots)</b> 同步阻塞执行三阶段：</p>"
                                  "<ul>"
                                  "<li><b>Phase 1 - Marking</b>：从 roots 出发深度优先 mark 所有可达容器节点。"
                                  "markValue 递归遍历 array / dict / instance / tuple / enum variant / closure 内嵌的"
                                  " Value，marked 集合记录可达对象指针。</li>"
                                  "<li><b>Phase 2 - Sweeping</b>：遍历 tracked_，对 aliveSet_ 仍在但 marked 未命中的"
                                  "对象（即不可达循环孤岛）执行回收。RefCountWithCycleGc 模式清空其子元素打破循环"
                                  "（由后续 refCount 归零释放）；GcOnly 模式直接 delete。</li>"
                                  "<li><b>Phase 3 - Finalizing</b>：重建 tracked_ / aliveSet_，重置 marked 标志，"
                                  "过滤悬垂指针（已析构但尚未 compact 的节点）。</li>"
                                  "</ul>"
                                  "<p>collectCycle 结束后 currentPhase_ 回归 Idle；UI 在 animTimer 周期内观察到的常态"
                                  "即 Idle，lastMarkedCount / lastCollectedCount 反映上次 GC 结果。</p>"

                                  "<h3>二、三种 GcMode</h3>"
                                  "<p>GcManager 支持三种内存管理策略（仅 reset() 后切换安全）：</p>"
                                  "<ul>"
                                  "<li><b>RefCountOnly</b>：纯引用计数，跳过 registerTracked，collectCycle 为 no-op。"
                                  "循环引用会泄漏直至进程退出。基线对比用。</li>"
                                  "<li><b>RefCountWithCycleGc</b>（默认）：引用计数主导 + GC 仅回收循环孤岛。"
                                  "sweep 清空不可达孤岛的子元素打破循环，由后续 refCount 归零释放。项目历史模式。</li>"
                                  "<li><b>GcOnly</b>：GC 主导，sweep 阶段直接 delete 不可达对象。"
                                  "实验性，存在 COW / VM root 限制。</li>"
                                  "</ul>"

                                  "<h3>三、引用计数与 COW 的关系</h3>"
                                  "<p>COW（Copy-On-Write）基于 RefCounted 引用计数，而非 GC：</p>"
                                  "<ul>"
                                  "<li><b>共享</b>：赋值时浅拷贝指针，refCount 加 1，不复制底层数据。</li>"
                                  "<li><b>写前 detach</b>：修改前检查 refCount，若 &gt;1 则复制底层数据（detach），"
                                  "使修改方独占副本。</li>"
                                  "<li><b>COW 克隆也注册 GcManager</b>：detach 后克隆出的新容器会调用"
                                  " registerTracked，否则 COW 路径产生的循环引用不会被回收，导致永久内存泄漏。</li>"
                                  "</ul>"
                                  "<p>引用计数管「正常生命周期」，GcManager 管「引用计数管不到的循环」。"
                                  "——两者协同覆盖所有场景。</p>"

                                  "<h3>教学价值</h3>"
                                  "<p>本面板可视化 MiniLang 内存模型中 GC 的核心概念，帮助理解：</p>"
                                  "<ul>"
                                  "<li>mark-sweep-finalize 三阶段如何协同回收循环引用孤岛</li>"
                                  "<li>三种 GcMode 的差异及适用场景</li>"
                                  "<li>引用计数、COW、GC 三者的分工与协作关系</li>"
                                  "</ul>"
                                  "<p style='color:#666;font-size:small;'>"
                                  "提示：切换到「② 交互式 GC 模拟器」子页，选择预设场景体验 GC 流程。</p>");
    theoryBrowser_->setHtml(html);

    // GC 阶段表
    const auto& phases = GcVisualizerLibrary::phaseRows();
    phaseTable_->setRowCount(static_cast<int>(phases.size()));
    for (int i = 0; i < static_cast<int>(phases.size()); ++i) {
        const auto& p = phases[i];
        phaseTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(p.phase.c_str())));
        phaseTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(p.stateField.c_str())));
        phaseTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(p.role.c_str())));
        phaseTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(p.keyOp.c_str())));
        phaseTable_->item(i, 2)->setToolTip(QString::fromUtf8(p.role.c_str()));
        phaseTable_->item(i, 3)->setToolTip(QString::fromUtf8(p.keyOp.c_str()));
    }

    // GcMode 对比表
    const auto& modes = GcVisualizerLibrary::modeRows();
    modeTable_->setRowCount(static_cast<int>(modes.size()));
    for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
        const auto& m = modes[i];
        modeTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m.modeName.c_str())));
        modeTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(m.refCount.c_str())));
        modeTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(m.cycleDetect.c_str())));
        modeTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(m.sweepBehavior.c_str())));
        modeTable_->item(i, 3)->setToolTip(QString::fromUtf8(m.sweepBehavior.c_str()));
    }

    // 容器类型注册表
    const auto& types = GcVisualizerLibrary::containerTypeRows();
    containerTable_->setRowCount(static_cast<int>(types.size()));
    for (int i = 0; i < static_cast<int>(types.size()); ++i) {
        const auto& t = types[i];
        containerTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(t.typeName.c_str())));
        containerTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(t.tracked.c_str())));
        containerTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(t.reason.c_str())));
        containerTable_->item(i, 2)->setToolTip(QString::fromUtf8(t.reason.c_str()));
    }
}

// ============================================================
// 子页 2：交互式 GC 模拟器
// ============================================================

void GcVisualizerPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：预设场景选择 + 运行 + 重置
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(mlTr("预设场景：")));
    presetCombo_ = new QComboBox;
    presetCombo_->setMinimumWidth(220);
    inputBar->addWidget(presetCombo_);
    runBtn_ = new QPushButton(mlTr("运行模拟"));
    inputBar->addWidget(runBtn_);
    resetBtn_ = new QPushButton(mlTr("重置 GcManager"));
    inputBar->addWidget(resetBtn_);
    inputBar->addStretch();
    layout->addLayout(inputBar);

    // 中间：垂直分割器（上方实时统计表 + 下方模拟过程 HTML）
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：实时统计表（2 列 8 行）
    statsTable_ = new QTableWidget(8, 2);
    statsTable_->setHorizontalHeaderLabels({mlTr("指标"), mlTr("值")});
    statsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    statsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    statsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    statsTable_->setFont(GuiTextUtils::monospaceFont(10));
    // 预置 8 行标签
    statsTable_->setItem(0, 0, new QTableWidgetItem(mlTr("当前阶段")));
    statsTable_->setItem(1, 0, new QTableWidgetItem(mlTr("tracked 节点数")));
    statsTable_->setItem(2, 0, new QTableWidgetItem(mlTr("上次标记数")));
    statsTable_->setItem(3, 0, new QTableWidgetItem(mlTr("上次回收数")));
    statsTable_->setItem(4, 0, new QTableWidgetItem(mlTr("累计 GC 次数")));
    statsTable_->setItem(5, 0, new QTableWidgetItem(mlTr("分配计数")));
    statsTable_->setItem(6, 0, new QTableWidgetItem(mlTr("阈值（默认）")));
    statsTable_->setItem(7, 0, new QTableWidgetItem(mlTr("当前 GcMode")));
    vSplitter->addWidget(statsTable_);

    // 下方：模拟过程 HTML
    simBrowser_ = new QTextBrowser;
    simBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    simBrowser_->setHtml(mlTr("<p style='color:#666;'>选择预设场景，点击「运行模拟」查看 mark-sweep-finalize "
                              "三阶段追踪与回收结果。</p>"));
    vSplitter->addWidget(simBrowser_);

    // 设置分割比例
    vSplitter->setStretchFactor(0, 2);
    vSplitter->setStretchFactor(1, 3);
    layout->addWidget(vSplitter, 1);

    // 底部：单例警告（UX-R2 fix: 迁移到 TeachingTheme 警告卡片色系，替代硬编码 #b8860b）
    warningLabel_ = new QLabel(mlTr("⚠️ 模拟会影响全局 GcManager 单例状态，每次模拟前后自动 reset"));
    warningLabel_->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: %2; padding: 6px 10px; "
                                                "border: 1px solid %3; border-radius: 4px; font-style: italic; }")
                                     .arg(TeachingTheme::warningBg().name(), TeachingTheme::warningText().name(),
                                          TeachingTheme::warningBorder().name()));
    layout->addWidget(warningLabel_);
}

void GcVisualizerPanel::populatePresets() {
    presetCombo_->clear();
    for (const auto& s : GcVisualizerLibrary::scenarios()) {
        presetCombo_->addItem(QString::fromUtf8(s.title.c_str()));
    }
}

void GcVisualizerPanel::refreshStats() {
    if (!statsTable_) {
        return;
    }
    auto setVal = [this](int row, const QString& value) {
        if (auto* item = statsTable_->item(row, 1)) {
            item->setText(value);
        } else {
            statsTable_->setItem(row, 1, new QTableWidgetItem(value));
        }
    };
    // ARCH-10: 通过 MemoryInspectionAPI 一次性获取 GC 统计快照，
    // 消除面板对 interpreter/GcManager.h 的直接依赖（含 7 次单例访问合并为 1 次快照读取）。
    GcStatsSnapshot stats = MemoryInspectionAPI::getGcStats();
    setVal(0, phaseToString(stats.phase));
    setVal(1, QString::number(static_cast<qulonglong>(stats.trackedCount)));
    setVal(2, QString::number(static_cast<qulonglong>(stats.lastMarkedCount)));
    setVal(3, QString::number(static_cast<qulonglong>(stats.lastCollectedCount)));
    setVal(4, QString::number(static_cast<qulonglong>(stats.totalGcCount)));
    setVal(5, QString::number(static_cast<qulonglong>(stats.allocationsSinceLastGc)));
    setVal(6, QString::number(static_cast<qulonglong>(stats.allocationThreshold)));
    setVal(7, modeToString(stats.mode));
}

// ============================================================
// 模拟逻辑 — 4 个预设场景
// ============================================================

GcSimResult GcVisualizerPanel::runScenario(int idx) {
    GcSimResult result;
    const auto& scenarios = GcVisualizerLibrary::scenarios();
    if (idx < 0 || idx >= static_cast<int>(scenarios.size())) {
        result.summary = "无效的场景索引";
        return result;
    }
    result.scenarioTitle = scenarios[idx].title;
    result.scenarioCode = scenarios[idx].codeSnippet;

    // 公共前置：完全重置，避免历史状态污染
    // ARCH-10: 通过 MemoryInspectionAPI 间接调用 GcManager，消除直接依赖
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);

    switch (idx) {
    case 0: {
        // 场景 1：简单孤岛 — 两个互相引用的数组
        result.modeUsed = GcMode::RefCountWithCycleGc;
        {
            auto s = gcStats();
            result.steps.push_back({"reset 后", s.trackedCount, 0, 0, s.totalGcCount});
        }
        {
            Value a(std::vector<Value>{});
            Value b(std::vector<Value>{});
            a.arrayVal().push_back(b); // a → b
            b.arrayVal().push_back(a); // b → a（循环）
            // a, b 离开作用域：refCount 各降 1，但互相引用使 refCount=1 无法归零
        }
        {
            auto s = gcStats();
            result.steps.push_back({"构造循环并离开作用域", s.trackedCount, 0, 0, s.totalGcCount});
        }
        {
            std::vector<const void*> emptyRoots;
            MemoryInspectionAPI::collectCycle(emptyRoots);
        }
        {
            auto s = gcStats();
            result.steps.push_back(
                {"collectCycle(空 roots)", s.trackedCount, s.lastMarkedCount, s.lastCollectedCount, s.totalGcCount});
            result.expectationMet = s.lastCollectedCount > 0;
        }
        result.summary = result.expectationMet ? "✅ 期望达成：循环引用孤岛被 mark-sweep 成功回收，"
                                                 "引用计数无法处理的循环由 GcManager 接管。"
                                               : "❌ 期望未达成：循环引用孤岛未被回收。";
        break;
    }
    case 1: {
        // 场景 2：多层循环 — A→B→C→A 三层循环
        result.modeUsed = GcMode::RefCountWithCycleGc;
        {
            auto s = gcStats();
            result.steps.push_back({"reset 后", s.trackedCount, 0, 0, s.totalGcCount});
        }
        {
            Value a(std::vector<Value>{});
            Value b(std::vector<Value>{});
            Value c(std::vector<Value>{});
            a.arrayVal().push_back(b); // A → B
            b.arrayVal().push_back(c); // B → C
            c.arrayVal().push_back(a); // C → A（三层循环）
            // 离开作用域：三个数组互相引用，refCount=1，无法归零
        }
        {
            auto s = gcStats();
            result.steps.push_back({"构造三层循环并离开作用域", s.trackedCount, 0, 0, s.totalGcCount});
        }
        {
            std::vector<const void*> emptyRoots;
            MemoryInspectionAPI::collectCycle(emptyRoots);
        }
        {
            auto s = gcStats();
            result.steps.push_back(
                {"collectCycle(空 roots)", s.trackedCount, s.lastMarkedCount, s.lastCollectedCount, s.totalGcCount});
            result.expectationMet = s.lastCollectedCount > 0;
        }
        result.summary = result.expectationMet ? "✅ 期望达成：三层循环整条链被 mark-sweep 回收，"
                                                 "mark 阶段从空根集出发无可达对象，sweep 回收全部循环节点。"
                                               : "❌ 期望未达成：多层循环未被回收。";
        break;
    }
    case 2: {
        // 场景 3：可达保留 — 循环引用但仍有外部根
        result.modeUsed = GcMode::RefCountWithCycleGc;
        {
            auto s = gcStats();
            result.steps.push_back({"reset 后", s.trackedCount, 0, 0, s.totalGcCount});
        }
        {
            // arr 在内层作用域持有，作为 root
            Value arr(std::vector<Value>{});
            arr.arrayVal().push_back(arr); // 自循环
            {
                auto s = gcStats();
                result.steps.push_back({"构造自循环（arr 仍存活）", s.trackedCount, 0, 0, s.totalGcCount});
            }
            std::vector<const void*> roots = {arr.gcRootPtr()};
            MemoryInspectionAPI::collectCycle(roots); // arr 作为 root 标记可达
            auto s = gcStats();
            result.steps.push_back(
                {"collectCycle(roots={arr})", s.trackedCount, s.lastMarkedCount, s.lastCollectedCount, s.totalGcCount});
            result.expectationMet = (s.lastCollectedCount == 0 && s.trackedCount >= 1);
            // arr 即将离开作用域：释放后仅剩自循环引用，refCount=1
        }
        // arr 已释放，自循环孤岛仍存活（refCount=1），再次 collectCycle 才回收
        {
            std::vector<const void*> emptyRoots;
            MemoryInspectionAPI::collectCycle(emptyRoots);
        }
        {
            auto s = gcStats();
            result.steps.push_back({"arr 释放后再 collectCycle(空 roots)", s.trackedCount, s.lastMarkedCount,
                                    s.lastCollectedCount, s.totalGcCount});
        }
        result.summary = result.expectationMet
                             ? "✅ 期望达成：作为 root 的循环数组未被 sweep 回收"
                               "（mark 阶段从 roots 出发标记可达容器）；arr 释放后再 collectCycle 才回收。"
                             : "❌ 期望未达成：可达容器被错误回收。";
        break;
    }
    case 3: {
        // 场景 4：增量 GC — 多次分配 + GC，trackedCount 趋势
        result.modeUsed = GcMode::RefCountWithCycleGc;
        {
            auto s = gcStats();
            result.steps.push_back({"reset 后", s.trackedCount, 0, 0, s.totalGcCount});
        }
        for (int i = 0; i < 5; ++i) {
            {
                Value arr(std::vector<Value>{});
                arr.arrayVal().push_back(arr); // 自循环
            }
            std::vector<const void*> emptyRoots;
            MemoryInspectionAPI::collectCycle(emptyRoots);
            auto s = gcStats();
            result.steps.push_back({"第 " + std::to_string(i + 1) + " 轮 collectCycle 后", s.trackedCount,
                                    s.lastMarkedCount, s.lastCollectedCount, s.totalGcCount});
        }
        size_t finalTracked = gcStats().trackedCount;
        result.expectationMet = (finalTracked <= 1);
        result.summary = result.expectationMet
                             ? "✅ 期望达成：5 轮 collectCycle 后 trackedCount 稳定（≤1），"
                               "Finalizing 阶段重建 tracked_ 仅保留存活节点，无悬垂指针残留。"
                             : "❌ 期望未达成：trackedCount 异常增长，Finalizing 阶段未正确清理悬垂指针。";
        break;
    }
    default:
        result.summary = "未知场景索引";
        break;
    }

    // 公共清理：恢复干净状态（reset 不重置 gcMode_，需显式恢复默认）
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);
    return result;
}

void GcVisualizerPanel::renderScenarioResult(const GcSimResult& result) {
    QString monoFamily = GuiTextUtils::monoFontFamilyQss();
    QString html;
    html += QStringLiteral("<html><body style='font-family:%1; font-size:14px;'>").arg(monoFamily);
    html += QStringLiteral("<h2>%1</h2>").arg(htmlEscape(result.scenarioTitle));
    html += QStringLiteral("<p><b>GcMode：</b>%1</p>").arg(modeToString(result.modeUsed));

    if (!result.summary.empty()) {
        QString color = result.expectationMet ? QStringLiteral("#0a7a28") : QStringLiteral("#c0392b");
        html += QStringLiteral("<p style='color:%1;font-weight:bold;'>%2</p>").arg(color, htmlEscape(result.summary));
    }

    // 步骤追踪表
    if (!result.steps.empty()) {
        html += QStringLiteral("<h3>步骤追踪</h3>");
        html += QStringLiteral("<table border='1' cellpadding='6' cellspacing='0' "
                               "style='font-family:%1;font-size:13px;border-collapse:collapse;'>")
                    .arg(monoFamily);
        html += QStringLiteral("<tr style='background:#f0f0f0;'><th>步骤</th><th>trackedCount</th>"
                               "<th>lastMarked</th><th>lastCollected</th><th>totalGc</th></tr>");
        for (const auto& step : result.steps) {
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
                        .arg(htmlEscape(step.label), QString::number(static_cast<qulonglong>(step.trackedCount)),
                             QString::number(static_cast<qulonglong>(step.lastMarked)),
                             QString::number(static_cast<qulonglong>(step.lastCollected)),
                             QString::number(static_cast<qulonglong>(step.totalGc)));
        }
        html += QStringLiteral("</table>");
    }

    // 模拟代码片段
    if (!result.scenarioCode.empty()) {
        html += QStringLiteral("<h3>模拟代码</h3>");
        html += QStringLiteral("<pre style='background:#f7f7f7;padding:8px;border:1px solid #ddd;"
                               "font-family:%1;font-size:12px;white-space:pre-wrap;'>")
                    .arg(monoFamily);
        html += htmlEscape(result.scenarioCode);
        html += QStringLiteral("</pre>");
    }

    html += QStringLiteral("</body></html>");
    simBrowser_->setHtml(html);
}

// ============================================================
// 槽函数
// ============================================================

void GcVisualizerPanel::onRunSimulation() {
    // AUDIT-R5 R7 fix (BUG-2): 模拟器 runScenario 会 reset() 全局 GcManager 单例。
    // 若此时 Worker 线程正在执行用户程序，reset 会清空 tracked_/aliveSet_，
    // 破坏其 collectCycle 的存活判定（可能提前回收 UAF 或统计崩坏）。
    // 运行期拒绝模拟（与 BackendExecutionService 的 AUDIT-R2 P1-5 防护策略对齐）。
    if (controller_ && (controller_->isRunning() || controller_->isVmRunning())) {
        simBrowser_->setHtml(
            mlTr("<p style='color:red;'>程序正在运行，GC 模拟会重置全局 GcManager 单例并破坏运行中的对象跟踪。"
                 "请先停止执行后再运行模拟。</p>"));
        return;
    }
    int idx = presetCombo_->currentIndex();
    if (idx < 0) {
        simBrowser_->setHtml(mlTr("<p style='color:red;'>请先选择一个预设场景</p>"));
        return;
    }
    GcSimResult result = runScenario(idx);
    renderScenarioResult(result);
    refreshStats();
}

void GcVisualizerPanel::onResetGcManager() {
    // AUDIT-R5 R7 fix (BUG-2): 运行期拒绝重置——resetGc 无条件重置全局
    // GcManager 单例，Worker 线程执行期间重置会破坏 collectCycle 的存活判定。
    if (controller_ && (controller_->isRunning() || controller_->isVmRunning())) {
        simBrowser_->setHtml(mlTr("<p style='color:red;'>程序正在运行，不能重置全局 GcManager（会破坏"
                                  "运行中的对象跟踪）。请先停止执行。</p>"));
        return;
    }
    // ARCH-10: 通过 MemoryInspectionAPI 间接调用 GcManager
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);
    refreshStats();
    simBrowser_->setHtml(mlTr("<p style='color:#666;'>GcManager 已重置（reset + 恢复 RefCountWithCycleGc 默认模式），"
                              "tracked / 统计计数 / 阶段均已清零。</p>"));
}

void GcVisualizerPanel::onLoadSample() {
    // 加载一个展示 GC 相关概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// GC（垃圾回收）样例\n"
                                    "// 演示循环引用与 GcManager 的 mark-sweep 回收\n"
                                    "\n"
                                    "// 1. 循环引用：引用计数无法回收\n"
                                    "var a = [];\n"
                                    "a.push(a);            // a 引用自身，refCount=2\n"
                                    "// 离开作用域后 refCount=1（数组元素引用自身），无法自然释放\n"
                                    "// → 需要 GcManager.collectCycle 回收\n"
                                    "\n"
                                    "// 2. 跨容器循环\n"
                                    "var x = [];\n"
                                    "var y = [x];\n"
                                    "x.push(y);            // x ↔ y 双向循环\n"
                                    "\n"
                                    "// 3. 字典循环\n"
                                    "var d = {};\n"
                                    "d[\"self\"] = d;      // d 引用自身\n"
                                    "\n"
                                    "// 4. 闭包通过 weak_ptr 静态打破循环（无需 GC）\n"
                                    "fun makeCounter() {\n"
                                    "  var count = 0;\n"
                                    "  fun inc() {\n"
                                    "    count = count + 1;\n"
                                    "    return count;\n"
                                    "  }\n"
                                    "  return inc;\n"
                                    "}\n"
                                    "\n"
                                    "var c = makeCounter();\n"
                                    "print(c());           // 1\n"
                                    "print(c());           // 2\n"
                                    "// makeCounter 返回后 count 仍存活（闭包持有），但 env 是 weak_ptr\n"
                                    "// 不会形成循环引用\n"
                                    "\n"
                                    "print(\"循环引用容器依赖 GcManager.collectCycle 回收\");\n");
    emit loadSampleRequested(sample);
}

// ============================================================
// createGuidedTour — 新手引导（4 步）
// ============================================================

GuidedTour* GcVisualizerPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageTheoryBtn_, mlTr("引用计数 GC"),
                  mlTr("MiniLang 堆类型采用侵入式引用计数，refCount 归零即释放。"
                       "点击「GC 原理」查看 mark-sweep-finalize 三阶段与三种 GcMode 对比。"));
    tour->addStep(phaseTable_, mlTr("对象图与三阶段"),
                  mlTr("阶段表展示 GC 三阶段（Idle/Marking/Sweeping/Finalizing）的关键操作，"
                       "对应对象图中可达性标记与不可达孤岛回收。"));
    tour->addStep(pageSimulatorBtn_, mlTr("循环引用检测"),
                  mlTr("点击「GC 模拟器」切到交互式子页：引用计数无法回收循环引用，"
                       "需 collectCycle 通过 mark-sweep 检测并回收孤岛。"));
    tour->addStep(runBtn_, mlTr("模拟回收"),
                  mlTr("选择预设场景后点击「运行模拟」，观察 tracked / marked / collected "
                       "统计变化，验证循环引用被正确回收。"));
    return tour;
}
