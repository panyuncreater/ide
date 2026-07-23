#pragma once

// ============================================================
// InlineCachePanel — 内联缓存可视化教学面板（教学板块拓展 4/4）
// ------------------------------------------------------------
// 可视化内联缓存（Inline Cache, IC）的核心原理与状态转换，让
// 学习者直观理解方法分发/类型分派的优化机制。
//
// 与 jit-visualizer 面板的分工：
//   - jit-visualizer 侧重"运行真实 JIT 采集类型反馈数据"
//   - 本面板侧重"IC 原理教学 + 交互式状态转换模拟器"，
//     不运行 JIT，纯教学/模拟
//
// 两个子页：
//   1. IC 原理与三态：monomorphic / polymorphic / megamorphic
//      三态说明 + MiniLang JIT 的 IC 实现对照（R152/R153
//      类型反馈与特化作为 IC 的等价形式）
//   2. IC 状态模拟器：输入类型序列（如 int,int,float,string），
//      逐步可视化 IC 状态转换 + 缓存内容 + 命中/未命中统计
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学/模拟面板，不运行任何执行引擎
// ============================================================

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 静态教学数据结构 ----

/// IC 三态条目（子页 1）
struct IcStateInfo {
    std::string name;        // 状态名（如 "Monomorphic"）
    std::string chineseName; // 中文名（如 "单态"）
    std::string threshold;   // 观测类型数阈值（如 "1 种"）
    std::string strategy;    // 分发策略（如 "直接内联/直接跳转"）
    std::string speed;       // 相对速度（如 "最快"）
    std::string description; // 详细说明
};

/// MiniLang IC 实现对照条目（子页 1）
struct MiniLangIcMapping {
    std::string icConcept;    // 经典 IC 概念（如 "类型反馈"）
    std::string miniLangImpl; // MiniLang 实现（如 "R152 per-chunk typeFeedback"）
    std::string version;      // 引入版本（如 "R152"）
    std::string explanation;  // 说明
};

/// IC 状态枚举（模拟器用）
enum class IcSimState {
    Uninitialized, // 未初始化（首次调用前）
    Monomorphic,   // 单态（缓存 1 种类型）
    Polymorphic,   // 多态（缓存 2-4 种类型）
    Megamorphic    // 超多态（缓存 5+ 种类型，放弃缓存）
};

/// IC 模拟器单步记录（子页 2）
struct IcSimStep {
    int stepNumber;           // 步骤号（从 1 开始）
    std::string observedType; // 本次观测到的类型
    IcSimState stateBefore;   // 转换前状态
    IcSimState stateAfter;    // 转换后状态
    bool hit;                 // 是否命中缓存
    std::string cacheContent; // 转换后缓存内容（如 "int" / "int,float" / "溢出"）
    std::string note;         // 说明（如 "首次观测，进入单态"）
};

/// InlineCacheLibrary — 静态教学数据
class InlineCacheLibrary {
public:
    static const std::vector<IcStateInfo>& stateInfos();
    static const std::vector<MiniLangIcMapping>& miniLangMappings();
    static std::string stateToString(IcSimState s);
    static std::string stateToChinese(IcSimState s);
    /// 经典 IC 多态阈值（>=5 进入 megamorphic）
    static constexpr int kPolymorphicMax = 4;
};

// ---- 主面板 ----

class InlineCachePanel : public QWidget {
    Q_OBJECT
public:
    explicit InlineCachePanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onPageSwitch(int idx);
    void onRunSimulation();
    void onStepForward();
    void onResetSimulation();
    void onSelectPresetSequence(int idx);
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageTheoryBtn_ = nullptr;
    QPushButton* pageSimulatorBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：IC 原理与三态
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* stateTable_ = nullptr;   // 三态对比表
    QTableWidget* mappingTable_ = nullptr; // MiniLang IC 实现对照表

    // 子页 2：IC 状态模拟器
    QLineEdit* typeSeqEdit_ = nullptr;     // 类型序列输入（如 int,int,float,string）
    QPushButton* runBtn_ = nullptr;        // 运行完整模拟
    QPushButton* stepBtn_ = nullptr;       // 单步执行
    QPushButton* resetBtn_ = nullptr;      // 重置
    QListWidget* presetList_ = nullptr;    // 预设类型序列
    QTableWidget* simStepTable_ = nullptr; // 模拟步骤表
    QTextBrowser* simSummary_ = nullptr;   // 汇总（命中率/最终状态）
    QLabel* currentStateLabel_ = nullptr;  // 当前 IC 状态

    // 模拟器内部状态
    IcSimState simState_ = IcSimState::Uninitialized;
    std::vector<std::string> simCache_;     // 当前缓存类型列表
    std::vector<IcSimStep> simSteps_;       // 已执行步骤
    std::vector<std::string> pendingTypes_; // 待执行类型序列（单步用）
    int simTotalCalls_ = 0;
    int simHits_ = 0;

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();

    // 模拟器逻辑
    IcSimStep simulateOneStep(const std::string& observedType);
    void renderSimSteps();
    void renderSimSummary();
    void parseTypeSequence(const std::string& text, std::vector<std::string>& out);
    std::string formatCache(const std::vector<std::string>& cache);
};
