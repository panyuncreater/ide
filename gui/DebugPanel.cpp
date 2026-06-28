#include "gui/DebugPanel.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
#include <QHeaderView>
#include <QSplitter>
#include <tuple>
#include <vector>

// ============================================================
// DebugPanel 调试面板实现
// ============================================================

// P1-14 fix: 变量树填充共享逻辑（updateVariables/onStackFrameSelected 共用）
void DebugPanel::populateVariableTree(
    const std::vector<std::tuple<QString, QString, QString>>& rows) {
    variableTree_->clear();
    for (const auto& [name, value, scope] : rows) {
        auto* item = new QTreeWidgetItem(variableTree_);
        item->setText(0, name);
        item->setText(1, value);
        item->setText(2, scope);
    }
}

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
    callStackList_->setFont(GuiTextUtils::monospaceFont(10));
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
    // P1-14 fix: 委托给 populateVariableTree
    std::vector<std::tuple<QString, QString, QString>> rows;
    rows.reserve(vars.size());
    for (const auto& v : vars) {
        rows.emplace_back(QString::fromStdString(v.name),
                          QString::fromStdString(v.value.toString()),
                          QString::fromStdString(v.scope));
    }
    populateVariableTree(rows);
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
    // P1-14 fix: 委托给 populateVariableTree
    // Bug-7 fix: scope 列语义与 populateFromSnapshot 保持一致——按帧深度标记
    // "局部" (depth=0) / "外层" (depth>0)，而非函数名。函数名已在 callStackList_
    // 中显示，重复填充 scope 列无信息增益且与快照路径的 "局部"/"外层"/"全局"
    // 语义冲突。
    const QString scopeLabel = (frame.depth == 0) ? QStringLiteral("局部")
                                                  : QStringLiteral("外层");
    std::vector<std::tuple<QString, QString, QString>> rows;
    rows.reserve(frame.locals.size());
    for (const auto& kv : frame.locals) {
        rows.emplace_back(QString::fromStdString(kv.first),
                          QString::fromStdString(kv.second.toString()),
                          scopeLabel);
    }
    populateVariableTree(rows);
}

void DebugPanel::clearAll() {
    variableTree_->clear();
    callStackList_->clear();
    currentStack_.clear();
}
