#pragma once

// ============================================================
// MemoryLayoutPanel — 内存布局可视化教学面板
// ------------------------------------------------------------
// 可视化 MiniLang 内存模型的核心概念：
//   - NaN-boxing 8 字节 Value 的位模式
//     （double / bool / null / pointer / array / dict / closure / string）
//   - Copy-On-Write (COW) 写时复制过程
//     （数组 / 字典的引用计数 + 写前 detach）
//   - 闭包 upvalue 捕获链（open / closed upvalue 的内存结构）
//
// 两个子页：
//   1. 内存布局原理：理论概览 + NaN-boxing 类型位模式表
//      + COW 过程表 + upvalue 状态表
//   2. 交互式内存布局模拟器：输入值，展示对应的 NaN-boxing 位模式
//      + 引用计数 + 堆分配状态 + COW 状态，并可视化 8 字节位布局
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学/模拟面板，不运行任何执行引擎
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class TeachingSubPageBar;

// ---- 静态教学数据结构 ----

/// NaN-boxing 类型位模式条目（子页 1，类型位模式表）
struct NaNBoxingTypeInfo {
    std::string typeName;    // 类型名（如 double / bool / null / pointer / array ...）
    std::string tag;         // 高16位 tag（如 0x7FF9 / 0x7FFA / 0x7FFB）
    std::string bitPattern;  // 位模式示例（如 0x7FF9000000000001）
    std::string description; // 说明
};

/// COW 过程条目（子页 1，COW 过程表）
struct CowProcessInfo {
    std::string operation;   // 操作（如「创建 arr1」「赋值 arr2 = arr1」）
    std::string refcount;    // 引用计数
    std::string detach;      // 是否 detach
    std::string description; // 说明
};

/// upvalue 状态条目（子页 1，upvalue 状态表）
struct UpvalueStateInfo {
    std::string state;           // 状态（open / closed）
    std::string memoryStructure; // 内存结构
    std::string description;     // 说明
};

/// 内存布局模拟器字段（子页 2，值类型分析表）
struct MemorySimField {
    std::string field; // 字段名（如 类型 / 位模式 / 引用计数 ...）
    std::string value; // 值
};

/// 内存布局预设场景（子页 2）
struct MemoryScenario {
    std::string title;                  // 场景标题
    std::string input;                  // 输入值
    std::string description;            // 描述
    std::vector<MemorySimField> fields; // 分析结果
    std::string bitPattern;             // 64 位十六进制位模式
    bool isPointer = false;             // 是否为 pointer tag
};

/// MemoryLayoutLibrary — 静态教学数据
class MemoryLayoutLibrary {
public:
    static const std::vector<NaNBoxingTypeInfo>& typeInfos();
    static const std::vector<CowProcessInfo>& cowProcesses();
    static const std::vector<UpvalueStateInfo>& upvalueStates();
    static const std::vector<MemoryScenario>& scenarios();
};

// ---- 主面板 ----

class MemoryLayoutPanel : public QWidget {
    Q_OBJECT
public:
    explicit MemoryLayoutPanel(QWidget* parent = nullptr);

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

    // UX-R2 fix: 子页切换统一为 TeachingSubPageBar 组件（替代手写 2 按钮互斥逻辑）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QStackedWidget* stack_ = nullptr; // 由 TeachingSubPageBar 持有

    // 子页 1：内存布局原理
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* typeTable_ = nullptr;    // NaN-boxing 类型位模式表
    QTableWidget* cowTable_ = nullptr;     // COW 过程表
    QTableWidget* upvalueTable_ = nullptr; // upvalue 状态表

    // 子页 2：交互式内存布局模拟器
    QComboBox* presetCombo_ = nullptr;      // 预设场景选择
    QLineEdit* valueEdit_ = nullptr;        // 自定义值输入
    QPushButton* analyzeBtn_ = nullptr;     // 分析按钮
    QTableWidget* fieldTable_ = nullptr;    // 值类型分析表
    QTextBrowser* layoutBrowser_ = nullptr; // 内存布局可视化

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();

    // 模拟器逻辑
    void analyzeValue(const std::string& input, std::vector<MemorySimField>& out, std::string& bitPattern,
                      bool& isPointer);
    void renderAnalysis(const std::vector<MemorySimField>& fields, const std::string& bitPattern, bool isPointer);
};
