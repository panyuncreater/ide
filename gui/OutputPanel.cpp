#include "gui/OutputPanel.h"
#include <QLabel>

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
    outputLabel->setStyleSheet("font-weight: bold; color: #333;");
    outputHeaderLayout->addWidget(outputLabel);
    outputHeaderLayout->addStretch();

    clearOutputBtn_ = new QPushButton("清空");
    clearOutputBtn_->setFixedWidth(60);
    clearOutputBtn_->setStyleSheet("QPushButton { padding: 2px 8px; }");
    outputHeaderLayout->addWidget(clearOutputBtn_);
    outputLayout->addLayout(outputHeaderLayout);

    outputEdit_ = new QTextEdit;
    outputEdit_->setReadOnly(true);
    outputEdit_->setFont(QFont("Consolas", 10));
    outputEdit_->setStyleSheet("QTextEdit { background-color: #FFFFFF; border: 1px solid #DDD; }");
    outputLayout->addWidget(outputEdit_);

    splitter->addWidget(outputWidget);

    // ---- 错误区域 ----
    auto* errorWidget = new QWidget;
    auto* errorLayout = new QVBoxLayout(errorWidget);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(2);

    auto* errorHeaderLayout = new QHBoxLayout;
    auto* errorLabel = new QLabel("错误");
    errorLabel->setStyleSheet("font-weight: bold; color: #C00;");
    errorHeaderLayout->addWidget(errorLabel);
    errorHeaderLayout->addStretch();

    clearErrorBtn_ = new QPushButton("清空");
    clearErrorBtn_->setFixedWidth(60);
    clearErrorBtn_->setStyleSheet("QPushButton { padding: 2px 8px; }");
    errorHeaderLayout->addWidget(clearErrorBtn_);
    errorLayout->addLayout(errorHeaderLayout);

    errorEdit_ = new QTextEdit;
    errorEdit_->setReadOnly(true);
    errorEdit_->setFont(QFont("Consolas", 10));
    errorEdit_->setStyleSheet("QTextEdit { background-color: #FFF5F5; border: 1px solid #FCC; }");
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
    outputEdit_->append(text);
}

void OutputPanel::appendError(const QString& text) {
    errorEdit_->append(text);
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
