#pragma once

#include <QWidget>
#include <QListWidget>
#include <vector>
#include "compiler/IR.h"

// ============================================================
// IrViewer - IR 中间表示可视化面板（方向三）
// ------------------------------------------------------------
// 在 IDE 右侧 Tab 中显示 AST → IR 转换后的中间表示：
//   - 函数元信息（常量数/全局变量数/vreg 数/基本块数）
//   - 每个基本块的指令序列（含源码行号注释）
//   - 支持源码行号 → IR 指令高亮联动
//
// 方向四扩展：VM 单步执行字节码时，通过 IR→字节码偏移映射
// 高亮对应的 IR 指令（highlightByBytecodeOffset）。
// ============================================================

class IrViewer : public QWidget {
    Q_OBJECT

public:
    explicit IrViewer(QWidget* parent = nullptr);

    /// 设置 IR 函数并渲染（nullptr 时清空）
    void setIR(const IRFunction* ir);

    /// 清空显示
    void clearIR();

    /// 按源码行号高亮 IR 指令（高亮所有 line 匹配的指令行）
    /// line <= 0 时清除高亮
    void highlightBySourceLine(int line);

    /// 方向四：按字节码偏移高亮 IR 指令
    /// irToBytecodeOffset 是 IR 指令行号 → 字节码偏移的映射表
    /// currentBytecodeOffset 是当前 VM 执行到的字节码偏移
    /// 找到 ≤ currentBytecodeOffset 的最大映射项，高亮对应的 IR 行
    void highlightByBytecodeOffset(const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset,
                                    size_t currentBytecodeOffset);

    /// 清除高亮
    void clearHighlight();

private:
    QListWidget* list_ = nullptr;

    /// 每行对应的源码行号（0 表示无关联，如头部/块头）
    std::vector<int> rowToSourceLine_;

    /// 每行对应的 IR 指令索引（在所有基本块指令展平后的序号）
    /// 用于方向四的 IR→字节码偏射映射定位
    std::vector<size_t> rowToInstrIndex_;

    /// 当前高亮的行号（-1 表示无）
    int highlightedRow_ = -1;
};
