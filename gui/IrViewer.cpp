#include "gui/IrViewer.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
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
    list_->setFont(GuiTextUtils::monospaceFont(10));
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setAlternatingRowColors(false);
    list_->setWordWrap(false);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mainLayout->addWidget(list_, 1);
}

// ---- IR 操作码名称与指令格式化（Dedup-4E/4F）----
// 原本 IrViewer 维护了一份 localIrOpName 副本，缺失 SUPER_MEMBER_GET/SUPER_CALL case
// 导致显示为 "?"，现统一使用 IR.h 公共 API irOpName() / formatIRInstruction()。

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
    // Dedup-4A: 通过 GuiTextUtils::monospaceFont 共享全局 QFont 缓存（原 static const QFont）
    const QFont& irFont = GuiTextUtils::monospaceFont(10);
    const QFont& headerFont = GuiTextUtils::monospaceFont(10, true);

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
