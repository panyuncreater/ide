#pragma once

#include <QMainWindow>
#include <QCloseEvent>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QAction>
#include <QTableWidget>
#include <QTextEdit>
#include <QListWidget>
#include <QTimer>
#include <memory>

#include "Diagnostic.h"
#include "IdeController.h"
#include "gui/CodeEditor.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/AstViewer.h"
#include "gui/OutputPanel.h"
#include "gui/DebugPanel.h"
#include "gui/ReplPanel.h"
#include "gui/VmStackPanel.h"
#include "gui/FindReplacePanel.h"

// ============================================================
// Ide — GUI 交互层
// ------------------------------------------------------------
// 从原上帝对象拆分而来，仅负责：
//   - GUI 组件创建与布局
//   - 文件操作（新建/打开/保存）
//   - 用户操作槽函数（委托给 IdeController 处理业务逻辑）
//   - 通过 IdeController 信号更新 UI
//
// 业务逻辑层 → IdeController
// Worker 任务处理层 → InterpreterWorker（已拆分至独立文件）
// ============================================================

class Ide : public QMainWindow {
    Q_OBJECT

public:
    Ide(QWidget* parent = nullptr);
    ~Ide();

private slots:
    /// 运行程序
    void onRun();

    /// 调试运行
    void onDebug();

    /// 单步进入
    void onStepIn();

    /// 单步跳过
    void onStepOver();

    /// 单步跳出
    void onStepOut();

    /// 继续运行（到下一个断点）
    void onResume();

    /// 停止运行
    void onStop();

    /// 清空输出
    void onClearOutput();

    /// GUI-04: 文件操作
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();

    /// 调试暂停在某行
    void onPausedAt(int line);

    /// 格式化代码
    void onFormat();

    /// 显示字节码
    void onShowBytecode();

    /// VM 单步执行字节码（step-in）
    void onVmStep();

    /// A4 fix: VM 单步跨过（step-over，不进入函数调用）
    void onVmStepOver();

    /// A4 fix: VM 单步跨出（step-out，跳出当前函数）
    void onVmStepOut();

    /// A4 fix: VM 全速运行（命中断点时暂停）
    void onVmRun();

    /// VM 停止执行
    void onVmStop();

    // A4 fix: VM 步进共享辅助方法
    /// 处理 vmStepByMode 结果，更新 UI（栈/全局变量/调用栈/高亮）
    void handleVmStepResult(IdeController::VmStepResult result);
    /// 批量启用/禁用 VM 步进按钮
    /// running=true 表示 VM 处于暂停状态（可继续步进），需启用所有步进按钮
    /// running=false 表示 VM 已停止/未初始化
    void setVmStepActionsEnabled(bool enabled, bool running = false);

    /// F6: 显示查找面板 (Ctrl+F)
    void onFind();
    /// F6: 显示替换面板 (Ctrl+H)
    void onReplace();
    /// F6: 查找下一个 (F3)
    void onFindNext();
    /// F6: 查找上一个 (Shift+F3)
    void onFindPrev();

    /// F9: 切换深色/浅色主题
    void onToggleTheme(bool dark);

private:
    /// 窗口关闭事件：确保调试器和VM安全停止
    void closeEvent(QCloseEvent* event) override;

    // ---- 业务逻辑层 ----
    IdeController* controller_ = nullptr;

    // ---- GUI 组件 ----
    CodeEditor* codeEditor_ = nullptr;
    FindReplacePanel* findReplacePanel_ = nullptr;
    SyntaxHighlighter* highlighter_ = nullptr;
    AstViewer* astViewer_ = nullptr;
    OutputPanel* outputPanel_ = nullptr;
    DebugPanel* debugPanel_ = nullptr;
    ReplPanel* replPanel_ = nullptr;

    QTabWidget* rightTabWidget_ = nullptr;   // 右侧 Tab：Token 列表 / AST 视图 / 字节码视图
    QTabWidget* bottomTabWidget_ = nullptr;  // 底部 Tab：输出 / 调试 / REPL

    QTableWidget* tokenTable_ = nullptr;    // Token 列表表格
    QListWidget* bytecodeList_ = nullptr;   // 字节码指令列表（支持行高亮）
    VmStackPanel* vmStackPanel_ = nullptr;  // VM 栈状态面板

    QSplitter* mainSplitter_ = nullptr;      // 主水平分割
    QSplitter* vSplitter_ = nullptr;         // 垂直分割

    // ---- 工具栏动作 ----
    QAction* runAction_ = nullptr;
    QAction* debugAction_ = nullptr;
    QAction* stepInAction_ = nullptr;
    QAction* stepOverAction_ = nullptr;
    QAction* stepOutAction_ = nullptr;
    QAction* resumeAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* clearAction_ = nullptr;
    QAction* formatAction_ = nullptr;
    QAction* bytecodeAction_ = nullptr;

    QAction* vmStepAction_ = nullptr;       // VM 单步（step-in）
    QAction* vmStepOverAction_ = nullptr;   // A4 fix: VM 单步跨过（step-over）
    QAction* vmStepOutAction_ = nullptr;    // A4 fix: VM 单步跨出（step-out）
    QAction* vmRunAction_ = nullptr;        // A4 fix: VM 全速运行（命中断点暂停）
    QAction* vmStopAction_ = nullptr;       // VM 停止

    // F9: 主题切换
    QAction* darkThemeAction_ = nullptr;    // 深色主题（checkable）
    bool isDarkTheme_ = false;              // 当前主题状态

    // GUI-04: 文件操作 actions
    QAction* newAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;

    // GUI-04: 文件状态
    QString currentFilePath_;              // 当前文件路径（空=未保存）
    bool isDirty_ = false;                 // 是否有未保存修改

    // F13: 补全词更新防抖定时器
    QTimer* completionTimer_ = nullptr;    // 文本变化后延迟更新补全词
    QStringList staticCompletionWords_;    // 静态补全词（关键字 + 内置函数）

    /// chunk→行号映射（用于多 chunk 高亮定位）
    struct ChunkRowInfo {
        std::string name;   // chunk 名称（"main" 或函数名）
        int startRow;       // 在 bytecodeList_ 中的起始行
        int rowCount;       // 该 chunk 占用的行数
    };
    std::vector<ChunkRowInfo> chunkRowMap_;

    // D21 fix: 缓存上次编译的源码哈希，若源码未变则跳过字节码列表重建
    size_t lastBytecodeSourceHash_ = 0;

    /// 初始化 UI
    void initUI();

    /// 初始化工具栏
    void initToolbar();

    /// 初始化信号连接（工具栏 + controller 信号）
    void initConnections();

    /// F9: 应用主题（true=深色，false=浅色）
    void applyTheme(bool dark);

    /// F13: 初始化自动补全（关键字 + 内置函数）
    void setupCompletion();

    /// F13: 从当前文档扫描用户定义的符号（var/fun/class 名称）
    void updateCompletionWords();

    /// 更新字节码指令列表高亮
    void highlightBytecodeLine(const std::string& chunkName, size_t ip);

    /// 填充字节码指令列表
    void populateBytecodeList();

    /// 更新 Token 列表表格
    void updateTokenTable();

    /// 更新 AST 视图
    void updateAstViewer();

    /// 更新调试面板
    void updateDebugInfo();

    /// 将诊断信息输出到输出面板，并标记编辑器错误行
    void displayDiagnostics(const DiagnosticBag& bag);

    /// 设置运行状态（启用/禁用按钮）
    void setRunningState(bool running);

    /// Worker 线程结束后的 UI 清理
    void onWorkerFinished(bool wasDebug);

    // GUI-04: 文件操作辅助方法
    bool maybeSave();                        // 未保存提示，返回 true 可以继续
    void updateWindowTitle();                // 更新窗口标题
    void loadFile(const QString& path);      // 加载文件到编辑器
};
