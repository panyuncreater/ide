// ============================================================
// TokenPuzzlePanel.h — 交互式 Token 拼图游戏面板（功能 3）
// ------------------------------------------------------------
// 通过游戏化方式理解"词法分析就是切分字符流"。玩家将打乱的
// token 按正确顺序排列，还原目标语句。
//
// UI 布局：
//   顶部：关卡选择 QComboBox + 得分显示 QLabel + 关卡进度
//   中部上：目标语句展示（只读）
//   中部中：打乱的 token 按钮（QPushButton，点击添加到答案区）
//   中部下：玩家答案区（QListWidget，点击移除）
//   底部：[✓ 检查答案] [💡 提示] [⏭ 跳过] [🔄 重置] + 反馈 QLabel
//
// 交互逻辑：
//   - 点击打乱 token 按钮：添加到答案区末尾，原按钮禁用
//   - 点击答案区 token：移除并恢复对应打乱按钮
//   - 检查答案：逐 token 比对；正确则显示 ⭐ 并解锁下一关；错误则高亮错误位置
//   - 提示：显示 hint 文本，每次使用降低星级评分
//   - 跳过：直接解锁下一关（不计星）
//   - 重置：清空答案区，恢复所有打乱按钮
//
// 纯前端游戏，不依赖引擎层（不调用 Lexer/Parser/Interpreter）。
// ============================================================

#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QList>

class TokenPuzzlePanel : public QWidget {
    Q_OBJECT
public:
    explicit TokenPuzzlePanel(QWidget* parent = nullptr);

signals:
    /// 完成关卡时发射（levelId 形如 "token-puzzle-1"）
    void activityCompleted(const QString& levelId);

private slots:
    void onLevelChanged(int index);
    void onCheckAnswer();
    void onShowHint();
    void onSkipLevel();
    void onResetLevel();
    void onAnswerItemClicked(int row);

private:
    // 顶部栏
    QComboBox* levelCombo_   = nullptr;
    QLabel*    scoreLabel_   = nullptr;
    QLabel*    progressLabel_ = nullptr;
    QList<QPushButton*> levelChips_;  // 关卡芯片按钮栏（与 levelCombo_ 双向同步）

    // 中部
    QLabel*    targetCodeLabel_   = nullptr;
    QLabel*    teachingPointLabel_ = nullptr;
    QWidget*   shuffledContainer_ = nullptr;   // 打乱 token 按钮容器
    QListWidget* answerList_       = nullptr;  // 玩家答案区

    // 底部
    QPushButton* checkBtn_  = nullptr;
    QPushButton* hintBtn_   = nullptr;
    QPushButton* skipBtn_   = nullptr;
    QPushButton* resetBtn_  = nullptr;
    QLabel*      feedbackLabel_ = nullptr;

    // 状态
    int currentLevelIndex_ = 0;     // 当前关卡索引（0-4）
    int hintUsedCount_     = 0;     // 当前关卡已使用提示次数
    QList<QPushButton*> shuffledButtons_;  // 当前关卡的打乱 token 按钮

    // 完成记录（按关卡索引记录星级，-1=未完成，0=跳过，1-3=星级）
    QList<int> levelStars_;

    void buildUi();
    void loadLevel(int index);
    void rebuildShuffledButtons();
    void clearAnswer();
    void addShuffledTokenToAnswer(int buttonIndex);
    QString levelId(int index) const;
    QString starsToText(int stars) const;
    int computeStars(int hintUsed) const;
    void unlockNextLevel();
    void updateScoreDisplay();
    void setFeedback(const QString& text, bool isError = false);

    /// 刷新关卡芯片按钮栏的状态（locked / current / unlocked）
    void refreshLevelChips();
};
