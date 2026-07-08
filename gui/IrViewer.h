#pragma once

#include "compiler/IR.h"
#include <QTextBrowser>
#include <QWidget>
#include <vector>

// ============================================================
// IrViewer - IR 中间表示可视化面板（第八轮重构）
// ------------------------------------------------------------
// 使用 QTextBrowser + HTML 富文本实现语法高亮：
//   - opcode（LOAD_*/ADD/CALL 等）→ #0078d4 蓝色
//   - 寄存器/局部变量（v0..vN）→ #107c10 绿色
//   - 常量/字符串/数字 → #d83b01 橙色
//   - 行号注释（; line N）→ #6e6e6e 灰色
//   - 函数标题分隔线（---- xxx ----）→ #8764b8 紫色加粗
//
// 支持源码行号 → IR 指令高亮联动（背景黄色）
// 支持 VM 单步执行时按字节码偏移高亮（背景亮绿色）
// ============================================================

class IrViewer : public QWidget {
    Q_OBJECT

public:
    explicit IrViewer(QWidget* parent = nullptr);

    /// 设置 IR 函数并渲染（nullptr 时清空）
    /// BUG-IRV-1 fix: 内部用 try/catch 包裹，渲染失败时显示错误占位文本
    void setIR(const IRFunction* ir);

    /// 清空显示
    void clearIR();

    /// 按源码行号高亮 IR 指令。
    /// BUG-IRV-2 fix: 修正注释——只高亮第一个 line 匹配的指令行（非所有匹配行）。
    /// line <= 0 时清除高亮
    void highlightBySourceLine(int line);

    /// 按字节码偏移高亮 IR 指令
    void highlightByBytecodeOffset(const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset,
                                   size_t currentBytecodeOffset);

    /// 清除高亮
    void clearHighlight();

private:
    QTextBrowser* browser_ = nullptr;

    /// 每行对应的源码行号（0 表示无关联，如头部/块头）
    /// 索引对应 QTextDocument 的 block 编号
    std::vector<int> rowToSourceLine_;

    /// 每行对应的 IR 指令索引
    std::vector<size_t> rowToInstrIndex_;

    /// 当前高亮的行号（-1 表示无）
    int highlightedRow_ = -1;

    /// 将单条 IR 指令文本转为带语法高亮的 HTML
    QString formatIRLineHtml(const std::string& text) const;

    /// HTML 转义
    static QString htmlEscape(const std::string& s);
};
