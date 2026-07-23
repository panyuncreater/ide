#pragma once

// ============================================================
// LintExplorerPanel — 静态分析探索器教学面板
// ------------------------------------------------------------
// 可视化 R162 lint 静态分析的 8 条规则触发与诊断结果。面板内独立
// 完成 Lexer → Parser → LintPass 三步管线（不依赖 IdeController，
// 也不依赖 cli/lint_core.cpp——在面板内复刻 lintSource 三步逻辑）。
//
// 两个子页：
//   1. Lint 规则说明：8 条规则列表（emoji + 标题）+ 选中规则的
//      触发条件 / 错误消息模板 / 示例代码 / 修复建议
//   2. 交互式 Lint 分析：源码编辑器 + 运行按钮 + 8 个规则开关 +
//      圈复杂度阈值 + 5 个预设样例 + 诊断列表表格 + 诊断详情
//
// 本面板仅消费自带规则/样例文档库 + 面板内 LintPass 产物，不修改
// 引擎层，不注册 vmStateChanged 监听器。
// ============================================================

#include "common/Diagnostic.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>

#include <string>
#include <vector>

class IdeController;
class GuidedTour;
class TeachingSubPageBar;

// ---- 教学文档数据结构 ----

/// 单条 Lint 规则的教学说明
struct LintRuleInfo {
    std::string code;    // 规则码，如 "lint-unused-variable"
    std::string name;    // 规则类名，如 "UnusedVariable"
    std::string title;   // 中文标题，如 "未使用变量"
    std::string emoji;   // 图标 emoji
    std::string trigger; // 触发条件说明
    std::string message; // 错误消息模板
    std::string example; // 触发示例代码
    std::string fix;     // 修复建议
};

/// LintRuleLibrary — 8 条 Lint 规则的静态教学文档库
class LintRuleLibrary {
public:
    /// 返回 8 条规则的静态文档数据（顺序与 minilang::lint::LintRule 枚举一致）
    static const std::vector<LintRuleInfo>& rules();

    /// 按规则码查找规则文档（未找到返回 nullptr）
    static const LintRuleInfo* findByCode(const std::string& code);
};

/// 单个预设样例
struct LintSample {
    std::string title;       // 样例标题
    std::string code;        // 样例源码
    std::string description; // 触发规则说明
};

/// LintSampleLibrary — 5 个预设样例的静态文档库
class LintSampleLibrary {
public:
    /// 返回 5 个预设样例
    static const std::vector<LintSample>& samples();
};

// ---- 主面板 ----

class LintExplorerPanel : public QWidget {
    Q_OBJECT
public:
    explicit LintExplorerPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 StepExplainerPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

    /// 创建该面板的新手引导（4 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRunLint();
    void onRuleSelected(int row);
    void onDiagnosticSelected();
    void onLoadSample();
    void onSampleClicked(int index);

private:
    IdeController* controller_ = nullptr;

    // 子页切换（统一组件 TeachingSubPageBar）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    // 保留 pageRulesBtn_/pageInteractiveBtn_ 指针供 GuidedTour 定位（指向 subPageBar_ 内部按钮）
    QPushButton* pageRulesBtn_ = nullptr;
    QPushButton* pageInteractiveBtn_ = nullptr;

    // 子页 1：Lint 规则说明
    QListWidget* ruleList_ = nullptr;
    QTextBrowser* ruleDetail_ = nullptr;

    // 子页 2：交互式 Lint 分析
    QTextEdit* sourceEdit_ = nullptr;
    QPushButton* runLintBtn_ = nullptr;
    std::vector<QCheckBox*> ruleChecks_; // 索引即 LintRule 枚举值（0..7）
    QSpinBox* complexitySpin_ = nullptr;
    std::vector<QPushButton*> sampleButtons_;
    QTableWidget* diagTable_ = nullptr;
    QTextBrowser* diagDetail_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    // 上次分析的诊断副本（供选中行时展示详情）
    std::vector<Diagnostic> lastDiagnostics_;

    // 构造辅助
    void buildRulesPage(QWidget* host);
    void buildInteractivePage(QWidget* host);

    // 数据填充
    void populateRules();
    void showRule(int index);
};
