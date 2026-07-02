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

    QTreeWidget* variableTree_ = nullptr;   // 变量监视树
    QListWidget* callStackList_ = nullptr;  // 调用栈列表
    std::vector<CallStackEntry> currentStack_;  // 当前调用栈数据（含局部变量）
};
