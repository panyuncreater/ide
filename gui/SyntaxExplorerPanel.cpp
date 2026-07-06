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
#include <QButtonGroup>
#include <sstream>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（CaptionLabel）

SyntaxExplorerPanel::SyntaxExplorerPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏（运行/加载 + 状态）
    auto* btnBar = new QHBoxLayout;
    runBtn_ = new PrimaryPushButton(QString::fromUtf8("运行样例"), this);
    loadBtn_ = new QPushButton(QString::fromUtf8("加载到主编辑器"), this);
    statusLabel_ = new CaptionLabel(QString::fromUtf8("请选择产生式"), this);
    btnBar->addWidget(runBtn_);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    mainLayout->addLayout(btnBar);

    // F8: 视图模式切换按钮栏（EBNF / 自然语言 / 混合）
    auto* viewBar = new QHBoxLayout;
    viewBar->addWidget(new QLabel(QString::fromUtf8("视图模式："), this));
    ebnfBtn_    = new QPushButton(QString::fromUtf8("EBNF"), this);
    naturalBtn_ = new QPushButton(QString::fromUtf8("自然语言"), this);
    mixedBtn_   = new QPushButton(QString::fromUtf8("混合"), this);
    ebnfBtn_   ->setCheckable(true);
    naturalBtn_->setCheckable(true);
    mixedBtn_   ->setCheckable(true);
    // 用 QButtonGroup 实现互斥切换
    auto* viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    viewGroup->addButton(ebnfBtn_,    static_cast<int>(ViewMode::EBNF));
    viewGroup->addButton(naturalBtn_, static_cast<int>(ViewMode::Natural));
    viewGroup->addButton(mixedBtn_,   static_cast<int>(ViewMode::Mixed));
    viewBar->addWidget(ebnfBtn_);
    viewBar->addWidget(naturalBtn_);
    viewBar->addWidget(mixedBtn_);
    viewBar->addStretch();
    mainLayout->addLayout(viewBar);
    // 默认选中混合视图
    mixedBtn_->setChecked(true);
    updateViewModeButtons();
    connect(viewGroup, &QButtonGroup::idClicked,
            this, &SyntaxExplorerPanel::onViewModeChanged);

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

void SyntaxExplorerPanel::onViewModeChanged(int mode) {
    viewMode_ = static_cast<ViewMode>(mode);
    updateViewModeButtons();
    showCurrentItem();  // 切换视图后立即刷新
}

void SyntaxExplorerPanel::updateViewModeButtons() {
    // 选中态由 QButtonGroup 自动管理，这里仅做状态同步
    // (按钮的 checked 状态已由 setChecked 设置)
}

void SyntaxExplorerPanel::showCurrentItem() {
    const auto& items = SyntaxProductionLibrary::items();
    if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& p = items[currentItemIndex_];

    // F8: 根据视图模式组装 HTML 内容
    QString html;
    QString title = QString::fromUtf8(p.title.c_str());
    QString ebnf  = QString::fromUtf8(p.ebnf.c_str()).toHtmlEscaped();
    QString desc  = QString::fromUtf8(p.description.c_str()).toHtmlEscaped();
    QString nat   = QString::fromUtf8(p.naturalLanguage.c_str()).toHtmlEscaped();

    switch (viewMode_) {
        case ViewMode::EBNF:
            html = QString(
                "<html><body>"
                "<h2>%1</h2>"
                "<p><b>EBNF:</b></p><pre>%2</pre>"
                "<p><b>说明:</b></p><p>%3</p>"
                "</body></html>")
                .arg(title).arg(ebnf).arg(desc);
            break;
        case ViewMode::Natural:
            html = QString(
                "<html><body>"
                "<h2>%1</h2>"
                "<p><b>📝 自然语言描述:</b></p><pre>%2</pre>"
                "</body></html>")
                .arg(title).arg(nat);
            break;
        case ViewMode::Mixed:
        default:
            html = QString(
                "<html><body>"
                "<h2>%1</h2>"
                "<p><b>EBNF:</b></p><pre>%2</pre>"
                "<p><b>说明:</b></p><p>%3</p>"
                "<hr>"
                "<p><b>📝 自然语言描述:</b></p><pre>%4</pre>"
                "</body></html>")
                .arg(title).arg(ebnf).arg(desc).arg(nat);
            break;
    }
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
