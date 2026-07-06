#pragma once

// ============================================================
// BreakpointConditionPanel — 条件断点可视化面板（第二档 P1-2）
// ------------------------------------------------------------
// 可视化断点条件表达式与命中次数，让学习者理解条件断点的求值机制。
//
// 两个子页：
//   1. 实时断点列表：消费 IdeController 断点查询 API，显示当前所有断点
//      （行号 / 条件表达式 / 命中次数 / 状态），500ms 轮询刷新
//   2. 教学场景库：8 个典型条件断点场景，含条件表达式 + 文字说明
//      + 触发行为描述 + 示例代码
//
// 本面板消费 IdeController 断点查询 API（getBreakpoints /
// getBreakpointCondition / getBreakpointHitCount），不修改引擎层。
//
// 关键架构决策：使用 QTimer 轮询而非订阅 IdeController 信号，
// 避免测试目标链接 IdeController.cpp（与 BytecodeTracePanel 一致）。
// ============================================================

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextBrowser>
#include <QTableWidget>
#include <QTimer>
#include <string>
#include <vector>

class IdeController;

// ---- 教学场景库数据结构 ----

/// 条件断点教学场景
struct BreakpointScenario {
    std::string id;             // 场景 ID
    std::string title;          // 显示名
    std::string condition;       // 条件表达式
    std::string description;     // 文字说明（条件求值机制）
    std::string expectedBehavior;// 触发行为描述（何时暂停）
    std::string sampleCode;      // 示例代码
};

/// BreakpointConditionLibrary — 静态教学场景库
class BreakpointConditionLibrary {
public:
    static const std::vector<BreakpointScenario>& scenarios();
};

// ---- 主面板 ----

class BreakpointConditionPanel : public QWidget {
    Q_OBJECT
public:
    explicit BreakpointConditionPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时启动 QTimer 轮询，隐藏时停止（避免后台空转浪费 CPU）
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    IdeController* controller_ = nullptr;

    // 顶部页面切换
    QPushButton* pageLiveBtn_    = nullptr;
    QPushButton* pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_       = nullptr;

    // 子页 1：实时断点列表
    QTableWidget* breakpointTable_ = nullptr;  // 4 列：行号 / 条件 / 命中次数 / 状态
    QLabel* liveStatusLabel_     = nullptr;
    QTimer* refreshTimer_        = nullptr;

    // 子页 2：教学场景库
    QListWidget* scenarioList_    = nullptr;
    QTextBrowser* scenarioDetail_ = nullptr;

    /// 构造辅助
    void buildLivePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    /// 刷新实时断点列表
    void refreshLive();

    /// 填充教学场景详情
    void populateScenarioDetail(int index);
};
