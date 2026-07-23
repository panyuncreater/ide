#pragma once

// ============================================================
// GcVisualizerPanel — GC 垃圾回收可视化教学面板
// ------------------------------------------------------------
// 可视化 GcManager 的 mark-sweep-finalize 三阶段垃圾回收流程、
// 三种 GcMode（RefCountOnly / RefCountWithCycleGc / GcOnly）、
// 引用计数与 COW 的关系。
//
// 纯教学/模拟面板（不运行执行引擎），但实时读取 GcManager 单例统计。
//
// 两个子页：
//   1. GC 原理与三阶段流程：HTML 文档 + 阶段表 + GcMode 对比表 + 容器注册表
//   2. 交互式 GC 模拟器：4 个预设场景（纯 C++ 操作 Value + GcManager 单例）
//      + 实时统计表 + 模拟过程 HTML
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学/模拟面板，不运行任何执行引擎
//   - GcManager 是进程级单例，模拟会影响全局状态（每次模拟前后 reset）
//   - 字体使用 GuiTextUtils::monospaceFont(N) 工厂
//   - 静态教学数据用 MemoryLayoutLibrary 模式（static const 局部变量）
// ============================================================

#include "interpreter/GcManager.h" // GcMode, GcPhase 枚举
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class GuidedTour;
class TeachingSubPageBar;

// ---- 静态教学数据结构 ----

/// GC 阶段表条目（子页 1，4 行：Idle / Marking / Sweeping / Finalizing）
struct GcPhaseRow {
    std::string phase;      // 阶段名（如 "Idle" / "Marking"）
    std::string stateField; // 状态字段（如 "currentPhase_ = Marking"）
    std::string role;       // 作用描述
    std::string keyOp;      // 关键操作
};

/// GcMode 对比表条目（子页 1，3 行：RefCountOnly / RefCountWithCycleGc / GcOnly）
struct GcModeRow {
    std::string modeName;      // 模式名
    std::string refCount;      // 引用计数（"是" / "弱化"）
    std::string cycleDetect;   // 循环检测（"是" / "否"）
    std::string sweepBehavior; // sweep 行为描述
};

/// 容器类型注册表条目（子页 1，6 行：ArrayData / DictData / InstanceData / TupleData /
///                       EnumVariantData / ClosureData）
struct GcContainerTypeRow {
    std::string typeName; // 类型名
    std::string tracked;  // 是否注册 tracked（"是" / "否"）
    std::string reason;   // 原因说明
};

/// GC 模拟预设场景元信息（子页 2，4 个场景）
struct GcScenarioInfo {
    std::string title;       // 场景标题
    std::string description; // 场景描述
    std::string codeSnippet; // 模拟代码片段（用于展示，实际由 C++ lambda 执行）
};

// ---- 模拟运行结果（子页 2 渲染用）----

/// 单步追踪：每次 collectCycle 后捕获的统计快照
struct GcSimStep {
    std::string label;        // 步骤标签
    size_t trackedCount = 0;  // 当前 tracked 节点数
    size_t lastMarked = 0;    // 上次 GC 标记的可达节点数
    size_t lastCollected = 0; // 上次 GC 回收的孤岛数
    size_t totalGc = 0;       // 累计 GC 次数
};

/// 模拟运行结果
struct GcSimResult {
    std::string scenarioTitle;                     // 场景标题
    std::string scenarioCode;                      // 场景代码片段
    GcMode modeUsed = GcMode::RefCountWithCycleGc; // 使用的 GcMode
    std::vector<GcSimStep> steps;                  // 步骤追踪
    std::string summary;                           // 总结（含期望是否达成）
    bool expectationMet = false;                   // 期望是否达成
};

/// GcVisualizerLibrary — 静态教学数据
class GcVisualizerLibrary {
public:
    static const std::vector<GcPhaseRow>& phaseRows();
    static const std::vector<GcModeRow>& modeRows();
    static const std::vector<GcContainerTypeRow>& containerTypeRows();
    static const std::vector<GcScenarioInfo>& scenarios();
};

// ---- 主面板 ----

class GcVisualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit GcVisualizerPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

    /// 创建该面板的新手引导（4 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRunSimulation();
    void onResetGcManager();
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 子页切换（统一组件 TeachingSubPageBar）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    // 保留 pageTheoryBtn_/pageSimulatorBtn_ 指针供 GuidedTour 定位（指向 subPageBar_ 内部按钮）
    QPushButton* pageTheoryBtn_ = nullptr;
    QPushButton* pageSimulatorBtn_ = nullptr;

    // 子页 1：GC 原理与三阶段流程
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* phaseTable_ = nullptr;     // GC 阶段表（4 列 4 行）
    QTableWidget* modeTable_ = nullptr;      // GcMode 对比表（4 列 3 行）
    QTableWidget* containerTable_ = nullptr; // 容器类型注册表（3 列 6 行）

    // 子页 2：交互式 GC 模拟器
    QComboBox* presetCombo_ = nullptr;   // 预设场景选择
    QPushButton* runBtn_ = nullptr;      // 运行模拟按钮
    QPushButton* resetBtn_ = nullptr;    // 重置 GcManager 按钮
    QTableWidget* statsTable_ = nullptr; // 实时统计表（2 列 8 行）
    QTextBrowser* simBrowser_ = nullptr; // 模拟过程 HTML
    QLabel* warningLabel_ = nullptr;     // 单例警告标签

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();
    void refreshStats(); // 从 GcManager 单例读取实时统计

    // 模拟逻辑
    GcSimResult runScenario(int idx);
    void renderScenarioResult(const GcSimResult& result);
};
