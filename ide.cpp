#include "ide.h"
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QHeaderView>
#include <QApplication>
#include <QColor>
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

    // VM 输出回调
    vm_.setOutputCallback([this](const std::string& text) {
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

    // 字节码视图：左右分割（左：指令列表 + 右：栈状态面板）
    auto* bytecodeSplitter = new QSplitter(Qt::Horizontal, this);

    bytecodeList_ = new QListWidget(this);
    bytecodeList_->setFont(QFont("Consolas", 10));
    bytecodeList_->setStyleSheet(
        "QListWidget {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  border: none;"
        "}"
        "QListWidget::item {"
        "  padding: 2px 6px;"
        "  border-bottom: 1px solid #333;"
        "}"
        "QListWidget::item:selected {"
        "  background-color: #264f78;"
        "}"
    );
    bytecodeList_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    vmStackPanel_ = new VmStackPanel(this);
    vmStackPanel_->setMinimumWidth(200);

    bytecodeSplitter->addWidget(bytecodeList_);
    bytecodeSplitter->addWidget(vmStackPanel_);
    bytecodeSplitter->setStretchFactor(0, 3);
    bytecodeSplitter->setStretchFactor(1, 2);

    rightTabWidget_->addTab(bytecodeSplitter, "字节码视图");

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

    toolbar->addSeparator();

    // ---- VM 调试按钮 ----
    vmRunAction_ = toolbar->addAction("⚡ VM运行");
    vmRunAction_->setToolTip("全速运行字节码 (Ctrl+Shift+R)");
    vmRunAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    vmRunAction_->setEnabled(false);

    vmStepAction_ = toolbar->addAction("👉 VM单步");
    vmStepAction_->setToolTip("单步执行字节码 (Ctrl+Shift+N)");
    vmStepAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    vmStepAction_->setEnabled(false);

    vmStopAction_ = toolbar->addAction("⏹ VM停止");
    vmStopAction_->setToolTip("停止 VM 执行");
    vmStopAction_->setEnabled(false);
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

    // VM 调试连接
    connect(vmRunAction_, &QAction::triggered, this, &Ide::onVmRun);
    connect(vmStepAction_, &QAction::triggered, this, &Ide::onVmStep);
    connect(vmStopAction_, &QAction::triggered, this, &Ide::onVmStop);
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
        bytecodeList_->clear();
        bytecodeList_->addItem(QString("编译失败 - 语法错误 (行 %1, 列 %2): %3")
                                   .arg(e.line).arg(e.column).arg(e.what()));
        return;
    }

    if (!astRoot_) return;

    // 编译
    runCompiler();

    // 填充指令列表
    populateBytecodeList();

    // 清空栈面板
    vmStackPanel_->clearAll();

    // 启用 VM 调试按钮
    vmRunAction_->setEnabled(true);
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);

    // 切换到字节码 Tab
    rightTabWidget_->setCurrentIndex(2);
}

// ============================================================
// VM 调试执行
// ============================================================

void Ide::onVmRun() {
    if (isVmRunning_) return;
    if (lastCompileResult_.mainChunk.code.empty()) return;

    isVmRunning_ = true;
    isVmStepMode_ = false;

    vmRunAction_->setEnabled(false);
    vmStepAction_->setEnabled(false);
    vmStopAction_->setEnabled(true);
    bytecodeAction_->setEnabled(false);

    // 启用步进回调，全速执行模式下也更新 UI
    vm_.setStepCallbackEnabled(true);

    try {
        VMResult result = vm_.execute(lastCompileResult_);
        if (result == VMResult::VM_RUNTIME_ERROR) {
            outputPanel_->appendError(QString("VM 运行时错误: %1")
                                          .arg(QString::fromStdString(vm_.getLastError())));
        }
        outputPanel_->appendOutput("--- VM 执行结束 ---");
    } catch (...) {
        outputPanel_->appendError("VM 执行异常");
    }

    vm_.setStepCallbackEnabled(false);
    isVmRunning_ = false;

    vmRunAction_->setEnabled(true);
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);
    bytecodeAction_->setEnabled(true);
}

void Ide::onVmStep() {
    if (isVmRunning_) return;
    if (lastCompileResult_.mainChunk.code.empty()) return;

    isVmRunning_ = true;
    isVmStepMode_ = true;

    vmRunAction_->setEnabled(false);
    vmStepAction_->setEnabled(false);
    vmStopAction_->setEnabled(true);
    bytecodeAction_->setEnabled(false);

    // 单步模式：只执行一条指令
    // 通过步进回调，第一条指令执行后自动暂停
    int stepCount = 0;
    vm_.setStepCallbackEnabled(true);
    vm_.setStepCallback([&stepCount, this](const VMStepInfo& info) {
        stepCount++;
        // 单步模式：执行一条后即停止（通过抛异常中断）
        if (stepCount >= 1 && isVmStepMode_) {
            // 更新 UI
            onVmStepCallback(info);
            throw std::runtime_error("__VM_STEP_STOP__");
        }
        // 全速运行时每条也更新 UI
        onVmStepCallback(info);
    });

    try {
        VMResult result = vm_.execute(lastCompileResult_);
        if (result == VMResult::VM_RUNTIME_ERROR) {
            outputPanel_->appendError(QString("VM 运行时错误: %1")
                                          .arg(QString::fromStdString(vm_.getLastError())));
        }
        outputPanel_->appendOutput("--- VM 执行结束 ---");
        vmStackPanel_->clearAll();
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        if (msg == "__VM_STEP_STOP__") {
            // 单步暂停，正常
        } else {
            outputPanel_->appendError(QString("VM 错误: %1").arg(e.what()));
        }
    }

    vm_.setStepCallbackEnabled(false);
    isVmRunning_ = false;

    vmRunAction_->setEnabled(true);
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);
    bytecodeAction_->setEnabled(true);
}

void Ide::onVmStop() {
    // VM 单次执行模式不需要异步停止
    // 按钮状态在执行完成后自动更新
    isVmStepMode_ = false;
    vmStackPanel_->clearAll();
}

void Ide::onVmStepCallback(const VMStepInfo& info) {
    // 更新栈面板
    vmStackPanel_->updateStack(info.stackSnapshot);
    vmStackPanel_->updateGlobals(info.globalsSnapshot);

    // 更新当前指令信息
    int line = 0;
    if (info.ip < lastCompileResult_.mainChunk.lines.size()) {
        line = lastCompileResult_.mainChunk.lines[info.ip];
    }
    vmStackPanel_->updateCurrentOp(info.ip, info.opcode, line);

    // 高亮当前指令
    highlightBytecodeLine(info.ip);

    // 处理 GUI 事件，保持响应
    QApplication::processEvents();
}

void Ide::highlightBytecodeLine(size_t ip) {
    // 根据指令偏移找到对应的列表行
    // 由于反汇编指令长度不同，需要遍历计算
    if (lastCompileResult_.mainChunk.code.empty()) return;

    size_t offset = 0;
    int row = 0;
    while (offset < lastCompileResult_.mainChunk.code.size()) {
        if (offset == ip) {
            // 找到对应行，设置高亮
            bytecodeList_->setCurrentRow(row);
            bytecodeList_->scrollToItem(bytecodeList_->item(row));
            return;
        }
        // 跳过当前指令，计算下一条指令的偏移
        OpCode op = static_cast<OpCode>(lastCompileResult_.mainChunk.code[offset]);
        switch (op) {
        case OpCode::OP_CONSTANT:
        case OpCode::OP_INT:
        case OpCode::OP_FLOAT:
        case OpCode::OP_STRING:
        case OpCode::OP_DEFINE_VAR:
        case OpCode::OP_GET_VAR:
        case OpCode::OP_SET_VAR:
        case OpCode::OP_JUMP:
        case OpCode::OP_JUMP_IF_FALSE:
        case OpCode::OP_LOOP:
        case OpCode::OP_MEMBER_GET:
        case OpCode::OP_MEMBER_SET:
            offset += 3; break;
        case OpCode::OP_CALL:
        case OpCode::OP_METHOD_CALL:
        case OpCode::OP_CLOSURE:
        case OpCode::OP_CLASS_NEW:
            offset += 4; break;
        case OpCode::OP_BUILD_ARRAY:
        case OpCode::OP_GET_LOCAL:
        case OpCode::OP_SET_LOCAL:
            offset += 2; break;
        default:
            offset += 1; break;
        }
        row++;
    }
}

void Ide::populateBytecodeList() {
    bytecodeList_->clear();

    if (lastCompileResult_.mainChunk.code.empty()) {
        bytecodeList_->addItem("(无字节码)");
        return;
    }

    // 逐条反汇编主 chunk 并添加到列表
    size_t offset = 0;
    while (offset < lastCompileResult_.mainChunk.code.size()) {
        std::string instr = lastCompileResult_.mainChunk.disassembleInstruction(offset);
        auto* item = new QListWidgetItem(QString::fromStdString(instr));
        item->setFont(QFont("Consolas", 10));
        bytecodeList_->addItem(item);
    }

    // 也显示函数 chunk 的字节码
    for (const auto& kv : lastCompileResult_.functionChunks) {
        auto* header = new QListWidgetItem(QString("---- %1 (arity=%2) ----")
                                               .arg(QString::fromStdString(kv.first))
                                               .arg(kv.second.arity));
        header->setFont(QFont("Consolas", 10));
        header->setForeground(QColor("#569CD6"));
        bytecodeList_->addItem(header);

        size_t funcOffset = 0;
        while (funcOffset < kv.second.code.size()) {
            std::string instr = kv.second.disassembleInstruction(funcOffset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(QFont("Consolas", 10));
            bytecodeList_->addItem(item);
        }
    }
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

    lastCompileResult_ = compiler_.compile(*astRoot_);

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
