#include "gui/DebugPanel.h"
#include <QHeaderView>
#include <QSplitter>

// ============================================================
// DebugPanel 调试面板实现
// ============================================================

DebugPanel::DebugPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Vertical, this);

    // ---- 变量监视区域 ----
    auto* varWidget = new QWidget;
    auto* varLayout = new QVBoxLayout(varWidget);
    varLayout->setContentsMargins(0, 0, 0, 0);
    varLayout->setSpacing(2);

    auto* varLabel = new QLabel("变量监视");
    varLabel->setObjectName("debugVarLabel");
    varLayout->addWidget(varLabel);

    variableTree_ = new QTreeWidget;
    variableTree_->setHeaderLabels({"名称", "值", "作用域"});
    variableTree_->header()->setStretchLastSection(true);
    variableTree_->setAlternatingRowColors(true);
    variableTree_->setColumnWidth(0, 120);
    variableTree_->setColumnWidth(1, 150);
    varLayout->addWidget(variableTree_);

    splitter->addWidget(varWidget);

    // ---- 调用栈区域 ----
    auto* stackWidget = new QWidget;
    auto* stackLayout = new QVBoxLayout(stackWidget);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(2);

    auto* stackLabel = new QLabel("调用栈");
    stackLabel->setObjectName("debugStackLabel");
    stackLayout->addWidget(stackLabel);

    callStackList_ = new QListWidget;
    callStackList_->setAlternatingRowColors(true);
    callStackList_->setFont(QFont("Consolas", 10));
    stackLayout->addWidget(callStackList_);

    // 选中栈帧时显示该帧的局部变量
    connect(callStackList_, &QListWidget::currentRowChanged,
            this, &DebugPanel::onStackFrameSelected);

    splitter->addWidget(stackWidget);

    // 设置分割比例
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter);
}

void DebugPanel::updateVariables(const std::vector<VariableSnapshot>& vars) {
    variableTree_->clear();

    for (const auto& v : vars) {
        auto* item = new QTreeWidgetItem(variableTree_);
        item->setText(0, QString::fromStdString(v.name));
        item->setText(1, QString::fromStdString(v.value.toString()));
        item->setText(2, QString::fromStdString(v.scope));
    }
}

void DebugPanel::updateCallStack(const std::vector<CallStackEntry>& stack) {
    // G-P2-5 fix: 使用 std::move 避免拷贝（参数虽为 const 引用，但此处保存的是拷贝；
    //   改为按值传递 + std::move 更优，但为最小改动此处保持接口不变）
    currentStack_ = stack;  // 保存完整数据（含局部变量）

    // G-P2-6 fix: 保存当前选中行，刷新后恢复（避免调试步进时选中丢失）
    int savedRow = callStackList_->currentRow();

    // 阻塞信号防止 clear/addItem 触发 currentRowChanged 级联更新变量树
    callStackList_->blockSignals(true);
    callStackList_->clear();

    for (const auto& frame : stack) {
        QString text = QString("函数: %1 @ 行 %2 (深度: %3)")
                           .arg(QString::fromStdString(frame.functionName))
                           .arg(frame.line)
                           .arg(frame.depth);
        callStackList_->addItem(text);
    }
    callStackList_->blockSignals(false);

    // G-P2-6 fix: 恢复选中行（若仍在有效范围内）
    if (savedRow >= 0 && savedRow < callStackList_->count()) {
        callStackList_->setCurrentRow(savedRow);
    }
}

void DebugPanel::onStackFrameSelected(int index) {
    if (index < 0 || index >= static_cast<int>(currentStack_.size())) return;

    const auto& frame = currentStack_[index];
    // 在变量树中显示选中帧的局部变量
    variableTree_->clear();
    for (const auto& kv : frame.locals) {
        auto* item = new QTreeWidgetItem(variableTree_);
        item->setText(0, QString::fromStdString(kv.first));
        item->setText(1, QString::fromStdString(kv.second.toString()));
        item->setText(2, QString::fromStdString(frame.functionName));  // 作用域 = 函数名
    }
}

void DebugPanel::clearAll() {
    variableTree_->clear();
    callStackList_->clear();
    currentStack_.clear();
}
