#pragma once

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

// ============================================================
// SyntaxExplorerPanel — 交互式语法探索器（第三波 P2-1，降级方案）
// ------------------------------------------------------------
// 左侧：产生式参考列表（不可修改文法，仅展示）
// 右侧上：当前产生式说明 + 样例代码（可编辑）
// 右侧下：运行结果 / AST 摘要
//
// 降级原因：项目 Parser 是手写递归下降，不支持"修改文法即时看效果"。
// 改为"修改样例代码即时看解析路径"——仍能传递"语法 → AST"的对应关系。
// ============================================================

class IdeController;

struct SyntaxProduction {
    std::string name;            // 产生式 ID（如 "if-stmt"）
    std::string title;           // 显示名（如 "if 语句"）
    std::string ebnf;            // EBNF 形式（如 'ifStmt := "if" "(" expr ")" block ("else" block)?'）
    std::string description;     // 文字说明
    std::string sampleCode;      // 可加载到样例代码编辑器的代码
    std::string naturalLanguage; // F8: 自然语言描述（人话翻译，含示例和注意事项）
};

class SyntaxProductionLibrary {
public:
    static const std::vector<SyntaxProduction>& items();
};

class SyntaxExplorerPanel : public QWidget {
    Q_OBJECT
public:
    explicit SyntaxExplorerPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

    /// F8: 视图模式枚举
    enum class ViewMode {
        EBNF,    ///< 仅 EBNF 视图
        Natural, ///< 仅自然语言视图
        Mixed    ///< 混合视图（默认）
    };

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onItemSelected(int row);
    void onRunSample();
    void onViewModeChanged(int mode); // F8: 视图切换

private:
    IdeController* controller_ = nullptr;
    QListWidget* itemList_ = nullptr;
    QTextBrowser* descBrowser_ = nullptr;
    QTextEdit* codeEditor_ = nullptr;
    QTextEdit* outputEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* loadBtn_ = nullptr;

    // F8: 视图模式切换按钮
    QPushButton* ebnfBtn_ = nullptr;
    QPushButton* naturalBtn_ = nullptr;
    QPushButton* mixedBtn_ = nullptr;
    ViewMode viewMode_ = ViewMode::Mixed; // 默认混合视图

    int currentItemIndex_ = -1;

    void populateItemList();
    void showCurrentItem();
    void updateViewModeButtons(); // F8: 同步按钮选中态
};
