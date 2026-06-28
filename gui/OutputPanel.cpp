#include "gui/OutputPanel.h"
#include "gui/GuiTextUtils.h"  // P1-12 fix: 共享文本追加逻辑
#include <QLabel>
#include <QTextCursor>
#include <QTextCharFormat>

// ============================================================
// OutputPanel 输出与错误面板实现
// ============================================================

OutputPanel::OutputPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Vertical, this);

    // ---- 输出区域 ----
    auto* outputWidget = new QWidget;
    auto* outputLayout = new QVBoxLayout(outputWidget);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(2);

    auto* outputHeaderLayout = new QHBoxLayout;
    auto* outputLabel = new QLabel("输出");
    outputLabel->setObjectName("outputLabel");
    outputHeaderLayout->addWidget(outputLabel);
    outputHeaderLayout->addStretch();

    clearOutputBtn_ = new QPushButton("清空");
    clearOutputBtn_->setObjectName("clearOutputBtn");
    clearOutputBtn_->setFixedWidth(60);
    outputHeaderLayout->addWidget(clearOutputBtn_);
    outputLayout->addLayout(outputHeaderLayout);

    outputEdit_ = new QTextEdit;
    outputEdit_->setReadOnly(true);
    outputEdit_->setFont(GuiTextUtils::monospaceFont(10));
    outputEdit_->document()->setMaximumBlockCount(10000);  // OP-2 fix: 限制输出行数
    outputEdit_->setObjectName("outputEdit");
    outputLayout->addWidget(outputEdit_);

    splitter->addWidget(outputWidget);

    // ---- 错误区域 ----
    auto* errorWidget = new QWidget;
    auto* errorLayout = new QVBoxLayout(errorWidget);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(2);

    auto* errorHeaderLayout = new QHBoxLayout;
    auto* errorLabel = new QLabel("错误");
    errorLabel->setObjectName("errorLabel");
    errorHeaderLayout->addWidget(errorLabel);
    errorHeaderLayout->addStretch();

    clearErrorBtn_ = new QPushButton("清空");
    clearErrorBtn_->setObjectName("clearErrorBtn");
    clearErrorBtn_->setFixedWidth(60);
    errorHeaderLayout->addWidget(clearErrorBtn_);
    errorLayout->addLayout(errorHeaderLayout);

    errorEdit_ = new QTextEdit;
    errorEdit_->setReadOnly(true);
    errorEdit_->setFont(GuiTextUtils::monospaceFont(10));
    errorEdit_->document()->setMaximumBlockCount(5000);  // OP-2 fix: 限制错误行数
    errorEdit_->setObjectName("errorEdit");
    errorLayout->addWidget(errorEdit_);

    splitter->addWidget(errorWidget);

    // 设置分割比例
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter);

    // 连接信号
    connect(clearOutputBtn_, &QPushButton::clicked, this, &OutputPanel::clearOutput);
    connect(clearErrorBtn_, &QPushButton::clicked, this, &OutputPanel::clearErrors);
}

void OutputPanel::appendOutput(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine
    GuiTextUtils::appendLine(outputEdit_, text);
}

void OutputPanel::appendError(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine，使用红色格式
    QTextCharFormat fmt;
    fmt.setForeground(Qt::red);
    GuiTextUtils::appendLine(errorEdit_, text, &fmt);
}

void OutputPanel::clearAll() {
    outputEdit_->clear();
    errorEdit_->clear();
}

void OutputPanel::clearOutput() {
    outputEdit_->clear();
}

void OutputPanel::clearErrors() {
    errorEdit_->clear();
}
