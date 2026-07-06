#pragma once

#include <QMainWindow>
#include <QCloseEvent>
#include <QTabWidget>
#include <QToolBar>
#include <QAction>
#include <QTableWidget>
#include <QTextEdit>
#include <QListWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QGridLayout>
#include <memory>
#include <vector>

#include <QSettings>

// ADS 停靠框架
#include "DockManager.h"
#include "DockWidget.h"

// QFluentKit 主题
#include "FluentGlobal.h"
#include "ToolButton.h"  // TransparentToolButton（主题切换按钮）

#include "Diagnostic.h"
#include "IdeController.h"
#include "gui/CodeEditor.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/AstViewer.h"
#include "gui/DebugPanel.h"
#include "gui/ReplPanel.h"
#include "gui/VmStackPanel.h"
#include "gui/IrViewer.h"
#include "gui/FindReplacePanel.h"
#include "gui/ActivityBar.h"
#include "gui/PipelineViewer.h"
#include "gui/BackendComparePanel.h"
#include "gui/BugHuntPanel.h"
#include "gui/SyntaxExplorerPanel.h"
#include "gui/LabManualPanel.h"
#include "gui/MemoryModelPanel.h"
#include "gui/IRTransformPanel.h"
#include "gui/ProfileDashboardPanel.h"
#include "gui/CallStackPanel.h"
#include "gui/VariableInspectorPanel.h"
#include "gui/BytecodeTracePanel.h"
#include "gui/BreakpointConditionPanel.h"
#include "gui/ExceptionFlowPanel.h"
#include "gui/ClosureInspectorPanel.h"
#include "gui/LearningPathPanel.h"
#include "gui/TokenPuzzlePanel.h"
#include "gui/AstBuilderToyPanel.h"
#include "gui/VmStackSandboxPanel.h"
#include "gui/CodeJourneyInfoPanel.h"
#include "gui/LearningHubDialog.h"
#include "gui/TeachingPanelHeader.h"

class Pivot;
class QLabel;
class QLineEdit;
class ComboBox;   // QFluentKit ComboBox

// ============================================================
// Ide — MiniLang IDE 主窗口
// ------------------------------------------------------------
// 第六轮重构：活动栏 + Pivot 标签 + Fluent 组件 + AST 独立窗口
// ============================================================

// Output level for structured output panel
enum class OutputLevel {
    Plain,      // User program output (default, no icon prefix)
    Info,       // Compilation progress messages
    Success,    // Completion / pass messages
    Warning,    // Warning messages
    ErrorMsg    // Error messages (named ErrorMsg to avoid conflict with ERROR macro)
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

private:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

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
    QStackedWidget* centerStack_ = nullptr;
    QTabWidget* editorTabWidget_ = nullptr;
    int untitledCount_ = 0;

    // ---- ADS 停靠管理器 ----
    ads::CDockManager* dockManager_ = nullptr;
    /// 缓存上一次应用到 qApp 的 ADS QSS 片段，避免 setStyleSheet 追加导致全局样式表无限增长
    QString lastAdsQss_;

    // 活动栏
    ActivityBar* activityBar_ = nullptr;

    // ---- 第九轮 → 十二轮统一：单一 36px 标题栏（融合菜单+工具栏+窗口控制）----
    QWidget* titleBar_ = nullptr;           // 统一标题栏容器
    QLabel* titleIconLabel_ = nullptr;      // 程序图标
    QLabel* titleTextLabel_ = nullptr;      // "MiniLang IDE" 文字
    QLabel* titlePathLabel_ = nullptr;      // 当前文件路径（灰色小字）
    QToolButton* titleMinBtn_ = nullptr;    // 最小化
    QToolButton* titleMaxBtn_ = nullptr;    // 最大化/还原
    QToolButton* titleCloseBtn_ = nullptr;  // 关闭
    // 深色主题已移除：themeToggleBtn_ 成员已删除
    ComboBox* engineCombo_ = nullptr;       // 执行引擎切换
    bool syncingViewAction_ = false;        // 防止视图菜单与面板 toggleView 递归

    // 左侧面板
    ads::CDockWidget* fileTreeDock_ = nullptr;
    ads::CDockWidget* debugPanelDock_ = nullptr;
    QTreeWidget* fileTree_ = nullptr;
    QLineEdit* fileTreeFilterEdit_ = nullptr;
    QTimer* fileTreeFilterTimer_ = nullptr;  // 文件树过滤防抖（200ms）  // 文件树搜索过滤框
    DebugPanel* debugPanel_ = nullptr;

    // 底部面板（单一 dock + Pivot 标签切换）
    ads::CDockWidget* bottomDock_ = nullptr;
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
    // 第一波 P0-1：编译管线可视化（源码→Token→AST→IR→字节码）
    ads::CDockWidget* pipelineDock_ = nullptr;
    PipelineViewer* pipelineViewer_ = nullptr;
    // 第一波 P0-3：三后端并行对比
    ads::CDockWidget* backendCompareDock_ = nullptr;
    BackendComparePanel* backendComparePanel_ = nullptr;
    // 第一波 P1-3：Bug 狩猎模式
    ads::CDockWidget* bugHuntDock_ = nullptr;
    BugHuntPanel* bugHuntPanel_ = nullptr;
    // 第三波 P2-1：交互式语法探索器
    ads::CDockWidget* syntaxExplorerDock_ = nullptr;
    SyntaxExplorerPanel* syntaxExplorerPanel_ = nullptr;
    // 第三波 P2-2：内置实验手册
    ads::CDockWidget* labManualDock_ = nullptr;
    LabManualPanel* labManualPanel_ = nullptr;

    // ---- 教学增强面板（第二波）----
    // P0-2：内存模型可视化（NaN-boxing / RefCounted / COW / GC）
    ads::CDockWidget* memoryModelDock_ = nullptr;
    MemoryModelPanel* memoryModelPanel_ = nullptr;
    // P1-1：IR 变换过程动画（AST → IR lowering + 优化 pass 前后对比）
    ads::CDockWidget* irTransformDock_ = nullptr;
    IRTransformPanel* irTransformPanel_ = nullptr;
    // P1-2：性能剖析仪表盘（三后端时间对比 + 热点 + 内存/GC 统计）
    ads::CDockWidget* profileDashboardDock_ = nullptr;
    ProfileDashboardPanel* profileDashboardPanel_ = nullptr;

    // ---- 教学增强面板（第三波）----
    // P0-1：调用栈可视化（运行期函数调用层次 + 本地变量）
    ads::CDockWidget* callStackDock_ = nullptr;
    CallStackPanel* callStackPanel_ = nullptr;
    // P0-2：变量检查器（按作用域分组 + NaN-boxing 位详情）
    ads::CDockWidget* variableInspectorDock_ = nullptr;
    VariableInspectorPanel* variableInspectorPanel_ = nullptr;
    // P0-3：字节码执行轨迹（IP/OpCode/栈快照时间轴）
    ads::CDockWidget* bytecodeTraceDock_ = nullptr;
    BytecodeTracePanel* bytecodeTracePanel_ = nullptr;
    // 第二档 P1-2：条件断点可视化（断点列表 + 条件表达式 + 命中次数）
    ads::CDockWidget* breakpointConditionDock_ = nullptr;
    BreakpointConditionPanel* breakpointConditionPanel_ = nullptr;
    // 第三档 P2-3a：异常流可视化（教学场景库 + 传播图解）
    ads::CDockWidget* exceptionFlowDock_ = nullptr;
    ExceptionFlowPanel* exceptionFlowPanel_ = nullptr;
    // 第三档 P2-3b：闭包检查器（教学场景库 + upvalue 生命周期）
    ads::CDockWidget* closureInspectorDock_ = nullptr;
    ClosureInspectorPanel* closureInspectorPanel_ = nullptr;

    // ---- 教学增强面板（第四档：MINILANG_IDE_IMPROVEMENT_PLAN 功能 1-6）----
    // 功能 6：学习路径地图（中央导航枢纽，5 阶段 21 活动 + JSON 进度持久化）
    ads::CDockWidget* learningPathDock_ = nullptr;
    LearningPathPanel* learningPathPanel_ = nullptr;
    // 功能 3：交互式 Token 拼图游戏（5 关卡，星级评分）
    ads::CDockWidget* tokenPuzzleDock_ = nullptr;
    TokenPuzzlePanel* tokenPuzzlePanel_ = nullptr;
    // 功能 4：AST 节点搭建玩具（6 题，QTreeWidget + 工具箱）
    ads::CDockWidget* astBuilderToyDock_ = nullptr;
    AstBuilderToyPanel* astBuilderToyPanel_ = nullptr;
    // 功能 5：VM 栈沙盒（5 关卡，push/pop 模拟栈状态机）
    ads::CDockWidget* vmStackSandboxDock_ = nullptr;
    VmStackSandboxPanel* vmStackSandboxPanel_ = nullptr;
    // 功能 2 降级：代码生命旅程静态信息图（HTML 信息图 + 6 个跳转按钮）
    ads::CDockWidget* codeJourneyDock_ = nullptr;
    CodeJourneyInfoPanel* codeJourneyPanel_ = nullptr;

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
    QAction* debugSepAction_ = nullptr;   // 第八轮：动态分隔线，随调试按钮显隐

    QAction* vmStepAction_ = nullptr;
    QAction* vmStepOverAction_ = nullptr;
    QAction* vmStepOutAction_ = nullptr;
    QAction* vmRunAction_ = nullptr;
    QAction* vmStopAction_ = nullptr;

    QWidget* vmButtonContainer_ = nullptr;
    QAction* vmSepAction_ = nullptr;      // 第八轮：动态分隔线，随 VM 按钮显隐

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
    QAction* viewPipelineAction_       = nullptr;
    QAction* viewBackendCompareAction_ = nullptr;
    QAction* viewBugHuntAction_        = nullptr;
    QAction* viewSyntaxExplorerAction_ = nullptr;
    QAction* viewLabManualAction_      = nullptr;
    // 第二波教学面板视图菜单项
    QAction* viewMemoryModelAction_       = nullptr;
    QAction* viewIRTransformAction_       = nullptr;
    QAction* viewProfileDashboardAction_  = nullptr;
    // 第三波教学面板视图菜单项
    QAction* viewCallStackAction_           = nullptr;
    QAction* viewVariableInspectorAction_   = nullptr;
    QAction* viewBytecodeTraceAction_       = nullptr;
    // 第二档 P1-2 教学面板视图菜单项
    QAction* viewBreakpointConditionAction_ = nullptr;
    // 第三档 P2-3 教学面板视图菜单项
    QAction* viewExceptionFlowAction_      = nullptr;
    QAction* viewClosureInspectorAction_    = nullptr;
    // 第四档教学面板视图菜单项（功能 1-6）
    QAction* viewLearningPathAction_      = nullptr;
    QAction* viewTokenPuzzleAction_       = nullptr;
    QAction* viewAstBuilderToyAction_     = nullptr;
    QAction* viewVmStackSandboxAction_    = nullptr;
    QAction* viewCodeJourneyAction_       = nullptr;
    // 学习中心入口（ActivityBar + 视图菜单）
    QAction* viewLearningHubAction_       = nullptr;

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
    int bottomPanelHeight_ = 220;  // 第十二轮：输出面板默认高度，用户调整后记忆

    // ---- 防抖定时器 ----
    QTimer* completionTimer_ = nullptr;     // 补全词刷新（500ms）
    QTimer* syntaxCheckTimer_ = nullptr;    // 语法检查（300ms）
    QTimer* splitterSaveTimer_ = nullptr;   // 布局保存防抖（500ms）
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
    void initTitleBar();       // 十二轮：统一标题栏（融合菜单+工具栏+窗口控制，36px）
    void applyFluentStyle();
    void setupCompletion();
    void updateCompletionWords();
    void updateStatusBar();

    /// 第九轮：同步视图菜单勾选状态与 dock 实际显隐
    void syncViewMenuChecks();

    // ---- 输出/错误 ----
    void appendOutput(const QString& text, OutputLevel level = OutputLevel::Plain);
    void appendError(const QString& text, int line = 0, int column = 0,
                     DiagLevel level = DiagLevel::Error);
    void clearOutput();
    void updateErrorBadge();
    void applyErrorFilter();   // 错误列表类型过滤

    // ---- 可视化 ----
    void highlightBytecodeLine(const std::string& chunkName, size_t ip);
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
    void showAstWindow();
    void onActivityChanged(int index);
    void onActivityChangedById(const QString& id);
    void onRightPivotChanged(const QString& routeKey);
    void onBottomPivotChanged(const QString& routeKey);

    // ---- 第四档教学面板：跨面板跳转路由 ----
    /// CodeJourneyInfoPanel 跳转按钮 → 显示对应面板 dock
    /// panelId 取值 "editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"
    void onJumpToPanel(const QString& panelId);
    /// LearningPathPanel 活动项点击 → 路由到对应面板 dock
    /// activityId 取值参见 LearningPathData::activities()
    void onActivityRequested(const QString& activityId);

    // ---- 学习中心对话框（ActivityBar 「学习」入口）----
    /// 弹出 LearningHubDialog；panelId 见 LearningHubDialog 实现
    void showLearningHub();
    /// LearningHubDialog 卡片点击 → 路由到对应教学面板 dock
    void onLearningHubPanelRequested(const QString& panelId);

    /// 教学面板包装器：在教学面板顶部插入 TeachingPanelHeader
    /// panelId 用于查找帮助文案，title 为标题文本，panel 为原始面板
    QWidget* wrapTeachingPanel(const QString& panelId,
                                const QString& title,
                                QWidget* panel);

    void showDebugButtons(bool show);
    void showVmButtons(bool show);

    // ---- 帮助弹窗 ----
    void showHelpDialog();

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
    void openWorkspace(const QString& dirPath);
    void saveLayout();
    void restoreLayout();
    void ensureEditorVisible();

    // ---- 教学增强面板：将面板内代码加载到主编辑器 ----
    void loadCodeIntoMainEditor(const QString& code);
};
