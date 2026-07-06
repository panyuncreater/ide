#pragma once

#include <QWidget>

class QTextBrowser;
class QPushButton;

// ============================================================
// CodeJourneyInfoPanel — 代码生命旅程静态信息图（功能 2 降级方案）
// ------------------------------------------------------------
// 原方案是 30 秒动画，但维护成本高（与语言特性紧耦合）。
// 降级为静态信息图：用一张 HTML/SVG 风格的图展示编译管线，
// 配合"逐步查看"按钮可跳转到对应面板。
// 成本低且不易过时。
// ============================================================

class CodeJourneyInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit CodeJourneyInfoPanel(QWidget* parent = nullptr);

signals:
    /// 请求跳转到指定面板（"editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"）
    void jumpToPanelRequested(const QString& panelId);

private slots:
    void onJumpToEditor();
    void onJumpToTokens();
    void onJumpToAst();
    void onJumpToIr();
    void onJumpToBytecode();
    void onJumpToOutput();

private:
    QTextBrowser* infoBrowser_ = nullptr;
    QString buildJourneyHtml() const;
};
