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
#include <memory>
#include <vector>

#include <QSettings>

// ADS 停靠框架
#include "DockManager.h"
#include "DockWidget.h"

// QFluentKit 主题
#include "FluentGlobal.h"

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

class Pivot;
class QStatusBar;
class QLabel;

// ============================================================
// Ide — MiniLang IDE 主窗口
// ------------------------------------------------------------
// 第六轮重构：活动栏 + Pivot 标签 + Fluent 组件 + AST 独立窗口
// ============================================================

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

    // 活动栏
    ActivityBar* activityBar_ = nullptr;

    // 左侧面板
    ads::CDockWidget* fileTreeDock_ = nullptr;
    ads::CDockWidget* debugPanelDock_ = nullptr;
    QTreeWidget* fileTree_ = nullptr;
    DebugPanel* debugPanel_ = nullptr;

    // 底部面板（单一 dock + Pivot 标签切换）
    ads::CDockWidget* bottomDock_ = nullptr;
    Pivot* bottomPivot_ = nullptr;
    QStackedWidget* bottomStack_ = nullptr;
    QTextEdit* outputTextEdit_ = nullptr;
    QListWidget* errorListWidget_ = nullptr;
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

    // 状态栏
    QLabel* statusLineLabel_ = nullptr;
    QLabel* statusColLabel_ = nullptr;
    QLabel* statusSaveLabel_ = nullptr;
    QLabel* statusRunLabel_ = nullptr;
    QLabel* statusEncodingLabel_ = nullptr;  // 第八轮：文件编码显示

    // ---- 状态 ----
    QString workspaceDir_;
    QString currentFilePath_;
    bool isDirty_ = false;
    bool hasWorkspace_ = false;
    int bottomPanelHeight_ = 220;  // 第八轮：输出面板默认高度，用户调整后记忆

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
    void initToolbar();
    void initConnections();
    void initFileTree();
    void initWelcomePage();
    void initMenuBar();
    void initStatusBar();
    void applyFluentStyle();
    void setupCompletion();
    void updateCompletionWords();
    void updateStatusBar();

    // ---- 输出/错误 ----
    void appendOutput(const QString& text);
    void appendError(const QString& text, int line = 0, int column = 0);
    void clearOutput();

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
    void onRightPivotChanged(const QString& routeKey);
    void onBottomPivotChanged(const QString& routeKey);

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
};
