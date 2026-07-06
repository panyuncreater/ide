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
    // 样式由 Ide::applyFluentStyle() 通过 objectName 集中管理（亮/暗主题自适应）。
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

    // BUG-VSP-6 fix: stackTitle 改为成员变量，便于在栈式/寄存器模式切换时更新标题
    stackTitle_ = new QLabel("操作数栈 (栈顶 ↑)", this);
    // 样式由 Ide::applyFluentStyle() 通过 objectName 集中管理（亮/暗主题自适应）
    stackTitle_->setObjectName("vmStackTitle");
    stackLayout->addWidget(stackTitle_);

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
    // 样式由 Ide::applyFluentStyle() 通过 objectName 集中管理（亮/暗主题自适应）
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

void VmStackPanel::updateStack(std::vector<Value> stack) {
    // BUG-VSP-6 fix: 模式切换时标题适配——栈式 VM 模式
    if (stackTitle_) {
        stackTitle_->setText(QString::fromUtf8("操作数栈 (栈顶 ↑)"));
    }

    stackList_->clear();

    // 栈顶在上，栈底在下
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
        // BUG-VSP-1 fix: toString() 可能抛异常（如 NaN-boxing 解码失败），用 try/catch 兜底
        std::string valStr;
        try {
            valStr = stack[i].toString();
            if (valStr.size() > 200) valStr = valStr.substr(0, 200) + "...";
        } catch (...) {
            valStr = "<error>";
        }
        QString itemText;
        if (i == static_cast<int>(stack.size()) - 1) {
            itemText = QString("TOP [%1]  %2").arg(i).arg(QString::fromStdString(valStr));
        } else {
            itemText = QString("    [%1]  %2").arg(i).arg(QString::fromStdString(valStr));
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
void VmStackPanel::updateRegisters(std::vector<Value> registers) {
    // BUG-VSP-6 fix: 模式切换时标题适配——寄存器 VM 模式
    if (stackTitle_) {
        stackTitle_->setText(QString::fromUtf8("寄存器 (R0..Rn)"));
    }

    stackList_->clear();

    for (size_t i = 0; i < registers.size(); ++i) {
        // BUG-VSP-1 fix: toString() 可能抛异常，用 try/catch 兜底
        std::string valStr;
        try {
            valStr = registers[i].toString();
            if (valStr.size() > 200) valStr = valStr.substr(0, 200) + "...";
        } catch (...) {
            valStr = "<error>";
        }
        QString itemText = QString("R%1  %2")
            .arg(static_cast<int>(i))
            .arg(QString::fromStdString(valStr));
        auto* item = new QListWidgetItem(itemText);
        stackList_->addItem(item);
    }

    if (registers.empty()) {
        auto* item = new QListWidgetItem("(无激活寄存器)");
        item->setForeground(palette().placeholderText().color());
        stackList_->addItem(item);
    }
}

void VmStackPanel::updateGlobals(std::unordered_map<std::string, Value> globals) {
    // BUG-VSP-4 fix: 原实现用 unordered_map 元素裸指针排序，若 globals 在排序后
    // 被修改/销毁，指针即悬垂。改为按值接收参数（BUG-VSP-2），并 move 到本地 vector，
    // 排序与访问均基于本地 vector，生命周期完全独立。
    std::vector<std::pair<std::string, Value>> entries;
    entries.reserve(globals.size());
    for (auto& kv : globals) {
        entries.push_back(std::move(kv));
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // G-P2-4 fix: 仅在行数变化时调用 resizeColumnsToContents（O(n) 操作），避免每次更新都重算列宽
    const int newRowCount = static_cast<int>(entries.size());
    const bool rowCountChanged = (globalsTable_->rowCount() != newRowCount);
    globalsTable_->setRowCount(newRowCount);

    int row = 0;
    int maxValueWidth = 0;  // BUG-VSP-3 fix: 跟踪值列最大文本长度
    for (const auto& entry : entries) {
        // BUG-VSP-1 fix: toString() 可能抛异常，用 try/catch 兜底
        std::string valStr;
        try {
            valStr = entry.second.toString();
            if (valStr.size() > 200) valStr = valStr.substr(0, 200) + "...";
        } catch (...) {
            valStr = "<error>";
        }
        QString valQStr = QString::fromStdString(valStr);
        globalsTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(entry.first)));
        globalsTable_->setItem(row, 1, new QTableWidgetItem(valQStr));
        if (valQStr.size() > maxValueWidth) maxValueWidth = valQStr.size();
        ++row;
    }

    // BUG-VSP-3 fix: 除行数变化外，值列最大文本长度增长超过阈值时也重算列宽，
    // 否则同行数下列宽不会随内容变长而扩展，导致长值被截断。
    if (rowCountChanged || maxValueWidth > lastMaxValueWidth_ + 5) {
        globalsTable_->resizeColumnsToContents();
        lastMaxValueWidth_ = maxValueWidth;
    }
}

// A1 fix: 统一接受 opName 字符串，兼容栈式 VM (opCodeName) 和 RegisterVM (regOpName)
void VmStackPanel::updateCurrentOp(size_t ip, const std::string& opName, int line) {
    // BUG-VSP-5 fix: 对空 opName 与无效行号（<=0）显示兜底占位文本，避免显示空白
    QString opDisplay = opName.empty()
        ? QString::fromUtf8("(未知指令)")
        : QString::fromStdString(opName);
    QString lineDisplay = (line <= 0)
        ? QString::fromUtf8("(无行号)")
        : QString::number(line);
    opLabel_->setText(QString("IP: %1  |  %2  |  行: %3")
                        .arg(static_cast<qulonglong>(ip))
                        .arg(opDisplay)
                        .arg(lineDisplay));
}

void VmStackPanel::clearAll() {
    stackList_->clear();
    globalsTable_->setRowCount(0);
    opLabel_->setText("等待执行...");
}
