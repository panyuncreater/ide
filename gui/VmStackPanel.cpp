#include "gui/VmStackPanel.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
#include <QVBoxLayout>
#include <QHeaderView>
#include <QAbstractItemView>
#include <algorithm>  // std::sort

// ============================================================
// VmStackPanel 实现
// ============================================================

VmStackPanel::VmStackPanel(QWidget* parent)
    : QWidget(parent) {

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 当前指令信息
    opLabel_ = new QLabel("等待执行...", this);
    // D12 fix: 移除内联硬编码样式表，改用 objectName 让 styles.qss 集中管理，
    // 便于主题切换时统一调整颜色（原内联 #2d2d2d/#00ff88/#444 不适配浅色主题）。
    opLabel_->setObjectName("vmOpLabel");
    opLabel_->setWordWrap(true);
    mainLayout->addWidget(opLabel_);

    // 上下分割：栈 + 全局变量
    auto* splitter = new QSplitter(Qt::Vertical, this);

    // 操作数栈
    auto* stackGroup = new QWidget(this);
    auto* stackLayout = new QVBoxLayout(stackGroup);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(2);

    auto* stackTitle = new QLabel("操作数栈 (栈顶 ↑)", this);
    // D12 fix: 移除内联样式，由 styles.qss 的 QLabel#vmStackTitle 规则统一定义
    stackTitle->setObjectName("vmStackTitle");
    stackLayout->addWidget(stackTitle);

    stackList_ = new QListWidget(this);
    stackList_->setObjectName("vmStackList");
    stackList_->setFont(GuiTextUtils::monospaceFont(10));
    stackLayout->addWidget(stackList_);
    splitter->addWidget(stackGroup);

    // 全局变量表
    auto* globalsGroup = new QWidget(this);
    auto* globalsLayout = new QVBoxLayout(globalsGroup);
    globalsLayout->setContentsMargins(0, 0, 0, 0);
    globalsLayout->setSpacing(2);

    auto* globalsTitle = new QLabel("全局变量", this);
    // D12 fix: 移除内联样式，由 styles.qss 的 QLabel#vmGlobalsTitle 规则统一定义
    globalsTitle->setObjectName("vmGlobalsTitle");
    globalsLayout->addWidget(globalsTitle);

    globalsTable_ = new QTableWidget(this);
    globalsTable_->setObjectName("vmGlobalsTable");
    globalsTable_->setColumnCount(2);
    globalsTable_->setHorizontalHeaderLabels({"变量名", "值"});
    globalsTable_->horizontalHeader()->setStretchLastSection(true);
    globalsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    globalsTable_->setAlternatingRowColors(true);
    globalsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    globalsTable_->setFont(GuiTextUtils::monospaceFont(10));
    globalsLayout->addWidget(globalsTable_);
    splitter->addWidget(globalsGroup);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    mainLayout->addWidget(splitter);
}

void VmStackPanel::updateStack(const std::vector<Value>& stack) {
    stackList_->clear();

    // 栈顶在上，栈底在下
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
        QString itemText;
        if (i == static_cast<int>(stack.size()) - 1) {
            itemText = QString("TOP [%1]  %2").arg(i).arg(QString::fromStdString(stack[i].toString()));
        } else {
            itemText = QString("    [%1]  %2").arg(i).arg(QString::fromStdString(stack[i].toString()));
        }
        auto* item = new QListWidgetItem(itemText);

        // 栈顶项高亮
        if (i == static_cast<int>(stack.size()) - 1) {
            // D12 fix: 使用 palette Link 角色替代硬编码 #00ff88，适配主题
            item->setForeground(palette().link().color());
        }

        stackList_->addItem(item);
    }

    if (stack.empty()) {
        auto* item = new QListWidgetItem("(栈为空)");
        // D12 fix: 使用 palette PlaceholderText 角色替代硬编码 #666
        item->setForeground(palette().placeholderText().color());
        stackList_->addItem(item);
    }
}

// A1 fix: RegisterVM 寄存器列表显示。寄存器按 R0..Rn 顺序展示，
// 与栈式 VM 的「栈顶在上」不同——寄存器无栈语义，按编号升序更直观。
void VmStackPanel::updateRegisters(const std::vector<Value>& registers) {
    stackList_->clear();

    for (size_t i = 0; i < registers.size(); ++i) {
        QString itemText = QString("R%1  %2")
            .arg(static_cast<int>(i))
            .arg(QString::fromStdString(registers[i].toString()));
        auto* item = new QListWidgetItem(itemText);
        stackList_->addItem(item);
    }

    if (registers.empty()) {
        auto* item = new QListWidgetItem("(无激活寄存器)");
        item->setForeground(palette().placeholderText().color());
        stackList_->addItem(item);
    }
}

void VmStackPanel::updateGlobals(const std::unordered_map<std::string, Value>& globals) {
    // P6 fix: 排序时只拷贝键的指针，避免深拷贝所有 Value
    std::vector<const std::pair<const std::string, Value>*> entries;
    entries.reserve(globals.size());
    for (const auto& kv : globals) {
        entries.push_back(&kv);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });

    // G-P2-4 fix: 仅在行数变化时调用 resizeColumnsToContents（O(n) 操作），避免每次更新都重算列宽
    const int newRowCount = static_cast<int>(entries.size());
    const bool rowCountChanged = (globalsTable_->rowCount() != newRowCount);
    globalsTable_->setRowCount(newRowCount);

    int row = 0;
    for (const auto* entry : entries) {
        globalsTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(entry->first)));
        globalsTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry->second.toString())));
        ++row;
    }

    if (rowCountChanged) {
        globalsTable_->resizeColumnsToContents();
    }
}

// A1 fix: 统一接受 opName 字符串，兼容栈式 VM (opCodeName) 和 RegisterVM (regOpName)
void VmStackPanel::updateCurrentOp(size_t ip, const std::string& opName, int line) {
    opLabel_->setText(QString("IP: %1  |  %2  |  行: %3")
                        .arg(static_cast<qulonglong>(ip))
                        .arg(QString::fromStdString(opName))
                        .arg(line));
}

void VmStackPanel::clearAll() {
    stackList_->clear();
    globalsTable_->setRowCount(0);
    opLabel_->setText("等待执行...");
}
