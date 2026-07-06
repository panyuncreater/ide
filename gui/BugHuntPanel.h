#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
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

// 难度分级（功能 9：Bug 狩猎分级题库重构）
//   BEGINNER      — 入门级（阅读理解型，培养 Debug 直觉，无需定位代码）
//   INTERMEDIATE  — 进阶级（需阅读代码、理解逻辑、定位问题区域）
//   EXPERT        — 专家级（需深入理解引擎架构、跨模块追踪）
enum class BugHuntDifficulty {
    BEGINNER,
    INTERMEDIATE,
    EXPERT
};

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
    BugHuntDifficulty difficulty = BugHuntDifficulty::EXPERT;  // 难度分级（默认 EXPERT 以暴露遗漏设置）
};

/// 题库：内置 10+ 条来自项目历史的 Bug 训练题
class BugHuntLibrary {
public:
    static const std::vector<BugHuntItem>& items();
};

// ============================================================
// BugHuntVariant — 变体挑战题
// ------------------------------------------------------------
// 基于现有 Bug 题自动生成变体（修改参数/结构/反向挑战），
// 让学习者在新场景下应用所学。变体无标准答案，鼓励探索。
// ============================================================

struct BugHuntVariant {
    std::string id;                  // 变体 ID（如 "variant-cp-1-param"）
    std::string parentId;            // 父题 ID（如 "BUG-CP-1"）
    std::string title;               // 变体标题
    std::string description;         // 变体说明（与父题的差异 + 新挑战点）
    std::string sourceCode;          // 变体 MiniLang 源码
    std::string challengeGoal;       // 挑战目标（如 "预测输出顺序" / "找出仍存在的 Bug" / "改造为不依赖该优化"）
    std::string expectedBehavior;    // 期望行为
    std::string hint;                // 单条提示
};

/// 变体库：基于 BugHuntLibrary 父题生成的变体挑战题
class BugHuntVariantLibrary {
public:
    static const std::vector<BugHuntVariant>& variants();
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
    void onTripleVerify();
    void onToggleVariantMode();
    void onVariantSelected(int row);
    void onDifficultyChanged(int id);

private:
    IdeController* controller_ = nullptr;
    QListWidget* itemList_      = nullptr;
    QLineEdit*   searchEdit_   = nullptr;  // M11: 题目搜索框
    QTextBrowser* descBrowser_ = nullptr;
    QTextEdit* codeEditor_      = nullptr;
    QTextEdit* outputEdit_      = nullptr;
    QLabel* statusLabel_        = nullptr;
    QPushButton* runBtn_        = nullptr;
    QPushButton* hintBtn_       = nullptr;
    QPushButton* answerBtn_     = nullptr;
    QPushButton* loadBtn_       = nullptr;
    QPushButton* tripleVerifyBtn_ = nullptr;  // 三后端对比验证按钮
    QPushButton* variantBtn_    = nullptr;     // 切换到变体模式按钮
    QListWidget* variantList_   = nullptr;     // 变体题目列表（变体模式下显示）
    QLabel* variantStatusLabel_ = nullptr;    // 变体状态标签

    // 难度筛选 UI（功能 9）：4 个互斥按钮 — 全部 / 入门 / 进阶 / 专家
    QPushButton* diffAllBtn_         = nullptr;
    QPushButton* diffBeginnerBtn_    = nullptr;
    QPushButton* diffIntermediateBtn_= nullptr;
    QPushButton* diffExpertBtn_      = nullptr;
    // 当前难度筛选（-1 = 全部，0/1/2 = BEGINNER/INTERMEDIATE/EXPERT）
    int currentDifficultyFilter_ = 0;  // 默认显示"入门级"

    int currentItemIndex_ = -1;
    int hintLevel_ = 0;        // 0=未提示, 1..N=已显示前 N 条提示
    bool variantMode_ = false;           // 是否变体模式
    int currentVariantIndex_ = -1;        // 当前选中的变体索引

    void populateItemList();
    void showCurrentItem();
    void showCurrentVariant();
};
