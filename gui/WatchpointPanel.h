#pragma once

// ============================================================
// WatchpointPanel — 数据断点（Watchpoint）可视化面板（R161）
// ------------------------------------------------------------
// 可视化数据断点（监视变量/字段被修改时暂停）的设置与命中状态。
//
// 两个子页：
//   1. 实时 Watchpoint 列表：消费 IdeController watchpoint API
//      （setWatchpoint/removeWatchpoint/getWatchpoints），显示当前所有
//      watchpoint（类型 / 变量名 / 字段名 / 条件 / 命中次数 / 状态）
//      vmStateChanged 监听器即时刷新 + 2s 安全网
//   2. 教学场景库：6 个典型 watchpoint 场景，含目标 + 条件 + 说明 + 示例代码
//
// 本面板消费 IdeController watchpoint facade API（setWatchpoint /
// removeWatchpoint / clearWatchpoints / getWatchpoints / hasWatchpoints），
// 不直接访问 VmStepper。
//
// 与 BreakpointConditionPanel 设计对齐：
//   - vmStateChanged 监听器即时刷新（OPT-1 模式）
//   - 析构时反注册监听器避免悬垂
//   - 双路径状态守卫（VM 运行 + Interpreter 调试 resume 期间不刷新）
//   - TeachingTheme 主题色 + MarkdownRenderer 散文式说明
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class GuidedTour;

// ---- 教学场景库数据结构 ----

/// Watchpoint 教学场景
struct WatchpointScenario {
    std::string id;               // 场景 ID
    std::string title;            // 显示名
    std::string target;           // 监视目标（如 "x" 或 "obj.field"）
    std::string condition;        // 条件表达式（可选，空=无条件）
    std::string description;      // 文字说明（watchpoint 机制）
    std::string expectedBehavior; // 触发行为描述
    std::string sampleCode;       // 示例代码
};

/// WatchpointLibrary — 静态教学场景库
class WatchpointLibrary {
public:
    static const std::vector<WatchpointScenario>& scenarios();
};

// ---- 主面板 ----

class WatchpointPanel : public QWidget {
    Q_OBJECT
public:
    explicit WatchpointPanel(QWidget* parent = nullptr);
    /// AUDIT-P0 fix: 析构时反注册 IdeController 监听器，避免悬垂回调。
    ~WatchpointPanel() override;

    /// setController 注册 vmStateChanged 监听器，替代 QTimer 轮询。
    /// 监听器在 watchpoint 命中/步进/暂停时即时触发 refreshLive。
    void setController(IdeController* controller);

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时启动 QTimer 安全网，隐藏时停止
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// vmStateChanged 监听回调——仅当面板可见时即时刷新列表
    void onVmStateChanged() {
        if (isVisible()) {
            refreshLive();
        }
    }

    IdeController* controller_ = nullptr;

    // 顶部页面切换
    QPushButton* pageLiveBtn_ = nullptr;
    QPushButton* pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：实时 Watchpoint 列表
    QTableWidget* watchpointTable_ = nullptr; // 6 列：类型 / 变量名 / 字段名 / 条件 / 命中次数 / 状态
    QLabel* liveStatusLabel_ = nullptr;
    QTimer* refreshTimer_ = nullptr;

    // 添加 Watchpoint 控件
    QComboBox* kindCombo_ = nullptr;     // Variable / Field 选择
    QLineEdit* varNameEdit_ = nullptr;   // 变量名输入
    QLineEdit* fieldNameEdit_ = nullptr; // 字段名输入（仅 Field 模式）
    QLineEdit* conditionEdit_ = nullptr; // 条件表达式输入（可选）
    QPushButton* addBtn_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;

    // 子页 2：教学场景库
    QListWidget* scenarioList_ = nullptr;
    QTextBrowser* scenarioDetail_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    /// 构造辅助
    void buildLivePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    /// 刷新实时 watchpoint 列表
    void refreshLive();

    /// 填充教学场景详情
    void populateScenarioDetail(int index);

    /// 当前选中场景的示例代码
    std::string currentSampleCode_;

    /// 添加 watchpoint（从输入控件读取）
    void addWatchpointFromInput();

    /// 根据当前 kind 切换字段名输入控件的可用性
    void updateFieldInputVisibility();
};
