// ============================================================
// gui/CrashReportDialog.cpp — 崩溃报告对话框实现（R128）
// ============================================================

#include "gui/CrashReportDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/I18n.h"

namespace minilang::gui {

CrashReportDialog::CrashReportDialog(const CrashReport& report, QWidget* parent)
    : QDialog(parent), report_(report) {
    setWindowTitle(mlTr("MiniLang IDE - 上次会话崩溃"));
    setMinimumSize(560, 420);
    buildUi(report);
}

void CrashReportDialog::buildUi(const CrashReport& report) {
    auto* layout = new QVBoxLayout(this);

    // 标题
    titleLabel_ = new QLabel(mlTr("检测到上次会话异常退出"), this);
    QFont titleFont = titleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel_->setFont(titleFont);
    layout->addWidget(titleLabel_);

    // 信息标签
    QString infoText;
    if (report.valid) {
        infoText = mlTr("信号/异常：") + QString::fromStdString(report.signalName) + "\n"
                 + mlTr("时间：") + QString::fromStdString(report.timestamp) + "\n"
                 + mlTr("崩溃文件：") + QString::fromStdString(report.dumpFilePath);
    } else {
        infoText = mlTr("（无有效崩溃报告）");
    }
    infoLabel_ = new QLabel(infoText, this);
    infoLabel_->setWordWrap(true);
    infoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(infoLabel_);

    // 栈回溯
    layout->addWidget(new QLabel(mlTr("栈回溯："), this));
    traceView_ = new QPlainTextEdit(this);
    traceView_->setReadOnly(true);
    if (!report.stackTrace.empty()) {
        traceView_->setPlainText(QString::fromStdString(report.stackTrace));
    } else {
#ifdef _WIN32
        traceView_->setPlainText(mlTr(
            "（Windows minidump 为二进制格式，请用 Visual Studio 或 WinDbg 打开 .dmp 文件查看调用栈）"));
#else
        traceView_->setPlainText(mlTr("（无文本栈回溯）"));
#endif
    }
    layout->addWidget(traceView_, 1);

    // 复选框：不再提示
    dontShowAgain_ = new QCheckBox(mlTr("忽略此报告，下次启动不再提示"), this);
    layout->addWidget(dontShowAgain_);

    // 按钮行
    auto* btnLayout = new QHBoxLayout();
    openDirButton_ = new QPushButton(mlTr("打开所在目录"), this);
    copyButton_    = new QPushButton(mlTr("复制信息"), this);
    deleteButton_  = new QPushButton(mlTr("删除报告"), this);
    ignoreButton_  = new QPushButton(mlTr("关闭"), this);
    ignoreButton_->setDefault(true);

    btnLayout->addWidget(openDirButton_);
    btnLayout->addWidget(copyButton_);
    btnLayout->addStretch();
    btnLayout->addWidget(deleteButton_);
    btnLayout->addWidget(ignoreButton_);
    layout->addLayout(btnLayout);

    connect(openDirButton_, &QPushButton::clicked, this, &CrashReportDialog::onOpenDirectory);
    connect(copyButton_,    &QPushButton::clicked, this, &CrashReportDialog::onCopyInfo);
    connect(deleteButton_,  &QPushButton::clicked, this, &CrashReportDialog::onDeleteReport);
    connect(ignoreButton_,  &QPushButton::clicked, this, &CrashReportDialog::onIgnore);
}

void CrashReportDialog::onOpenDirectory() {
    if (report_.dumpFilePath.empty()) return;
    QFileInfo fi(QString::fromStdString(report_.dumpFilePath));
    QString dir = fi.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

QString CrashReportDialog::formatReportForClipboard() const {
    QString text;
    text += "=== MiniLang IDE Crash Report ===\n";
    text += "Signal: " + QString::fromStdString(report_.signalName) + "\n";
    text += "Timestamp: " + QString::fromStdString(report_.timestamp) + "\n";
    text += "DumpFile: " + QString::fromStdString(report_.dumpFilePath) + "\n";
    text += "Version: " + QCoreApplication::applicationVersion() + "\n";
    text += "\n=== Stack Trace ===\n";
    text += QString::fromStdString(report_.stackTrace);
    return text;
}

void CrashReportDialog::onCopyInfo() {
    QGuiApplication::clipboard()->setText(formatReportForClipboard());
    QMessageBox::information(this, mlTr("已复制"),
                             mlTr("崩溃信息已复制到剪贴板，可粘贴到 Issue 报告中。"));
}

void CrashReportDialog::onDeleteReport() {
    auto ret = QMessageBox::question(this, mlTr("确认删除"),
                                     mlTr("确定要删除此崩溃报告吗？此操作不可撤销。"),
                                     QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    // 删除磁盘上的报告文件
    if (!report_.dumpFilePath.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(report_.dumpFilePath, ec);
    }
    reportDeleted_ = true;
    accept();
}

void CrashReportDialog::onIgnore() {
    ignoreRequested_ = dontShowAgain_->isChecked();
    accept();
}

} // namespace minilang::gui
