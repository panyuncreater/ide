#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <vector>
#include <tuple>
#include "debug/DebugController.h"

// ============================================================
// DebugPanel 调试面板
// ============================================================

/// 调试面板：变量监视 + 调用栈
/// Round 7: 变量按作用域分层展示（全局/局部/闭包）
class DebugPanel : public QWidget {
    Q_OBJECT

public:
    explicit DebugPanel(QWidget* parent = nullptr);

    /// 更新变量显示（自动按 scope 字段分组）
    void updateVariables(const std::vector<VariableSnapshot>& vars);

    /// 更新调用栈显示
    void updateCallStack(const std::vector<CallStackEntry>& stack);

    /// 清空所有
    void clearAll();

signals:
    /// M5: 选中调用栈帧时请求主窗口跳转到对应源码行
    void gotoLineRequested(int line);

private slots:
    /// 调用栈帧被选中时，显示该帧的局部变量
    void onStackFrameSelected(int index);

private:
    /// 按作用域分组填充变量树
    /// rows: (name, value, scope) — scope 为 "局部"/"外层"/"全局"
    void populateVariableTree(
        const std::vector<std::tuple<QString, QString, QString>>& rows);

    /// 创建作用域分组顶层节点
    QTreeWidgetItem* createScopeGroup(const QString& title, int count);

    /// 集中应用主题色板样式（构造函数与主题切换回调均调用）
    /// 主题切换时通过 Theme::onThemeModeChanged 重新调用此方法刷新
    void applyThemeStyles();

    QTreeWidget* variableTree_ = nullptr;   // 变量监视树
    QLabel* varLabel_ = nullptr;            // 变量监视区标题（保存以便主题刷新）
    QLabel* stackLabel_ = nullptr;          // 调用栈区标题（保存以便主题刷新）
    QListWidget* callStackList_ = nullptr;  // 调用栈列表
    std::vector<CallStackEntry> currentStack_;  // 当前调用栈数据（含局部变量）
};
