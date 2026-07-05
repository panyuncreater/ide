#include "gui/SyntaxExplorerPanel.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <sstream>

SyntaxExplorerPanel::SyntaxExplorerPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runBtn_ = new QPushButton(QString::fromUtf8("运行样例"), this);
    loadBtn_ = new QPushButton(QString::fromUtf8("加载到主编辑器"), this);
    statusLabel_ = new QLabel(QString::fromUtf8("请选择产生式"), this);
    btnBar->addWidget(runBtn_);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    mainLayout->addLayout(btnBar);

    // 主体：三栏（产生式列表 + 说明 + 代码/输出）
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    itemList_ = new QListWidget(this);
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    codeEditor_ = new QTextEdit(this);
    outputEdit_ = new QTextEdit(this);
    outputEdit_->setReadOnly(true);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("样例代码：")), 0);
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
            this, &SyntaxExplorerPanel::onItemSelected);
    connect(runBtn_, &QPushButton::clicked,
            this, &SyntaxExplorerPanel::onRunSample);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (currentItemIndex_ < 0) return;
        const auto& items = SyntaxProductionLibrary::items();
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sampleCode.c_str()));
        statusLabel_->setText(QString::fromUtf8("已请求加载到主编辑器"));
    });
}

void SyntaxExplorerPanel::populateItemList() {
    itemList_->clear();
    const auto& items = SyntaxProductionLibrary::items();
    for (const auto& p : items) {
        itemList_->addItem(QString::fromUtf8(p.name.c_str()) + " — " +
                            QString::fromUtf8(p.title.c_str()));
    }
    if (!items.empty()) {
        itemList_->setCurrentRow(0);
    }
}

void SyntaxExplorerPanel::onItemSelected(int row) {
    currentItemIndex_ = row;
    showCurrentItem();
}

void SyntaxExplorerPanel::showCurrentItem() {
    const auto& items = SyntaxProductionLibrary::items();
    if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& p = items[currentItemIndex_];
    QString html = QString(
        "<html><body>"
        "<h2>%1</h2>"
        "<p><b>EBNF:</b></p><pre>%2</pre>"
        "<p><b>说明:</b></p><p>%3</p>"
        "</body></html>")
        .arg(QString::fromUtf8(p.title.c_str()))
        .arg(QString::fromUtf8(p.ebnf.c_str()).toHtmlEscaped())
        .arg(QString::fromUtf8(p.description.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(p.sampleCode.c_str()));
    outputEdit_->clear();
    statusLabel_->setText(QString::fromUtf8("当前产生式：%1").arg(
        QString::fromUtf8(p.title.c_str())));
}

void SyntaxExplorerPanel::onRunSample() {
    if (currentItemIndex_ < 0) {
        statusLabel_->setText(QString::fromUtf8("请先选择产生式"));
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    std::ostringstream out;
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
        out << "[运行完成]";
    } catch (const std::exception& e) {
        out << "[异常] " << e.what();
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("运行结束"));
}
