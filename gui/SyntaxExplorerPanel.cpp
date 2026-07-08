/**
 * SyntaxExplorerPanel.cpp — 语法探索面板实现
 *
 * 以「产生式列表 + 说明 + 样例代码/输出」三栏布局，帮助学习者对照
 * MiniLang 的语法产生式，支持 EBNF / 自然语言 / 混合三种视图模式。
 * 可一键运行样例（Lexer → Parser → Interpreter）并将代码加载到主编辑器。
 */

#include "gui/SyntaxExplorerPanel.h"
#include "app/IdeController.h"
#include "gui/MarkdownRenderer.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QVBoxLayout>
#include <sstream>

#include "Label.h"      // QFluentKit（CaptionLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

/// 构造面板：组装顶部按钮栏（运行/加载）、视图模式切换栏，以及
/// 主体三栏（产生式列表 / 说明浏览器 / 样例代码 + 输出）。
/// 用 QButtonGroup 互斥管理三种视图按钮，默认选中「混合」视图并加载首项。
SyntaxExplorerPanel::SyntaxExplorerPanel(QWidget* parent) : QWidget(parent) {
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
    ebnfBtn_ = new QPushButton(QString::fromUtf8("EBNF"), this);
    naturalBtn_ = new QPushButton(QString::fromUtf8("自然语言"), this);
    mixedBtn_ = new QPushButton(QString::fromUtf8("混合"), this);
    ebnfBtn_->setCheckable(true);
    naturalBtn_->setCheckable(true);
    mixedBtn_->setCheckable(true);
    // 用 QButtonGroup 实现互斥切换
    auto* viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    viewGroup->addButton(ebnfBtn_, static_cast<int>(ViewMode::EBNF));
    viewGroup->addButton(naturalBtn_, static_cast<int>(ViewMode::Natural));
    viewGroup->addButton(mixedBtn_, static_cast<int>(ViewMode::Mixed));
    viewBar->addWidget(ebnfBtn_);
    viewBar->addWidget(naturalBtn_);
    viewBar->addWidget(mixedBtn_);
    viewBar->addStretch();
    mainLayout->addLayout(viewBar);
    // 默认选中混合视图
    mixedBtn_->setChecked(true);
    updateViewModeButtons();
    connect(viewGroup, &QButtonGroup::idClicked, this, &SyntaxExplorerPanel::onViewModeChanged);

    // 主体：三栏（产生式列表 + 说明 + 代码/输出）
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    itemList_ = new QListWidget(this);
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    codeEditor_ = new QTextEdit(this);
    // AUDIT-P2 fix: 样例代码设为只读——onRunSample 同步执行 Lexer→Parser→Interpreter，
    // 无 busy_ 守卫、无超时、无步骤上限。若允许编辑，用户输入 while(true){} 会无限循环，
    // IDE 完全冻结只能强制杀死。样例代码通过产生式选择切换，教学价值不受影响。
    codeEditor_->setReadOnly(true);
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

    // 先连接信号再填充列表 —— populateItemList() 末尾会 setCurrentRow(0)，
    // 若 connect 在其后则首次选中不会触发 onItemSelected，导致首次进入面板
    // 时右侧详情/代码区为空（需手动切换章节才加载）。
    connect(itemList_, &QListWidget::currentRowChanged, this, &SyntaxExplorerPanel::onItemSelected);
    connect(runBtn_, &QPushButton::clicked, this, &SyntaxExplorerPanel::onRunSample);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (currentItemIndex_ < 0)
            return;
        const auto& items = SyntaxProductionLibrary::items();
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sampleCode.c_str()));
        statusLabel_->setText(QString::fromUtf8("已请求加载到主编辑器"));
    });

    populateItemList();
    // 兜底：即使 setCurrentRow(0) 未触发 currentRowChanged（如列表为空后首项），
    // 也显式加载第一项内容，确保首次进入即显示。
    if (currentItemIndex_ < 0 && itemList_->count() > 0) {
        itemList_->setCurrentRow(0);
    }
    if (currentItemIndex_ >= 0) {
        showCurrentItem();
    }
}

/// 用 SyntaxProductionLibrary 的全部条目（名称 + 标题）填充左侧产生式列表，
/// 并默认选中首项（触发 currentRowChanged → onItemSelected 加载右侧内容）。
void SyntaxExplorerPanel::populateItemList() {
    itemList_->clear();
    const auto& items = SyntaxProductionLibrary::items();
    for (const auto& p : items) {
        itemList_->addItem(QString::fromUtf8(p.name.c_str()) + " — " + QString::fromUtf8(p.title.c_str()));
    }
    if (!items.empty()) {
        itemList_->setCurrentRow(0);
    }
}

/// 列表选中项变化时记录当前索引并刷新右侧详情/代码区。
void SyntaxExplorerPanel::onItemSelected(int row) {
    currentItemIndex_ = row;
    showCurrentItem();
}

/// 视图模式按钮切换时更新当前模式枚举、同步按钮选中态并立即重渲染当前条目。
void SyntaxExplorerPanel::onViewModeChanged(int mode) {
    viewMode_ = static_cast<ViewMode>(mode);
    updateViewModeButtons();
    showCurrentItem(); // 切换视图后立即刷新
}

/// 同步视图模式按钮的选中态。实际单选由 QButtonGroup 自动管理，
/// 此处为空实现，保留为样式/状态扩展点。
void SyntaxExplorerPanel::updateViewModeButtons() {
    // 选中态由 QButtonGroup 自动管理，这里仅做状态同步
    // (按钮的 checked 状态已由 setChecked 设置)
}

/// 根据当前选中条目与视图模式组装 HTML（标题 / EBNF / 说明 / 自然语言描述），
/// 渲染到说明浏览器，并将样例代码填入代码编辑器、清空输出区与状态栏。
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
    QString ebnf = QString::fromUtf8(p.ebnf.c_str()).toHtmlEscaped();
    QString desc = MarkdownRenderer::markdownToHtmlFragment(p.description);
    QString nat = MarkdownRenderer::markdownToHtmlFragment(p.naturalLanguage);

    switch (viewMode_) {
    case ViewMode::EBNF:
        html = QString("<html><body>"
                       "<h2>%1</h2>"
                       "<p><b>EBNF:</b></p><pre>%2</pre>"
                       "<p><b>说明:</b></p>%3"
                       "</body></html>")
                   .arg(title)
                   .arg(ebnf)
                   .arg(desc);
        break;
    case ViewMode::Natural:
        html = QString("<html><body>"
                       "<h2>%1</h2>"
                       "<p><b>📝 自然语言描述:</b></p>%2"
                       "</body></html>")
                   .arg(title)
                   .arg(nat);
        break;
    case ViewMode::Mixed:
    default:
        html = QString("<html><body>"
                       "<h2>%1</h2>"
                       "<p><b>EBNF:</b></p><pre>%2</pre>"
                       "<p><b>说明:</b></p>%3"
                       "<hr>"
                       "<p><b>📝 自然语言描述:</b></p>%4"
                       "</body></html>")
                   .arg(title)
                   .arg(ebnf)
                   .arg(desc)
                   .arg(nat);
        break;
    }
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(p.sampleCode.c_str()));
    outputEdit_->clear();
    statusLabel_->setText(QString::fromUtf8("当前产生式：%1").arg(QString::fromUtf8(p.title.c_str())));
}

/// 运行当前样例代码：Lexer 扫描 → Parser 解析（遇错显示诊断摘要）→
/// Interpreter 执行，标准输出（含异常信息）写入右侧输出区。
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
        interp.setOutputCallback([&out](const std::string& s) { out << s << "\n"; });
        interp.execute(*ast);
        out << "[运行完成]";
    } catch (const std::exception& e) {
        out << "[异常] " << e.what();
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("运行结束"));
}
