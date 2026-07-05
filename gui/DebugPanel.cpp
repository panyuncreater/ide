#include "gui/DebugPanel.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
#include <QHeaderView>
#include <QListWidgetItem>  // BUG-DBG-G3 fix: 超长调用栈提示项
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
    // BUG-DBG-G4 fix (P2): pixelSize() 在字体未显式设置 pixelSize 时返回 1（而非
    // 实际像素值），直接 -1 会得到 0 甚至负数导致字体渲染异常。增加 >1 守卫，
    // 仅当 pixelSize 有效（>1）时才缩减 1 像素，否则保持原字号。
    int ps = f.pixelSize();
    if (ps > 1) f.setPixelSize(ps - 1);  // 小字号
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
    // BUG-DBG-G5 (P2, 功能缺失/已知限制): 变量值列当前为只读展示，不支持就地编辑。
    // 完整实现需双向绑定机制：itemChanged 信号 → 写回 Interpreter/VM 当前作用域变量、
    // 类型校验、COW 容器写回、跨后端（Interpreter/StackVM/RegisterVM）一致性处理。
    // 工程量大且调试场景下修改变量易引发状态不一致，作为功能增强暂不实现。
    // TODO: 未来可通过 DebugController::setVariable(name, value) 接口实现。
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

    // BUG-DBG-G3 fix (P2): 限制调用栈显示帧数上限，避免深度递归（如未优化的
    // 斐波那契 fib(40)）产生上万帧导致 QListWidget 卡顿与内存膨胀。
    // 上限 200 帧对调试场景足够（用户通常只关心最近的调用层级）。
    constexpr int MAX_CALL_STACK_DISPLAY = 200;
    int displayed = 0;
    for (size_t i = 0; i < stack.size() && displayed < MAX_CALL_STACK_DISPLAY; ++i) {
        const auto& frame = stack[i];
        QString text = QString("函数: %1 @ 行 %2 (深度: %3)")
                           .arg(QString::fromStdString(frame.functionName))
                           .arg(frame.line)
                           .arg(frame.depth);
        callStackList_->addItem(text);
        ++displayed;
    }
    if (static_cast<int>(stack.size()) > MAX_CALL_STACK_DISPLAY) {
        // 添加提示项标注未显示的帧数
        auto* item = new QListWidgetItem(
            QString::fromUtf8("... (还有 %1 帧未显示)")
                .arg(static_cast<int>(stack.size()) - MAX_CALL_STACK_DISPLAY));
        // 提示项设为不可选中，避免与真实栈帧混淆
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        callStackList_->addItem(item);
    }
    callStackList_->blockSignals(false);

    // 恢复选中行（若仍在有效范围内）
    // BUG-DBG-G1 fix (P2): 原 setCurrentRow(savedRow) 在 blockSignals(false) 之后调用，
    // 会触发 currentRowChanged 信号 → onStackFrameSelected → populateVariableTree，
    // 覆盖刚由 updateVariables 设置的完整变量视图（仅显示选中帧的局部变量）。
    // 此处保持 blockSignals(true) 阻塞信号，仅恢复视觉选中状态，不触发变量树更新，
    // 让用户保留 updateVariables 提供的完整作用域视图。
    if (savedRow >= 0 && savedRow < callStackList_->count()) {
        bool wasBlocked = callStackList_->blockSignals(true);
        callStackList_->setCurrentRow(savedRow);
        callStackList_->blockSignals(wasBlocked);
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
