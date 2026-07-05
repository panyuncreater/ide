#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <vector>
#include <string>

// ============================================================
// BugHuntPanel — "编译器 Bug 狩猎"模式（第一波 P1-3）
// ------------------------------------------------------------
// 题库基于项目自身沉淀的真实 Bug（project_memory.md / CHANGELOG）。
// 学习者：选题目 → 阅读背景 → 在内嵌编辑器中预测行为或编写修复 →
// 点击"运行验证"看实际输出 → 查看提示/答案 → "学习模式"在主编辑器
// 加载源码便于进一步调试。
// ============================================================

class IdeController;

struct BugHuntItem {
    std::string id;            // 如 "BUG-CP-1"
    std::string title;         // 题目标题
    std::string severity;      // "P0" / "P1" / "P2"
    std::string category;      // 如 "常量池去重"、"IR 路径栈泄漏"
    std::string background;    // 背景/不变量说明
    std::string sourceCode;    // 触发该 Bug 的 MiniLang 代码
    std::string expectedBehavior;   // 期望行为（修复后）
    std::string buggyBehavior;      // 触发 Bug 时的行为
    std::vector<std::string> hints; // 递进提示
    std::string explanation;   // 根因分析 + 修复要点
};

/// 题库：内置 10+ 条来自项目历史的 Bug 训练题
class BugHuntLibrary {
public:
    static const std::vector<BugHuntItem>& items();
};

class BugHuntPanel : public QWidget {
    Q_OBJECT
public:
    explicit BugHuntPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将源码加载到主编辑器（便于进一步调试）
    void loadSampleRequested(const QString& code);

private slots:
    void onItemSelected(int row);
    void onRunVerify();
    void onShowHint();
    void onShowAnswer();

private:
    IdeController* controller_ = nullptr;
    QListWidget* itemList_      = nullptr;
    QTextBrowser* descBrowser_ = nullptr;
    QTextEdit* codeEditor_      = nullptr;
    QTextEdit* outputEdit_      = nullptr;
    QLabel* statusLabel_        = nullptr;
    QPushButton* runBtn_        = nullptr;
    QPushButton* hintBtn_       = nullptr;
    QPushButton* answerBtn_     = nullptr;
    QPushButton* loadBtn_       = nullptr;

    int currentItemIndex_ = -1;
    int hintLevel_ = 0;        // 0=未提示, 1..N=已显示前 N 条提示

    void populateItemList();
    void showCurrentItem();
};
