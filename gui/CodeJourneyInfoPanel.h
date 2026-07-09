#pragma once

#include <QHash>
#include <QWidget>

class QTextBrowser;
class QLabel;

// ============================================================
// CodeJourneyInfoPanel — 代码生命旅程静态信息图
// ------------------------------------------------------------
// 用一张 HTML/SVG 风格的静态信息图展示编译管线（源码→Token→AST→
// IR→字节码→输出）。
//
// 完成判断标准：用户点进本面板观看即算完成（showEvent 首次触发
// 时发射 journeyCompleted 信号）。
// ============================================================

class CodeJourneyInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit CodeJourneyInfoPanel(QWidget* parent = nullptr);

signals:
    /// 用户点进本面板观看即发射（完成判断标准）
    void journeyCompleted();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QTextBrowser* infoBrowser_ = nullptr;
    QLabel* progressLabel_ = nullptr;

    // 完成状态跟踪：首次显示即标记完成
    bool journeyCompleted_ = false;
    void markCompleted();
    void refreshProgress();
    QString buildJourneyHtml() const;
};
