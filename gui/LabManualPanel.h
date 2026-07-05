#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QPushButton>
#include <vector>
#include <string>

// ============================================================
// LabManualPanel — 内置实验手册集成（第三波 P2-2）
// ------------------------------------------------------------
// 左侧：实验章节列表
// 右侧：QTextBrowser 渲染 Markdown 内容（含目标、样例代码、验证断言、
// 参考答案链接）
// 顶部：按钮组（"加载章节示例到主编辑器"）
//
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

private slots:
    void onChapterSelected(int row);
    void onLoadSampleToEditor();

private:
    IdeController* controller_ = nullptr;
    QListWidget* chapterList_  = nullptr;
    QTextBrowser* contentBrowser_ = nullptr;
    QLabel* statusLabel_       = nullptr;
    QPushButton* loadBtn_      = nullptr;

    int currentChapterIndex_ = -1;

    void populateChapterList();
    void showCurrentChapter();
};
