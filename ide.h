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
#include <memory>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "formatter/Formatter.h"
#include "debug/DebugController.h"
#include "Diagnostic.h"
#include "gui/CodeEditor.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/AstViewer.h"
#include "gui/OutputPanel.h"
#include "gui/DebugPanel.h"
#include "gui/ReplPanel.h"
#include "gui/VmStackPanel.h"

// ============================================================
// InterpreterWorker — 解释器线程工作对象（OP-1 fix）
// ============================================================

class InterpreterWorker : public QObject {
    Q_OBJECT
public:
    InterpreterWorker(Interpreter& interp, Block& ast, DebugController* dbg)
        : interp_(interp), ast_(ast), debugger_(dbg) {}

public slots:
    void run();

signals:
    void outputReady(const QString& text);
    void finishedOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);

private:
    Interpreter& interp_;
    Block& ast_;
    DebugController* debugger_;
};

// ============================================================
// Ide 主窗口
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

    /// 运行线程结束后的清理回调
    void onRunFinished();

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

    /// VM 单步执行字节码
    void onVmStep();

    /// VM 停止执行
    void onVmStop();

private:
    /// 窗口关闭事件：确保调试器和VM安全停止
    void closeEvent(QCloseEvent* event) override;

    // ---- 核心组件 ----
    Lexer lexer_;
    Parser parser_;
    Interpreter interpreter_;
    Compiler compiler_;
    VM vm_;
    Formatter formatter_;
    DebugController* debugger_ = nullptr;

    // ---- GUI 组件 ----
    CodeEditor* codeEditor_ = nullptr;
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

    QAction* vmStepAction_ = nullptr;       // VM 单步
    QAction* vmStopAction_ = nullptr;       // VM 停止

    // GUI-04: 文件操作 actions
    QAction* newAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;

    // ---- 状态 ----
    std::unique_ptr<Block> astRoot_;        // AST 根节点
    std::vector<Token> lastTokens_;         // 上次词法分析的 Token 列表
    CompileResult lastCompileResult_;     // 上次编译的结果
    bool isRunning_ = false;               // 是否正在运行
    bool isDebugRun_ = false;              // GUI-01 fix: 是否调试运行（控制单步按钮）
    QThread* workerThread_ = nullptr;      // OP-1: 解释器运行线程
    InterpreterWorker* worker_ = nullptr;  // OP-1: 解释器工作对象
    bool isVmRunning_ = false;             // VM 是否正在运行
    bool isVmInitialized_ = false;         // VM 执行环境是否已初始化（单步模式）
    DiagnosticBag diagnostics_;             // 统一诊断收集器
    // GUI-04: 文件状态
    QString currentFilePath_;              // 当前文件路径（空=未保存）
    bool isDirty_ = false;                 // 是否有未保存修改

    /// VM 步进回调处理
    void onVmStepCallback(const VMStepInfo& info);

    /// 更新字节码指令列表高亮
    void highlightBytecodeLine(const std::string& chunkName, size_t ip);

    /// 填充字节码指令列表
    void populateBytecodeList();

    /// chunk→行号映射（用于多 chunk 高亮定位）
    struct ChunkRowInfo {
        std::string name;   // chunk 名称（"main" 或函数名）
        int startRow;       // 在 bytecodeList_ 中的起始行
        int rowCount;       // 该 chunk 占用的行数
    };
    std::vector<ChunkRowInfo> chunkRowMap_;

    /// 初始化 UI
    void initUI();

    /// 初始化工具栏
    void initToolbar();

    /// 初始化信号连接
    void initConnections();

    /// 执行词法分析并更新 Token 列表
    void runLexer(const std::string& source);

    /// 执行语法分析并更新 AST 视图
    void runParser(const std::vector<Token>& tokens);

    /// 执行字节码编译并更新字节码视图
    void runCompiler();

    /// 更新调试面板
    void updateDebugInfo();

    /// 设置运行状态（启用/禁用按钮）
    void setRunningState(bool running);

    /// 将诊断信息输出到输出面板，并标记编辑器错误行
    void displayDiagnostics(const DiagnosticBag& bag);

    // GUI-04: 文件操作辅助方法
    bool maybeSave();                        // 未保存提示，返回 true 可以继续
    void updateWindowTitle();                // 更新窗口标题
    void loadFile(const QString& path);      // 加载文件到编辑器
};
