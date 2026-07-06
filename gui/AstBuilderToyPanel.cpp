// ============================================================
// AstBuilderToyPanel.cpp — AST 节点搭建玩具面板实现（功能 4）
// ------------------------------------------------------------
// 实现要点：
//   1. 顶部 QComboBox 选择题目（6 题）+ 进度 QLabel
//   2. 左侧 QVBoxLayout 工具箱：7 个 QPushButton（Number / Add / Sub /
//      Mul / Div / Print / VarDecl）
//   3. 右侧 QTreeWidget 显示已搭建的树（1 列：节点标签）
//   4. 底部 5 个操作按钮 + 反馈 QLabel + 题目说明 QLabel
//   5. 交互：点击工具箱 → 弹输入对话框（Number / VarDecl）→ 添加节点
//      到选中项之下或树根
//   6. 检查答案：把 QTreeWidget 转 TreeNode，与 answer 递归比对
//   7. 所有用户可见文本用 mlTr() 包裹（i18n）
//   8. Delete 键删除选中节点（通过 QShortcut）
// ============================================================

#include "gui/AstBuilderToyPanel.h"
#include "gui/I18n.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTreeWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QInputDialog>
#include <QShortcut>
#include <QKeySequence>
#include <QMessageBox>
#include <QHeaderView>
#include <algorithm>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（StrongBodyLabel）

// ============================================================
// 构造函数
// ============================================================
AstBuilderToyPanel::AstBuilderToyPanel(QWidget* parent)
    : QWidget(parent) {

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // ---- 顶部：题目选择 + 进度 ----
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(mlTr("题目：")), 0);
    levelCombo_ = new QComboBox(this);
    const auto& ls = AstToyLibrary::levels();
    for (const auto& lv : ls) {
        QString text = QString::fromUtf8("L%1 (%2) — %3")
            .arg(lv.level)
            .arg(QString::fromUtf8(lv.difficulty == 1 ? "⭐"
                                       : lv.difficulty == 2 ? "⭐⭐"
                                       : "⭐⭐⭐"))
            .arg(QString::fromUtf8(lv.targetExpression.c_str()));
        levelCombo_->addItem(text);
    }
    progressLabel_ = new StrongBodyLabel(mlTr("进度: 0/6"), this);
    topBar->addWidget(levelCombo_, 1);
    topBar->addWidget(progressLabel_);
    mainLayout->addLayout(topBar);

    // ---- 主体：左工具箱 + 右画布 ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // 左侧：工具箱
    auto* toolboxContainer = new QWidget(this);
    auto* toolboxLayout = new QVBoxLayout(toolboxContainer);
    toolboxLayout->setContentsMargins(0, 0, 0, 0);
    toolboxLayout->setSpacing(4);
    toolboxLayout->addWidget(new QLabel(mlTr("节点工具箱：")));
    btnNumber_  = new QPushButton(mlTr("🔢 Number（数字）"), this);
    btnAdd_     = new QPushButton(mlTr("➕ Add（+）"), this);
    btnSub_     = new QPushButton(mlTr("➖ Sub（-）"), this);
    btnMul_     = new QPushButton(mlTr("✖ Mul（*）"), this);
    btnDiv_     = new QPushButton(mlTr("➗ Div（/）"), this);
    btnPrint_   = new QPushButton(mlTr("🖨 Print"), this);
    btnVarDecl_ = new QPushButton(mlTr("📦 VarDecl（变量声明）"), this);
    toolboxLayout->addWidget(btnNumber_);
    toolboxLayout->addWidget(btnAdd_);
    toolboxLayout->addWidget(btnSub_);
    toolboxLayout->addWidget(btnMul_);
    toolboxLayout->addWidget(btnDiv_);
    toolboxLayout->addWidget(btnPrint_);
    toolboxLayout->addWidget(btnVarDecl_);
    toolboxLayout->addStretch(1);

    // 右侧：画布
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(1);
    tree_->setHeaderLabel(mlTr("你搭建的 AST 树"));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setExpandsOnDoubleClick(true);
    tree_->header()->setStretchLastSection(true);

    splitter->addWidget(toolboxContainer);
    splitter->addWidget(tree_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({220, 480});
    mainLayout->addWidget(splitter, 1);

    // ---- 题目说明区 ----
    descLabel_ = new QLabel(this);
    descLabel_->setWordWrap(true);
    descLabel_->setStyleSheet(QString::fromUtf8(
        "QLabel { background: #f5f5f5; padding: 6px; border-radius: 4px; }"));
    mainLayout->addWidget(descLabel_);

    // ---- 底部：操作按钮 + 反馈 ----
    auto* bottomBar = new QHBoxLayout;
    checkBtn_  = new PrimaryPushButton(mlTr("✓ 检查"), this);
    answerBtn_ = new QPushButton(mlTr("👁 查看标准答案"), this);
    nextBtn_   = new QPushButton(mlTr("➡ 下一题"), this);
    clearBtn_  = new QPushButton(mlTr("🔄 清空"), this);
    deleteBtn_ = new QPushButton(mlTr("🗑 删除选中节点"), this);
    bottomBar->addWidget(checkBtn_);
    bottomBar->addWidget(answerBtn_);
    bottomBar->addWidget(nextBtn_);
    bottomBar->addWidget(deleteBtn_);
    bottomBar->addWidget(clearBtn_);
    bottomBar->addStretch(1);
    mainLayout->addLayout(bottomBar);

    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setStyleSheet(QString::fromUtf8(
        "QLabel { padding: 4px; }"));
    mainLayout->addWidget(feedback_);

    // ---- 信号槽 ----
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AstBuilderToyPanel::onLevelChanged);
    connect(btnNumber_,  &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxNumber);
    connect(btnAdd_,     &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxAdd);
    connect(btnSub_,     &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxSub);
    connect(btnMul_,     &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxMul);
    connect(btnDiv_,     &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxDiv);
    connect(btnPrint_,   &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxPrint);
    connect(btnVarDecl_, &QPushButton::clicked, this, &AstBuilderToyPanel::onToolboxVarDecl);
    connect(checkBtn_,   &QPushButton::clicked, this, &AstBuilderToyPanel::onCheck);
    connect(answerBtn_,  &QPushButton::clicked, this, &AstBuilderToyPanel::onShowAnswer);
    connect(nextBtn_,    &QPushButton::clicked, this, &AstBuilderToyPanel::onNext);
    connect(clearBtn_,   &QPushButton::clicked, this, &AstBuilderToyPanel::onClear);
    connect(deleteBtn_,  &QPushButton::clicked, this, &AstBuilderToyPanel::onDeleteSelected);
    connect(tree_,       &QTreeWidget::currentItemChanged,
            this, &AstBuilderToyPanel::onTreeItemChanged);

    // Delete 键删除选中节点
    auto* delShortcut = new QShortcut(QKeySequence::Delete, this);
    delShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delShortcut, &QShortcut::activated, this, &AstBuilderToyPanel::onDeleteSelected);
    // Backspace 也支持删除（macOS 键盘）
    auto* bsShortcut = new QShortcut(QKeySequence::Backspace, this);
    bsShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(bsShortcut, &QShortcut::activated, this, &AstBuilderToyPanel::onDeleteSelected);

    // 初始化：加载第 1 题
    if (!ls.empty()) {
        loadLevel(0);
    }
    refreshProgress();
}

// ============================================================
// 题目切换
// ============================================================
void AstBuilderToyPanel::onLevelChanged(int index) {
    if (index < 0) return;
    loadLevel(index);
}

void AstBuilderToyPanel::loadLevel(int index) {
    currentIndex_ = index;
    completed_ = false;
    tree_->clear();
    feedback_->clear();

    const auto& ls = AstToyLibrary::levels();
    if (index < 0 || index >= (int)ls.size()) return;
    const auto& lv = ls[index];

    QString desc = QString::fromUtf8(
        "<b>题目 %1</b>：搭建 <code>%2</code> 的 AST<br>"
        "<b>教学点</b>：%3<br>"
        "<b>提示</b>：%4")
        .arg(lv.level)
        .arg(QString::fromUtf8(lv.targetExpression.c_str()))
        .arg(QString::fromUtf8(lv.teachingPoint.c_str()))
        .arg(QString::fromUtf8(lv.hint.c_str()));
    descLabel_->setText(desc);
    refreshProgress();
}

void AstBuilderToyPanel::refreshProgress() {
    feedback_->setText(QString());
    progressLabel_->setText(QString::fromUtf8("进度: %1/6").arg(completedLevels_.size()));
}

// ============================================================
// 工具箱按钮处理
// ============================================================
void AstBuilderToyPanel::onToolboxNumber() {
    QString value;
    if (!promptValue(mlTr("添加 Number 节点"), mlTr("请输入数字值（如 42）："), value)) {
        return;
    }
    value = value.trimmed();
    if (value.isEmpty()) {
        feedback_->setText(mlTr("❌ 数字值不能为空"));
        return;
    }
    addNodeWithLabel(QString::fromUtf8("Number:%1").arg(value));
}

void AstBuilderToyPanel::onToolboxAdd() {
    addNodeWithLabel(QString::fromUtf8("BinaryOp:+"));
}
void AstBuilderToyPanel::onToolboxSub() {
    addNodeWithLabel(QString::fromUtf8("BinaryOp:-"));
}
void AstBuilderToyPanel::onToolboxMul() {
    addNodeWithLabel(QString::fromUtf8("BinaryOp:*"));
}
void AstBuilderToyPanel::onToolboxDiv() {
    addNodeWithLabel(QString::fromUtf8("BinaryOp:/"));
}
void AstBuilderToyPanel::onToolboxPrint() {
    addNodeWithLabel(QString::fromUtf8("Print"));
}
void AstBuilderToyPanel::onToolboxVarDecl() {
    QString name;
    if (!promptValue(mlTr("添加 VarDecl 节点"), mlTr("请输入变量名（如 x）："), name)) {
        return;
    }
    name = name.trimmed();
    if (name.isEmpty()) {
        feedback_->setText(mlTr("❌ 变量名不能为空"));
        return;
    }
    addNodeWithLabel(QString::fromUtf8("VarDecl:%1").arg(name));
}

// ============================================================
// 添加节点到树
// ============================================================
void AstBuilderToyPanel::addNodeWithLabel(const QString& label) {
    QTreeWidgetItem* current = tree_->currentItem();
    if (!current) {
        // 无选中项
        if (tree_->topLevelItemCount() == 0) {
            // 树空 → 添加为根
            auto* item = new QTreeWidgetItem(tree_);
            item->setText(0, label);
            tree_->addTopLevelItem(item);
            tree_->setCurrentItem(item);
            feedback_->setText(QString::fromUtf8("✓ 已添加根节点：%1").arg(label));
        } else {
            feedback_->setText(mlTr("⚠ 请先选中一个节点作为父节点，或点击「清空」重新开始"));
        }
        return;
    }
    // 有选中项 → 添加为子节点
    auto* child = new QTreeWidgetItem(current);
    child->setText(0, label);
    current->addChild(child);
    tree_->setCurrentItem(child);
    tree_->expandItem(current);
    feedback_->setText(QString::fromUtf8("✓ 已添加子节点：%1（父节点：%2）")
                           .arg(label).arg(current->text(0)));
}

// ============================================================
// 弹输入对话框
// ============================================================
bool AstBuilderToyPanel::promptValue(const QString& title, const QString& label,
                                     QString& outValue) {
    bool ok = false;
    QString text = QInputDialog::getText(this, title, label,
                                         QLineEdit::Normal, QString(), &ok);
    if (!ok) return false;
    outValue = text;
    return true;
}

// ============================================================
// 检查答案
// ============================================================
void AstBuilderToyPanel::onCheck() {
    if (tree_->topLevelItemCount() == 0) {
        feedback_->setText(mlTr("❌ 画布为空，请先搭建 AST"));
        return;
    }
    if (tree_->topLevelItemCount() > 1) {
        feedback_->setText(mlTr("❌ 画布有多个根节点，AST 只能有 1 个根"));
        return;
    }

    AstToyLevel::TreeNode userTree;
    if (!treeToTreeNode(userTree)) {
        feedback_->setText(mlTr("❌ 无法读取你搭建的树"));
        return;
    }

    const auto& ls = AstToyLibrary::levels();
    if (currentIndex_ < 0 || currentIndex_ >= (int)ls.size()) return;
    const auto& ans = ls[currentIndex_].answer;

    if (treeEquals(userTree, ans)) {
        completed_ = true;
        feedback_->setText(mlTr("🎉 完全正确！拓扑结构与标准答案一致。"));
        markCurrentCompleted();
    } else {
        QString userStr, ansStr;
        treeNodeToString(userTree, userStr);
        treeNodeToString(ans, ansStr);
        feedback_->setText(QString::fromUtf8(
            "❌ 不匹配。\n\n你的树：\n%1\n\n标准答案：\n%2").arg(userStr).arg(ansStr));
    }
}

void AstBuilderToyPanel::markCurrentCompleted() {
    const auto& ls = AstToyLibrary::levels();
    if (currentIndex_ < 0 || currentIndex_ >= (int)ls.size()) return;
    int lvl = ls[currentIndex_].level;
    if (std::find(completedLevels_.begin(), completedLevels_.end(), lvl)
        == completedLevels_.end()) {
        completedLevels_.push_back(lvl);
        refreshProgress();
    }
    QString levelId = QString::fromUtf8("ast-toy-level-%1").arg(lvl);
    emit activityCompleted(levelId);
}

// ============================================================
// 查看标准答案
// ============================================================
void AstBuilderToyPanel::onShowAnswer() {
    const auto& ls = AstToyLibrary::levels();
    if (currentIndex_ < 0 || currentIndex_ >= (int)ls.size()) return;
    const auto& ans = ls[currentIndex_].answer;

    tree_->clear();
    QTreeWidgetItem* root = treeNodeToItem(ans);
    if (root) {
        tree_->addTopLevelItem(root);
        tree_->expandAll();
        tree_->setCurrentItem(root);
    }
    feedback_->setText(mlTr("👁 已展示标准答案。点击「清空」后可重新尝试。"));
}

// ============================================================
// 下一题
// ============================================================
void AstBuilderToyPanel::onNext() {
    int next = (currentIndex_ + 1) % AstToyLibrary::levelCount();
    levelCombo_->setCurrentIndex(next);
}

// ============================================================
// 清空
// ============================================================
void AstBuilderToyPanel::onClear() {
    tree_->clear();
    feedback_->clear();
    completed_ = false;
}

// ============================================================
// 删除选中节点
// ============================================================
void AstBuilderToyPanel::onDeleteSelected() {
    QTreeWidgetItem* current = tree_->currentItem();
    if (!current) {
        feedback_->setText(mlTr("⚠ 未选中任何节点"));
        return;
    }
    // 从父节点移除（若是顶层则从 tree 移除）
    QTreeWidgetItem* parent = current->parent();
    if (parent) {
        parent->removeChild(current);
    } else {
        int idx = tree_->indexOfTopLevelItem(current);
        if (idx >= 0) tree_->takeTopLevelItem(idx);
    }
    delete current;
    feedback_->setText(mlTr("🗑 已删除选中节点（及其子树）"));
}

void AstBuilderToyPanel::onTreeItemChanged(QTreeWidgetItem* /*current*/) {
    // 当前仅占位，可扩展为选中节点信息显示
}

// ============================================================
// 树转 TreeNode
// ============================================================
bool AstBuilderToyPanel::treeToTreeNode(AstToyLevel::TreeNode& out) const {
    if (tree_->topLevelItemCount() == 0) return false;
    QTreeWidgetItem* root = tree_->topLevelItem(0);
    out = itemToTreeNode(root);
    return true;
}

AstToyLevel::TreeNode AstBuilderToyPanel::itemToTreeNode(const QTreeWidgetItem* item) {
    AstToyLevel::TreeNode node;
    node.label = item->text(0).toStdString();
    for (int i = 0; i < item->childCount(); ++i) {
        node.children.push_back(itemToTreeNode(item->child(i)));
    }
    return node;
}

QTreeWidgetItem* AstBuilderToyPanel::treeNodeToItem(const AstToyLevel::TreeNode& node) {
    auto* item = new QTreeWidgetItem();
    item->setText(0, QString::fromUtf8(node.label.c_str()));
    for (const auto& c : node.children) {
        QTreeWidgetItem* childItem = treeNodeToItem(c);
        if (childItem) item->addChild(childItem);
    }
    return item;
}

// ============================================================
// 树拓扑比对（label 严格相等 + children 顺序一致）
// ============================================================
bool AstBuilderToyPanel::treeEquals(const AstToyLevel::TreeNode& a,
                                    const AstToyLevel::TreeNode& b) {
    if (a.label != b.label) return false;
    if (a.children.size() != b.children.size()) return false;
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (!treeEquals(a.children[i], b.children[i])) return false;
    }
    return true;
}

void AstBuilderToyPanel::treeNodeToString(const AstToyLevel::TreeNode& node,
                                          QString& out, int indent) {
    QString pad(indent * 2, QChar::fromLatin1(' '));
    out += pad + QString::fromUtf8(node.label.c_str()) + QString::fromUtf8("\n");
    for (const auto& c : node.children) {
        treeNodeToString(c, out, indent + 1);
    }
}
