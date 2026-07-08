#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

class QTimer;

// ============================================================
// BugHuntPanel — "编译器 Bug 狩猎"模式（第一波 P1-3）
// ------------------------------------------------------------
// 题库基于项目自身沉淀的真实 Bug（project_memory.md / CHANGELOG）。
// 学习者：选题目 → 阅读背景 → 在内嵌编辑器中预测行为或编写修复 →
// 点击"运行验证"看实际输出 → 查看提示/答案 → "学习模式"在主编辑器
// 加载源码便于进一步调试。
// ============================================================

class IdeController;
class GuidedTour;

// 难度分级（功能 9：Bug 狩猎分级题库重构）
//   BEGINNER      — 入门级（阅读理解型，培养 Debug 直觉，无需定位代码）
//   INTERMEDIATE  — 进阶级（需阅读代码、理解逻辑、定位问题区域）
//   EXPERT        — 专家级（需深入理解引擎架构、跨模块追踪）
enum class BugHuntDifficulty { BEGINNER, INTERMEDIATE, EXPERT };

struct BugHuntItem {
    std::string id;                                           // 如 "BUG-CP-1"
    std::string title;                                        // 题目标题
    std::string severity;                                     // "P0" / "P1" / "P2"
    std::string category;                                     // 如 "常量池去重"、"IR 路径栈泄漏"
    std::string background;                                   // 背景/不变量说明
    std::string sourceCode;                                   // 触发该 Bug 的 MiniLang 代码
    std::string expectedBehavior;                             // 期望行为（修复后）
    std::string buggyBehavior;                                // 触发 Bug 时的行为
    std::vector<std::string> hints;                           // 递进提示
    std::string explanation;                                  // 根因分析 + 修复要点
    BugHuntDifficulty difficulty = BugHuntDifficulty::EXPERT; // 难度分级（默认 EXPERT 以暴露遗漏设置）
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
    std::string id;               // 变体 ID（如 "variant-cp-1-param"）
    std::string parentId;         // 父题 ID（如 "BUG-CP-1"）
    std::string title;            // 变体标题
    std::string description;      // 变体说明（与父题的差异 + 新挑战点）
    std::string sourceCode;       // 变体 MiniLang 源码
    std::string challengeGoal;    // 挑战目标（如 "预测输出顺序" / "找出仍存在的 Bug" / "改造为不依赖该优化"）
    std::string expectedBehavior; // 期望行为
    std::string hint;             // 单条提示
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

    /// 创建该面板的新手引导（5 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 请求将源码加载到主编辑器（便于进一步调试）
    void loadSampleRequested(const QString& code);

    /// P0-B fix: 学员"狩猎"成功时发射——三后端对比验证通过（输出一致且无异常）
    /// 即视为修复成功。difficulty 取当前题目的 BugHuntDifficulty int 值：
    ///   0=BEGINNER / 1=INTERMEDIATE / 2=EXPERT
    /// 主窗口连接后调用 markActivityCompleted("bug-hunt-<level>") 回写学习路径。
    /// 此前 BugHuntPanel 仅 emit loadSampleRequested 加载代码到编辑器，从不
    /// emit activityCompleted——Bug-Hunt 在学习路径地图里永远显示"未完成"。
    void challengeSolved(int difficulty);

private slots:
    void onItemSelected(int itemIndex); // issue 5: 芯片点击 → items() 索引
    void onRunVerify();
    void onShowHint();
    void onShowAnswer();
    void onTripleVerify();
    void onToggleVariantMode();
    void onVariantSelected(int variantIndex); // issue 5: 芯片点击 → variants() 索引
    void onDifficultyChanged(int id);
    void onSubmitPrediction();
    void onCodeModified();

private:
    IdeController* controller_ = nullptr;
    // issue 5: 用芯片栏替代大目录列表（参考 TokenPuzzlePanel 关卡芯片）
    QWidget* chipBar_ = nullptr;       // 芯片栏容器（含 bug 芯片 + 搜索框）
    QList<QPushButton*> bugChips_;     // 标准 Bug 芯片按钮列表
    QList<QPushButton*> variantChips_; // 变体芯片按钮列表
    QLineEdit* searchEdit_ = nullptr;  // 题目搜索框（芯片栏右侧）
    // OPT-2 fix: 搜索框防抖——textChanged 每字符触发会 O(n) 重建 haystack，
    // 与 LearningPathPanel 的 250ms 防抖设计不一致。改用 200ms 单次定时器聚合输入。
    QTimer* searchDebounceTimer_ = nullptr; // 搜索防抖定时器
    QString lastSearchText_;                // 最近一次输入文本（定时器触发时使用）
    QTextBrowser* descBrowser_ = nullptr;
    QTextEdit* codeEditor_ = nullptr;
    QTextEdit* outputEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* hintBtn_ = nullptr;
    QPushButton* answerBtn_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* tripleVerifyBtn_ = nullptr; // 三后端对比验证按钮
    QPushButton* variantBtn_ = nullptr;      // 切换到变体模式按钮
    QLabel* variantStatusLabel_ = nullptr;   // 变体状态标签

    // 调试流程指引（问题 4）：4 步检查清单 + 预测输入
    QLabel* debugStepsLabel_ = nullptr;          // 调试步骤检查清单
    QLineEdit* predictionEdit_ = nullptr;        // 用户预测输入框
    QPushButton* submitPredictionBtn_ = nullptr; // 提交预测按钮
    // 调试进度状态
    bool stepPredicted_ = false;    // 用户已提交预测
    bool stepObserved_ = false;     // 用户已运行验证
    bool stepAttemptedFix_ = false; // 用户已修改代码尝试修复
    bool stepVerified_ = false;     // 用户已三后端对比验证
    QString originalCode_;          // 原始题目代码（用于检测用户是否修改）
    void refreshDebugSteps();       // 刷新检查清单显示

    // 可折叠目录侧栏（已移除：芯片栏替代大目录）
    QSplitter* mainSplitter_ = nullptr; // 主体 splitter 引用（2 栏：描述 | 代码+输出）

    // 难度筛选 UI（功能 9）：4 个互斥按钮 — 全部 / 入门 / 进阶 / 专家
    QPushButton* diffAllBtn_ = nullptr;
    QPushButton* diffBeginnerBtn_ = nullptr;
    QPushButton* diffIntermediateBtn_ = nullptr;
    QPushButton* diffExpertBtn_ = nullptr;
    // 当前难度筛选（-1 = 全部，0/1/2 = BEGINNER/INTERMEDIATE/EXPERT）
    int currentDifficultyFilter_ = 0; // 默认显示"入门级"

    int currentItemIndex_ = -1;
    int hintLevel_ = 0;            // 0=未提示, 1..N=已显示前 N 条提示
    bool variantMode_ = false;     // 是否变体模式
    int currentVariantIndex_ = -1; // 当前选中的变体索引
    // AUDIT-P1 fix: 跟踪已解决题目索引，仅当当前难度所有题目全部解决时
    // 才发射 challengeSolved。原实现单题通过即发射，与游戏面板"全部子关卡完成"
    // 语义不一致（TokenPuzzle 要求 5 关全通、AstToy 要求 6 关全通）。
    QSet<int> solvedItemIndices_;
    // AUDIT-P2 fix: 防重复守卫——同步执行链期间快速重复点击会触发多次
    // Lexer→Parser→VM 执行，导致输出闪烁、重复 save() 磁盘 I/O、重复信号发射。
    bool verifying_ = false;
    bool tripleVerifying_ = false;

    void populateBugChips();     // issue 5: 构建 Bug 芯片栏
    void refreshBugChips();      // issue 5: 刷新芯片状态（当前/隐藏）
    void populateVariantChips(); // issue 5: 构建变体芯片栏
    void refreshVariantChips();  // issue 5: 刷新变体芯片状态
    // OPT-2 fix: 提取搜索过滤逻辑为独立方法，供防抖定时器调用。
    // 按 title/id/category 小写包含匹配，控制 bugChips_ 可见性。
    void applySearchFilter(const QString& filter);
    void showCurrentItem();
    void showCurrentVariant();
};
