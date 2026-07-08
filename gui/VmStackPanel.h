#pragma once

#include "compiler/Bytecode.h"
#include "interpreter/Value.h"
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QTableWidget>
#include <QWidget>
#include <string>

// ============================================================
// VmStackPanel - VM 栈状态可视化面板
// ============================================================

/// 显示 VM 执行时的操作数栈和全局变量状态
class VmStackPanel : public QWidget {
    Q_OBJECT

public:
    explicit VmStackPanel(QWidget* parent = nullptr);

    /// 更新栈显示（栈式 VM 模式）
    /// BUG-VSP-2 fix: 接口改为 by-value，避免调用方持有引用导致生命周期耦合。
    /// 调用方 IdeController 已 by-value 返回，改为 by-value 参数不影响调用方。
    /// BUG-VSP-6 fix: 入口切换 stackTitle_ 为栈式模式标题。
    void updateStack(std::vector<Value> stack);

    /// A1 fix: 更新寄存器显示（RegisterVM 模式）。
    /// 复用 stackList_ 控件，把寄存器视作线性槽位序列展示。
    /// BUG-VSP-2 fix: 接口改为 by-value。
    /// BUG-VSP-6 fix: 入口切换 stackTitle_ 为寄存器模式标题。
    void updateRegisters(std::vector<Value> registers);

    /// 更新全局变量显示
    /// BUG-VSP-2 fix: 接口改为 by-value。
    void updateGlobals(std::unordered_map<std::string, Value> globals);

    /// 更新当前指令信息（A1 fix: 统一为字符串 opName，兼容 OpCode/RegOp）
    /// BUG-VSP-5 fix: 空名/无效行号时显示兜底占位文本
    void updateCurrentOp(size_t ip, const std::string& opName, int line);

    /// 清空所有显示
    void clearAll();

private:
    QLabel* opLabel_ = nullptr;            // 当前指令标签
    QListWidget* stackList_ = nullptr;     // 栈内容列表
    QTableWidget* globalsTable_ = nullptr; // 全局变量表
    QLabel* stackTitle_ = nullptr;         // BUG-VSP-6 fix: 栈/寄存器区标题（模式切换时适配）

    /// BUG-VSP-3 fix: 上次 updateGlobals 时值列的最大文本长度，
    /// 用于检测内容变长时重算列宽（仅行数变化时重算不足以覆盖所有场景）。
    int lastMaxValueWidth_ = 0;
};
