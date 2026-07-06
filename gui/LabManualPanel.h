#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTextBrowser>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <vector>
#include <string>

// ============================================================
// LabManualPanel — 内置实验手册集成（头歌风格三栏布局）
// ------------------------------------------------------------
// 左侧：实验章节列表（步骤导航，窄列）
// 中间：QTextBrowser 渲染 Markdown 教学内容（目标/概念/步骤/验证）
// 右侧：QPlainTextEdit 显示当前章节样例代码 + "加载到主编辑器"按钮
// 顶部：按钮组（"加载章节示例到主编辑器"）+ 字号调节 SpinBox
//
// 设计灵感：头歌实验平台 - 左侧步骤导航，右侧代码编辑器并排
// 内容策划：从项目特性出发，每个实验对应一个核心教学主题。
// ============================================================

class IdeController;

struct LabChapter {
    std::string id;            // 如 "lab-01"
    std::string title;         // 章节标题
    std::string markdown;     // Markdown 内容
    std::string sampleCode;    // 可一键加载到主编辑器的样例代码
};

class LabManualContent {
public:
    static const std::vector<LabChapter>& chapters();
};

class LabManualPanel : public QWidget {
    Q_OBJECT
public:
    explicit LabManualPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器（由 Ide 监听处理）
    void loadSampleRequested(const QString& code);

protected:
    /// 首次显示时选中第一章并渲染（推迟 Markdown 渲染到面板可见时，
    /// 避免 IDE 启动时为隐藏 dock 渲染第一章）
    void showEvent(QShowEvent* event) override;

private slots:
    void onChapterSelected(int row);
    void onLoadSampleToEditor();

private:
    IdeController* controller_ = nullptr;
    QListWidget* chapterList_  = nullptr;
    QTextBrowser* contentBrowser_ = nullptr;
    QPlainTextEdit* sampleCodeEdit_ = nullptr;  ///< 右侧样例代码预览（头歌风格）
    QLabel* statusLabel_       = nullptr;
    QPushButton* loadBtn_      = nullptr;
    QSpinBox* fontSizeSpin_    = nullptr;  ///< 实验手册正文字号调节

    int currentChapterIndex_ = -1;
    bool firstShowDone_ = false;  ///< 是否已完成首次显示渲染

    void populateChapterList();
    void showCurrentChapter();
};
