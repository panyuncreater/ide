#pragma once

// ============================================================
// BreakpointConditionPanel — 条件断点可视化面板（第二档 P1-2）
// ------------------------------------------------------------
// 可视化断点条件表达式与命中次数，让学习者理解条件断点的求值机制。
//
// 两个子页：
//   1. 实时断点列表：消费 IdeController 断点查询 API，显示当前所有断点
//      （行号 / 条件表达式 / 命中次数 / 状态），vmStateChanged 监听器即时刷新 + 2s 安全网
//   2. 教学场景库：8 个典型条件断点场景，含条件表达式 + 文字说明
//      + 触发行为描述 + 示例代码
//
// 本面板消费 IdeController 断点查询 API（getBreakpoints /
// getBreakpointCondition / getBreakpointHitCount），不修改引擎层。
//
// OPT-1: 原设计使用 QTimer 500ms 轮询避免订阅 IdeController Qt 信号（信号需 moc，
// 测试目标不链接 IdeController.cpp）。现改为纯 C++ 观察者（addVmStateChangedListener
// std::function 回调，头文件内联无 moc 依赖），状态变更即时刷新，QTimer 降为 2s 安全网。
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
class GuidedTour;

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

    // OPT-1: setController 注册 vmStateChanged 监听器，替代 500ms QTimer 轮询。
    // 监听器在断点变化/步进/暂停时即时触发 refreshLive，让命中次数显示零延迟。
    // 实现移至 .cpp（调用 addVmStateChangedListener 需 IdeController 完整类型定义）
    void setController(IdeController* controller);

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时启动 QTimer 轮询，隐藏时停止（避免后台空转浪费 CPU）
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// OPT-1: vmStateChanged 监听回调——仅当面板可见时即时刷新断点列表。
    void onVmStateChanged() {
        if (isVisible()) {
            refreshLive();
        }
    }

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
    QPushButton* loadSampleBtn_   = nullptr;  // 加载示例代码到编辑器

    /// 构造辅助
    void buildLivePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    /// 刷新实时断点列表
    void refreshLive();

    /// 填充教学场景详情
    void populateScenarioDetail(int index);

    /// 当前选中场景的示例代码（供 loadSampleBtn_ 加载到编辑器）
    std::string currentSampleCode_;
};
