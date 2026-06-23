#include "ide.h"
#include "Logger.h"
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QHeaderView>
#include <QApplication>
#include <QColor>
#include <QScrollBar>
#include <QFile>
#include <QTextStream>
#include <QMenuBar>
#include <QFileInfo>
#include <sstream>

// ============================================================
// Ide — GUI 交互层实现
// ------------------------------------------------------------
// 仅负责 GUI 创建/布局/事件处理，业务逻辑委托给 IdeController。
// ============================================================

Ide::Ide(QWidget* parent)
    : QMainWindow(parent) {

    // 创建业务逻辑层（作为子 QObject，自动释放）
    controller_ = new IdeController(this);

    initUI();
    initToolbar();
    initConnections();

    // REPL 面板需要解释器引用
    replPanel_->setInterpreter(&controller_->interpreter());

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
    // controller_ 是子 QObject，由 QObject 析构链自动释放
    // 其析构函数会安全停止 worker 线程
}

void Ide::closeEvent(QCloseEvent* event) {
    // GUI-04: 关闭前检查未保存的修改
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    if (controller_->isRunning()) {
        // 先停止调试器，让解释器通过 DebugStopException 正常退出
        if (!controller_->stopForClose(3000)) {
            // M5 fix: 线程未响应，弹窗让用户选择
            auto ret = QMessageBox::warning(
                this, tr("程序仍在运行"),
                tr("程序未能在 3 秒内停止。强制终止可能导致数据丢失。\n是否强制终止？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                controller_->forceStop();
            } else {
                event->ignore();
                return;
            }
        }
    }
    if (controller_->isVmRunning()) {
        onVmStop();
    }
    event->accept();
}

// ============================================================
// UI 初始化
// ============================================================

void Ide::initUI() {
    // GUI-04: 菜单栏
    auto* fileMenu = menuBar()->addMenu(QString::fromUtf8("文件(&F)"));

    newAction_ = new QAction(QString::fromUtf8("新建(&N)"), this);
    newAction_->setShortcut(QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, &Ide::onNew);
    fileMenu->addAction(newAction_);

    openAction_ = new QAction(QString::fromUtf8("打开(&O)..."), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &Ide::onOpen);
    fileMenu->addAction(openAction_);

    fileMenu->addSeparator();

    saveAction_ = new QAction(QString::fromUtf8("保存(&S)"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &Ide::onSave);
    fileMenu->addAction(saveAction_);

    saveAsAction_ = new QAction(QString::fromUtf8("另存为(&A)..."), this);
    saveAsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction_, &QAction::triggered, this, &Ide::onSaveAs);
    fileMenu->addAction(saveAsAction_);

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
    bytecodeList_->setObjectName("bytecodeList");
    bytecodeList_->setFont(QFont("Consolas", 10));
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
    bottomTabWidget_->addTab(replPanel_, "REPL");

    vSplitter_->addWidget(mainSplitter_);
    vSplitter_->addWidget(bottomTabWidget_);
    vSplitter_->setStretchFactor(0, 3);
    vSplitter_->setStretchFactor(1, 1);

    mainLayout->addWidget(vSplitter_);

    // 窗口属性
    updateWindowTitle();
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
    vmStepAction_ = toolbar->addAction("👉 VM单步");
    vmStepAction_->setToolTip("单步执行字节码 (Ctrl+Shift+N)");
    vmStepAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    vmStepAction_->setEnabled(false);

    vmStopAction_ = toolbar->addAction("⏹ VM停止");
    vmStopAction_->setToolTip("停止 VM 执行");
    vmStopAction_->setEnabled(false);
}

void Ide::initConnections() {
    // ---- 工具栏动作 ----
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

    // 条件断点：编辑器右键设置条件时同步到调试控制器
    connect(codeEditor_, &CodeEditor::breakpointConditionRequested,
            this, [this](int line, const QString& condition) {
                controller_->debugger()->setBreakpointCondition(line, condition.toStdString());
            });

    // VM 调试连接
    connect(vmStepAction_, &QAction::triggered, this, &Ide::onVmStep);
    connect(vmStopAction_, &QAction::triggered, this, &Ide::onVmStop);

    // GUI-04: 文件修改追踪
    connect(codeEditor_->document(), &QTextDocument::modificationChanged,
            this, [this](bool changed) {
        isDirty_ = changed;
        updateWindowTitle();
    });

    // ---- IdeController 信号 → UI 更新 ----
    connect(controller_, &IdeController::outputReady, outputPanel_, &OutputPanel::appendOutput);

    connect(controller_, &IdeController::runOk, this, [this]() {
        outputPanel_->appendOutput("--- 程序执行结束 ---");
    });

    connect(controller_, &IdeController::stoppedByUser, this, [this]() {
        outputPanel_->appendOutput("--- 调试终止 ---");
    });

    connect(controller_, &IdeController::runtimeError, this, [this](const QString& msg, int line, int column) {
        Diagnostic diag(DiagLevel::Error, msg.toStdString(), line, column, DiagSource::Interpreter);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        // GUI-07 fix: 标记错误行（line > 0 时才标记）
        if (line > 0) {
            QSet<int> errorLines;
            errorLines.insert(line);
            codeEditor_->setErrorLines(errorLines);
        }
    });

    connect(controller_, &IdeController::genericError, this, [this](const QString& msg) {
        outputPanel_->appendError(msg);
    });

    connect(controller_, &IdeController::pausedAt, this, &Ide::onPausedAt);

    connect(controller_, &IdeController::workerFinished, this, &Ide::onWorkerFinished);

    connect(controller_, &IdeController::vmStepInfo, this, [this](const VMStepInfo& info) {
        vmStackPanel_->updateStack(controller_->vm().getStackRef());
        vmStackPanel_->updateGlobals(controller_->vm().getGlobalsRef());
        int line = controller_->vm().getCurrentLine();
        vmStackPanel_->updateCurrentOp(info.ip, info.opcode, line);
        highlightBytecodeLine(controller_->vm().getCurrentChunkName(), info.ip);
    });

    connect(controller_, &IdeController::diagnosticsReady, this, &Ide::displayDiagnostics);
}

// ============================================================
// 运行 / 调试
// ============================================================

void Ide::onRun() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出和调试面板
    outputPanel_->clearAll();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    if (!controller_->prepareRun(false, source)) return;

    // 更新 UI
    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    controller_->startWorker();
}

void Ide::onDebug() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出和调试面板
    outputPanel_->clearAll();
    debugPanel_->clearAll();  // H7 fix: 清空旧调试数据
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    if (!controller_->prepareRun(true, source)) return;

    // 更新 UI
    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    // 设置断点及条件
    QSet<int> breakpoints = codeEditor_->getBreakpoints();
    QMap<int, std::string> conditions;
    for (int line : breakpoints) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty()) {
            conditions[line] = cond;
        }
    }
    controller_->setupDebug(breakpoints, conditions);

    controller_->startWorker();
}

void Ide::onStepIn() {
    controller_->setBreakpoints(codeEditor_->getBreakpoints());
    controller_->stepIn();
}

void Ide::onStepOver() {
    controller_->setBreakpoints(codeEditor_->getBreakpoints());
    controller_->stepOver();
}

void Ide::onStepOut() {
    controller_->setBreakpoints(codeEditor_->getBreakpoints());
    controller_->stepOut();
}

void Ide::onResume() {
    controller_->setBreakpoints(codeEditor_->getBreakpoints());
    controller_->resume();
}

void Ide::onStop() {
    controller_->stop();
}

void Ide::onClearOutput() {
    outputPanel_->clearAll();
}

void Ide::onPausedAt(int line) {
    codeEditor_->setCurrentLine(line);
    updateDebugInfo();
    // GUI-09 fix: 暂停时自动切换到调试面板
    bottomTabWidget_->setCurrentWidget(debugPanel_);
}

void Ide::onWorkerFinished(bool wasDebug) {
    setRunningState(false);
    codeEditor_->clearCurrentLine();
    codeEditor_->clearErrorLines();  // L-新2 fix: 运行结束时清除错误标记
    replPanel_->setInputEnabled(true);
}

// ============================================================
// 格式化
// ============================================================

void Ide::onFormat() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 词法分析
    try {
        if (!controller_->runLexer(source)) {
            updateTokenTable();
            return;
        }
    } catch (const std::exception& e) {
        Diagnostic diag(DiagLevel::Error, e.what(), 0, 0, DiagSource::Lexer);
        outputPanel_->appendError(QString::fromStdString("[格式化] " + diag.format()));
        return;
    }
    updateTokenTable();

    // 语法分析
    try {
        if (!controller_->runParser()) {
            updateAstViewer();
            return;
        }
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("[格式化] 解析异常: %1").arg(e.what()));
        return;
    }
    if (controller_->parser().hasErrors()) {
        for (const auto& diag : controller_->parserDiagnostics().all()) {
            outputPanel_->appendError(QString::fromStdString("[格式化] " + diag.format()));
        }
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) return;

    // 格式化：行号会变化，需清除断点并保存光标位置
    bool hadBreakpoints = !controller_->debugger()->getBreakpoints().isEmpty();

    QTextCursor savedCursor = codeEditor_->textCursor();
    int scrollPos = codeEditor_->verticalScrollBar()->value();

    std::string formatted;
    try {
        if (!controller_->formatCode(formatted)) {
            return;
        }
        codeEditor_->setPlainText(QString::fromStdString(formatted));
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("[格式化] 格式化异常: %1").arg(e.what()));
        return;  // D4 fix: 格式化失败时断点保留不清除
    }

    // D4 fix: 格式化成功后才清除断点
    if (hadBreakpoints) {
        outputPanel_->appendOutput(QString("[格式化] 断点已清除（行号变化，断点不再有效）"));
    }
    codeEditor_->setBreakpoints(QSet<int>());
    controller_->setBreakpoints(QSet<int>());

    // D3 fix: 刷新 AST 查看器（格式化后 astRoot_ 已更新）
    updateAstViewer();

    // 恢复光标位置和滚动位置（尽可能）
    if (savedCursor.position() <= codeEditor_->document()->characterCount()) {
        codeEditor_->setTextCursor(savedCursor);
    }
    codeEditor_->verticalScrollBar()->setValue(scrollPos);
}

// ============================================================
// 字节码视图
// ============================================================

void Ide::onShowBytecode() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // L15 fix: 清除编辑器中残留的错误行标记
    codeEditor_->clearErrorLines();
    // GUI-10 fix: 清除调试执行行高亮
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        if (!controller_->runLexer(source)) {
            updateTokenTable();
            return;
        }
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("字节码生成失败 - 词法错误: %1").arg(e.what()));
        return;
    }
    updateTokenTable();

    // 语法分析
    try {
        if (!controller_->runParser()) {
            updateAstViewer();
            return;
        }
    } catch (const std::exception& e) {
        bytecodeList_->clear();
        bytecodeList_->addItem(QString("字节码生成失败 - 解析异常: %1").arg(e.what()));
        return;
    }
    if (controller_->parser().hasErrors()) {
        bytecodeList_->clear();
        for (const auto& diag : controller_->parserDiagnostics().all()) {
            bytecodeList_->addItem(QString::fromStdString("[编译] " + diag.format()));
        }
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) return;

    // 编译
    try {
        controller_->runCompiler();
    } catch (const std::exception& e) {
        bytecodeList_->clear();
        bytecodeList_->addItem(QString("字节码编译异常: %1").arg(e.what()));
        return;
    }

    // 填充指令列表
    populateBytecodeList();

    // 清空栈面板
    vmStackPanel_->clearAll();

    // 启用 VM 调试按钮
    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);

    // 重置 VM 状态
    controller_->vmReset();

    // 切换到字节码 Tab
    rightTabWidget_->setCurrentIndex(2);
}

void Ide::onVmStep() {
    if (controller_->isVmRunning()) return;
    if (controller_->lastCompileResult().mainChunk.code.empty()) return;

    bool firstStep = !controller_->isVmInitialized();

    vmStepAction_->setEnabled(false);  // 防止重入

    if (firstStep) {
        vmStopAction_->setEnabled(true);
        bytecodeAction_->setEnabled(false);
    }

    auto result = controller_->vmStep();

    switch (result) {
    case IdeController::VmStepResult::NOT_READY:
        vmStepAction_->setEnabled(true);
        return;

    case IdeController::VmStepResult::ERROR: {
        Diagnostic diag(DiagLevel::Error, controller_->vm().getLastError(),
                        controller_->vm().getLastErrorLine(), 0, DiagSource::VM);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        if (controller_->vm().getLastErrorLine() > 0) {
            QSet<int> errorLines;
            errorLines.insert(controller_->vm().getLastErrorLine());
            codeEditor_->setErrorLines(errorLines);
        }
        vmStackPanel_->clearAll();
        vmStepAction_->setEnabled(true);
        vmStopAction_->setEnabled(false);
        bytecodeAction_->setEnabled(true);
        return;
    }

    case IdeController::VmStepResult::FINISHED:
        outputPanel_->appendOutput("--- VM 执行结束 ---");
        vmStackPanel_->clearAll();
        vmStepAction_->setEnabled(true);
        vmStopAction_->setEnabled(false);
        bytecodeAction_->setEnabled(true);
        return;

    case IdeController::VmStepResult::OK:
        // 更新 UI：栈 + 全局变量 + 当前指令高亮
        vmStackPanel_->updateStack(controller_->vm().getStackRef());
        vmStackPanel_->updateGlobals(controller_->vm().getGlobalsRef());
        {
            size_t currentIP = controller_->vm().getCurrentIP();
            OpCode currentOp = controller_->vm().getCurrentOpCode();
            int opLine = controller_->vm().getCurrentLine();
            vmStackPanel_->updateCurrentOp(currentIP, currentOp, opLine);
            highlightBytecodeLine(controller_->vm().getCurrentChunkName(), currentIP);
        }
        vmStepAction_->setEnabled(true);
        return;
    }
}

void Ide::onVmStop() {
    controller_->vmStop();
    vmStackPanel_->clearAll();
    // GUI-11 fix: 清除字节码列表当前行高亮
    bytecodeList_->setCurrentRow(-1);

    vmStepAction_->setEnabled(true);
    vmStopAction_->setEnabled(false);
    bytecodeAction_->setEnabled(true);
}

// ============================================================
// 字节码列表 UI 辅助
// ============================================================

void Ide::highlightBytecodeLine(const std::string& chunkName, size_t ip) {
    const CompileResult& compileResult = controller_->lastCompileResult();
    const BytecodeChunk* targetChunk = nullptr;

    // 找到目标 chunk
    if (chunkName == "main" || chunkName.empty()) {
        targetChunk = &compileResult.mainChunk;
    } else {
        auto it = compileResult.functionChunks.find(chunkName);
        if (it != compileResult.functionChunks.end()) {
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
    const CompileResult& compileResult = controller_->lastCompileResult();

    bytecodeList_->clear();
    chunkRowMap_.clear();

    if (compileResult.mainChunk.code.empty()) {
        bytecodeList_->addItem("(无字节码)");
        return;
    }

    bytecodeList_->setUpdatesEnabled(false);  // P6 fix: 批量填充时禁用重绘
    int currentRow = 0;

    // ---- 主 chunk ----
    {
        int startRow = currentRow;
        size_t offset = 0;
        while (offset < compileResult.mainChunk.code.size()) {
            std::string instr = compileResult.mainChunk.disassembleInstruction(offset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(QFont("Consolas", 10));
            bytecodeList_->addItem(item);
            currentRow++;
        }
        chunkRowMap_.push_back({"main", startRow, currentRow - startRow});
    }

    // ---- 函数 chunk ----
    for (const auto& kv : compileResult.functionChunks) {
        auto* header = new QListWidgetItem(QString("---- %1 (arity=%2) ----")
                                               .arg(QString::fromStdString(kv.first))
                                               .arg(kv.second.arity));
        header->setFont(QFont("Consolas", 10));
        header->setForeground(QColor("#569CD6"));
        bytecodeList_->addItem(header);
        currentRow++;

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
    bytecodeList_->setUpdatesEnabled(true);  // P6 fix: 恢复重绘
}

// ============================================================
// UI 更新辅助方法
// ============================================================

void Ide::updateTokenTable() {
    const std::vector<Token>& tokens = controller_->lastTokens();
    tokenTable_->setRowCount(static_cast<int>(tokens.size()));
    tokenTable_->setUpdatesEnabled(false);
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& tok = tokens[i];
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
        }
    }
    tokenTable_->setUpdatesEnabled(true);
    tokenTable_->resizeColumnsToContents();
}

void Ide::updateAstViewer() {
    if (controller_->astRoot()) {
        astViewer_->setAst(controller_->astRoot());
    } else {
        astViewer_->clearAst();
    }
}

void Ide::updateDebugInfo() {
    auto vars = controller_->debugger()->getVariableSnapshot();
    debugPanel_->updateVariables(vars);

    auto stack = controller_->debugger()->getCallStack();
    debugPanel_->updateCallStack(stack);
}

void Ide::displayDiagnostics(const DiagnosticBag& bag) {
    for (const auto& diag : bag.all()) {
        QString text = QString::fromStdString(diag.format());
        if (diag.isError()) {
            outputPanel_->appendError(text);
        } else if (diag.isWarning()) {
            outputPanel_->appendOutput(QString("[警告] ") + text);
        } else {
            outputPanel_->appendOutput(text);
        }
    }

    // 标记编辑器错误行（EU-1 fix: 使用精确列范围）
    if (!bag.empty()) {
        std::vector<CodeEditor::ErrorRange> ranges;
        for (const auto& diag : bag.all()) {
            if (diag.isError() && diag.line > 0) {
                ranges.push_back({diag.line, diag.column, 0});
            }
        }
        if (!ranges.empty()) {
            codeEditor_->setErrorRanges(ranges);
        }
    }

    // 显示摘要（当有多条诊断时）
    if (bag.size() > 1) {
        outputPanel_->appendOutput(QString::fromStdString("--- " + bag.summary() + " ---"));
    }
}

void Ide::setRunningState(bool running) {
    bool isDebug = controller_->isDebugRun() && running;  // GUI-01 fix: 普通运行时禁用单步按钮
    runAction_->setEnabled(!running);
    debugAction_->setEnabled(!running);
    stepInAction_->setEnabled(isDebug);
    stepOverAction_->setEnabled(isDebug);
    stepOutAction_->setEnabled(isDebug);
    resumeAction_->setEnabled(isDebug);
    stopAction_->setEnabled(running);
    formatAction_->setEnabled(!running);
    bytecodeAction_->setEnabled(!running);
    codeEditor_->setReadOnly(running);
}

// ============================================================
// GUI-04: 文件操作实现
// ============================================================

void Ide::onNew() {
    if (!maybeSave()) return;
    codeEditor_->clear();
    currentFilePath_.clear();
    isDirty_ = false;
    codeEditor_->document()->setModified(false);
    updateWindowTitle();
}

void Ide::onOpen() {
    if (!maybeSave()) return;
    QString path = QFileDialog::getOpenFileName(this,
        QString::fromUtf8("打开文件"), QString(),
        "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty()) return;
    loadFile(path);
}

void Ide::onSave() {
    if (currentFilePath_.isEmpty()) {
        onSaveAs();
        return;
    }
    QFile file(currentFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QString::fromUtf8("错误"),
            QString::fromUtf8("无法保存文件: ") + file.errorString());
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << codeEditor_->toPlainText();
    file.close();
    isDirty_ = false;
    codeEditor_->document()->setModified(false);
    updateWindowTitle();
}

void Ide::onSaveAs() {
    QString path = QFileDialog::getSaveFileName(this,
        QString::fromUtf8("保存文件"), QString(),
        "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty()) return;
    currentFilePath_ = path;
    onSave();
}

bool Ide::maybeSave() {
    if (!isDirty_) return true;
    auto ret = QMessageBox::question(this,
        QString::fromUtf8("MiniLang IDE"),
        QString::fromUtf8("文件已修改，是否保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (ret == QMessageBox::Save) {
        onSave();
        return !isDirty_;
    }
    if (ret == QMessageBox::Cancel) return false;
    return true; // Discard
}

void Ide::updateWindowTitle() {
    QString title = "MiniLang IDE";
    if (!currentFilePath_.isEmpty()) {
        QFileInfo fi(currentFilePath_);
        title += " - " + fi.fileName();
    }
    if (isDirty_) title += " *";
    setWindowTitle(title);
}

void Ide::loadFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QString::fromUtf8("错误"),
            QString::fromUtf8("无法打开文件: ") + file.errorString());
        return;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    codeEditor_->setPlainText(in.readAll());
    file.close();
    currentFilePath_ = path;
    isDirty_ = false;
    codeEditor_->document()->setModified(false);
    updateWindowTitle();
}
