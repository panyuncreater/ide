#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QSplitter>
#include <QPushButton>
#include <QHBoxLayout>

// ============================================================
// OutputPanel 输出与错误面板
// ============================================================

/// 输出与错误面板：上下分割显示程序输出和错误信息
class OutputPanel : public QWidget {
    Q_OBJECT

public:
    explicit OutputPanel(QWidget* parent = nullptr);

    /// 追加输出文本
    void appendOutput(const QString& text);

    /// 追加错误文本
    void appendError(const QString& text);

    /// 清空所有输出
    void clearAll();

    /// 清空输出
    void clearOutput();

    /// 清空错误
    void clearErrors();

private:
    QTextEdit* outputEdit_ = nullptr;      // 输出文本区域
    QTextEdit* errorEdit_ = nullptr;        // 错误文本区域
    QPushButton* clearOutputBtn_ = nullptr; // 清空输出按钮
    QPushButton* clearErrorBtn_ = nullptr;  // 清空错误按钮
};
