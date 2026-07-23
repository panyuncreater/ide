#pragma once

// ============================================================
// RegisterAllocatorPanel — 寄存器分配可视化教学面板
// ------------------------------------------------------------
// 可视化寄存器分配（Register Allocation）的核心原理与线性扫描
// 算法，让学习者直观理解变量到寄存器的映射过程。
//
// 与 memory-model 面板的分工：
//   - memory-model 展示寄存器帧布局（R0-R31 的内存视图）
//   - 本面板展示寄存器分配过程（变量如何映射到寄存器/溢出）
//
// 两个子页：
//   1. 寄存器分配原理：理论概览 + 经典算法对比 +
//      MiniLang RegisterVM 寄存器模型
//   2. 寄存器分配模拟器：输入 MiniLang 代码，运行简化版
//      线性扫描算法，可视化变量-寄存器分配结果 + 溢出决策
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
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 静态教学数据结构 ----

/// 寄存器分配算法信息（子页 1 对比表）
struct RegAllocAlgoInfo {
    std::string name;       // 算法名（如 "图着色"）
    std::string complexity; // 复杂度（如 "O(n^2)"）
    std::string pros;       // 优点
    std::string cons;       // 缺点
};

/// MiniLang RegisterVM 寄存器模型条目（子页 1 模型表）
struct MiniLangRegModel {
    std::string conceptName;  // 概念
    std::string miniLangImpl; // MiniLang 实现
    std::string explanation;  // 说明
};

/// 变量分配结果（子页 2 分配表）
struct VarAllocInfo {
    std::string varName;   // 变量名
    std::string regName;   // 分配的寄存器（R0-R31 或 "溢出"）
    std::string liveRange; // 活跃区间（如 "行1-行5"）
    bool spilled;          // 是否溢出
};

/// 寄存器分配场景（子页 2 预设）
struct RegAllocScenario {
    std::string title;                     // 场景标题
    std::string code;                      // 代码
    std::string description;               // 描述
    std::vector<VarAllocInfo> allocations; // 分配结果（分析后填充）
    int activePeak;                        // 活跃峰值
    int spillCount;                        // 溢出数
};

/// RegisterAllocatorLibrary — 静态教学数据
class RegisterAllocatorLibrary {
public:
    static const std::vector<RegAllocAlgoInfo>& algoInfos();
    static const std::vector<MiniLangRegModel>& miniLangModels();
    static const std::vector<RegAllocScenario>& scenarios();
    /// MiniLang RegisterVM 虚拟寄存器数
    static constexpr int kRegisterCount = 32;
};

// ---- 主面板 ----

class RegisterAllocatorPanel : public QWidget {
    Q_OBJECT
public:
    explicit RegisterAllocatorPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onPageSwitch(int idx);
    void onAnalyze();
    void onSelectPreset(int idx);
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageTheoryBtn_ = nullptr;
    QPushButton* pageSimulatorBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：寄存器分配原理
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* algoTable_ = nullptr;     // 经典算法对比表
    QTableWidget* regModelTable_ = nullptr; // MiniLang 寄存器模型表

    // 子页 2：寄存器分配模拟器
    QLineEdit* codeEdit_ = nullptr;         // 代码输入
    QPushButton* analyzeBtn_ = nullptr;     // 分析按钮
    QListWidget* presetList_ = nullptr;     // 预设场景列表
    QTableWidget* varAllocTable_ = nullptr; // 变量-寄存器分配表
    QTableWidget* regUsageTable_ = nullptr; // 寄存器使用情况表（32 行 R0-R31）
    QTextBrowser* detailBrowser_ = nullptr; // 分配详情

    // 当前分析结果
    std::vector<VarAllocInfo> currentAllocations_;
    int currentActivePeak_ = 0;
    int currentSpillCount_ = 0;

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();

    // 模拟器逻辑（简化版线性扫描）
    void analyzeCode(const std::string& code);
    void renderAllocations();
    void renderRegisterUsage();
    void renderDetails();
};
