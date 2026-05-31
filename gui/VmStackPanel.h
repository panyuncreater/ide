#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTableWidget>
#include <QLabel>
#include <QSplitter>
#include "interpreter/Value.h"
#include "compiler/Bytecode.h"

// ============================================================
// VmStackPanel - VM 栈状态可视化面板
// ============================================================

/// 显示 VM 执行时的操作数栈和全局变量状态
class VmStackPanel : public QWidget {
    Q_OBJECT

public:
    explicit VmStackPanel(QWidget* parent = nullptr);

    /// 更新栈显示
    void updateStack(const std::vector<Value>& stack);

    /// 更新全局变量显示
    void updateGlobals(const std::unordered_map<std::string, Value>& globals);

    /// 更新当前指令信息
    void updateCurrentOp(size_t ip, OpCode opcode, int line);

    /// 清空所有显示
    void clearAll();

private:
    QLabel* opLabel_ = nullptr;          // 当前指令标签
    QListWidget* stackList_ = nullptr;   // 栈内容列表
    QTableWidget* globalsTable_ = nullptr; // 全局变量表
};
