#pragma once

#include <QButtonGroup>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>
#include <string>
#include <vector>

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

// P1-1 fix (F6): 实验手册可机检练习类型
enum class LabExerciseType {
    CHOICE,         // 选择题：4 选项 + 单选正确答案
    EXPECTED_OUTPUT // 预期输出型：运行样例代码并比对实际输出
};

/// P1-1 fix (F6): 单个可机检练习项
struct LabExercise {
    LabExerciseType type = LabExerciseType::CHOICE;
    std::string prompt;               // 题干
    std::vector<std::string> options; // CHOICE: 选项列表
    int correctIndex = 0;             // CHOICE: 正确选项索引（0-based）
    std::string sampleCode;           // EXPECTED_OUTPUT: 待运行代码
    std::string expectedOutput;       // EXPECTED_OUTPUT: 预期输出（trim 比对）
    std::string explanation;          // 解析（答对/答错都显示）
};

struct LabChapter {
    std::string id;         // 如 "lab-01"
    std::string title;      // 章节标题
    std::string markdown;   // Markdown 内容
    std::string sampleCode; // 可一键加载到主编辑器的样例代码
    // P1-1 fix (F6): 章节末尾的可机检练习（可空）
    std::vector<LabExercise> exercises;
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
    /// P0-1 fix (F5): 请求加载样例代码到主编辑器并立即运行（一键闭环）
    void runSampleRequested(const QString& code);
    /// P1-4 fix (F16): 请求跳转到指定面板（教学面板 id 或底层视图 id）。
    /// 由 Markdown 内容中的链接 `[文字](panel:panel-id)` 触发，
    /// 如 `[编译管线可视化](panel:pipeline)` / `[Token 步骤](panel:tokens)`。
    void jumpToPanelRequested(const QString& panelId);
    /// P1-1 fix (F6): 章节练习全部答对时发射，主窗口连接后回写 LearningPathPanel
    void exerciseCompleted(const QString& chapterId);

protected:
    /// 首次显示时选中第一章并渲染（推迟 Markdown 渲染到面板可见时，
    /// 避免 IDE 启动时为隐藏 dock 渲染第一章）
    void showEvent(QShowEvent* event) override;

private slots:
    void onChapterSelected(int row);
    void onLoadSampleToEditor();
    void onRunSample(); ///< P0-1 fix: 加载示例并运行
    /// P1-4 fix (F16): 处理 Markdown 内容中的链接点击。
    /// 支持 `panel:xxx` 协议（如 panel:pipeline 跳转到编译管线面板），
    /// 其他协议（http/https/file 等）忽略。
    void onAnchorClicked(const QUrl& url);
    /// P1-1 fix (F6): 提交选择题答案，即时判分并显示解析
    void onSubmitChoiceExercise(int exerciseIndex);
    /// N1 fix: 提交预期输出题答案——同步运行样例代码并比对实际输出
    void onSubmitExpectedOutputExercise(int exerciseIndex);
    /// P2 fix (长章折叠): 切换次要章节（进阶/思考题/常见错误等）的折叠状态。
    /// 折叠时次要章节内容替换为一行提示，重新渲染当前章节；展开恢复完整内容。
    void onToggleFold();

private:
    IdeController* controller_ = nullptr;
    // issue 6: 用章节芯片栏替代大目录列表（参考 TokenPuzzlePanel 关卡芯片）
    QWidget* chapterChipBar_ = nullptr; ///< 章节芯片栏容器
    QList<QPushButton*> chapterChips_;  ///< 章节芯片按钮列表
    QTextBrowser* contentBrowser_ = nullptr;
    QPlainTextEdit* sampleCodeEdit_ = nullptr; ///< 右侧样例代码预览（头歌风格）
    QLabel* statusLabel_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* runBtn_ = nullptr;    ///< P0-1 fix: 一键运行示例
    QPushButton* foldBtn_ = nullptr;   ///< P2 fix: 折叠/展开次要章节
    QSpinBox* fontSizeSpin_ = nullptr; ///< 实验手册正文字号调节

    QSplitter* mainSplitter_ = nullptr; ///< 主体 splitter 引用（2 栏：内容 | 代码+练习）

    // P1-1 fix (F6): 练习区域容器 + 控件状态
    QWidget* exercisesContainer_ = nullptr; ///< 章节底部练习列表容器
    QVBoxLayout* exercisesLayout_ = nullptr;
    std::vector<QButtonGroup*> choiceGroups_; ///< 每道选择题的选项按钮组
    std::vector<bool> exercisePassed_;        ///< 每道题是否已通过

    int currentChapterIndex_ = -1;
    bool firstShowDone_ = false;     ///< 是否已完成首次显示渲染
    bool foldMinorSections_ = false; ///< P2 fix: 是否折叠次要章节（进阶/思考题/常见错误等）
    // AUDIT-P2 fix: 防重复提交守卫——同步执行链期间快速重复点击会触发多次
    // runStringCaptureOutput + 多次 save() 磁盘 I/O + recordFailure 计数虚高。
    bool submitting_ = false;

    void populateChapterChips(); ///< issue 6: 构建章节芯片栏
    void refreshChapterChips();  ///< issue 6: 刷新芯片状态（当前高亮）
    void showCurrentChapter();
    /// P1-1 fix (F6): 重建当前章节的练习控件
    void rebuildExercises();
    /// P1-1 fix (F6): 检查所有练习是否全部通过，是则 emit exerciseCompleted
    void checkAllExercisesPassed();
    /// P2 fix (长章折叠): 判断 ## 标题是否属于「次要章节」（进阶/思考题/常见错误等）。
    /// 次要章节在折叠模式下只保留标题 + 一行提示，主章节（目标/概念/步骤/验证）始终展开。
    static bool isMinorSection(const QString& headingText);
    /// P2 fix (长章折叠): 对 Markdown 应用折叠——把次要章节内容替换为简短提示。
    /// 折叠模式下 TOC 中的次要章节链接仍可点击（标题保留），但跳转后只看到提示行。
    /// @param markdown 原始 Markdown
    /// @return 折叠后的 Markdown（若 foldMinorSections_=false 则原样返回）
    QString applyFolding(const std::string& markdown) const;
    /// P2-UX fix: 持久化学习进度并在失败时弹出 toast 通知用户。
    /// 包裹 LearnerProgressStore::instance().save()，失败时显示 InfoBar 警告，
    /// 避免进度静默丢失（磁盘满/权限不足等场景）。
    void saveProgressWithFeedback();
};
