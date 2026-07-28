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
#include "gui/AstVisualizerPanel.h"
#include "gui/BackendComparePanel.h"
#include "gui/BackendParallelPanel.h"
#include "gui/BreakpointConditionPanel.h"
#include "gui/BugHuntPanel.h"
#include "gui/BytecodeTracePanel.h"
#include "gui/CallStackPanel.h"
#include "gui/ClosureInspectorPanel.h"
#include "gui/CodeEditor.h"
#include "gui/CodeJourneyInfoPanel.h"
#include "gui/CoroutineVisualizerPanel.h"
#include "gui/CourseSystemPanel.h"
#include "gui/DebugPanel.h"
#include "gui/EscapeAnalysisPanel.h"
#include "gui/ExceptionFlowPanel.h"
#include "gui/ExecutionTimelinePanel.h" // R114: 可回放执行时间轴
#include "gui/ExerciseGraderPanel.h"
#include "gui/FindReplacePanel.h"
#include "gui/FuzzPlaygroundPanel.h"
#include "gui/GcVisualizerPanel.h"
#include "gui/GlossaryPanel.h"
#include "gui/IRTransformPanel.h"
#include "gui/InlineCachePanel.h"
#include "gui/IrViewer.h"
#include "gui/JitVisualizerPanel.h"
#include "gui/LabManualPanel.h"
#include "gui/LearningPathPanel.h"
#include "gui/LintExplorerPanel.h"
#include "gui/LoopUnrollingPanel.h"
#include "gui/MemoryLayoutPanel.h"
#include "gui/MemoryModelPanel.h"
#include "gui/ModuleSystemVisualizerPanel.h"
#include "gui/PanelCatalog.h" // P2-1 fix: 替代废弃的 LearningHubDialog
#include "gui/PerformanceRacePanel.h"
#include "gui/PipelineViewer.h"
#include "gui/ProfileDashboardPanel.h"
#include "gui/RegisterAllocatorPanel.h"
#include "gui/ReplPanel.h"
#include "gui/StepExplainerPanel.h"
#include "gui/SyntaxExplorerPanel.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/TeachingPanelHeader.h"
#include "gui/TeachingTreePanel.h"
#include "gui/TokenPuzzlePanel.h"
#include "gui/VariableInspectorPanel.h"
#include "gui/VmStackPanel.h"
#include "gui/VmStackSandboxPanel.h"
#include "gui/WatchPanel.h"      // R117: 观察表达式面板
#include "gui/WatchpointPanel.h" // R161: 数据断点（Watchpoint）面板

class Pivot;
class QLabel;
class QLineEdit;
class ComboBox;           // QFluentKit ComboBox
class GuidedTour;         // 新手引导组件
class QFileSystemWatcher; // 文件外部修改监听
class RoundMenu;          // QFluentKit RoundMenu（系统菜单/上下文菜单）

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
    // Legacy: right-panel switching now driven by onRightPivotChanged
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
    /// closeEvent 阶段 1：停止所有动画与定时器（splitterAnim_/bottomPanelAnim_ +
    /// 子 widget 的 QPropertyAnimation/QTimer + ROUND-89 Ide 直接子对象扫描）
    void stopCloseAnimationsAndTimers();
    /// closeEvent 阶段 2：清理所有活跃的面板特定 GuidedTour（disconnect + delete）
    void cleanupActivePanelTours();
    /// closeEvent 阶段 3：尽早切断 controller_ → Ide 信号槽并清空待处理事件
    void disconnectControllerEarly();
    /// closeEvent 阶段 4：停止运行中的 Worker/VM/REPL（stopForClose + vmStop + replPanel stop+wait）
    /// 返回 true 表示可继续关闭；返回 false 表示用户取消（已调用 event->ignore()，需 return）
    bool stopRunningWorkersForClose(QCloseEvent* event);
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
    QSplitter* centerSplitter_ = nullptr;          // 三栏布局：[centerStack_ | editorSplitter_]
    QSplitter* editorSplitter_ = nullptr;          // 编辑器区纵向分割：[editorTabWidget_ | bottomContainer_]
    QVariantAnimation* splitterAnim_ = nullptr;    // centerSplitter_ 尺寸动画
    QVariantAnimation* bottomPanelAnim_ = nullptr; // editorSplitter_ 底部面板展开/收起动画
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
    // AST 可视化编辑器（编译前端教学面板：源码 → AST 树形可视化 + 节点参考）
    AstVisualizerPanel* astVisualizerPanel_ = nullptr;
    // 第三波 P2-2：内置实验手册
    LabManualPanel* labManualPanel_ = nullptr;

    // ---- 教学增强面板（第二波）----
    // P0-2：内存模型可视化（NaN-boxing / RefCounted / COW / GC）
    MemoryModelPanel* memoryModelPanel_ = nullptr;
    // P1-1：IR 变换过程动画（AST → IR lowering + 优化 pass 前后对比）
    IRTransformPanel* irTransformPanel_ = nullptr;
    // 教学板块拓展：JIT 编译可视化（类型反馈/特化/热点/OpCode 覆盖）
    JitVisualizerPanel* jitVisualizerPanel_ = nullptr;
    // 教学板块拓展：执行步骤讲解生成器（逐指令自然语言讲解）
    StepExplainerPanel* stepExplainerPanel_ = nullptr;
    // 教学板块拓展：三后端并行可视化（Interpreter/StackVM/RegisterVM 同源执行对比）
    BackendParallelPanel* backendParallelPanel_ = nullptr;
    // 教学板块拓展：内联缓存可视化（IC 三态原理 + 交互式状态模拟器）
    InlineCachePanel* inlineCachePanel_ = nullptr;
    // 教学板块拓展：循环展开可视化（JIT 经典优化原理 + 交互式展开模拟器）
    LoopUnrollingPanel* loopUnrollingPanel_ = nullptr;
    // 教学板块拓展：逃逸分析可视化（逃逸三态原理 + 交互式逃逸分析模拟器）
    EscapeAnalysisPanel* escapeAnalysisPanel_ = nullptr;
    // 教学板块拓展：寄存器分配可视化（分配算法原理 + 线性扫描模拟器）
    RegisterAllocatorPanel* registerAllocatorPanel_ = nullptr;
    // 教学板块拓展：三后端性能竞赛（多次运行取平均 + 柱状图 + 性能分析）
    PerformanceRacePanel* performanceRacePanel_ = nullptr;
    // 教学板块拓展：内存布局可视化（NaN-boxing 位模式 + COW + upvalue 原理 + 交互式模拟器）
    MemoryLayoutPanel* memoryLayoutPanel_ = nullptr;
    // 教学板块拓展：教学课程系统（课程库 + JSON 课程编辑器，3 预设课程）
    CourseSystemPanel* courseSystemPanel_ = nullptr;
    // 教学板块拓展：交互式练习评分（题目 + 测试用例 + 评分报告，复用三后端执行模式）
    ExerciseGraderPanel* exerciseGraderPanel_ = nullptr;
    // 教学面板拓展（第七波）：协程/生成器可视化（yield/next 重放机制 + 四后端对比）
    CoroutineVisualizerPanel* coroutineVisualizerPanel_ = nullptr;
    // 教学面板拓展（第七波）：GC 垃圾回收可视化（mark-sweep-finalize 三阶段 + 交互式模拟器）
    GcVisualizerPanel* gcVisualizerPanel_ = nullptr;
    // 教学面板拓展（第七波）：静态分析探索器（8 条 lint 规则 + 交互式诊断）
    LintExplorerPanel* lintExplorerPanel_ = nullptr;
    // 教学面板拓展（第七波）：模糊测试游乐场（三后端差分 + 生成/变异模式）
    FuzzPlaygroundPanel* fuzzPlaygroundPanel_ = nullptr;
    // 教学面板拓展（第七波）：模块系统可视化（import/export + 路径解析 + 循环依赖检测）
    ModuleSystemVisualizerPanel* moduleSystemVisualizerPanel_ = nullptr;
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
    // R117：观察表达式面板（任意表达式求值 + 持久化 + 自动刷新）
    WatchPanel* watchPanel_ = nullptr;
    // R161：数据断点面板（Watchpoint，监视变量/字段被修改时暂停）
    WatchpointPanel* watchpointPanel_ = nullptr;
    // R114：可回放执行时间轴（三后端统一执行轨迹录制 + QSlider 随机访问回放）
    ExecutionTimelinePanel* executionTimelinePanel_ = nullptr;
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
    // 拓展二期·教学：课堂演示模式状态（大字号+隐藏侧边栏+全屏，退出时恢复）
    bool presentationMode_ = false;        // 是否处于演示模式
    int prePresentationFontSize_ = 11;     // 进入演示前的字号（退出时恢复）
    bool prePresentationMaximized_ = true; // 进入演示前是否最大化（退出时恢复）
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
    // PERF: 启动性能优化 - 合并重复的 applyFluentStyle 调用
    QTimer* applyStyleTimer_ = nullptr; // 样式应用防抖（多次请求合并为一次）
    bool applyingStyle_ = false;        // 防止 applyFluentStyle 重入
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
    /// initUI 子模块 1：创建内容控件（fileTree/fileTreeFilterEdit/debugPanel/outputTextEdit_/
    /// errorListWidget/errFilterXxxBtn/replPanel/tokenTable/irViewer/bytecodeList/vmStackPanel/
    /// astViewer）+ 调用 initWelcomePage()
    void createContentWidgets();
    /// initUI 子模块 2：构建 editorTabWidget_ + centerStack_ + centerSplitter_ +
    /// bottomContainer_（bottomPivot_ + bottomStack_ + 关闭按钮）+ editorSplitter_ 三栏布局
    void setupCentralSplitter();
    /// initUI 子模块 3：构建 activityBar_（资源管理器/调试/学习 3 项 + currentChangedById 连接）
    void setupActivityBar();
    /// initUI 子模块 4：构建 rightContainer（rightPivot_ + rightStack_ + bytecode 页面），
    /// 返回 rightContainer 给 setupDockWidgets 接管
    QWidget* setupRightPanel();
    /// initUI 子模块 5：构建 astWindow_ 独立顶层窗口（含 astViewer_ reparent）
    void setupAstWindow();
    /// initUI 子模块 6：构建 mainContainer + middleArea（activityBar_ + dockManager_）+
    /// 设置 ADS 配置标志 + setCentralWidget
    void setupMainContainerAndDockManager();
    /// initUI 子模块 7：创建 4 个 ADS dock（centralDock/fileTreeDock_/debugPanelDock_/
    /// rightDock_）；rightContainer 由 setupRightPanel 返回值传入
    void setupDockWidgets(QWidget* rightContainer);
    /// initUI 子模块 8：注册 19 个教学面板的懒加载工厂到 teachingPanelFactories_
    void registerLazyTeachingPanels();
    // ---- R132 fix: registerLazyTeachingPanels 239 行拆为 thin orchestrator + 4 helper（每个 < 80 行）----
    /// 第一波+第三波早期教学面板：编译管线 / 三后端对比 / Bug 狩猎 / 语法探索器 / 实验手册（5 个面板）
    /// 调用 registerLazyPanel 注册面板工厂到 teachingPanelFactories_，部分面板 connect
    /// loadSampleRequested/runSampleRequested/jumpToPanelRequested/exerciseCompleted 等信号
    void registerCompilationPipelinePanels(
        const std::function<void(const QString&, const QString&, std::function<QWidget*()>)>& registrar,
        const std::function<void(int)>& sourceLineHighlighter);
    /// 第二波教学面板：内存模型 / IR 变换 / 性能剖析（3 个面板）
    /// ir-transform 面板 connect sourceLineRequested 信号到 sourceLineHighlighter
    void registerMemoryIrProfilePanels(
        const std::function<void(const QString&, const QString&, std::function<QWidget*()>)>& registrar,
        const std::function<void(int)>& sourceLineHighlighter);
    /// 第三波调试检查器面板：调用栈 / 变量检查器 / 字节码轨迹 / 条件断点 / 观察表达式 /
    /// 可回放执行时间轴 / 异常流 / 闭包检查器（8 个面板）
    /// bytecode-trace 面板 connect sourceLineRequested 信号到 sourceLineHighlighter；
    /// execution-timeline 面板处理 engineCombo_ 后端类型同步 + recordingToggled 信号
    void registerDebugInspectorPanels(
        const std::function<void(const QString&, const QString&, std::function<QWidget*()>)>& registrar,
        const std::function<void(int)>& sourceLineHighlighter);
    /// 第四档教学面板：学习路径地图 / Token 拼图 / AST 构建器 / VM 栈沙盒 /
    /// 代码生命旅程 / 术语表（6 个面板）
    /// 多数面板 connect activityCompleted/journeyCompleted/termActivated 信号并联动
    /// learningPathPanel_->markActivityCompleted 标记活动完成
    void registerLearningPathPanels(
        const std::function<void(const QString&, const QString&, std::function<QWidget*()>)>& registrar);
    /// initUI 子模块 9：构建 teachingTreePanel_/teachingTreeDock_ + 面板尺寸约束 +
    /// 启动时隐藏所有 dock + 连接 dock viewToggled/focusedDockWidgetChanged 防抖保存
    void setupTeachingTreeAndFinalize();
    void initConnections();
    /// initConnections 子模块 1：连接 stepIn/stepOver/stepOut/resume/stop/clear/format/
    /// compileAnalysis/ast + vmStep/vmStepOver/vmStepOut/vmRun/vmStop 等工具栏动作
    void connectToolbarActions();
    /// initConnections 子模块 2：连接 engineCombo_ 执行引擎切换 + editorTabWidget_
    /// currentChanged/tabCloseRequested
    void connectEngineComboAndEditorTabs();
    /// initConnections 子模块 3：注册全局 QShortcut（Ctrl+F 查找 / Ctrl+H 替换 /
    /// F3 查找下一个 / Shift+F3 查找上一个 / Esc 关闭查找面板）
    void connectGlobalShortcuts();
    /// initConnections 子模块 4：连接 fileTree_ itemActivated + fileTreeFilterEdit_
    /// 200ms 防抖过滤 + errFilter{Error,Warning,Info,Hint}Btn_ 类型过滤按钮
    void connectFileTreeAndErrorFilters();
    /// initConnections 子模块 5：连接 controller_ 的 outputReady/runOk/stoppedByUser/
    /// runtimeError/genericError/pausedAt/workerFinished/vmRunPaused/diagnosticsReady 信号
    void connectControllerSignals();
    void initFileTree();
    void initWelcomePage();
    /// initWelcomePage 子模块 1：创建 welcomeRecentPanel（220px 宽，最近工作区 QListWidget）
    /// 并加入 outerLayout 左侧
    void setupWelcomeRecentPanel(QHBoxLayout* outerLayout);
    /// initWelcomePage 子模块 2：创建 centerArea 头部（Logo + TitleLabel + CaptionLabel）
    /// 并加入 centerLayout 顶部，含 addStretch(3) 与 addSpacing(24) 收尾
    void setupWelcomeCenterHeader(QVBoxLayout* centerLayout);
    /// initWelcomePage 子模块 3：创建 btnContainer + primaryBtn（打开文件夹）+
    /// secondaryBtn（新建文件）+ tourBtn（3 分钟 Hello World）并加入 centerLayout
    void setupWelcomeActionButtons(QVBoxLayout* centerLayout);
    /// initWelcomePage 子模块 4：创建 shortcutLayout（语法示例 + 帮助文档链接）+
    /// addStretch(4) 收尾并加入 centerLayout 底部
    void setupWelcomeShortcutLinks(QVBoxLayout* centerLayout);
    void initStatusBar();
    void initTitleBar(); // 十二轮：统一标题栏（融合菜单+工具栏+窗口控制，36px）
    /// initTitleBar 子模块 1：左侧应用图标 + 名称 + 路径标签
    void setupTitleBarLeftSide(QHBoxLayout* layout);
    /// initTitleBar 子模块 2-7：构建系统菜单的 5 个子菜单（File/Edit/View/Run/Help）+
    /// 教学面板快捷键注册（registerTeachingPanelShortcuts）
    RoundMenu* buildFileMenu(QWidget* parent);
    RoundMenu* buildEditMenu(QWidget* parent);
    RoundMenu* buildViewMenu(QWidget* parent);
    void registerTeachingPanelShortcuts();
    RoundMenu* buildRunMenu(QWidget* parent);
    RoundMenu* buildHelpMenu(QWidget* parent);
    /// R117: 构建语言切换子菜单（扫描 translations/ 目录中的 .qm 文件）
    RoundMenu* buildLanguageMenu(QWidget* parent);
    /// R117: 切换 UI 语言并提示用户重启以完整生效
    void switchLanguageWithPrompt(const QString& locale);
    /// initTitleBar 子模块 8：中间工具栏（分隔线 + Run/Debug split + 格式化 + AST 按钮）
    void setupTitleBarMiddleToolbar(QHBoxLayout* layout);
    /// initTitleBar 子模块 9：调试按钮组 + VM 按钮组（动态显隐）
    void setupTitleBarDebugAndVmButtons(QHBoxLayout* layout);
    /// initTitleBar 子模块 10：右侧区域（清空动作 + 弹性 + engineCombo_ + 分隔线 +
    /// min/max/close 窗口控制按钮 + eventFilter 安装）
    void setupTitleBarRightSide(QHBoxLayout* layout);

    /// applyFluentStyle 主题色板包：13 个 QString 颜色 + dark bool
    /// 提取自 TeachingTheme::ide*()，避免每个 helper 重复查询
    struct FluentPalette {
        bool dark = false;
        QString bgMain;
        QString bgPanel;
        QString bgSidebar;
        QString fgPrimary;
        QString fgSecondary;
        QString borderColor;
        QString accentColor;
        QString hoverBg;
        QString selectedBg;
        QString statusBg;
        QString editorBg;
        QString lineNumBg;
        QString lineNumFg;
    };

    void applyFluentStyle();
    /// applyFluentStyle 子模块 1：QFluentKit 注册 editorTabWidget_ + 替换 4 个 widget
    /// 的 ScrollBar 为 Fluent::ScrollBar（PERF: isFluentScrollBar 去重避免重复替换）
    void registerFluentWidgetsAndScrollBars();
    /// applyFluentStyle 子模块 2：构建 ADS QSS 字符串（覆盖 CDockAreaWidget/Tab/TitleBar/
    /// Splitter/FloatingDockContainer/DockManager/DockWidget/AutoHideTab/QToolTip）
    QString buildAdsQss(const FluentPalette& p);
    /// applyFluentStyle 子模块 3：应用 dockManager_ QSS + palette（R68/R72/R73 fix：
    /// ColorSchemeMode 锁 Light + 直接 setPalette + tab 子控件 palette）
    void applyAdsDockStyle(const QString& adsQss);
    /// applyFluentStyle 子模块 4：标题栏样式（渐变 bg + titleText/titlePath/titleMin/
    /// titleMax/titleCloseBtn hover/themeToggleBtn）
    void applyTitleBarStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 5：状态栏样式（statusBg bg + borderColor 顶部分隔线 +
    /// fgSecondary 文字色 + 11px 字号）
    void applyStatusBarStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 6：构建面板容器 QSS（#bottomPanelContainer/#rightPanelContainer/
    /// #bottomPivotRow/#rightPivotRow/#panelCloseBtn/#teachingPanelCard），返回 panelQss
    QString buildPanelContainerQss(const FluentPalette& p);
    /// applyFluentStyle 子模块 7：编辑器标签栏样式（QTabWidget::pane/QTabBar::tab:
    /// selected/hover/close-button）
    void applyEditorTabStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 8：Tree/Table/List 通用样式（itemViewQss），应用到
    /// fileTree_/errorListWidget_/bytecodeList_/tokenTable_/recentListWidget_
    void applyItemViewStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 9：活动栏样式（bgSidebar bg + borderColor 右侧分隔线）
    void applyActivityBarStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 10：全局兜底 palette（R60-1/R65-2/R68/R71/R74/R76 fix:
    /// QMainWindow 级别 palette + autoFillBackground + 全局 QToolTip 样式只设置一次）
    void applyGlobalPaletteFallback();
    /// applyFluentStyle 子模块 11：主容器/中间区域/中央栈/splitter 背景色（PERF:
    /// applyBgStyle lambda + 单次 findChildren 替代 3 次全树遍历）
    void applyContainerBackgrounds(const FluentPalette& p);
    /// applyFluentStyle 子模块 12：VM Stack Panel 主题化样式（vmOpBg/vmOpFg/vmOpBorder/
    /// vmItemBorder/vmSelectedBg/vmSelectedFg/vmHeaderBg + QLabel/QListWidget/QTableWidget）
    void applyVmStackPanelStyle(const FluentPalette& p);
    /// applyFluentStyle 子模块 13：QGroupBox 统一 Fluent 外观 + panelQss 合并到主窗口
    /// 样式表（PERF: 利用 Qt 样式表继承机制自动应用 #id 选择器到子控件）
    void applyMainQssWithGroupBox(const QString& panelQss);
    /// applyFluentStyle 子模块 14：同步编辑器暗色主题（强制 light 配色，遍历 editorTabs_）
    void syncEditorTheme();
    void scheduleApplyStyle(); // PERF: 延迟合并多次样式请求，避免启动时重复调用
    void setupCompletion();
    void updateCompletionWords();
    void updateStatusBar();

    /// 第九轮：同步视图菜单勾选状态与 dock 实际显隐
    void syncViewMenuChecks();

    // ---- 输出/错误 ----
    void appendOutput(const QString& text, OutputLevel level = OutputLevel::Plain);
    void appendError(const QString& text, int line = 0, int column = 0, DiagLevel level = DiagLevel::Error,
                     const std::string& diagCode = std::string());
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
    /// R165 体验优化：Alt+P 快速跳转面板（命令面板）
    /// 弹出含搜索框的面板列表对话框，选中后调用 showTeachingPanel
    void showQuickPanelJumpDialog();
    /// showTeachingPanel 阶段 1：懒加载 + 取消 pendingHideCenterTimer_/splitterAnim_ 挂起意图
    /// + panelToStackIndex_ 查找。返回 centerStack_ 索引；返回 -1 表示 panelId 不在映射中
    /// （已调用 showEditorArea 回退，调用方需直接 return）。
    int prepareTeachingPanelTransition(const QString& panelId);
    /// showTeachingPanel 阶段 2：BUG-R14-2 dock 可见状态记录 + kKeepBottomDockPanels/
    /// kKeepRightDockPanels 白名单显隐 + centerStack_ setCurrentIndex/show + pipeline/
    /// learning-path 刷新 + PanelAnimator::slideInWidget + editorTabWidget_ 显隐调整
    void applyTeachingPanelDockAndPageSwitch(const QString& panelId, int idx);
    /// showTeachingPanel 阶段 3：ROUND-60/ROUND-76 splitter 重分配动画 + teachingTreePanel_
    /// 同步高亮 + teachingTreeDock_ 兜底显示 + kBrowsePanelToActivity 浏览活动标记 +
    /// kAutoTourPanels 首次自动 GuidedTour + syncViewMenuChecks
    void finalizeTeachingPanelShow(const QString& panelId, const QList<int>& savedSplitterSizes, bool editorHasTabs,
                                   bool editorWasVisible);
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
    /// 填充帮助对话框的快捷键分组网格（4 组 × 8 项，两列布局）
    void populateHelpDialogShortcuts(QGridLayout* grid, QDialog* dlg);

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
    /// CodeEditor 右键菜单 contextActionRequested 信号路由
    /// 提取自 createNewEditorTab：处理 toggleComment/format/gotoLine/find/replace/
    /// toggleBreakpoint/editBreakpointCondition/runToCursor 等右键菜单动作
    void handleEditorContextAction(const QString& action);
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
