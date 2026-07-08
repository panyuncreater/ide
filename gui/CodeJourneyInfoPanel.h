#pragma once

#include <QWidget>
#include <QHash>

class QTextBrowser;
class QPushButton;
class QLabel;

// ============================================================
// CodeJourneyInfoPanel — 代码生命旅程静态信息图（功能 2 降级方案）
// ------------------------------------------------------------
// 原方案是 30 秒动画，但维护成本高（与语言特性紧耦合）。
// 降级为静态信息图：用一张 HTML/SVG 风格的图展示编译管线，
// 配合"逐步查看"按钮可跳转到对应面板。
// 成本低且不易过时。
//
// 问题 11: 完成判断标准 — 用户需依次点击 6 个阶段按钮（①~⑥），
// 全部访问后显示"完成"提示。已访问的按钮变为绿色勾选态。
// ============================================================

class CodeJourneyInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit CodeJourneyInfoPanel(QWidget* parent = nullptr);

signals:
    /// 请求跳转到指定面板（"editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"）
    void jumpToPanelRequested(const QString& panelId);

    /// 问题 11: 所有 6 个阶段均已访问时发射（完成判断标准）
    void journeyCompleted();

private slots:
    void onJumpToEditor();
    void onJumpToTokens();
    void onJumpToAst();
    void onJumpToIr();
    void onJumpToBytecode();
    void onJumpToOutput();

private:
    QTextBrowser* infoBrowser_ = nullptr;
    QLabel* progressLabel_     = nullptr;  // 问题 11: 进度提示

    // 问题 11: 6 阶段访问状态跟踪
    QHash<QString, bool> visitedStages_;
    static constexpr int kTotalStages = 6;
    void markStageVisited(const QString& stageId);
    void refreshProgress();
    QString buildJourneyHtml() const;
};
