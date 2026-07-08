#pragma once

// ============================================================
// ClosureInspectorPanel — 闭包检查器面板（第三档 P2-3b）
// ------------------------------------------------------------
// 可视化闭包 upvalue 的捕获/关闭/逃逸机制，让学习者理解
// 闭包如何捕获外层变量、upvalue 的生命周期、以及闭包调用
// 时的变量绑定机制。
//
// 两个子页：
//   1. 闭包机制教学场景库：8 个典型闭包场景，含源码 +
//      捕获变量列表 + upvalue 类型 + 教学注释
//   2. upvalue 生命周期图解：6 个阶段条目，展示 upvalue 的
//      创建/捕获/堆化/关闭/销毁过程
//
// 本面板为纯静态教学面板（Library 静态数据），不依赖
// IdeController，不需要引擎层改造。
// ============================================================

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

// ---- 教学场景库数据结构 ----

/// 闭包教学场景
struct ClosureScenario {
    std::string id;                        // 场景 ID
    std::string title;                     // 显示名
    std::string description;               // 文字说明（闭包机制）
    std::string sampleCode;                // 示例代码
    std::vector<std::string> capturedVars; // 捕获的变量列表
    std::string captureType;               // 捕获类型（by-value / by-reference / upvalue）
    std::string teachingNote;              // 教学注释
};

/// ClosureInspectorLibrary — 静态教学场景库
class ClosureInspectorLibrary {
public:
    static const std::vector<ClosureScenario>& scenarios();
};

// ---- upvalue 生命周期图解条目 ----

/// upvalue 生命周期阶段
struct UpvaluePhaseDoc {
    std::string phase;       // 阶段名
    std::string category;    // 分类（create/capture/heap/close/destroy）
    std::string description; // 文字说明
    std::string stackEffect; // 栈/堆效应
};

/// UpvaluePhaseLibrary — 静态生命周期图解库
class UpvaluePhaseLibrary {
public:
    static const std::vector<UpvaluePhaseDoc>& phases();
};

// ---- 主面板 ----

class ClosureInspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ClosureInspectorPanel(QWidget* parent = nullptr);

signals:
    void loadSampleRequested(const QString& code);

private:
    // 顶部页面切换
    QPushButton* pageScenarioBtn_ = nullptr;
    QPushButton* pagePhaseBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：闭包机制教学场景库
    QListWidget* scenarioList_ = nullptr;
    QTextBrowser* scenarioDetail_ = nullptr;

    // 子页 2：upvalue 生命周期图解
    QListWidget* phaseList_ = nullptr;
    QTextBrowser* phaseDetail_ = nullptr;

    /// 构造辅助
    void buildScenarioPage(QWidget* host);
    void buildPhasePage(QWidget* host);

    /// 填充详情
    void populateScenarioDetail(int index);
    void populatePhaseDetail(int index);
};
