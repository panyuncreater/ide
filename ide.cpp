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

    // Step Out
    stepOutAction_ = toolbar->addAction("⬅ Step Out");
    stepOutAction_->setToolTip("单步跳出 (Shift+F11)");
    stepOutAction_->setShortcut(Qt::SHIFT | Qt::Key_F11);
    stepOutAction_->setEnabled(false);

    // Resume（继续运行到下一个断点）
    resumeAction_ = toolbar->addAction("▶ Resume");
    resumeAction_->setToolTip("继续运行到下一个断点 (F9)");
    resumeAction_->setShortcut(Qt::Key_F9);
    resumeAction_->setEnabled(false);

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
    connect(stepOutAction_, &QAction::triggered, this, &Ide::onStepOut);
    connect(resumeAction_, &QAction::triggered, this, &Ide::onResume);
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
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("词法分析异常: %1").arg(e.what()));
        return;
    }

    // 语法分析（runParser 内部已收集并显示所有错误）
    runParser(lastTokens_);

    // 如果有解析错误，不再继续执行
    if (parser_.hasErrors()) return;

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
    } catch (const DebugStopException&) {
        outputPanel_->appendOutput("--- 调试终止 ---");
    } catch (const std::runtime_error& e) {
        outputPanel_->appendError(QString("错误: %1").arg(e.what()));
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
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("词法分析异常: %1").arg(e.what()));
        return;
    }

    // 语法分析（runParser 内部已收集并显示所有错误）
    runParser(lastTokens_);

    // 如果有解析错误，不再继续
    if (parser_.hasErrors()) return;

    if (!astRoot_) return;

    // 设置断点
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());

    // 设置调试变量回调
    debugger_->setVariableCallback([this]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> result;
        Environment* env = interpreter_.currentEnvironment();
        if (env) {
            // 沿作用域链收集变量，标注每个变量的作用域层级
            int depth = 0;
            Environment* current = env;
            while (current) {
                const auto& locals = current->localVariables();
                for (const auto& kv : locals) {
                    VariableSnapshot snap;
                    snap.name = kv.first;
                    snap.value = kv.second;
                    snap.scope = (depth == 0) ? "局部" : (current->parent ? "外层" : "全局");
                    result.push_back(snap);
                }
                current = current->parent.get();
                depth++;
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
    } catch (const DebugStopException&) {
        // 用户点击停止按钮 — 正常调试终止，不显示错误
        outputPanel_->appendOutput("--- 调试终止 ---");
    } catch (const std::runtime_error& e) {
        outputPanel_->appendError(QString("错误: %1").arg(e.what()));
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

void Ide::onStepOut() {
    debugger_->stepOut();
    updateDebugInfo();
}

void Ide::onResume() {
    // 同步编辑器断点到调试控制器（用户可能在暂停期间修改了断点）
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());
    debugger_->resume();
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
    astRoot_ = parser_.parse(lastTokens_);
    if (parser_.hasErrors()) {
        for (const auto& err : parser_.getErrors()) {
            outputPanel_->appendError(QString("格式化失败 - 语法错误 (行 %1, 列 %2): %3")
                                          .arg(err.line).arg(err.column).arg(err.what()));
        }
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
    astRoot_ = parser_.parse(lastTokens_);
    if (parser_.hasErrors()) {
        bytecodeList_->clear();
        for (const auto& err : parser_.getErrors()) {
            bytecodeList_->addItem(QString("编译失败 - 语法错误 (行 %1, 列 %2): %3")
                                       .arg(err.line).arg(err.column).arg(err.what()));
        }
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

    // 重置 VM 状态（确保单步模式从头开始）
    vm_.resetState();
    isVmInitialized_ = false;

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

    vmRunAction_->setEnabled(false);
    vmStepAction_->setEnabled(false);
    vmStopAction_->setEnabled(true);
    bytecodeAction_->setEnabled(false);

    // 重置 VM 状态，全速执行
    vm_.resetState();

    // 全速执行时不启用步进回调：避免每条指令深拷贝栈/全局变量
    // 以及 processEvents() 导致的重入风险
    vm_.setStepCallbackEnabled(false);
    vm_.setStepCallback(nullptr);

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
    isVmInitialized_ = false;

    vmRunAction_->setEnabled(true);
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);
    bytecodeAction_->setEnabled(true);
    vmStackPanel_->clearAll();
}

void Ide::onVmStep() {
    if (isVmRunning_) return;
    if (lastCompileResult_.mainChunk.code.empty()) return;

    // 首次点击：初始化 VM 执行环境
    if (!isVmInitialized_) {
        vm_.initExecution(lastCompileResult_);
        isVmInitialized_ = true;
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        bytecodeAction_->setEnabled(false);
    }

    isVmRunning_ = true;
    vmStepAction_->setEnabled(false);  // 防止重入

    // 单步执行时启用回调，让 onVmStepCallback 统一处理UI更新
    vm_.setStepCallbackEnabled(true);
    vm_.setStepCallback([this](const VMStepInfo& info) {
        onVmStepCallback(info);
    });

    // 执行一条指令
    VMResult result = vm_.stepOnce();

    if (result == VMResult::VM_RUNTIME_ERROR) {
        outputPanel_->appendError(QString("VM 运行时错误: %1")
                                      .arg(QString::fromStdString(vm_.getLastError())));
        vmStackPanel_->clearAll();
        isVmInitialized_ = false;
        vmRunAction_->setEnabled(true);
        vmStepAction_->setEnabled(true);
        vmStopAction_->setEnabled(false);
        bytecodeAction_->setEnabled(true);
        isVmRunning_ = false;
        return;
    }

    // 检查是否执行完毕
    if (vm_.isFinished()) {
        outputPanel_->appendOutput("--- VM 执行结束 ---");
        vmStackPanel_->clearAll();
        isVmInitialized_ = false;
        vmRunAction_->setEnabled(true);
        vmStepAction_->setEnabled(true);
        vmStopAction_->setEnabled(false);
        bytecodeAction_->setEnabled(true);
        isVmRunning_ = false;
        return;
    }

    // 更新 UI：栈 + 全局变量 + 当前指令高亮
    vmStackPanel_->updateStack(vm_.getStackRef());
    vmStackPanel_->updateGlobals(vm_.getGlobalsRef());

    // 高亮当前字节码指令
    size_t currentIP = vm_.getCurrentIP();
    OpCode currentOp = vm_.getCurrentOpCode();
    int opLine = vm_.getCurrentLine();
    vmStackPanel_->updateCurrentOp(currentIP, currentOp, opLine);
    highlightBytecodeLine(vm_.getCurrentChunkName(), currentIP);

    vmStepAction_->setEnabled(true);
    isVmRunning_ = false;
}

void Ide::onVmStop() {
    vm_.resetState();
    isVmInitialized_ = false;
    vmStackPanel_->clearAll();

    vmRunAction_->setEnabled(true);
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);
    bytecodeAction_->setEnabled(true);
}

void Ide::onVmStepCallback(const VMStepInfo& info) {
    // 单步执行时使用步进回调实时刷新UI
    // 注意：不调用 processEvents()，避免在执行中响应用户事件导致重入
    vmStackPanel_->updateStack(vm_.getStackRef());
    vmStackPanel_->updateGlobals(vm_.getGlobalsRef());

    int line = vm_.getCurrentLine();
    vmStackPanel_->updateCurrentOp(info.ip, info.opcode, line);
    highlightBytecodeLine(vm_.getCurrentChunkName(), info.ip);
}

void Ide::highlightBytecodeLine(const std::string& chunkName, size_t ip) {
    // 根据当前 chunk 名和 ip 定位到正确的列表行
    // 使用预计算的 ipToInstrIndex 做 O(1) 查找
    const BytecodeChunk* targetChunk = nullptr;

    // 找到目标 chunk
    if (chunkName == "main" || chunkName.empty()) {
        targetChunk = &lastCompileResult_.mainChunk;
    } else {
        auto it = lastCompileResult_.functionChunks.find(chunkName);
        if (it != lastCompileResult_.functionChunks.end()) {
            targetChunk = &it->second;
        }
    }
    if (!targetChunk || targetChunk->code.empty()) return;

    // O(1) 查找：ip → 指令索引
    int instrIndex = 0;
    if (ip < targetChunk->ipToInstrIndex.size() && targetChunk->ipToInstrIndex[ip] >= 0) {
        instrIndex = targetChunk->ipToInstrIndex[ip];
    } else {
        // fallback：遍历查找（不应发生）
        size_t offset = 0;
        while (offset < targetChunk->code.size()) {
            if (offset == ip) break;
            OpCode op = static_cast<OpCode>(targetChunk->code[offset]);
            offset += BytecodeChunk::instructionSize(op);
            instrIndex++;
        }
    }

    // 在 chunkRowMap_ 中找到该 chunk 的起始行
    int startRow = 0;
    for (const auto& info : chunkRowMap_) {
        if (info.name == chunkName) {
            startRow = info.startRow;
            break;
        }
    }

    int targetRow = startRow + instrIndex;
    if (targetRow >= 0 && targetRow < bytecodeList_->count()) {
        bytecodeList_->setCurrentRow(targetRow);
        bytecodeList_->scrollToItem(bytecodeList_->item(targetRow));
    }
}

void Ide::populateBytecodeList() {
    bytecodeList_->clear();
    chunkRowMap_.clear();

    if (lastCompileResult_.mainChunk.code.empty()) {
        bytecodeList_->addItem("(无字节码)");
        return;
    }

    int currentRow = 0;

    // ---- 主 chunk ----
    {
        int startRow = currentRow;
        size_t offset = 0;
        while (offset < lastCompileResult_.mainChunk.code.size()) {
            std::string instr = lastCompileResult_.mainChunk.disassembleInstruction(offset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(QFont("Consolas", 10));
            bytecodeList_->addItem(item);
            currentRow++;
        }
        chunkRowMap_.push_back({"main", startRow, currentRow - startRow});
    }

    // ---- 函数 chunk ----
    for (const auto& kv : lastCompileResult_.functionChunks) {
        auto* header = new QListWidgetItem(QString("---- %1 (arity=%2) ----")
                                               .arg(QString::fromStdString(kv.first))
                                               .arg(kv.second.arity));
        header->setFont(QFont("Consolas", 10));
        header->setForeground(QColor("#569CD6"));
        bytecodeList_->addItem(header);
        currentRow++;

        // startRow 指向第一条指令所在行（标题行之后），而非标题行
        // 这样 highlightBytecodeLine 中 startRow + instrIndex 才能正确对应指令行
        int startRow = currentRow;

        size_t funcOffset = 0;
        while (funcOffset < kv.second.code.size()) {
            std::string instr = kv.second.disassembleInstruction(funcOffset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(QFont("Consolas", 10));
            bytecodeList_->addItem(item);
            currentRow++;
        }
        chunkRowMap_.push_back({kv.first, startRow, currentRow - startRow});
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

    // 显示所有收集到的解析错误
    if (parser_.hasErrors()) {
        QSet<int> errorLines;
        for (const auto& err : parser_.getErrors()) {
            outputPanel_->appendError(QString("语法错误 (行 %1, 列 %2): %3")
                                          .arg(err.line).arg(err.column).arg(err.what()));
            errorLines.insert(err.line);
        }
        codeEditor_->setErrorLines(errorLines);
    }

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
    stepOutAction_->setEnabled(running);
    resumeAction_->setEnabled(running);
    stopAction_->setEnabled(running);
    formatAction_->setEnabled(!running);
    bytecodeAction_->setEnabled(!running);
    codeEditor_->setReadOnly(running);
}
