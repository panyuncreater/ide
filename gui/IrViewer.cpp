#include "gui/IrViewer.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QAbstractItemView>
#include <QColor>
#include <QScrollBar>
#include <sstream>

// ============================================================
// IrViewer 实现（方向三：IR 可视化面板）
// ============================================================

IrViewer::IrViewer(QWidget* parent)
    : QWidget(parent) {

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(2);

    auto* title = new QLabel("IR 中间表示", this);
    title->setObjectName("irViewerTitle");
    mainLayout->addWidget(title);

    list_ = new QListWidget(this);
    list_->setObjectName("irViewerList");
    list_->setFont(QFont("Consolas", 10));
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setAlternatingRowColors(false);
    list_->setWordWrap(false);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mainLayout->addWidget(list_, 1);
}

// ---- IR 操作码名称（与 IR.cpp irOpName 保持一致）----
// 复制而非暴露匿名命名空间函数，避免污染 IR.h 公共接口
static const char* localIrOpName(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:      return "LOAD_CONST";
    case IROp::LOAD_NULL:       return "LOAD_NULL";
    case IROp::LOAD_TRUE:       return "LOAD_TRUE";
    case IROp::LOAD_FALSE:      return "LOAD_FALSE";
    case IROp::LOAD_LOCAL:      return "LOAD_LOCAL";
    case IROp::STORE_LOCAL:     return "STORE_LOCAL";
    case IROp::LOAD_GLOBAL:     return "LOAD_GLOBAL";
    case IROp::STORE_GLOBAL:    return "STORE_GLOBAL";
    case IROp::DEFINE_GLOBAL:   return "DEFINE_GLOBAL";
    case IROp::LOAD_UPVALUE:    return "LOAD_UPVALUE";
    case IROp::STORE_UPVALUE:   return "STORE_UPVALUE";
    case IROp::CLOSE_UPVALUE:   return "CLOSE_UPVALUE";
    case IROp::ADD:             return "ADD";
    case IROp::SUB:             return "SUB";
    case IROp::MUL:             return "MUL";
    case IROp::DIV:             return "DIV";
    case IROp::MOD:             return "MOD";
    case IROp::NEGATE:          return "NEGATE";
    case IROp::EQ:              return "EQ";
    case IROp::NEQ:             return "NEQ";
    case IROp::LT:              return "LT";
    case IROp::GT:              return "GT";
    case IROp::LTE:             return "LTE";
    case IROp::GTE:             return "GTE";
    case IROp::NOT:             return "NOT";
    case IROp::JUMP:            return "JUMP";
    case IROp::JUMP_IF_FALSE:   return "JUMP_IF_FALSE";
    case IROp::LABEL:           return "LABEL";
    case IROp::CALL:            return "CALL";
    case IROp::CALL_EXPR:       return "CALL_EXPR";
    case IROp::RETURN:          return "RETURN";
    case IROp::RETURN_NULL:     return "RETURN_NULL";
    case IROp::MAKE_CLOSURE:    return "MAKE_CLOSURE";
    case IROp::BUILD_ARRAY:     return "BUILD_ARRAY";
    case IROp::BUILD_DICT:      return "BUILD_DICT";
    case IROp::INDEX_GET:       return "INDEX_GET";
    case IROp::INDEX_SET:       return "INDEX_SET";
    case IROp::MEMBER_GET:      return "MEMBER_GET";
    case IROp::MEMBER_SET:      return "MEMBER_SET";
    case IROp::METHOD_CALL:     return "METHOD_CALL";
    case IROp::DEFINE_CLASS:    return "DEFINE_CLASS";
    case IROp::CLASS_NEW:       return "CLASS_NEW";
    case IROp::INIT_FIELD:      return "INIT_FIELD";
    case IROp::TRY_BEGIN:       return "TRY_BEGIN";
    case IROp::TRY_END:         return "TRY_END";
    case IROp::THROW:           return "THROW";
    case IROp::WRITEBACK_MEMBER_VAR:    return "WRITEBACK_MEMBER_VAR";
    case IROp::WRITEBACK_MEMBER_LOCAL:  return "WRITEBACK_MEMBER_LOCAL";
    case IROp::WRITEBACK_INDEX_VAR:     return "WRITEBACK_INDEX_VAR";
    case IROp::WRITEBACK_INDEX_LOCAL:   return "WRITEBACK_INDEX_LOCAL";
    case IROp::WRITEBACK_MEMBER_UPVALUE: return "WRITEBACK_MEMBER_UPVALUE";
    case IROp::WRITEBACK_INDEX_UPVALUE:  return "WRITEBACK_INDEX_UPVALUE";
    case IROp::PRINT:           return "PRINT";
    case IROp::POP:             return "POP";
    case IROp::DUP:             return "DUP";
    }
    return "?";
}

/// 格式化单条 IR 指令为可读字符串（与 IRToString 的 per-instruction 格式一致）
static std::string formatIRInstruction(const IRInstruction& instr) {
    std::ostringstream oss;
    oss << localIrOpName(instr.op);
    for (const auto& operand : instr.operands) {
        const char* kindStr = "?";
        switch (operand.kind) {
        case IROperandKind::CONSTANT:    kindStr = "c"; break;
        case IROperandKind::VIRTUAL:     kindStr = "v"; break;
        case IROperandKind::LABEL:       kindStr = "L"; break;
        case IROperandKind::GLOBAL_NAME: kindStr = "g"; break;
        case IROperandKind::LOCAL_SLOT:  kindStr = "s"; break;
        case IROperandKind::UPVALUE_IDX: kindStr = "u"; break;
        case IROperandKind::FIELD_NAME:  kindStr = "f"; break;
        case IROperandKind::FUNC_NAME:   kindStr = "fn"; break;
        case IROperandKind::IMM_UINT:    kindStr = "#"; break;
        }
        oss << " " << kindStr << operand.index;
    }
    if (instr.line > 0) oss << "  ; line " << instr.line;
    return oss.str();
}

void IrViewer::setIR(const IRFunction* ir) {
    list_->clear();
    rowToSourceLine_.clear();
    rowToInstrIndex_.clear();
    highlightedRow_ = -1;

    if (!ir) {
        list_->addItem("(未启用 IR 编译 — 点击工具栏 🔮 IR 按钮生成)");
        return;
    }

    if (ir->blocks.empty()) {
        list_->addItem("(空 IR — 无基本块)");
        return;
    }

    // PERF: 大 IR 保护（与 AstViewer MAX_AST_NODES 同理）
    // 统计指令总数，超过上限则截断显示
    size_t totalInstrs = 0;
    for (const auto& block : ir->blocks) {
        totalInstrs += block.instructions.size();
    }
    static constexpr size_t MAX_IR_ROWS = 10000;

    list_->setUpdatesEnabled(false);  // P6 fix: 批量填充时禁用重绘
    static const QFont irFont("Consolas", 10);
    static const QFont headerFont("Consolas", 10, QFont::Bold);

    // ---- 函数元信息头 ----
    {
        std::ostringstream oss;
        oss << "IRFunction \"" << ir->name << "\"  "
            << "blocks=" << ir->blocks.size() << "  "
            << "constants=" << ir->constants.size() << "  "
            << "globals=" << ir->globalNames.size() << "  "
            << "vregs=" << ir->nextVReg;
        auto* item = new QListWidgetItem(QString::fromStdString(oss.str()));
        item->setFont(headerFont);
        item->setForeground(QColor("#569CD6"));  // 与字节码 chunk 头同色
        list_->addItem(item);
        rowToSourceLine_.push_back(0);
        rowToInstrIndex_.push_back(SIZE_MAX);  // 非指令行
    }

    size_t instrIndex = 0;  // 展平的全局指令序号（用于方向四映射）

    for (size_t bi = 0; bi < ir->blocks.size(); ++bi) {
        const auto& block = ir->blocks[bi];

        // 截断保护
        if (static_cast<size_t>(list_->count()) > MAX_IR_ROWS) {
            auto* item = new QListWidgetItem("... (IR 超过 10000 行，已截断显示)");
            item->setFont(irFont);
            item->setForeground(QColor("#808080"));
            list_->addItem(item);
            rowToSourceLine_.push_back(0);
            rowToInstrIndex_.push_back(SIZE_MAX);
            break;
        }

        // ---- 基本块头 ----
        {
            std::ostringstream oss;
            oss << "  BB" << bi << " (label=" << block.labelIndex << "):";
            auto* item = new QListWidgetItem(QString::fromStdString(oss.str()));
            item->setFont(headerFont);
            item->setForeground(QColor("#4EC9B0"));  // 青色区分基本块
            list_->addItem(item);
            rowToSourceLine_.push_back(0);
            rowToInstrIndex_.push_back(SIZE_MAX);
        }

        // ---- 块内指令 ----
        for (const auto& instr : block.instructions) {
            std::string text = formatIRInstruction(instr);
            auto* item = new QListWidgetItem(QString::fromStdString("    " + text));
            item->setFont(irFont);
            list_->addItem(item);
            rowToSourceLine_.push_back(instr.line);
            rowToInstrIndex_.push_back(instrIndex);
            instrIndex++;
        }
    }

    list_->setUpdatesEnabled(true);
}

void IrViewer::clearIR() {
    list_->clear();
    rowToSourceLine_.clear();
    rowToInstrIndex_.clear();
    highlightedRow_ = -1;
}

void IrViewer::highlightBySourceLine(int line) {
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < list_->count()) {
        auto* item = list_->item(highlightedRow_);
        if (item) {
            item->setBackground(QColor("transparent"));
        }
    }
    highlightedRow_ = -1;

    if (line <= 0) return;

    // 找到第一个匹配源码行的指令并高亮
    for (int i = 0; i < static_cast<int>(rowToSourceLine_.size()); ++i) {
        if (rowToSourceLine_[i] == line) {
            auto* item = list_->item(i);
            if (item) {
                item->setBackground(QColor("#FFF09B"));  // 与编辑器当前行高亮同色系
            }
            highlightedRow_ = i;
            // 滚动到可见
            list_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;  // 只高亮第一个匹配
        }
    }
}

void IrViewer::highlightByBytecodeOffset(const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset,
                                          size_t currentBytecodeOffset) {
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < list_->count()) {
        auto* item = list_->item(highlightedRow_);
        if (item) {
            item->setBackground(QColor("transparent"));
        }
    }
    highlightedRow_ = -1;

    if (irToBytecodeOffset.empty()) return;

    // 二分查找：找到 ≤ currentBytecodeOffset 的最大映射项
    // irToBytecodeOffset 按 .second（字节码偏移）升序排列
    size_t bestInstrIdx = SIZE_MAX;
    size_t lo = 0, hi = irToBytecodeOffset.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (irToBytecodeOffset[mid].second <= currentBytecodeOffset) {
            bestInstrIdx = irToBytecodeOffset[mid].first;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (bestInstrIdx == SIZE_MAX) return;

    // 在 rowToInstrIndex_ 中找到 bestInstrIdx 对应的行
    for (int i = 0; i < static_cast<int>(rowToInstrIndex_.size()); ++i) {
        if (rowToInstrIndex_[i] == bestInstrIdx) {
            auto* item = list_->item(i);
            if (item) {
                item->setBackground(QColor("#7CFC00"));  // 亮绿色，区别于源码行高亮
            }
            highlightedRow_ = i;
            list_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}

void IrViewer::clearHighlight() {
    if (highlightedRow_ >= 0 && highlightedRow_ < list_->count()) {
        auto* item = list_->item(highlightedRow_);
        if (item) {
            item->setBackground(QColor("transparent"));
        }
    }
    highlightedRow_ = -1;
}
