#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QAction>
#include <QTableWidget>
#include <QTextEdit>
#include <memory>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "formatter/Formatter.h"
#include "debug/DebugController.h"
#include "gui/CodeEditor.h"
#include "gui/SyntaxHighlighter.h"
#include "gui/AstViewer.h"
#include "gui/OutputPanel.h"
#include "gui/DebugPanel.h"
#include "gui/ReplPanel.h"

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

    /// 停止运行
    void onStop();

    /// 清空输出
    void onClearOutput();

    /// 调试暂停在某行
    void onPausedAt(int line);

    /// 格式化代码
    void onFormat();

    /// 显示字节码
    void onShowBytecode();

private:

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
    QTextEdit* bytecodeView_ = nullptr;    // 字节码视图

    QSplitter* mainSplitter_ = nullptr;      // 主水平分割
    QSplitter* vSplitter_ = nullptr;         // 垂直分割

    // ---- 工具栏动作 ----
    QAction* runAction_ = nullptr;
    QAction* debugAction_ = nullptr;
    QAction* stepInAction_ = nullptr;
    QAction* stepOverAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* clearAction_ = nullptr;
    QAction* formatAction_ = nullptr;
    QAction* bytecodeAction_ = nullptr;

    // ---- 状态 ----
    std::unique_ptr<Block> astRoot_;        // AST 根节点
    std::vector<Token> lastTokens_;         // 上次词法分析的 Token 列表
    BytecodeChunk lastBytecode_;            // 上次编译的字节码
    bool isRunning_ = false;               // 是否正在运行

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
};
