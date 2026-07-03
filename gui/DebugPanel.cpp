#include "gui/DebugPanel.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
#include <QHeaderView>
#include <QSplitter>
#include <QTreeWidgetItem>
#include <tuple>
#include <vector>

// ============================================================
// DebugPanel 调试面板实现
// Round 7: 变量按作用域分层展示（全局/局部/闭包）
// ============================================================

// 作用域分组样式：灰色小字号标题，不可选中
static void styleScopeGroupHeader(QTreeWidgetItem* item) {
    QFont f = item->font(0);
    f.setBold(true);
    // RA-C fix: 全局字体用 setPixelSize(14) 设置（main.cpp），pointSize() 返回 -1，
    // pointSize()-1 = -2 触发 QFont::setPointSize 警告。改用 pixelSize 对齐项目策略。
    f.setPixelSize(f.pixelSize() - 1);  // 小字号
    item->setFont(0, f);
    item->setFont(1, f);
    // 灰色文字
    QBrush grayBrush(QColor("#616161"));
    item->setForeground(0, grayBrush);
    item->setForeground(1, grayBrush);
    // 分组标题不可选中
    item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEditable));
}

QTreeWidgetItem* DebugPanel::createScopeGroup(const QString& title, int count) {
    auto* group = new QTreeWidgetItem(variableTree_);
    group->setText(0, title);
    group->setText(1, count > 0 ? QString("(%1)").arg(count) : QString());
    styleScopeGroupHeader(group);
    return group;
}

void DebugPanel::populateVariableTree(
    const std::vector<std::tuple<QString, QString, QString>>& rows) {
    variableTree_->clear();

    // Round 7: 按作用域分三组
    // scope 值来自 DebugCoordinator.cpp: "局部"(depth 0) / "外层"(depth>0, has parent) / "全局"(depth>0, no parent)
    std::vector<std::tuple<QString, QString>> globalVars;
    std::vector<std::tuple<QString, QString>> localVars;
    std::vector<std::tuple<QString, QString>> closureVars;

    for (const auto& [name, value, scope] : rows) {
        if (scope == QStringLiteral("全局")) {
            globalVars.emplace_back(name, value);
        } else if (scope == QStringLiteral("局部")) {
            localVars.emplace_back(name, value);
        } else {
            // "外层" 或其他 → 闭包作用域
            closureVars.emplace_back(name, value);
        }
    }

    // 固定顺序：全局 → 局部 → 闭包
    auto* globalGroup = createScopeGroup(QStringLiteral("全局作用域"),
                                         static_cast<int>(globalVars.size()));
    for (const auto& [name, value] : globalVars) {
        auto* item = new QTreeWidgetItem(globalGroup);
        item->setText(0, name);
        item->setText(1, value);
        item->setToolTip(1, value);
    }

    auto* localGroup = createScopeGroup(QStringLiteral("当前函数局部作用域"),
                                        static_cast<int>(localVars.size()));
    for (const auto& [name, value] : localVars) {
        auto* item = new QTreeWidgetItem(localGroup);
        item->setText(0, name);
        item->setText(1, value);
        item->setToolTip(1, value);
    }

    auto* closureGroup = createScopeGroup(QStringLiteral("闭包作用域"),
                                          static_cast<int>(closureVars.size()));
    for (const auto& [name, value] : closureVars) {
        auto* item = new QTreeWidgetItem(closureGroup);
        item->setText(0, name);
        item->setText(1, value);
        item->setToolTip(1, value);
    }

    // 展开所有分组
    variableTree_->expandAll();
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
    varLabel->setStyleSheet("color: #616161; font-size: 12px; font-weight: 500; padding: 2px;");
    varLayout->addWidget(varLabel);

    variableTree_ = new QTreeWidget;
    // Round 7: 移除作用域列（分组已替代），仅保留 名称/值 两列
    variableTree_->setHeaderLabels({"名称", "值"});
    variableTree_->header()->setStretchLastSection(true);
    variableTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    variableTree_->setAlternatingRowColors(false);
    variableTree_->setColumnWidth(0, 120);
    variableTree_->setColumnWidth(1, 150);
    variableTree_->setStyleSheet(
        "QTreeWidget { background: #ffffff; border: 1px solid #e5e5e5; }"
        "QTreeWidget::item { padding: 2px 0px; }"
        "QTreeWidget::item:selected { background: #cfe4f5; color: #1e1e1e; }");
    varLayout->addWidget(variableTree_);

    splitter->addWidget(varWidget);

    // ---- 调用栈区域 ----
    auto* stackWidget = new QWidget;
    auto* stackLayout = new QVBoxLayout(stackWidget);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(2);

    auto* stackLabel = new QLabel("调用栈");
    stackLabel->setObjectName("debugStackLabel");
    stackLabel->setStyleSheet("color: #616161; font-size: 12px; font-weight: 500; padding: 2px;");
    stackLayout->addWidget(stackLabel);

    callStackList_ = new QListWidget;
    callStackList_->setAlternatingRowColors(false);
    callStackList_->setFont(GuiTextUtils::monospaceFont(10));
    callStackList_->setStyleSheet(
        "QListWidget { background: #ffffff; border: 1px solid #e5e5e5; }"
        "QListWidget::item { padding: 2px 4px; }"
        "QListWidget::item:selected { background: #cfe4f5; color: #1e1e1e; }");
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
    currentStack_ = stack;

    // 保存当前选中行，刷新后恢复
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

    // 恢复选中行（若仍在有效范围内）
    if (savedRow >= 0 && savedRow < callStackList_->count()) {
        callStackList_->setCurrentRow(savedRow);
    }
}

void DebugPanel::onStackFrameSelected(int index) {
    if (index < 0 || index >= static_cast<int>(currentStack_.size())) return;

    const auto& frame = currentStack_[index];
    // Round 7: 选中栈帧时按作用域分组展示
    // depth==0 的帧标记为"局部"，其他帧标记为"外层"（闭包）
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
