#include "gui/VmStackPanel.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QAbstractItemView>

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
    stackList_->setFont(QFont("Consolas", 10));
    stackList_->setStyleSheet(
        "QListWidget {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  border: 1px solid #444;"
        "  border-radius: 4px;"
        "}"
        "QListWidget::item {"
        "  padding: 3px 6px;"
        "  border-bottom: 1px solid #333;"
        "}"
        "QListWidget::item:selected {"
        "  background-color: #264f78;"
        "}"
    );
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
    globalsTable_->setColumnCount(2);
    globalsTable_->setHorizontalHeaderLabels({"变量名", "值"});
    globalsTable_->horizontalHeader()->setStretchLastSection(true);
    globalsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    globalsTable_->setAlternatingRowColors(true);
    globalsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    globalsTable_->setFont(QFont("Consolas", 10));
    globalsTable_->setStyleSheet(
        "QTableWidget {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  border: 1px solid #444;"
        "  border-radius: 4px;"
        "}"
        "QHeaderView::section {"
        "  background-color: #2d2d2d;"
        "  color: #ccc;"
        "  padding: 4px;"
        "  border: 1px solid #444;"
        "  font-weight: bold;"
        "}"
    );
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
    globalsTable_->setRowCount(static_cast<int>(globals.size()));

    int row = 0;
    for (const auto& [name, val] : globals) {
        globalsTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(name)));
        globalsTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(val.toString())));
        ++row;
    }

    globalsTable_->resizeColumnsToContents();
}

void VmStackPanel::updateCurrentOp(size_t ip, OpCode opcode, int line) {
    // 操作码名称映射
    static const std::unordered_map<OpCode, QString> opNames = {
        {OpCode::OP_CONSTANT, "OP_CONSTANT"},
        {OpCode::OP_INT, "OP_INT"},
        {OpCode::OP_FLOAT, "OP_FLOAT"},
        {OpCode::OP_STRING, "OP_STRING"},
        {OpCode::OP_NULL, "OP_NULL"},
        {OpCode::OP_TRUE, "OP_TRUE"},
        {OpCode::OP_FALSE, "OP_FALSE"},
        {OpCode::OP_ADD, "OP_ADD"},
        {OpCode::OP_SUBTRACT, "OP_SUBTRACT"},
        {OpCode::OP_MULTIPLY, "OP_MULTIPLY"},
        {OpCode::OP_DIVIDE, "OP_DIVIDE"},
        {OpCode::OP_MODULO, "OP_MODULO"},
        {OpCode::OP_NEGATE, "OP_NEGATE"},
        {OpCode::OP_NOT, "OP_NOT"},
        {OpCode::OP_EQUAL, "OP_EQUAL"},
        {OpCode::OP_NOT_EQUAL, "OP_NOT_EQUAL"},
        {OpCode::OP_LESS, "OP_LESS"},
        {OpCode::OP_GREATER, "OP_GREATER"},
        {OpCode::OP_LESS_EQUAL, "OP_LESS_EQUAL"},
        {OpCode::OP_GREATER_EQUAL, "OP_GREATER_EQUAL"},
        {OpCode::OP_AND, "OP_AND"},
        {OpCode::OP_OR, "OP_OR"},
        {OpCode::OP_PRINT, "OP_PRINT"},
        {OpCode::OP_POP, "OP_POP"},
        {OpCode::OP_DEFINE_VAR, "OP_DEFINE_VAR"},
        {OpCode::OP_GET_VAR, "OP_GET_VAR"},
        {OpCode::OP_SET_VAR, "OP_SET_VAR"},
        {OpCode::OP_JUMP, "OP_JUMP"},
        {OpCode::OP_JUMP_IF_FALSE, "OP_JUMP_IF_FALSE"},
        {OpCode::OP_LOOP, "OP_LOOP"},
        {OpCode::OP_RETURN, "OP_RETURN"},
        {OpCode::OP_CALL, "OP_CALL"},
        {OpCode::OP_BUILD_ARRAY, "OP_BUILD_ARRAY"},
        {OpCode::OP_INDEX_GET, "OP_INDEX_GET"},
        {OpCode::OP_INDEX_SET, "OP_INDEX_SET"},
        {OpCode::OP_MEMBER_GET, "OP_MEMBER_GET"},
        {OpCode::OP_MEMBER_SET, "OP_MEMBER_SET"},
        {OpCode::OP_METHOD_CALL, "OP_METHOD_CALL"},
        {OpCode::OP_DUP, "OP_DUP"},
        {OpCode::OP_CLOSURE, "OP_CLOSURE"},
        {OpCode::OP_GET_LOCAL, "OP_GET_LOCAL"},
        {OpCode::OP_SET_LOCAL, "OP_SET_LOCAL"},
        {OpCode::OP_CLASS_NEW, "OP_CLASS_NEW"},
    };

    QString opName = opNames.count(opcode)
        ? opNames.at(opcode)
        : QString("OP_UNKNOWN(%1)").arg(static_cast<int>(opcode));

    opLabel_->setText(QString("IP: %1  |  %2  |  行: %3").arg(ip).arg(opName).arg(line));
}

void VmStackPanel::clearAll() {
    stackList_->clear();
    globalsTable_->setRowCount(0);
    opLabel_->setText("等待执行...");
}
