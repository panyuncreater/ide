#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <vector>
#include "debug/DebugController.h"

// ============================================================
// DebugPanel 调试面板
// ============================================================

/// 调试面板：变量监视 + 调用栈
class DebugPanel : public QWidget {
    Q_OBJECT

public:
    explicit DebugPanel(QWidget* parent = nullptr);

    /// 更新变量显示
    void updateVariables(const std::vector<VariableSnapshot>& vars);

    /// 更新调用栈显示
    void updateCallStack(const std::vector<CallStackEntry>& stack);

    /// 清空所有
    void clearAll();

private:
    QTreeWidget* variableTree_;   // 变量监视树
    QListWidget* callStackList_;  // 调用栈列表
};
