#include "gui/BugHuntPanel.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <sstream>

BugHuntPanel::BugHuntPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runBtn_ = new QPushButton(QString::fromUtf8("运行验证"), this);
    hintBtn_ = new QPushButton(QString::fromUtf8("下一提示"), this);
    answerBtn_ = new QPushButton(QString::fromUtf8("查看答案"), this);
    loadBtn_ = new QPushButton(QString::fromUtf8("加载到主编辑器"), this);
    statusLabel_ = new QLabel(QString::fromUtf8("请选择题目"), this);
    btnBar->addWidget(runBtn_);
    btnBar->addWidget(hintBtn_);
    btnBar->addWidget(answerBtn_);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    mainLayout->addLayout(btnBar);

    // 主体：三栏
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    itemList_ = new QListWidget(this);
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    codeEditor_ = new QTextEdit(this);
    outputEdit_ = new QTextEdit(this);
    outputEdit_->setReadOnly(true);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("代码：")), 0);
    rightLayout->addWidget(codeEditor_, 2);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("运行结果：")), 0);
    rightLayout->addWidget(outputEdit_, 1);

    splitter->addWidget(itemList_);
    splitter->addWidget(descBrowser_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 3);
    splitter->setSizes({160, 300, 400});
    mainLayout->addWidget(splitter, 1);

    populateItemList();

    connect(itemList_, &QListWidget::currentRowChanged,
            this, &BugHuntPanel::onItemSelected);
    connect(runBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onRunVerify);
    connect(hintBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowHint);
    connect(answerBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowAnswer);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (currentItemIndex_ < 0) return;
        const auto& items = BugHuntLibrary::items();
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sourceCode.c_str()));
        statusLabel_->setText(QString::fromUtf8("已请求加载到主编辑器"));
    });
}

void BugHuntPanel::populateItemList() {
    itemList_->clear();
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        QString text = QString::fromUtf8("[%1] %2 — %3")
            .arg(QString::fromUtf8(it.severity.c_str()))
            .arg(QString::fromUtf8(it.id.c_str()))
            .arg(QString::fromUtf8(it.title.c_str()));
        itemList_->addItem(text);
    }
    if (!items.empty()) {
        itemList_->setCurrentRow(0);
    }
}

void BugHuntPanel::onItemSelected(int row) {
    currentItemIndex_ = row;
    hintLevel_ = 0;
    showCurrentItem();
}

void BugHuntPanel::showCurrentItem() {
    const auto& items = BugHuntLibrary::items();
    if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& it = items[currentItemIndex_];
    QString html = QString(
        "<html><body>"
        "<h2>[%1] %2</h2>"
        "<p><b>类别:</b> %3</p>"
        "<p><b>背景:</b></p><p>%4</p>"
        "<p><b>期望行为:</b></p><p>%5</p>"
        "<p><b>Bug 行为:</b></p><p>%6</p>"
        "</body></html>")
        .arg(QString::fromUtf8(it.severity.c_str()))
        .arg(QString::fromUtf8(it.title.c_str()))
        .arg(QString::fromUtf8(it.category.c_str()))
        .arg(QString::fromUtf8(it.background.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(it.expectedBehavior.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(it.buggyBehavior.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(it.sourceCode.c_str()));
    outputEdit_->clear();
    statusLabel_->setText(QString::fromUtf8("当前题目：%1").arg(
        QString::fromUtf8(it.id.c_str())));
}

void BugHuntPanel::onRunVerify() {
    if (currentItemIndex_ < 0) {
        statusLabel_->setText(QString::fromUtf8("请先选择题目"));
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    std::ostringstream out;
    // 运行 Interpreter 路径（最宽容，便于教学观察行为）
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            const auto& diags = parser.getDiagnostics();
            out << "[Parser 错误]\n" << diags.summary() << "\n";
            outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
            return;
        }
        Interpreter interp;
        interp.setOutputCallback([&out](const std::string& s) {
            out << s << "\n";
        });
        interp.execute(*ast);
        out << "\n[Interpreter 路径运行完成]";
    } catch (const std::exception& e) {
        out << "\n[Interpreter 异常] " << e.what();
    }

    // 也可选运行 StackVM 路径观察差异
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!parser.hasErrors()) {
            Compiler compiler;
            auto result = compiler.compile(*ast);
            if (!compiler.getDiagnostics().hasErrors()) {
                VM vm;
                vm.setOutputCallback([&out](const std::string& s) {
                    out << s << "\n";
                });
                auto vmres = vm.execute(result);
                out << "\n[StackVM 路径: " << (int)vmres << "]";
            }
        }
    } catch (const std::exception& e) {
        out << "\n[StackVM 异常] " << e.what();
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("验证完成 — 对比期望/Bug 行为"));
}

void BugHuntPanel::onShowHint() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    if (hintLevel_ >= (int)items[currentItemIndex_].hints.size()) {
        statusLabel_->setText(QString::fromUtf8("已无更多提示"));
        return;
    }
    QString hint = QString::fromUtf8(items[currentItemIndex_].hints[hintLevel_].c_str());
    hintLevel_++;
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 提示 %1 ===\n%2").arg(hintLevel_).arg(hint);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("已显示提示 %1/%2").arg(hintLevel_).arg(
        (int)items[currentItemIndex_].hints.size()));
}

void BugHuntPanel::onShowAnswer() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 答案 ===\n%1").arg(
        QString::fromUtf8(items[currentItemIndex_].explanation.c_str()));
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("答案已显示"));
}
