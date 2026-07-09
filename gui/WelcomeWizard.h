// ============================================================
// WelcomeWizard.h — 首次启动欢迎向导（功能 1）
// ------------------------------------------------------------
// 教学目标：让从没听过"编译原理"的人在 3 分钟内感受到
// "原来我写的代码是这样变成程序的"。
//
// 实现：QDialog + QStackedWidget 3 步导览
//   Step 1：欢迎页 + 角色选择（3 个 QRadioButton）
//   Step 2：Token 概念（代码 + Token 表，点击 token 高亮源码）
//   Step 3：字节码与运行（AST 树 + 字节码 + 模拟运行输出）
//
// 持久化：QSettings 的 welcome_completed 标记，首次启动显示，
//         完成/跳过后置 true，下次启动不再弹出。
//
// 本向导为纯前端模拟（不依赖 IdeController / 引擎层），
// "运行"按钮通过 QTimer 延迟显示 "Hello!" 模拟执行。
// 所有用户可见文本用 mlTr() 包裹（i18n）。
// ============================================================

#pragma once

#include "gui/I18n.h" // mlTr() 国际化（头文件内联使用）
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>

// QFluentKit 前向声明（PushButton / PrimaryPushButton 为全局类，不在 Fluent 命名空间）
class PushButton;
class PrimaryPushButton;

class WelcomeWizard : public QDialog {
    Q_OBJECT

public:
    explicit WelcomeWizard(QWidget* parent = nullptr);

    /// 用户是否完成了导览（用于 QSettings 标记）
    /// true = 走完 4 步或主动跳过；选"高级用户"直接关闭也视为完成。
    bool completed() const { return completed_; }

signals:
    /// 完成导览后请求显示学习路径面板（Step 4「开始学习」按钮触发）
    void learningPathRequested();

private slots:
    void onStartExplore(); // Step 1 [▶ 开始探索]
    void onSkip();         // Step 1 [跳过]
    void onNextStep();     // 通用 [下一步 →]
    void onPrevStep();     // 通用 [← 上一步]

    /// Step 2：点击 Token 表行 → 高亮编辑器中对应字符
    void onTokenRowClicked(int row);

    /// Step 3：点击 [▶ 运行] → 模拟执行，延迟输出 "Hello!"
    void onRunClicked();

private:
    // ---- 导航容器 ----
    QStackedWidget* pages_ = nullptr;
    QLabel* stepIndicator_ = nullptr; // 顶部 "1 / 4" 进度指示

    // ---- Step 1：角色选择 ----
    QRadioButton* roleBeginner_ = nullptr;     // 我完全新手
    QRadioButton* roleIntermediate_ = nullptr; // 我懂一点编程
    QRadioButton* roleExpert_ = nullptr;       // 我学过编译原理 → 跳过

    // ---- Step 2：Token 概念 ----
    QTextEdit* codeEdit_ = nullptr;      // 只读，预填 print("Hello!");
    QTableWidget* tokenTable_ = nullptr; // 3 行 token

    // ---- Step 3：AST + 字节码 + 运行 ----
    QTreeWidget* astTree_ = nullptr;      // Print → StringLiteral("Hello!")
    QListWidget* bytecodeList_ = nullptr; // OP_STRING / OP_PRINT
    QTextEdit* runOutput_ = nullptr;      // 运行输出区
    QPushButton* runBtn_ = nullptr;       // [▶ 运行]（保留 QPushButton，用 TeachingTheme::success() 着色）
    QLabel* runHint_ = nullptr;           // "②③④ 代码 → AST → ..."

    // ---- Step 4：学习路径推荐 ----
    PrimaryPushButton* startLearningBtn_ = nullptr; // [开始学习 ✓]（QFluentKit 主按钮）

    // ---- 导航按钮 ----
    PushButton* prevBtn_ = nullptr; // QFluentKit 次按钮
    PushButton* nextBtn_ = nullptr; // QFluentKit 次按钮

    // ---- 状态 ----
    bool completed_ = false;
    bool runExecuted_ = false; // Step 3 是否已点过"运行"（避免重复输出）

    // ---- 内部方法 ----
    void buildStep1();
    void buildStep2();
    void buildStep3();
    void buildStep4();
    void goToStep(int index);
    void updateNavButtons();
    void updateStepIndicator();

protected:
    /// R54-10 fix: Esc 键调用 onSkip()（而非基类 reject()），确保 completed_=true
    /// 被设置，下次启动不再弹出向导。原实现未重写 keyPressEvent，Esc 走 QDialog
    /// 默认 reject() 不经过 onSkip()，导致按 Esc 关闭后 completed_ 仍为 false。
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// 用 setExtraSelections 高亮 codeEdit_ 中 [start, end) 字符区间
    void highlightCodeRange(int start, int end);

    /// Token 在源码中的字符区间（与 print("Hello!"); 对应）
    struct TokenSpan {
        int start;
        int end;
    };
    QVector<TokenSpan> tokenSpans_;
};

/// QSettings 键名（首次启动标记）
/// 使用 inline 常量而非宏，避免头文件污染
inline constexpr const char* kWelcomeCompletedKey = "welcome_completed";
