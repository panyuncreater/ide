#pragma once

// ============================================================
// EscapeAnalysisPanel — 逃逸分析可视化教学面板
// ------------------------------------------------------------
// 可视化逃逸分析（Escape Analysis）的核心原理与对象逃逸状态
// 判定，让学习者直观理解栈上分配（Stack Allocation）与
// 标量替换（Scalar Replacement）等 JIT/GC 经典优化技术。
//
// 为 development.md「GC 替代引用计数」拓展项做前置教学：
//   - 展示对象逃逸/不逃逸的判定规则
//   - 展示栈上分配如何避免堆开销与引用计数
//   - MiniLang 现状：所有对象堆分配（RefCounted + COW），无逃逸分析
//
// 两个子页：
//   1. 逃逸分析原理：三种逃逸状态（No/Arg/Global Escape）
//      + 栈上分配 + 标量替换 + MiniLang 实现对照
//   2. 逃逸分析模拟器：输入代码，分析对象逃逸状态与分配策略
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

// ---- 静态教学数据结构 ----

/// 逃逸状态条目（子页 1，三种逃逸状态对比表）
struct EscapeStateInfo {
    std::string name;          // 状态名（如 "No Escape"）
    std::string chineseName;   // 中文名（如 "不逃逸"）
    std::string condition;     // 判定条件
    std::string allocStrategy; // 分配策略
    std::string performance;   // 性能影响
};

/// MiniLang 逃逸分析实现对照条目（子页 1）
struct MiniLangEscapeMapping {
    std::string conceptName;   // 经典概念
    std::string miniLangState; // MiniLang 状态
    std::string explanation;   // 说明
};

/// 逃逸分析模拟器对象分析结果（子页 2）
struct EscapeSimObject {
    std::string name;          // 对象名
    std::string type;          // 类型（数组/字典/类实例等）
    std::string state;         // 逃逸状态（No/Arg/Global Escape）
    std::string allocStrategy; // 分配策略
    std::string reason;        // 原因
};

/// 逃逸分析预设场景（子页 2）
struct EscapeScenario {
    std::string title;                    // 场景标题
    std::string code;                     // 代码
    std::string description;              // 描述
    std::vector<EscapeSimObject> objects; // 对象分析结果
};

/// EscapeAnalysisLibrary — 静态教学数据
class EscapeAnalysisLibrary {
public:
    static const std::vector<EscapeStateInfo>& stateInfos();
    static const std::vector<MiniLangEscapeMapping>& miniLangMappings();
    static const std::vector<EscapeScenario>& scenarios();
    static std::string stateToChinese(const std::string& state);
};

// ---- 主面板 ----

class EscapeAnalysisPanel : public QWidget {
    Q_OBJECT
public:
    explicit EscapeAnalysisPanel(QWidget* parent = nullptr);

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

    // 子页 1：逃逸分析原理
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* stateTable_ = nullptr;   // 三种逃逸状态对比表
    QTableWidget* mappingTable_ = nullptr; // MiniLang 逃逸分析实现对照表

    // 子页 2：逃逸分析模拟器
    QComboBox* presetCombo_ = nullptr;          // 预设场景选择
    QLineEdit* codeEdit_ = nullptr;             // 代码输入
    QPushButton* analyzeBtn_ = nullptr;         // 分析按钮
    QTableWidget* objTable_ = nullptr;          // 对象逃逸分析表
    QTextBrowser* detailBrowser_ = nullptr;     // 逃逸分析详情
    QTextBrowser* suggestionBrowser_ = nullptr; // 优化建议

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();

    // 模拟器逻辑
    void analyzeCode(const std::string& code, std::vector<EscapeSimObject>& out);
    void renderAnalysis(const std::vector<EscapeSimObject>& objects);
    std::string normalizeCode(const std::string& code);
};
