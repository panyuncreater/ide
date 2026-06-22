#include "gui/VmStackPanel.h"
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
    opLabel_->setStyleSheet(
        "QLabel {"
        "  background-color: #2d2d2d;"
        "  color: #00ff88;"
        "  font-family: Consolas, monospace;"
        "  font-size: 12px;"
        "  padding: 6px;"
        "  border-radius: 4px;"
        "  border: 1px solid #444;"
        "}"
    );
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
    stackTitle->setStyleSheet(
        "QLabel {"
        "  color: #aaa;"
        "  font-size: 11px;"
        "  font-weight: bold;"
        "  padding: 2px;"
        "}"
    );
    stackLayout->addWidget(stackTitle);

    stackList_ = new QListWidget(this);
    stackList_->setObjectName("vmStackList");
    stackList_->setFont(QFont("Consolas", 10));
    stackLayout->addWidget(stackList_);
    splitter->addWidget(stackGroup);

    // 全局变量表
    auto* globalsGroup = new QWidget(this);
    auto* globalsLayout = new QVBoxLayout(globalsGroup);
    globalsLayout->setContentsMargins(0, 0, 0, 0);
    globalsLayout->setSpacing(2);

    auto* globalsTitle = new QLabel("全局变量", this);
    globalsTitle->setStyleSheet(
        "QLabel {"
        "  color: #aaa;"
        "  font-size: 11px;"
        "  font-weight: bold;"
        "  padding: 2px;"
        "}"
    );
    globalsLayout->addWidget(globalsTitle);

    globalsTable_ = new QTableWidget(this);
    globalsTable_->setObjectName("vmGlobalsTable");
    globalsTable_->setColumnCount(2);
    globalsTable_->setHorizontalHeaderLabels({"变量名", "值"});
    globalsTable_->horizontalHeader()->setStretchLastSection(true);
    globalsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    globalsTable_->setAlternatingRowColors(true);
    globalsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    globalsTable_->setFont(QFont("Consolas", 10));
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
            item->setForeground(QColor("#00ff88"));
        }

        stackList_->addItem(item);
    }

    if (stack.empty()) {
        auto* item = new QListWidgetItem("(栈为空)");
        item->setForeground(QColor("#666"));
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

    globalsTable_->setRowCount(static_cast<int>(entries.size()));

    int row = 0;
    for (const auto* entry : entries) {
        globalsTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(entry->first)));
        globalsTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry->second.toString())));
        ++row;
    }

    globalsTable_->resizeColumnsToContents();
}

void VmStackPanel::updateCurrentOp(size_t ip, OpCode opcode, int line) {
    QString opName = opCodeName(opcode);

    opLabel_->setText(QString("IP: %1  |  %2  |  行: %3").arg(ip).arg(opName).arg(line));
}

void VmStackPanel::clearAll() {
    stackList_->clear();
    globalsTable_->setRowCount(0);
    opLabel_->setText("等待执行...");
}
