#pragma once

#include <QWidget>
#include <QHash>

class QTextBrowser;
class QPushButton;
class QLabel;

// ============================================================
// CodeJourneyInfoPanel — 代码生命旅程静态信息图
// ------------------------------------------------------------
// 用一张 HTML/SVG 风格的静态信息图展示编译管线（源码→Token→AST→
// IR→字节码→输出），配合底部 6 个"逐步查看"按钮可跳转到对应面板。
// 这不是动画，而是一张可阅读、可交互的信息图。
//
// 完成判断标准：用户点进本面板观看即算完成（showEvent 首次触发
// 时发射 journeyCompleted 信号），不再要求访问全部 6 个阶段。
// ============================================================

class CodeJourneyInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit CodeJourneyInfoPanel(QWidget* parent = nullptr);

signals:
    /// 请求跳转到指定面板（"editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"）
    void jumpToPanelRequested(const QString& panelId);

    /// 用户点进本面板观看即发射（完成判断标准）
    void journeyCompleted();

private slots:
    void onJumpToEditor();
    void onJumpToTokens();
    void onJumpToAst();
    void onJumpToIr();
    void onJumpToBytecode();
    void onJumpToOutput();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QTextBrowser* infoBrowser_ = nullptr;
    QLabel* progressLabel_     = nullptr;

    // 完成状态跟踪：首次显示即标记完成
    bool journeyCompleted_ = false;
    void markCompleted();
    void refreshProgress();
    QString buildJourneyHtml() const;
};
