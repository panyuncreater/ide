// ============================================================
// WatchPanel.h — 观察表达式面板（R117 调试器拓展）
// ------------------------------------------------------------
// 让学习者在调试暂停期间观察任意表达式的值，理解作用域、求值与
// 沙箱机制。消费 IdeController::evaluateWatchExpression API，
// 不修改引擎层。
//
// 核心能力：
//   1. 表达式表格（4 列：表达式 / 类型 / 值 / 状态）
//      - 表达式列可编辑（双击或回车进入编辑）
//      - 其余列只读，由求值结果填充
//      - 添加/删除/清空按钮
//   2. 表达式持久化：通过 QSettings 保存用户输入的表达式列表，
//      下次启动自动恢复
//   3. 自动刷新：订阅 IdeController::addVmStateChangedListener，
//      在调试暂停/步进/断点命中时即时刷新所有 watch 表达式
//   4. 手动刷新按钮：强制重新求值所有表达式
//   5. 教学场景库：6 个典型 watch 表达式场景，含说明 + 示例代码
//
// 与 VariableInspectorPanel 的区别：
//   - VariableInspector 展示当前作用域的全部变量快照（被动观察）
//   - WatchPanel 允许用户输入任意表达式主动求值（含字段访问、
//     方法调用、算术运算等复合表达式）
// ============================================================
#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
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

/// Watch 表达式教学场景
struct WatchScenario {
    std::string id;           // 场景 ID
    std::string title;        // 显示名
    std::string expression;   // watch 表达式
    std::string description;  // 求值机制说明
    std::string sampleCode;   // 示例代码（含断点提示）
    std::string expectedType; // 预期类型名
};

/// WatchExpressionLibrary — 静态教学场景库
class WatchExpressionLibrary {
public:
    static const std::vector<WatchScenario>& scenarios();
};

// ---- 主面板 ----

class WatchPanel : public QWidget {
    Q_OBJECT
public:
    explicit WatchPanel(QWidget* parent = nullptr);
    ~WatchPanel() override;

    /// 绑定 IDE 控制器（注册 vmStateChanged 监听器）
    void setController(IdeController* controller);

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 信号：请求载入示例代码。
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时启动安全网 QTimer + 首次刷新，隐藏时停止 QTimer
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    /// 添加 watch 表达式（从输入框读取）
    void onAddExpression();
    /// 删除选中行的 watch 表达式
    void onRemoveSelected();
    /// 清空所有 watch 表达式
    void onClearAll();
    /// 手动刷新所有 watch 表达式
    void onRefresh();
    /// 表达式单元格编辑完成（用户修改了表达式）
    void onCellChanged(int row, int col);
    /// 教学场景选中变化
    void onScenarioSelected(int index);
    /// 载入示例代码到编辑器
    void onLoadSampleCode();

private:
    /// OPT-1: vmStateChanged 监听回调——仅当面板可见时即时刷新
    void onVmStateChanged() {
        if (isVisible()) {
            refreshAll();
        }
    }

    IdeController* controller_ = nullptr;

    // 顶部页面切换
    QPushButton* pageLiveBtn_ = nullptr;
    QPushButton* pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：实时 watch 表达式
    QLabel* liveStatusLabel_ = nullptr;
    QLineEdit* exprInput_ = nullptr;
    QPushButton* addBtn_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QTableWidget* watchTable_ = nullptr; // 4 列：表达式 / 类型 / 值 / 状态
    QTimer* refreshTimer_ = nullptr;     // 2s 安全网（vmStateChanged 即时刷新的兜底）
    bool editingGuard_ = false;          // 防止程序化 setItem 触发 onCellChanged

    // 子页 2：教学场景库
    QListWidget* scenarioList_ = nullptr;
    QTextBrowser* scenarioDetail_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;
    int currentScenarioIdx_ = -1;

    // 构造辅助
    void buildLivePage(QWidget* host);
    void buildLibraryPage(QWidget* host);

    // 数据操作
    /// 刷新所有 watch 表达式的求值结果（不修改表达式列）
    void refreshAll();
    /// 持久化当前表达式列表到 QSettings
    void saveExpressions() const;
    /// 从 QSettings 加载表达式列表
    void loadExpressions();
    /// 填充教学场景详情
    void populateScenarioDetail(int index);

    /// 当前选中场景的示例代码（供 loadSampleBtn_ 加载到编辑器）
    std::string currentSampleCode_;
};
