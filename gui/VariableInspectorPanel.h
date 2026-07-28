/**
 * @file VariableInspectorPanel.h
 * @brief 变量检视面板的类声明（功能：变量类型与取值可视化）
 *
 * 分为实时变量页与类型示例库页；支持自动/手动刷新。
 */
#pragma once

// ============================================================
// VariableInspectorPanel — 变量检查器面板（第三波 P0-2）
// ------------------------------------------------------------
// 树形展示当前作用域变量（含类型注解 + NaN-boxing 位 + 引用计数）+ 持续监视，
// 让学习者直观理解作用域链 / 类型注解 / NaN-boxing 编码 / 引用计数。
//
// 两个子页：
//   1. 实时变量树：消费 controller_->getDebugVariableSnapshot() / getVmGlobals()
//      + 按作用域分组（global / local / upvalue）+ 选中变量详情
//   2. 教学场景库：8 种变量类型示例（int/float/bool/null/string/array/dict/instance/closure）
//
// 本面板仅消费 IdeController 已有 API + 自带 VariableInspectorLibrary 静态场景库，
// 不修改引擎层。
// ============================================================

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class GuidedTour;

// ---- 教学场景库数据结构 ----

/// 单个变量类型示例
struct VariableTypeExample {
    std::string id;           // 示例 ID
    std::string typeName;     // 类型名（int/float/bool/null/string/array/dict/instance/closure）
    std::string displayName;  // 显示名
    std::string sourceExpr;   // 表达式源码
    std::string valueRepr;    // 值表示（toString 格式）
    std::string nanboxBits;   // NaN-boxing 位（hex，标量）
    std::string heapLayout;   // 堆布局说明（堆类型，描述 RefCounted 结构）
    std::string teachingNote; // 教学注解
};

/// VariableInspectorLibrary — 静态教学场景库
class VariableInspectorLibrary {
public:
    /// 返回变量类型示例库（静态数据）。
    static const std::vector<VariableTypeExample>& examples();
};

// ---- 主面板 ----

class VariableInspectorPanel : public QWidget {
    Q_OBJECT
public:
    /// 构造变量检视面板；parent 为父控件。
    explicit VariableInspectorPanel(QWidget* parent = nullptr);
    // AUDIT-P0 fix: 析构时反注册 IdeController 监听器。
    ~VariableInspectorPanel() override;

    // OPT-1: setController 注册 vmStateChanged 监听器，替代 500ms QTimer 轮询。
    // 实现移至 .cpp（调用 addVmStateChangedListener 需 IdeController 完整类型定义）
    /// 绑定 IDE 控制器。
    void setController(IdeController* controller);

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 信号：请求载入示例代码。
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时恢复自动刷新（若用户已勾选），隐藏时停止 QTimer
    void showEvent(QShowEvent* event) override;
    /// 隐藏事件：停止自动刷新。
    void hideEvent(QHideEvent* event) override;

private slots:
    /// 手动刷新实时变量。
    void onRefresh();
    /// 自动刷新开关切换。
    void onAutoRefreshToggled(bool checked);
    /// 变量选中项变化回调。
    void onVariableSelected();
    /// 拓展二期：双击变量项修改值（仅调试暂停/VM 暂停时生效）。
    void onVariableDoubleClicked(class QTreeWidgetItem* item, int column);
    /// 示例选中项变化回调。
    void onExampleSelected(int index);
    /// 载入示例代码到编辑器。
    void onLoadExampleCode();

private:
    /// OPT-1: vmStateChanged 监听回调——仅当面板可见且开启自动刷新时即时刷新。
    void onVmStateChanged() {
        if (isVisible() && autoRefreshCheck_ && autoRefreshCheck_->isChecked()) {
            refreshLive();
        }
    }

    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageLiveBtn_ = nullptr;
    QPushButton* pageLibraryBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：实时变量树
    QLabel* liveStatusLabel_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QTreeWidget* varTree_ = nullptr;
    QTextBrowser* varDetail_ = nullptr;
    QTimer* autoTimer_ = nullptr;

    // 子页 2：教学场景库
    QListWidget* exampleList_ = nullptr;
    QTextBrowser* exampleDetail_ = nullptr;
    QPushButton* loadCodeBtn_ = nullptr;
    int currentExampleIdx_ = -1;

    // 构造辅助
    /// 构建实时变量子页。
    void buildLivePage(QWidget* host);
    /// 构建类型示例库子页。
    void buildLibraryPage(QWidget* host);

    // 数据填充
    /// 刷新实时变量展示。
    void refreshLive();
    /// 填充示例库列表。
    void populateExamples();
    /// 展示指定示例。
    void showExample(int index);
};
