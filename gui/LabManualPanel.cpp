#include "gui/LabManualPanel.h"
#include "app/IdeController.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>

LabManualPanel::LabManualPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    loadBtn_ = new QPushButton(QString::fromUtf8("加载章节示例到主编辑器"), this);
    statusLabel_ = new QLabel(QString::fromUtf8("请选择章节"), this);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    mainLayout->addLayout(btnBar);

    // 主体：左右分栏
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    chapterList_ = new QListWidget(this);
    contentBrowser_ = new QTextBrowser(this);
    contentBrowser_->setOpenExternalLinks(false);

    splitter->addWidget(chapterList_);
    splitter->addWidget(contentBrowser_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({180, 600});
    mainLayout->addWidget(splitter, 1);

    populateChapterList();

    connect(chapterList_, &QListWidget::currentRowChanged,
            this, &LabManualPanel::onChapterSelected);
    connect(loadBtn_, &QPushButton::clicked,
            this, &LabManualPanel::onLoadSampleToEditor);
}

void LabManualPanel::populateChapterList() {
    chapterList_->clear();
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        chapterList_->addItem(QString::fromUtf8(ch.title.c_str()));
    }
    if (!chs.empty()) {
        chapterList_->setCurrentRow(0);
    }
}

void LabManualPanel::onChapterSelected(int row) {
    currentChapterIndex_ = row;
    showCurrentChapter();
}

void LabManualPanel::showCurrentChapter() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        contentBrowser_->clear();
        statusLabel_->setText(QString::fromUtf8("无选中章节"));
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    // QTextBrowser 支持 HTML 子集，将 Markdown 简单转 HTML
    // 注：完整 Markdown 渲染需要第三方库，这里做最小转换
    QString html = QString::fromUtf8(ch.markdown.c_str());
    html.replace("\n## ", "\n<h2>");
    html.replace("\n# ", "\n<h1>");
    // 简单段落换行
    html.replace("\n\n", "</p><p>");
    html.replace("\n", "<br>");
    html = "<html><body><p>" + html + "</p></body></html>";
    contentBrowser_->setHtml(html);
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
