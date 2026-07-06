#include "gui/LabManualPanel.h"
#include "app/IdeController.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QShowEvent>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（CaptionLabel）
#include "gui/GuiTextUtils.h"      // monospaceFont()
#include "gui/MarkdownRenderer.h"  // 统一 Markdown 渲染

LabManualPanel::LabManualPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    loadBtn_ = new PrimaryPushButton(QString::fromUtf8("加载章节示例到主编辑器"), this);
    statusLabel_ = new CaptionLabel(QString::fromUtf8("请选择章节"), this);

    // 字号调节 SpinBox（8-24pt，默认 11pt）
    auto* fontLabel = new QLabel(QString::fromUtf8("字号"), this);
    fontSizeSpin_ = new QSpinBox(this);
    fontSizeSpin_->setRange(8, 24);
    fontSizeSpin_->setValue(11);
    fontSizeSpin_->setSuffix(QString::fromUtf8("pt"));
    fontSizeSpin_->setFixedWidth(70);
    fontSizeSpin_->setToolTip(QString::fromUtf8("调节实验手册正文字号"));
    connect(fontSizeSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int size) {
        if (contentBrowser_) {
            QFont f = contentBrowser_->font();
            f.setPointSize(size);
            contentBrowser_->setFont(f);
        }
    });

    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    btnBar->addSpacing(8);
    btnBar->addWidget(fontLabel);
    btnBar->addWidget(fontSizeSpin_);
    mainLayout->addLayout(btnBar);

    // 主体：头歌风格三栏布局（左：步骤导航 | 中：教学内容 | 右：样例代码）
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    chapterList_ = new QListWidget(this);
    contentBrowser_ = new QTextBrowser(this);
    contentBrowser_->setOpenExternalLinks(false);

    // 右侧：样例代码编辑区（头歌风格 - 右侧编译器）
    auto* codeContainer = new QWidget(this);
    auto* codeLayout = new QVBoxLayout(codeContainer);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(2);
    auto* codeHeader = new QLabel(QString::fromUtf8("📝 样例代码"), codeContainer);
    codeHeader->setStyleSheet("font-weight:bold; padding:2px;");
    codeLayout->addWidget(codeHeader);
    sampleCodeEdit_ = new QPlainTextEdit(codeContainer);
    sampleCodeEdit_->setReadOnly(true);  // 预览模式，编辑请加载到主编辑器
    sampleCodeEdit_->setFont(GuiTextUtils::monospaceFont(10));
    sampleCodeEdit_->setPlaceholderText(QString::fromUtf8("选择章节后显示样例代码"));
    codeLayout->addWidget(sampleCodeEdit_, 1);

    splitter->addWidget(chapterList_);
    splitter->addWidget(contentBrowser_);
    splitter->addWidget(codeContainer);
    splitter->setStretchFactor(0, 1);   // 章节列表窄
    splitter->setStretchFactor(1, 2);   // 教学内容中等
    splitter->setStretchFactor(2, 2);   // 样例代码与内容并排
    splitter->setSizes({150, 400, 400});
    mainLayout->addWidget(splitter, 1);

    populateChapterList();

    connect(chapterList_, &QListWidget::currentRowChanged,
            this, &LabManualPanel::onChapterSelected);
    connect(loadBtn_, &QPushButton::clicked,
            this, &LabManualPanel::onLoadSampleToEditor);

    // PERF: 不在构造时 setCurrentRow(0)，推迟到首次 showEvent。
    // 避免为隐藏 dock 渲染第一章 Markdown（省 6 次正则编译 + HTML 转换）。
}

void LabManualPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!firstShowDone_) {
        firstShowDone_ = true;
        // 首次可见时选中第一章并渲染
        if (chapterList_->count() > 0 && chapterList_->currentRow() < 0) {
            chapterList_->setCurrentRow(0);
        }
    }
}

void LabManualPanel::populateChapterList() {
    chapterList_->clear();
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        chapterList_->addItem(QString::fromUtf8(ch.title.c_str()));
    }
    // setCurrentRow(0) 移至 showEvent，避免构造时触发 Markdown 渲染
}

void LabManualPanel::onChapterSelected(int row) {
    currentChapterIndex_ = row;
    showCurrentChapter();
}

void LabManualPanel::showCurrentChapter() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        contentBrowser_->clear();
        if (sampleCodeEdit_) sampleCodeEdit_->clear();
        statusLabel_->setText(QString::fromUtf8("无选中章节"));
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    // 使用统一 Markdown 渲染器：支持标题/粗体/列表/代码块/分割线等
    // 替代早期 5 行 html.replace 的最小转换（无法处理 **bold**、列表、代码块）
    QString html = MarkdownRenderer::markdownToHtml(ch.markdown);
    contentBrowser_->setHtml(html);

    // 右侧样例代码区显示当前章节的 sampleCode（头歌风格 - 右侧编译器）
    if (sampleCodeEdit_) {
        sampleCodeEdit_->setPlainText(QString::fromUtf8(ch.sampleCode.c_str()));
    }

    statusLabel_->setText(QString::fromUtf8("当前章节：%1").arg(
        QString::fromUtf8(ch.title.c_str())));
}

void LabManualPanel::onLoadSampleToEditor() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        statusLabel_->setText(QString::fromUtf8("请先选择章节"));
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    if (!controller_) {
        statusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }
    // 通过 outputReady 信号让 Ide 主窗口加载到编辑器
    // 这里简化：直接 emit 信号由 Ide 接管
    // 实际加载由 Ide 监听并处理
    QString code = QString::fromUtf8(ch.sampleCode.c_str());
    emit loadSampleRequested(code);
    statusLabel_->setText(QString::fromUtf8("已请求加载示例（请切到主编辑器查看）"));
}
