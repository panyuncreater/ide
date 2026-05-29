#include "ide.h"
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QHeaderView>
#include <sstream>

// ============================================================
// Ide 主窗口实现
// ============================================================

Ide::Ide(QWidget* parent)
    : QMainWindow(parent) {

    debugger_ = new DebugController(this);
    interpreter_.setDebugger(debugger_);
    interpreter_.setOutputCallback([this](const std::string& text) {
        // 解释器在主线程运行，直接调用即可
        outputPanel_->appendOutput(QString::fromStdString(text));
    });

    initUI();
    initToolbar();
    initConnections();

    // 设置默认示例代码
    codeEditor_->setPlainText(
        "// MiniLang 示例程序\n"
        "var x = 10;\n"
        "var y = 20;\n"
        "var sum = x + y;\n"
        "print(sum);\n"
        "\n"
        "fun factorial(n) {\n"
        "    if (n <= 1) {\n"
        "        return 1;\n"
        "    }\n"
        "    return n * factorial(n - 1);\n"
        "}\n"
        "\n"
        "print(factorial(5));\n"
        "\n"
        "for (var i = 0; i < 5; i = i + 1) {\n"
        "    print(i);\n"
        "}\n"
    );
}

Ide::~Ide() {
}

void Ide::initUI() {
    // 中央部件
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // ---- 垂直分割器（上：编辑区 + 下：输出区）----
    vSplitter_ = new QSplitter(Qt::Vertical, this);

    // ---- 水平分割器（左：代码编辑 + 右：视图）----
    mainSplitter_ = new QSplitter(Qt::Horizontal, this);

    // 左侧：代码编辑器
    codeEditor_ = new CodeEditor(this);
    codeEditor_->setMinimumWidth(300);
    highlighter_ = new SyntaxHighlighter(codeEditor_->document());

    // 右侧：Tab Widget（Token 列表 / AST 视图 / 字节码视图）
    rightTabWidget_ = new QTabWidget(this);

    // Token 列表表格
    tokenTable_ = new QTableWidget(this);
    tokenTable_->setColumnCount(5);
    tokenTable_->setHorizontalHeaderLabels({"类型", "词素", "字面量", "行", "列"});
    tokenTable_->horizontalHeader()->setStretchLastSection(true);
    tokenTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tokenTable_->setAlternatingRowColors(true);
    tokenTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rightTabWidget_->addTab(tokenTable_, "Token 列表");

    // AST 视图
    astViewer_ = new AstViewer(this);
    rightTabWidget_->addTab(astViewer_, "AST 视图");

    // 字节码视图
    bytecodeView_ = new QTextEdit(this);
    bytecodeView_->setReadOnly(true);
    bytecodeView_->setFont(QFont("Consolas", 10));
    bytecodeView_->setPlaceholderText("点击工具栏「字节码」按钮查看编译结果");
    rightTabWidget_->addTab(bytecodeView_, "字节码视图");

    mainSplitter_->addWidget(codeEditor_);
    mainSplitter_->addWidget(rightTabWidget_);
    mainSplitter_->setStretchFactor(0, 3);  // 60% : 40%
    mainSplitter_->setStretchFactor(1, 2);

    // 底部：Tab Widget（输出 / 调试 / REPL）
    bottomTabWidget_ = new QTabWidget(this);

    outputPanel_ = new OutputPanel(this);
    bottomTabWidget_->addTab(outputPanel_, "输出");

    debugPanel_ = new DebugPanel(this);
    bottomTabWidget_->addTab(debugPanel_, "调试");

    replPanel_ = new ReplPanel(this);
    replPanel_->setInterpreter(&interpreter_);
    bottomTabWidget_->addTab(replPanel_, "REPL");

    vSplitter_->addWidget(mainSplitter_);
    vSplitter_->addWidget(bottomTabWidget_);
    vSplitter_->setStretchFactor(0, 3);
    vSplitter_->setStretchFactor(1, 1);

    mainLayout->addWidget(vSplitter_);

    // 窗口属性
    setWindowTitle("MiniLang IDE - 迷你编程语言解释器与执行可视化平台");
    resize(1200, 800);
}

void Ide::initToolbar() {
    auto* toolbar = addToolBar("工具栏");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));

    // 运行
    runAction_ = toolbar->addAction("▶ 运行");
    runAction_->setToolTip("运行程序 (F5)");
    runAction_->setShortcut(Qt::Key_F5);

    // 调试
    debugAction_ = toolbar->addAction("🐛 调试");
    debugAction_->setToolTip("调试运行 (F6)");
    debugAction_->setShortcut(Qt::Key_F6);

    toolbar->addSeparator();

    // Step In
    stepInAction_ = toolbar->addAction("⬇ Step In");
    stepInAction_->setToolTip("单步进入 (F11)");
    stepInAction_->setShortcut(Qt::Key_F11);
    stepInAction_->setEnabled(false);

    // Step Over
    stepOverAction_ = toolbar->addAction("⬆ Step Over");
    stepOverAction_->setToolTip("单步跳过 (F10)");
    stepOverAction_->setShortcut(Qt::Key_F10);
    stepOverAction_->setEnabled(false);

    toolbar->addSeparator();

    // 停止
    stopAction_ = toolbar->addAction("⏹ 停止");
    stopAction_->setToolTip("停止运行 (Shift+F5)");
    stopAction_->setShortcut(Qt::SHIFT | Qt::Key_F5);
    stopAction_->setEnabled(false);

    // 清空输出
    clearAction_ = toolbar->addAction("🗑 清空输出");
    clearAction_->setToolTip("清空输出面板");

    toolbar->addSeparator();

    // 格式化
    formatAction_ = toolbar->addAction("📝 格式化");
    formatAction_->setToolTip("格式化代码 (Ctrl+Shift+F)");
    formatAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_F);

    // 字节码
    bytecodeAction_ = toolbar->addAction("🔧 字节码");
    bytecodeAction_->setToolTip("查看字节码 (Ctrl+B)");
    bytecodeAction_->setShortcut(Qt::CTRL | Qt::Key_B);
}

void Ide::initConnections() {
    connect(runAction_, &QAction::triggered, this, &Ide::onRun);
    connect(debugAction_, &QAction::triggered, this, &Ide::onDebug);
    connect(stepInAction_, &QAction::triggered, this, &Ide::onStepIn);
    connect(stepOverAction_, &QAction::triggered, this, &Ide::onStepOver);
    connect(stopAction_, &QAction::triggered, this, &Ide::onStop);
    connect(clearAction_, &QAction::triggered, this, &Ide::onClearOutput);
    connect(formatAction_, &QAction::triggered, this, &Ide::onFormat);
    connect(bytecodeAction_, &QAction::triggered, this, &Ide::onShowBytecode);

    connect(debugger_, &DebugController::pausedAt, this, &Ide::onPausedAt);
}

void Ide::onRun() {
    if (isRunning_) return;

    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出
    outputPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        runLexer(source);
    } catch (...) {
        return;
    }

    // 语法分析
    try {
        runParser(lastTokens_);
    } catch (const ParseError& e) {
        outputPanel_->appendError(QString("语法错误 (行 %1, 列 %2): %3")
                                      .arg(e.line).arg(e.column).arg(e.what()));
        QSet<int> errorLines;
        errorLines.insert(e.line);
        codeEditor_->setErrorLines(errorLines);
        return;
    }

    if (!astRoot_) return;

    // 执行
    isRunning_ = true;
    setRunningState(true);
    debugger_->reset();

    // 非调试模式：完全跳过 checkBreak
    interpreter_.setDebugMode(false);

    try {
        interpreter_.execute(*astRoot_);
        outputPanel_->appendOutput("--- 程序执行结束 ---");
    } catch (const RuntimeError& e) {
        outputPanel_->appendError(QString("运行时错误 (行 %1, 列 %2): %3")
                                      .arg(e.line).arg(e.column).arg(e.what()));
        QSet<int> errorLines;
        errorLines.insert(e.line);
        codeEditor_->setErrorLines(errorLines);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()) != "调试终止") {
            outputPanel_->appendError(QString("错误: %1").arg(e.what()));
        }
    }

    isRunning_ = false;
    setRunningState(false);
    interpreter_.setDebugMode(false);
}

void Ide::onDebug() {
    if (isRunning_) return;

    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出
    outputPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        runLexer(source);
    } catch (...) {
        return;
    }

    // 语法分析
    try {
        runParser(lastTokens_);
    } catch (const ParseError& e) {
        outputPanel_->appendError(QString("语法错误 (行 %1, 列 %2): %3")
                                      .arg(e.line).arg(e.column).arg(e.what()));
        return;
    }

    if (!astRoot_) return;

    // 设置断点
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());

    // 设置调试变量回调
    debugger_->setVariableCallback([this]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> result;
        Environment* env = interpreter_.currentEnvironment();
        if (env) {
            auto vars = env->allVariables();
            for (const auto& kv : vars) {
                VariableSnapshot snap;
                snap.name = kv.first;
                snap.value = kv.second;
                snap.scope = "global";
                result.push_back(snap);
            }
        }
        return result;
    });

    debugger_->setCallStackCallback([this]() -> std::vector<CallStackEntry> {
        std::vector<CallStackEntry> result;
        const auto& stack = interpreter_.getCallStack();
        for (const auto& frame : stack) {
            CallStackEntry entry;
            entry.functionName = frame.functionName;
            entry.line = frame.line;
            entry.depth = frame.depth;
            result.push_back(entry);
        }
        return result;
    });

    isRunning_ = true;
    setRunningState(true);

    // 调试模式：启用 checkBreak
    interpreter_.setDebugMode(true);

    // 重置调试状态，然后设置初始模式为 STEP_IN
    debugger_->reset();
    debugger_->stepIn();

    try {
        interpreter_.execute(*astRoot_);
        outputPanel_->appendOutput("--- 程序执行结束 ---");
    } catch (const RuntimeError& e) {
        outputPanel_->appendError(QString("运行时错误 (行 %1, 列 %2): %3")
                                      .arg(e.line).arg(e.column).arg(e.what()));
    } catch (const std::runtime_error& e) {
        // 调试终止（用户点击停止）— 不显示错误
        if (std::string(e.what()) != "调试终止") {
            outputPanel_->appendError(QString("错误: %1").arg(e.what()));
        } else {
            outputPanel_->appendOutput("--- 调试终止 ---");
        }
    }

    isRunning_ = false;
    setRunningState(false);
    interpreter_.setDebugMode(false);
    codeEditor_->clearCurrentLine();
    debugger_->reset();
}

void Ide::onStepIn() {
    debugger_->stepIn();
    updateDebugInfo();
}

void Ide::onStepOver() {
    debugger_->stepOver();
    updateDebugInfo();
}

void Ide::onStop() {
    debugger_->stop();
    // 不在这里设置 isRunning_ 和按钮状态
    // onDebug() 中 interpreter_.execute() 返回后会统一清理
}

void Ide::onClearOutput() {
    outputPanel_->clearAll();
}

void Ide::onPausedAt(int line) {
    codeEditor_->setCurrentLine(line);
    updateDebugInfo();
}

void Ide::onFormat() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 词法分析
    try {
        lastTokens_ = lexer_.scan(source);
    } catch (...) {
        return;
    }

    // 语法分析
    try {
        astRoot_ = parser_.parse(lastTokens_);
    } catch (const ParseError& e) {
        outputPanel_->appendError(QString("格式化失败 - 语法错误 (行 %1, 列 %2): %3")
                                      .arg(e.line).arg(e.column).arg(e.what()));
        return;
    }

    if (!astRoot_) return;

    // 格式化
    std::string formatted = formatter_.format(*astRoot_);
    codeEditor_->setPlainText(QString::fromStdString(formatted));
}

void Ide::onShowBytecode() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 词法分析
    try {
        lastTokens_ = lexer_.scan(source);
    } catch (...) {
        return;
    }

    // 语法分析
    try {
        astRoot_ = parser_.parse(lastTokens_);
    } catch (const ParseError& e) {
        bytecodeView_->setPlainText(QString("编译失败 - 语法错误 (行 %1, 列 %2): %3")
                                         .arg(e.line).arg(e.column).arg(e.what()));
        return;
    }

    if (!astRoot_) return;

    // 编译
    runCompiler();

    // 显示字节码
    std::string disasm = lastBytecode_.disassemble();
    bytecodeView_->setPlainText(QString::fromStdString(disasm));

    // 切换到字节码 Tab
    rightTabWidget_->setCurrentWidget(bytecodeView_);
}

void Ide::runLexer(const std::string& source) {
    lastTokens_ = lexer_.scan(source);

    // 更新 Token 列表
    tokenTable_->setRowCount(static_cast<int>(lastTokens_.size()));
    for (int i = 0; i < static_cast<int>(lastTokens_.size()); ++i) {
        const Token& tok = lastTokens_[i];
        tokenTable_->setItem(i, 0, new QTableWidgetItem(
            QString::fromStdString(Token::typeToString(tok.type))));
        tokenTable_->setItem(i, 1, new QTableWidgetItem(
            QString::fromStdString(tok.lexeme)));
        tokenTable_->setItem(i, 2, new QTableWidgetItem(
            QString::fromStdString(tok.literal.toString())));
        tokenTable_->setItem(i, 3, new QTableWidgetItem(
            QString::number(tok.line)));
        tokenTable_->setItem(i, 4, new QTableWidgetItem(
            QString::number(tok.column)));

        // 错误 Token 红色标记
        if (tok.type == TokenType::TK_ERROR) {
            for (int col = 0; col < 5; ++col) {
                tokenTable_->item(i, col)->setForeground(Qt::red);
            }
            outputPanel_->appendError(QString("词法错误 (行 %1, 列 %2): %3")
                                          .arg(tok.line).arg(tok.column)
                                          .arg(QString::fromStdString(tok.lexeme)));
        }
    }
    tokenTable_->resizeColumnsToContents();
}

void Ide::runParser(const std::vector<Token>& tokens) {
    astRoot_ = parser_.parse(tokens);

    // 更新 AST 视图
    if (astRoot_) {
        astViewer_->setAst(astRoot_.get());
    }
}

void Ide::runCompiler() {
    if (!astRoot_) return;

    lastBytecode_ = compiler_.compile(*astRoot_);

    // 检查编译错误
    std::string err = compiler_.getLastError();
    if (!err.empty()) {
        outputPanel_->appendError(QString::fromStdString(err));
    }
}

void Ide::updateDebugInfo() {
    auto vars = debugger_->getVariableSnapshot();
    debugPanel_->updateVariables(vars);

    auto stack = debugger_->getCallStack();
    debugPanel_->updateCallStack(stack);
}

void Ide::setRunningState(bool running) {
    runAction_->setEnabled(!running);
    debugAction_->setEnabled(!running);
    stepInAction_->setEnabled(running);
    stepOverAction_->setEnabled(running);
    stopAction_->setEnabled(running);
    formatAction_->setEnabled(!running);
    bytecodeAction_->setEnabled(!running);
    codeEditor_->setReadOnly(running);
}
