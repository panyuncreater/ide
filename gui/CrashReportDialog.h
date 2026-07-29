/**
 * @file gui/CrashReportDialog.h
 * @brief 崩溃报告对话框（R128）。
 *
 * 当 CrashHandler 检测到上次会话发生崩溃时，由 main.cpp 启动时弹出此对话框。
 * 提供：
 *   - 显示崩溃信号、时间戳、dump 文件路径
 *   - 显示文本栈回溯（POSIX）或提示用调试器打开 minidump（Windows）
 *   - "打开所在目录" 按钮（QDesktopServices::openUrl）
 *   - "复制信息" 按钮（QGuiApplication::clipboard()->setText）
 *   - "删除报告" 按钮（调用 CrashHandler::consumeLastCrashReport）
 *   - "忽略" 按钮（保留报告，下次启动仍会提示）
 *
 * 对话框为模态，确保用户在开始新会话前注意到上次崩溃。
 *
 * @see common/CrashHandler.h
 */
#pragma once

#include <QDialog>

#include "common/CrashHandler.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;

namespace minilang::gui {

class CrashReportDialog : public QDialog {
    Q_OBJECT

public:
    explicit CrashReportDialog(const CrashReport& report, QWidget* parent = nullptr);

    /// 用户是否选择删除报告
    bool reportDeleted() const { return reportDeleted_; }

    /// 用户是否选择不再提示（忽略当前报告，下次启动跳过）
    bool ignoreRequested() const { return ignoreRequested_; }

private slots:
    void onOpenDirectory();
    void onCopyInfo();
    void onDeleteReport();
    void onIgnore();

private:
    void buildUi(const CrashReport& report);

    CrashReport report_;
    QLabel* titleLabel_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QPlainTextEdit* traceView_ = nullptr;
    QPushButton* openDirButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* ignoreButton_ = nullptr;
    QCheckBox* dontShowAgain_ = nullptr;

    bool reportDeleted_ = false;
    bool ignoreRequested_ = false;

    QString formatReportForClipboard() const;
};

} // namespace minilang::gui
