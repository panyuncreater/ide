#pragma once

// ============================================================
// ModuleSystemVisualizerPanel — 模块系统可视化教学面板
// ------------------------------------------------------------
// 可视化 MiniLang 模块系统的核心机制：import/export 语义、路径解析、
// 模块缓存、循环依赖检测、预扫描阶段（FunDecl/ExportStmt）、错误传播链。
//
// 模块系统是 MiniLang 的高频 Bug 模式（路径安全、循环依赖、预扫描遗漏
// FunDecl/ExportStmt、错误传播），本面板以预设数据 + 交互式模拟器方式
// 教学这些概念，不运行执行引擎。
//
// 两个子页：
//   1. 模块系统原理：HTML 文档 + 概念表 + 路径解析规则表 + 预扫描清单
//   2. 交互式模块依赖模拟器：4 个预设场景（正常导入链 / 循环依赖 /
//      路径解析失败 / 预扫描遗漏）+ 加载顺序时间轴 + HTML 依赖图与日志
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学/模拟面板，不运行任何执行引擎
//   - 字体使用 GuiTextUtils::monospaceFont(N) 工厂
//   - 静态教学数据用 static const 局部变量模式（参考 GcVisualizerPanel）
// ============================================================

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
class TeachingSubPageBar;
class GuidedTour;

// ---- 静态教学数据结构 ----

/// 概念表条目（子页 1，3 列：概念 / 说明 / 关键代码位置）
struct ModuleConceptRow {
    std::string conceptName; // 避开 C++20 concept 关键字
    std::string description;
    std::string keyLocation;
};

/// 路径解析规则条目（子页 1，3 列：输入 / 解析结果 / 说明）
struct PathResolutionRow {
    std::string input;
    std::string resolved;
    std::string note;
};

/// 预扫描清单条目（子页 1，2 列：节点类型 / 是否预扫描收集；description 作为 tooltip）
struct PrescanCheckRow {
    std::string nodeType;
    std::string prescanned;  // "是" / "否"
    std::string description; // 详细说明，作为单元格 tooltip 显示
};

/// 预设场景元信息（子页 2，4 个场景）
struct ModuleScenarioInfo {
    std::string title;
    std::string description;
    std::string codeSnippet; // 展示用代码片段
};

/// 模拟步骤（时间轴条目）
struct ModuleSimStep {
    int step = 0;
    std::string module;
    std::string operation; // "开始加载" / "预扫描" / "执行" / "检测到循环" / "路径解析失败"
    std::string status;    // "成功" / "失败" / "循环" / "跳过(已缓存)"
};

/// 模拟结果
struct ModuleSimResult {
    std::string scenarioTitle;
    std::string scenarioCode;
    std::vector<ModuleSimStep> steps;
    std::string summary;
    bool success = false;
    std::string dependencyGraphHtml; // HTML 渲染的依赖图
};

/// ModuleSystemLibrary — 静态教学数据
class ModuleSystemLibrary {
public:
    static const std::vector<ModuleConceptRow>& conceptRows();
    static const std::vector<PathResolutionRow>& pathResolutionRows();
    static const std::vector<PrescanCheckRow>& prescanCheckRows();
    static const std::vector<ModuleScenarioInfo>& scenarios();
};

// ---- 主面板 ----

class ModuleSystemVisualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit ModuleSystemVisualizerPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

    /// 创建该面板的新手引导（4 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRunSimulation();
    void onLoadSample();
    void onSwitchPage(int idx);

private:
    IdeController* controller_ = nullptr;

    // 子页切换（统一组件 TeachingSubPageBar）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    // 保留 pageTheoryBtn_/pageSimulatorBtn_ 指针供 GuidedTour 定位（指向 subPageBar_ 内部按钮）
    QPushButton* pageTheoryBtn_ = nullptr;
    QPushButton* pageSimulatorBtn_ = nullptr;

    // 子页 1：模块系统原理
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* conceptTable_ = nullptr; // 概念表（3 列 6-8 行）
    QTableWidget* pathTable_ = nullptr;    // 路径解析规则表（3 列 5-6 行）
    QTableWidget* prescanTable_ = nullptr; // 预扫描清单表（2 列 4-5 行）

    // 子页 2：交互式模块依赖模拟器
    QComboBox* presetCombo_ = nullptr;      // 预设场景选择
    QPushButton* runBtn_ = nullptr;         // 运行模拟按钮
    QTableWidget* timelineTable_ = nullptr; // 模块加载顺序时间轴（4 列）
    QTextBrowser* simBrowser_ = nullptr;    // 模拟过程详情（HTML 依赖图 + 日志）
    QPushButton* loadToMainBtn_ = nullptr;  // 加载样例代码到主编辑器

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();
    void populatePresets();

    // 模拟逻辑
    ModuleSimResult runScenario(int idx);
    void renderScenarioResult(const ModuleSimResult& result);
};
