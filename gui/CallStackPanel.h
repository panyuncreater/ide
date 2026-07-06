#pragma once

// ============================================================
// CallStackPanel — 调用栈可视化面板（第三波 P0-1）
// ------------------------------------------------------------
// 运行期显示函数调用层次（栈帧列表）+ 每帧参数值 + 本地变量 + 返回地址，
// 让学习者直观理解闭包 / upvalue / 递归 / 异常传播 / 方法分派。
//
// 两个子页：
//   1. 实时调用栈：消费 controller_->getVmCallStack() / getDebugCallStack()
//      + 自动刷新（500ms 间隔）+ 帧详情
//   2. 教学场景库：6 个典型调用栈形态（递归 / 闭包 / 异常 / 方法分派等）
//
// 本面板仅消费 IdeController 已有 API + 自带 CallStackLibrary 静态场景库，
// 不修改引擎层。
// ============================================================

#include <QWidget>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>
#include <QListWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 教学场景库数据结构 ----

/// 单个调用栈教学场景
struct CallStackScenario {
    std::string id;                          // 场景 ID
    std::string title;                       // 显示名
    std::string description;                 // 说明
    std::string sourceCode;                  // 触发该调用栈形态的 MiniLang 代码
    std::vector<std::string> expectedFrames; // 期望的栈帧名序列（教学期望）
    std::string teachingNote;                // 教学注解（解释闭包/递归/异常等）
};

/// CallStackLibrary — 静态教学场景库
class CallStackLibrary {
public:
    static const std::vector<CallStackScenario>& scenarios();
};

// ---- 主面板 ----

class CallStackPanel : public QWidget {
    Q_OBJECT
public:
    explicit CallStackPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求加载样例代码到主编辑器
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时恢复自动刷新（若用户已勾选），隐藏时停止 QTimer
    /// 避免 dock 隐藏后定时器持续触发 controller 查询浪费 CPU
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onRefresh();
    void onAutoRefreshToggled(bool checked);
    void onFrameSelected();
    void onScenarioSelected(int index);
    void onLoadScenarioCode();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton*    pageLiveBtn_    = nullptr;
    QPushButton*    pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_          = nullptr;

    // 子页 1：实时调用栈
    QLabel*       liveStatusLabel_  = nullptr;
    QPushButton* refreshBtn_        = nullptr;
    QCheckBox*   autoRefreshCheck_ = nullptr;
    QTreeWidget* stackTree_         = nullptr;
    QTextBrowser* frameDetail_      = nullptr;
    QTimer*       autoTimer_        = nullptr;

    // 子页 2：教学场景库
    QListWidget* scenarioList_     = nullptr;
    QTextBrowser* scenarioDetail_  = nullptr;
    QPushButton* loadCodeBtn_      = nullptr;
    int          currentScenarioIdx_ = -1;

    // 构造辅助
    void buildLivePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    // 数据填充
    void refreshLive();
    void populateScenarios();
    void showScenario(int index);
};
