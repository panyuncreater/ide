#pragma once

#include <QAction>
#include <QCloseEvent>
#include <QGridLayout>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVariantAnimation>
#include <functional>
#include <memory>
#include <vector>

#include <QSettings>

// ADS 停靠框架
#include "DockManager.h"
#include "DockWidget.h"

// QFluentKit 主题
#include "FluentGlobal.h"
#include "ToolButton.h" // TransparentToolButton（主题切换按钮）

#include "Diagnostic.h"
#include "IdeController.h"
#include "gui/ActivityBar.h"
#include "gui/AstBuilderToyPanel.h"
#include "gui/AstViewer.h"
#include "gui/BackendComparePanel.h"
#include "gui/BreakpointConditionPanel.h"
#include "gui/BugHuntPanel.h"
#include "gui/BytecodeTracePanel.h"
#include "gui/CallStackPanel.h"
#include "gui/ClosureInspectorPanel.h"
#include "gui/CodeEditor.h"
#include "gui/CodeJourneyInfoPanel.h"
#include "gui/DebugPanel.h"
#include "gui/ExceptionFlowPanel.h"
#include "gui/FindReplacePanel.h"
#include "gui/GlossaryPanel.h"
#include "gui/IRTransformPanel.h"
#include "gui/IrViewer.h"
#include "gui/LabManualPanel.h"
#include "gui/LearningPathPanel.h"
#include "gui/MemoryModelPanel.h"
#include "gui/PanelCatalog.h" // P2-1 fix: 替代废弃的 LearningHubDialog
#include "gui/PipelineViewer.h"
#include "gui/ProfileDashboardPanel.h"
#include "gui/ReplPanel.h"
#include "gui/SyntaxExplorerPanel.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/TeachingPanelHeader.h"
#include "gui/TeachingTreePanel.h"
#include "gui/TokenPuzzlePanel.h"
#include "gui/VariableInspectorPanel.h"
#include "gui/VmStackPanel.h"
#include "gui/VmStackSandboxPanel.h"

class Pivot;
class QLabel;
class QLineEdit;
class ComboBox;           // QFluentKit ComboBox
class GuidedTour;         // 新手引导组件
class QFileSystemWatcher; // 文件外部修改监听

// ============================================================
// Ide — MiniLang IDE 主窗口
// ------------------------------------------------------------
// 第六轮重构：活动栏 + Pivot 标签 + Fluent 组件 + AST 独立窗口
// ============================================================

// Output level for structured output panel
enum class OutputLevel {
    Plain,   // User program output (default, no icon prefix)
    Info,    // Compilation progress messages
    Success, // Completion / pass messages
    Warning, // Warning messages
    ErrorMsg // Error messages (named ErrorMsg to avoid conflict with ERROR macro)
};

class Ide : public QMainWindow {
    Q_OBJECT

public:
    Ide(QWidget* parent = nullptr);
    ~Ide();

private slots:
    void onRun();
    void onDebug();

    void onStepIn();
    void onStepOver();
    void onStepOut();
    void onResume();
    void onStop();

    void onClearOutput();
    // 深色主题已移除：onThemeToggle slot 已删除

    void onNew();
    void onOpen();
    void onOpenFolder();
    void onSave();
    void onSaveAs();

    void onPausedAt(int line);

    void onFormat();
    void onCompileAnalysis();
    void onShowAstTree();
    void onRightTabChanged(int index);

    void onVmStep();
    void onVmStepOver();
    void onVmStepOut();
    void onVmRun();
    void onVmStop();

    void handleVmStepResult(IdeController::VmStepResult result);
    void setVmStepActionsEnabled(bool enabled, bool running = false);

    void onFind();
    void onReplace();
    void onFindNext();
    void onFindPrev();

    void onFileTreeItemActivated(QTreeWidgetItem* item, int column);
    void onCurrentTabChanged(int index);
    void onEditorTabCloseRequested(int index);

    void onFileTreeContextMenu(const QPoint& pos);
    void onNewFileInTree();
    void onNewFolderInTree();
    void onRenameInTree();
    void onDeleteInTree();

    /// 编辑器右键标签菜单
    void onEditorTabContextMenu(const QPoint& pos);
    void onCloseOtherTabs();
    void onCloseAllTabs();

    /// 教学面板标题栏「新手引导」按钮 → 启动对应面板的 GuidedTour
    void onPanelGuidedTourRequested(const QString& panelId);

private:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    void syncVmBreakpoints();
    void updateTabCloseButtons(int hoveredIndex);

    struct EditorTabData {
        QWidget* container = nullptr;
        CodeEditor* editor = nullptr;
        SyntaxHighlighter* highlighter = nullptr;
        FindReplacePanel* findPanel = nullptr;
        QString filePath;
        bool isUntitled = true;
    };

    IdeController* controller_ = nullptr;

    // ---- 当前活跃编辑器（由 editorTabWidget_ 当前标签驱动）----
    CodeEditor* codeEditor_ = nullptr;
    FindReplacePanel* findReplacePanel_ = nullptr;
    SyntaxHighlighter* highlighter_ = nullptr;

    std::vector<EditorTabData> editorTabs_;

    // ---- 中央区域：欢迎页 ↔ 编辑器标签页 ----
    QWidget* welcomePage_ = nullptr;
    QStackedWidget* centerStack_ = nullptr; // 教学面板/欢迎页栈
    QTabWidget* editorTabWidget_ = nullptr;
    QSplitter* centerSplitter_ = nullptr;       // 三栏布局：[centerStack_ | editorSplitter_]
    QSplitter* editorSplitter_ = nullptr;       // 编辑器区纵向分割：[editorTabWidget_ | bottomContainer_]
    QVariantAnimation* splitterAnim_ = nullptr;     // centerSplitter_ 尺寸动画
    QVariantAnimation* bottomPanelAnim_ = nullptr;  // editorSplitter_ 底部面板展开/收起动画
    int untitledCount_ = 0;

    // ---- ADS 停靠管理器 ----
    ads::CDockManager* dockManager_ = nullptr;

    // 活动栏
    ActivityBar* activityBar_ = nullptr;

    // ---- 第九轮 → 十二轮统一：单一 36px 标题栏（融合菜单+工具栏+窗口控制）----
    QWidget* titleBar_ = nullptr;          // 统一标题栏容器
    QLabel* titleIconLabel_ = nullptr;     // 程序图标
    QLabel* titleTextLabel_ = nullptr;     // "MiniLang IDE" 文字
    QLabel* titlePathLabel_ = nullptr;     // 当前文件路径（灰色小字）
    QToolButton* titleMinBtn_ = nullptr;   // 最小化
    QToolButton* titleMaxBtn_ = nullptr;   // 最大化/还原
    QToolButton* titleCloseBtn_ = nullptr; // 关闭
    // 深色主题已移除：themeToggleBtn_ 成员已删除
    ComboBox* engineCombo_ = nullptr; // 执行引擎切换
    bool syncingViewAction_ = false;  // 防止视图菜单与面板 toggleView 递归

    // 左侧面板
    ads::CDockWidget* fileTreeDock_ = nullptr;
    ads::CDockWidget* debugPanelDock_ = nullptr;
    QTreeWidget* fileTree_ = nullptr;
    QLineEdit* fileTreeFilterEdit_ = nullptr;
    QTimer* fileTreeFilterTimer_ = nullptr; // 文件树过滤防抖（200ms）  // 文件树搜索过滤框
    DebugPanel* debugPanel_ = nullptr;

    // 底部面板（editorSplitter_ 内部，仅覆盖代码编辑区，类似 VS Code 集成终端）
    QWidget* bottomContainer_ = nullptr;
    bool bottomVisible_ = false;
    Pivot* bottomPivot_ = nullptr;
    QStackedWidget* bottomStack_ = nullptr;
    QTextEdit* outputTextEdit_ = nullptr;
    QListWidget* errorListWidget_ = nullptr;
    // 错误列表类型过滤按钮（可点击切换显隐某级别错误）
    QToolButton* errFilterErrorBtn_ = nullptr;
    QToolButton* errFilterWarningBtn_ = nullptr;
    QToolButton* errFilterInfoBtn_ = nullptr;
    QToolButton* errFilterHintBtn_ = nullptr;
    ReplPanel* replPanel_ = nullptr;
    bool errorPanelHasErrors_ = false;

    // 右侧面板（单一 dock + Pivot 标签切换）
    ads::CDockWidget* rightDock_ = nullptr;
    Pivot* rightPivot_ = nullptr;
    QStackedWidget* rightStack_ = nullptr;
    QTableWidget* tokenTable_ = nullptr;
    QListWidget* bytecodeList_ = nullptr;
    VmStackPanel* vmStackPanel_ = nullptr;
    IrViewer* irViewer_ = nullptr;

    // AST 独立窗口（不嵌入停靠系统）
    QWidget* astWindow_ = nullptr;
    AstViewer* astViewer_ = nullptr;

    // ---- 教学增强面板（第一波 + 第三波）----
    // 第十四轮重构：19 个教学面板从独立 dock 迁移到 centerStack_ 子页，
    // xxxDock_ 成员已删除（grep 确认 ide.cpp 零引用）。xxxPanel_ 成员保留供 registerTeachingPanel 使用。
    // 第一波 P0-1：编译管线可视化（源码→Token→AST→IR→字节码）
    PipelineViewer* pipelineViewer_ = nullptr;
    // 第一波 P0-3：三后端并行对比
    BackendComparePanel* backendComparePanel_ = nullptr;
    // 第一波 P1-3：Bug 狩猎模式
    BugHuntPanel* bugHuntPanel_ = nullptr;
    // 第三波 P2-1：交互式语法探索器
    SyntaxExplorerPanel* syntaxExplorerPanel_ = nullptr;
    // 第三波 P2-2：内置实验手册
    LabManualPanel* labManualPanel_ = nullptr;

    // ---- 教学增强面板（第二波）----
    // P0-2：内存模型可视化（NaN-boxing / RefCounted / COW / GC）
    MemoryModelPanel* memoryModelPanel_ = nullptr;
    // P1-1：IR 变换过程动画（AST → IR lowering + 优化 pass 前后对比）
    IRTransformPanel* irTransformPanel_ = nullptr;
    // P1-2：性能剖析仪表盘（三后端时间对比 + 热点 + 内存/GC 统计）
    ProfileDashboardPanel* profileDashboardPanel_ = nullptr;

    // ---- 教学增强面板（第三波）----
    // P0-1：调用栈可视化（运行期函数调用层次 + 本地变量）
    CallStackPanel* callStackPanel_ = nullptr;
    // P0-2：变量检查器（按作用域分组 + NaN-boxing 位详情）
    VariableInspectorPanel* variableInspectorPanel_ = nullptr;
    // P0-3：字节码执行轨迹（IP/OpCode/栈快照时间轴）
    BytecodeTracePanel* bytecodeTracePanel_ = nullptr;
    // 第二档 P1-2：条件断点可视化（断点列表 + 条件表达式 + 命中次数）
    BreakpointConditionPanel* breakpointConditionPanel_ = nullptr;
    // 第三档 P2-3a：异常流可视化（教学场景库 + 传播图解）
    ExceptionFlowPanel* exceptionFlowPanel_ = nullptr;
    // 第三档 P2-3b：闭包检查器（教学场景库 + upvalue 生命周期）
    ClosureInspectorPanel* closureInspectorPanel_ = nullptr;

    // ---- 教学增强面板（第四档：MINILANG_IDE_IMPROVEMENT_PLAN 功能 1-6）----
    // 功能 6：学习路径地图（中央导航枢纽，5 阶段 21 活动 + JSON 进度持久化）
    LearningPathPanel* learningPathPanel_ = nullptr;
    // 功能 3：交互式 Token 拼图游戏（5 关卡，星级评分）
    TokenPuzzlePanel* tokenPuzzlePanel_ = nullptr;
    // 功能 4：AST 节点搭建玩具（6 题，QTreeWidget + 工具箱）
    AstBuilderToyPanel* astBuilderToyPanel_ = nullptr;
    // 功能 5：VM 栈沙盒（5 关卡，push/pop 模拟栈状态机）
    VmStackSandboxPanel* vmStackSandboxPanel_ = nullptr;
    // 功能 2 降级：代码生命旅程静态信息图（HTML 信息图 + 6 个跳转按钮）
    CodeJourneyInfoPanel* codeJourneyPanel_ = nullptr;
    // P2-8：术语表（Glossary）— 集中展示 MiniLang IDE 全部核心术语
    GlossaryPanel* glossaryPanel_ = nullptr;

    // ---- 教学面板树形导航（左侧停靠区，ActivityBar 「学习」入口）----
    // 替代原 LearningHubDialog 弹窗：4 大分类可折叠树 + 顶部「代码编辑器」入口
    ads::CDockWidget* teachingTreeDock_ = nullptr;
    TeachingTreePanel* teachingTreePanel_ = nullptr;
    /// panelId → centerStack_ 索引映射（教学面板从 dock 迁移到 centerStack_ 后建立）
    QMap<QString, int> panelToStackIndex_;
    /// 教学面板懒加载工厂：panelId → 构造函数（首次访问时调用，避免启动时全量构造 20 个面板）
    QMap<QString, std::function<void()>> teachingPanelFactories_;
    /// 确保教学面板已构造（若未构造则调用工厂），返回是否为首次构造
    void ensureTeachingPanelCreated(const QString& panelId);
    /// 当前 centerStack_ 是否处于「代码编辑器」模式（用于切换时显隐 bottomDock_/rightDock_）
    bool centerInEditorMode_ = true;
    // BUG-R14-2 fix: 进入教学面板模式前记录 bottomDock_/rightDock_ 的可见状态，
    // showEditorArea 切回编辑器时恢复，避免用户布局丢失。
    bool bottomDockWasVisibleBeforeTeaching_ = true;
    bool rightDockWasVisibleBeforeTeaching_ = true;

    // ---- 工具栏 ----
    QAction* runAction_ = nullptr;
    QAction* debugAction_ = nullptr;
    QAction* stepInAction_ = nullptr;
    QAction* stepOverAction_ = nullptr;
    QAction* stepOutAction_ = nullptr;
    QAction* resumeAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* clearAction_ = nullptr;
    QAction* formatAction_ = nullptr;
    QAction* compileAnalysisAction_ = nullptr;
    QAction* astAction_ = nullptr;
    QAction* openFolderAction_ = nullptr;

    QWidget* debugButtonContainer_ = nullptr;
    bool debugButtonsVisible_ = false;
    QAction* debugSepAction_ = nullptr; // 第八轮：动态分隔线，随调试按钮显隐

    QAction* vmStepAction_ = nullptr;
    QAction* vmStepOverAction_ = nullptr;
    QAction* vmStepOutAction_ = nullptr;
    QAction* vmRunAction_ = nullptr;
    QAction* vmStopAction_ = nullptr;

    QWidget* vmButtonContainer_ = nullptr;
    QAction* vmSepAction_ = nullptr; // 第八轮：动态分隔线，随 VM 按钮显隐

    QAction* newAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;

    // 视图菜单面板显隐
    QAction* viewExplorerAction_ = nullptr;
    QAction* viewDebugAction_ = nullptr;
    QAction* viewOutputAction_ = nullptr;
    QAction* viewCompileAnalysisAction_ = nullptr;
    // 教学增强面板视图菜单项
    QAction* viewPipelineAction_ = nullptr;
    QAction* viewBackendCompareAction_ = nullptr;
    QAction* viewBugHuntAction_ = nullptr;
    QAction* viewSyntaxExplorerAction_ = nullptr;
    QAction* viewLabManualAction_ = nullptr;
    // 第二波教学面板视图菜单项
    QAction* viewMemoryModelAction_ = nullptr;
    QAction* viewIRTransformAction_ = nullptr;
    QAction* viewProfileDashboardAction_ = nullptr;
    // 第三波教学面板视图菜单项
    QAction* viewCallStackAction_ = nullptr;
    QAction* viewVariableInspectorAction_ = nullptr;
    QAction* viewBytecodeTraceAction_ = nullptr;
    // 第二档 P1-2 教学面板视图菜单项
    QAction* viewBreakpointConditionAction_ = nullptr;
    // 第三档 P2-3 教学面板视图菜单项
    QAction* viewExceptionFlowAction_ = nullptr;
    QAction* viewClosureInspectorAction_ = nullptr;
    // 第四档教学面板视图菜单项（功能 1-6）
    QAction* viewLearningPathAction_ = nullptr;
    QAction* viewTokenPuzzleAction_ = nullptr;
    QAction* viewAstBuilderToyAction_ = nullptr;
    QAction* viewVmStackSandboxAction_ = nullptr;
    QAction* viewCodeJourneyAction_ = nullptr;
    // 学习中心入口（ActivityBar + 视图菜单）
    QAction* viewLearningHubAction_ = nullptr;
    // P2-8：术语表视图入口
    QAction* viewGlossaryAction_ = nullptr;

    // 状态栏
    QLabel* statusLineLabel_ = nullptr;
    QLabel* statusColLabel_ = nullptr;
    QLabel* statusSaveLabel_ = nullptr;
    QLabel* statusRunLabel_ = nullptr;
    QLabel* statusEncodingLabel_ = nullptr;  // 第八轮：文件编码显示
    QLabel* statusEngineLabel_ = nullptr;    // 执行引擎显示
    QLabel* statusSelectionLabel_ = nullptr; // M6：选中范围显示（行数 + 字符数）

    // ---- 状态 ----
    QString workspaceDir_;
    QString currentFilePath_;
    bool isDirty_ = false;
    bool hasWorkspace_ = false;
    // ISSUE-7 fix: 关闭流程进行中标志。closeEvent 入口置 true，
    // 所有信号处理回调（handleVmStepResult/onWorkerFinished/onPausedAt/
    // displayDiagnostics 等）入口检查此标志，避免 processEvents 或析构期间
    // 残留的 QueuedConnection 信号访问已部分析构的成员导致 UAF。
    bool closing_ = false;
    int bottomPanelHeight_ = 220; // 第十二轮：输出面板默认高度，用户调整后记忆
    int codeFontSize_ = 11;       // 代码编辑器全局字号（默认 11pt），应用于所有编辑器标签页
    // R60-2 fix: 标记是否有已保存的 dock 布局。首次启动时为 false，
    // 面板首次打开时应用默认尺寸；有保存布局时由 restoreState 恢复，不覆盖。
    bool hasSavedLayout_ = false;
    // R60-2 fix: 标记左侧 dock 是否已应用过默认尺寸（避免每次 toggleView 都 resize）
    bool leftDockDefaultSized_ = false;
    // R61-3 fix: 延迟到 showEvent 中恢复 dock 布局，确保窗口有有效几何尺寸
    bool firstShow_ = true;
    QByteArray pendingDockState_;

    // ---- 文件外部修改监听 ----
    QFileSystemWatcher* fileWatcher_ = nullptr;
    QString watchedFilePath_; // 当前被监视的文件路径
    bool selfSaving_ = false; // 标识 IDE 自身保存触发 fileChanged，跳过外部修改弹框

    // ---- 防抖定时器 ----
    QTimer* completionTimer_ = nullptr;   // 补全词刷新（500ms）
    QTimer* syntaxCheckTimer_ = nullptr;  // 语法检查（300ms）
    QTimer* splitterSaveTimer_ = nullptr; // 布局保存防抖（500ms）
    // ROUND-75 fix: 跟踪折叠教学区/隐藏编辑器栏的延迟回调。
    // 原 QTimer::singleShot 创建临时 QTimer，事件队列独立，
    // closeEvent 无法取消，showTeachingPanel 也无法在用户切换意图后撤销折叠。
    // 改为成员 QTimer：showTeachingPanel 入口 stop 取消挂起的 hide 意图；
    // closeEvent 中 stop 避免回调在 maybeSave 模态对话框期间触发 UAF。
    QTimer* pendingHideCenterTimer_ = nullptr; // 折叠教学区（350ms，ensureEditorVisible/showEditorArea）
    QTimer* pendingHideEditorTimer_ = nullptr; // 隐藏编辑器栏（360ms，onEditorTabCloseRequested）
    QStringList staticCompletionWords_;

    // ---- 欢迎页：最近打开列表 ----
    QListWidget* recentListWidget_ = nullptr;
    QStringList recentWorkspaces_;
    void loadRecentWorkspaces();
    void addRecentWorkspace(const QString& dir);
    void refreshRecentList();

    // ---- 字节码/IR 辅助 ----
    struct ChunkRowInfo {
        std::string name;
        int startRow;
        int rowCount;
    };
    std::vector<ChunkRowInfo> chunkRowMap_;

    size_t lastBytecodeSourceHash_ = 0;
    std::vector<std::pair<size_t, size_t>> irToBytecodeOffset_;

    // ---- 拼写纠错候选词 ----
    std::vector<std::string> spellCandidates_;

    // ---- 初始化 ----
    void initUI();
    void initConnections();
    void initFileTree();
    void initWelcomePage();
    void initStatusBar();
    void initTitleBar(); // 十二轮：统一标题栏（融合菜单+工具栏+窗口控制，36px）
    void applyFluentStyle();
    void setupCompletion();
    void updateCompletionWords();
    void updateStatusBar();

    /// 第九轮：同步视图菜单勾选状态与 dock 实际显隐
    void syncViewMenuChecks();

    // ---- 输出/错误 ----
    void appendOutput(const QString& text, OutputLevel level = OutputLevel::Plain);
    void appendError(const QString& text, int line = 0, int column = 0, DiagLevel level = DiagLevel::Error);
    void clearOutput();
    void updateErrorBadge();
    void applyErrorFilter(); // 错误列表类型过滤

    // ---- 可视化 ----
    void highlightBytecodeLine(const std::string& chunkName, size_t ip);
    void onBytecodeRowClicked(int row);
    void populateBytecodeList();
    void populateIRViewer();
    void highlightIRLine(size_t bytecodeOffset);
    void updateTokenTable();
    void updateAstViewer();
    void updateDebugInfo();
    void displayDiagnostics(const DiagnosticBag& bag);
    void setRunningState(bool running);
    void onWorkerFinished(bool wasDebug);
    void loadVisualizationForTab(int tabIndex);

    /// 第八轮：运行/调试前检查编译错误，存在错误时显示诊断并返回 true 拦截
    bool blockIfHasErrors();

    /// 第八轮：实时语法检查（300ms 防抖触发）
    /// 清空旧错误标记 → 全量扫描 → 按行号排序展示 → 更新波浪下划线
    void runRealTimeSyntaxCheck();

    // ---- 面板控制 ----
    void showBottomPanel(int tabIndex = 0);
    void hideBottomPanel();
    void showRightPanel(int tabIndex = 0);
    void hideRightPanel();
    void toggleBottomPanel();
    void toggleRightPanel();
    void switchLeftToFileTree();
    void switchLeftToDebugPanel();
    /// R60-2 fix: 首次启动时左侧 dock 首次打开应用默认宽度
    void ensureLeftDockDefaultSize();
    void showAstWindow();
    void onActivityChanged(int index);
    void onActivityChangedById(const QString& id);
    void onRightPivotChanged(const QString& routeKey);
    void onBottomPivotChanged(const QString& routeKey);

    // ---- 教学面板树形导航：centerStack_ 切换 ----
    /// 切换 centerStack_ 到指定教学面板（按 panelId 查 panelToStackIndex_）
    /// 若 panelId 不在映射中，回退到编辑器模式
    void showTeachingPanel(const QString& panelId);
    /// 切换 centerStack_ 到代码编辑器模式（editorTabWidget_ 或 welcomePage_）
    /// 同时显示 bottomDock_/rightDock_，隐藏教学面板
    void showEditorArea();
    /// 平滑动画 centerSplitter_ 尺寸变化（教学面板展开/折叠过渡）
    void animateCenterSplitter(const QList<int>& startSizes, const QList<int>& targetSizes, int durationMs = 300);
    /// 教学树点击 → panelId 路由（"editor" → showEditorArea，其余 → showTeachingPanel）
    void onTeachingPanelRequested(const QString& panelId);
    /// 应用全局 codeFontSize_ 到所有已打开的编辑器标签页
    void applyCodeFontSizeToAllEditors();
    /// 应用教学字号到所有教学面板的 QTextBrowser/QListWidget/QTextEdit
    /// 教学阅读字号 = codeFontSize_ + 2（比代码字号大 2pt，提升阅读舒适度）
    /// 遍历 centerStack_ 所有子 widget 中的文本控件
    void applyTeachingFontSize();

    // ---- 第四档教学面板：跨面板跳转路由 ----
    /// CodeJourneyInfoPanel 跳转按钮 → 显示对应面板 dock
    /// panelId 取值 "editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"
    void onJumpToPanel(const QString& panelId);
    /// LearningPathPanel 活动项点击 → 路由到对应面板 dock
    /// activityId 取值参见 LearningPathData::activities()
    void onActivityRequested(const QString& activityId);

    // ---- 学习中心（已废弃）----
    // 第十四轮重构：TeachingTreePanel 树形导航替代 LearningHubDialog 弹窗。
    // P2-1 fix：彻底删除 gui/LearningHubDialog.h/.cpp 与 cmake 注册，
    // 卡片设计如未来需要复用，从 git 历史恢复。
    // showLearningHub / onLearningHubPanelRequested 已删除（grep 确认零调用者）。

    /// 教学面板包装器：在教学面板顶部插入 TeachingPanelHeader
    /// panelId 用于查找帮助文案，title 为标题文本，panel 为原始面板
    QWidget* wrapTeachingPanel(const QString& panelId, const QString& title, QWidget* panel);

    void showDebugButtons(bool show);
    void showVmButtons(bool show);

    // ---- 帮助弹窗 ----
    void showHelpDialog();

    // ---- 新手引导：3 分钟 Hello World guided tour ----
    void startGuidedTour();
    GuidedTour* guidedTour_ = nullptr;
    // ROUND-67 P1 fix: 跟踪面板特定 GuidedTour（5 个教学面板首次访问自动触发）。
    // 这些 tour 是 Ide 子对象但不存储在 guidedTour_ 中，closeEvent 需显式停止并清理，
    // 否则 tour 的 QTimer::singleShot(0, tour, ...) 在 maybeSave 模态对话框期间派发，
    // 访问可能已被 reparent/清理的 targetWidget → UAF。
    QSet<GuidedTour*> activePanelTours_;

    // ---- AST 窗口 ----
    void saveAstWindowGeometry();
    void restoreAstWindowGeometry();

    // ---- 文件树 ----
    void populateFileTree();

    // ---- 编辑器标签 ----
    int createNewEditorTab(const QString& filePath = QString(), const QString& content = QString());
    void switchToTab(int index);
    int findTabForFile(const QString& path);
    void loadFileIntoTab(int tabIndex, const QString& path);

    bool maybeSave();
    void updateWindowTitle();
    void loadFile(const QString& path);
    // 文件外部修改监听：切换/保存后更新被监视路径；外部修改时弹框询问是否重载
    void setupFileWatcher(const QString& filePath);
    void onFileChangedExternally(const QString& filePath);
    void openWorkspace(const QString& dirPath);
    void saveLayout();
    void restoreLayout();
    void ensureEditorVisible();

    // ---- 教学增强面板：将面板内代码加载到主编辑器 ----
    void loadCodeIntoMainEditor(const QString& code);
};
