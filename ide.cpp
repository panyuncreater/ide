#include "ide.h"
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
// InterpreterWorker 实现（OP-1 fix）
// ============================================================

void InterpreterWorker::run() {
    interp_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });

    try {
        interp_.execute(ast_);
        emit finishedOk();
    } catch (const RuntimeError& e) {
        emit runtimeError(QString::fromStdString(e.what()), e.line, e.column);
    } catch (const DebugStopException&) {
        emit stoppedByUser();
    } catch (const std::runtime_error& e) {
        emit genericError(QString::fromStdString(e.what()));
    } catch (const std::exception& e) {
        emit genericError(QString("未预期的错误: %1").arg(e.what()));
    } catch (...) {
        emit genericError("未预期的异常");
    }
}

// ============================================================
// Ide 主窗口实现
// ============================================================

Ide::Ide(QWidget* parent)
    : QMainWindow(parent) {

    debugger_ = new DebugController(this);
    interpreter_.setDebugger(debugger_);
    interpreter_.setOutputCallback([this](const std::string& text) {
        // REPL 和调试模式在主线程运行，直接调用；onRun() 会替换为线程安全的回调
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
    // #10 fix: 确保工作线程已停止再删除，避免 delete running QThread 的 UB
    if (workerThread_ && workerThread_->isRunning()) {
        debugger_->stop();
        workerThread_->quit();
        if (!workerThread_->wait(3000)) {
            workerThread_->terminate();
            workerThread_->wait();
        }
    }
    delete worker_;
}

void Ide::closeEvent(QCloseEvent* event) {
    // GUI-04: 关闭前检查未保存的修改
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    if (isRunning_) {
        // 先停止调试器，让解释器通过 DebugStopException 正常退出
        debugger_->stop();
        // OP-1 fix: 等待工作线程退出
        if (workerThread_) {
            workerThread_->quit();
            if (!workerThread_->wait(3000)) {
                // M5 fix: 线程未响应，弹窗让用户选择，避免盲目 terminate()
                auto ret = QMessageBox::warning(
                    this, tr("程序仍在运行"),
                    tr("程序未能在 3 秒内停止。强制终止可能导致数据丢失。\n是否强制终止？"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret == QMessageBox::Yes) {
                    workerThread_->terminate();
                    workerThread_->wait();
                } else {
                    // 用户选择等待：忽略关闭事件，让程序继续运行
                    event->ignore();
                    return;
                }
            }
            delete worker_;
            worker_ = nullptr;
            delete workerThread_;
            workerThread_ = nullptr;
        }
    }
    if (isVmRunning_) {
        onVmStop();
    }
    event->accept();
}

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

    // 条件断点：编辑器右键设置条件时同步到调试控制器
    connect(codeEditor_, &CodeEditor::breakpointConditionRequested,
            this, [this](int line, const QString& condition) {
                debugger_->setBreakpointCondition(line, condition.toStdString());
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
}

void Ide::onRun() {
    if (isRunning_) return;

    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出和调试面板
    outputPanel_->clearAll();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        runLexer(source);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("词法分析异常: %1").arg(e.what()));
        return;
    }

    // 如果有词法错误，不再继续解析
    if (lexer_.getDiagnostics().hasErrors()) return;

    // GUI-02 fix: 解析器异常保护
    try {
        runParser(lastTokens_);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("解析异常: %1").arg(e.what()));
        return;
    }

    // 如果有解析错误，不再继续执行
    if (parser_.hasErrors()) return;

    if (!astRoot_) return;

    // GUI-01 fix: 启用 debugMode 使 checkBreak 生效，stop 按钮才能触发 DebugStopException
    interpreter_.setDebugMode(true);
    isDebugRun_ = false;  // 普通运行，禁用单步按钮

    // OP-1 fix: 在独立线程中执行解释器，避免 UI 冻结
    isRunning_ = true;
    setRunningState(true);
    codeEditor_->setReadOnly(true);
    replPanel_->setInputEnabled(false);

    // R2 fix: 保存 REPL 状态（Run 的 execute() 会重置全局环境/类注册表）
    interpreter_.saveReplState();

    // 清理上次运行的 worker（若有）
    delete worker_;
    worker_ = new InterpreterWorker(interpreter_, *astRoot_, debugger_);
    workerThread_ = new QThread(this);
    worker_->moveToThread(workerThread_);

    // 输出信号 → 主线程 UI（跨线程自动排队）
    connect(worker_, &InterpreterWorker::outputReady, outputPanel_, &OutputPanel::appendOutput);

    // 各类结果信号 → UI 更新
    connect(worker_, &InterpreterWorker::finishedOk, this, [this]() {
        outputPanel_->appendOutput("--- 程序执行结束 ---");
    });
    connect(worker_, &InterpreterWorker::stoppedByUser, this, [this]() {
        outputPanel_->appendOutput("--- 调试终止 ---");
    });
    connect(worker_, &InterpreterWorker::runtimeError, this, [this](const QString& msg, int line, int column) {
        Diagnostic diag(DiagLevel::Error, msg.toStdString(), line, column, DiagSource::Interpreter);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        QSet<int> errorLines;
        errorLines.insert(line);
        codeEditor_->setErrorLines(errorLines);
    });
    connect(worker_, &InterpreterWorker::genericError, this, [this](const QString& msg) {
        outputPanel_->appendError(msg);
    });

    // worker 完成 → 清理运行状态
    connect(workerThread_, &QThread::finished, this, &Ide::onRunFinished);

    // 任一终止信号 → 退出线程事件循环（确保 QThread::finished 能被触发）
    connect(worker_, &InterpreterWorker::finishedOk, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::stoppedByUser, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::runtimeError, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::genericError, workerThread_, &QThread::quit);

    workerThread_->start();
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection);
}

void Ide::onDebug() {
    if (isRunning_) return;

    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出和调试面板
    outputPanel_->clearAll();
    debugPanel_->clearAll();  // H7 fix: 清空旧调试数据
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        runLexer(source);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("词法分析异常: %1").arg(e.what()));
        return;
    }

    // 如果有词法错误，不再继续解析
    if (lexer_.getDiagnostics().hasErrors()) return;

    // GUI-02 fix: 解析器异常保护
    try {
        runParser(lastTokens_);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("解析异常: %1").arg(e.what()));
        return;
    }

    // 如果有解析错误，不再继续
    if (parser_.hasErrors()) return;

    if (!astRoot_) return;

    // 设置断点
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());

    // 同步断点条件到调试控制器
    for (int line : codeEditor_->getBreakpoints()) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty()) {
            debugger_->setBreakpointCondition(line, cond);
        }
    }

    // GUI-03 fix: 使用 evaluateCondition 安全求值条件断点，避免重入损坏运行中的状态
    debugger_->setConditionEvaluator([this](const std::string& condition) -> bool {
        try {
            Lexer condLexer;
            auto tokens = condLexer.scan(condition);
            Parser condParser;
            auto block = condParser.parse(tokens);
            if (condParser.getErrors().empty() && block && !block->statements.empty()) {
                Value result = interpreter_.evaluateCondition(block->statements[0].get());
                return result.isTruthy();
            }
        } catch (...) {
            // 条件求值失赅视为 false（不暂停）
        }
        return false;
    });

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
            // 填充该帧的局部变量快照
            if (frame.env) {
                for (const auto& kv : frame.env->localVariables()) {
                    entry.locals.emplace_back(kv.first, kv.second);
                }
            }
            result.push_back(entry);
        }
        return result;
    });

    isRunning_ = true;
    isDebugRun_ = true;  // GUI-01 fix: 调试运行，启用单步按钮
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    // 调试模式：启用 checkBreak
    interpreter_.setDebugMode(true);

    // 重置调试状态，然后设置初始模式为 STEP_IN
    debugger_->reset();
    debugger_->stepIn();

    // D1 fix: 保存 REPL 状态
    interpreter_.saveReplState();

    // A2: 在 worker 线程上执行调试，避免 UI 冻结
    worker_ = new InterpreterWorker(interpreter_, *astRoot_, debugger_);
    workerThread_ = new QThread(this);
    worker_->moveToThread(workerThread_);

    // 跨线程信号连接（auto-queued）
    connect(worker_, &InterpreterWorker::outputReady,
            outputPanel_, &OutputPanel::appendOutput);
    connect(worker_, &InterpreterWorker::finishedOk, this, [this]() {
        outputPanel_->appendOutput("--- 程序执行结束 ---");
    });
    connect(worker_, &InterpreterWorker::runtimeError, this,
            [this](const QString& msg, int line, int column) {
        Diagnostic diag(DiagLevel::Error, msg.toStdString(), line, column, DiagSource::Interpreter);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        // GUI-07 fix: 调试模式下也标记错误行
        if (line > 0) {
            QSet<int> errorLines;
            errorLines.insert(line);
            codeEditor_->setErrorLines(errorLines);
        }
    });
    connect(worker_, &InterpreterWorker::stoppedByUser, this, [this]() {
        outputPanel_->appendOutput("--- 调试终止 ---");
    });
    connect(worker_, &InterpreterWorker::genericError, this,
            [this](const QString& msg) {
        outputPanel_->appendError(msg);
    });

    // 线程结束时清理
    connect(workerThread_, &QThread::finished, this, [this]() {
        isRunning_ = false;
        isDebugRun_ = false;
        setRunningState(false);
        replPanel_->setInputEnabled(true);
        interpreter_.setDebugMode(false);
        interpreter_.restoreReplState();
        codeEditor_->clearCurrentLine();
        debugger_->reset();

        // 恢复主线程输出回调
        interpreter_.setOutputCallback([this](const std::string& text) {
            outputPanel_->appendOutput(QString::fromStdString(text));
        });

        delete worker_;
        worker_ = nullptr;
        workerThread_->deleteLater();
        workerThread_ = nullptr;
    });

    // 所有终止信号 → 退出线程事件循环
    connect(worker_, &InterpreterWorker::finishedOk, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::stoppedByUser, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::runtimeError, workerThread_, &QThread::quit);
    connect(worker_, &InterpreterWorker::genericError, workerThread_, &QThread::quit);

    workerThread_->start();
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection);
}

void Ide::onStepIn() {
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());
    debugger_->stepIn();
    // updateDebugInfo() 由下一次 onPausedAt() 触发，避免在嵌套事件循环中重复更新
}

void Ide::onStepOver() {
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());
    debugger_->stepOver();
}

void Ide::onStepOut() {
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());
    debugger_->stepOut();
}

void Ide::onResume() {
    // 同步编辑器断点到调试控制器（用户可能在暂停期间修改了断点）
    debugger_->setBreakpoints(codeEditor_->getBreakpoints());
    debugger_->resume();
}

void Ide::onStop() {
    debugger_->stop();
    // 不在这里设置 isRunning_ 和按钮状态
    // onRunFinished() 或 onDebug() 中 interpreter_.execute() 返回后会统一清理
}

void Ide::onRunFinished() {
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_.setDebugMode(false);  // GUI-01 fix: 普通运行结束关闭 debugMode
    setRunningState(false);
    codeEditor_->clearCurrentLine();
    replPanel_->setInputEnabled(true);

    // 安全删除 worker（QThread::finished 在所有 worker 信号之后到达）
    delete worker_;
    worker_ = nullptr;

    if (workerThread_) {
        workerThread_->quit();
        workerThread_->wait();
        delete workerThread_;
        workerThread_ = nullptr;
    }

    // R3 fix: 恢复主线程输出回调（worker 的回调 lambda 捕获了已删除的 worker this 指针）
    interpreter_.setOutputCallback([this](const std::string& text) {
        outputPanel_->appendOutput(QString::fromStdString(text));
    });

    // R2 fix: 恢复 REPL 状态（变量/类定义/函数定义）
    interpreter_.restoreReplState();
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

void Ide::onFormat() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 词法分析
    try {
        lastTokens_ = lexer_.scan(source);
    } catch (const std::exception& e) {
        Diagnostic diag(DiagLevel::Error, e.what(), 0, 0, DiagSource::Lexer);
        outputPanel_->appendError(QString::fromStdString("[格式化] " + diag.format()));
        return;
    }

    // 语法分析
    try {
        astRoot_ = parser_.parse(lastTokens_);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("[格式化] 解析异常: %1").arg(e.what()));
        return;
    }
    if (parser_.hasErrors()) {
        for (const auto& diag : parser_.getDiagnostics().all()) {
            outputPanel_->appendError(QString::fromStdString(
                "[格式化] " + diag.format()));
        }
        return;
    }

    if (!astRoot_) return;

    // 格式化：行号会变化，需清除断点并保存光标位置
    bool hadBreakpoints = !debugger_->getBreakpoints().isEmpty();

    QTextCursor savedCursor = codeEditor_->textCursor();
    int scrollPos = codeEditor_->verticalScrollBar()->value();

    // F1 fix: 传入注释 token，使格式化后保留注释
    formatter_.setComments(lastTokens_);
    try {
        std::string formatted = formatter_.format(*astRoot_);
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
    debugger_->setBreakpoints(QSet<int>());

    // D3 fix: 刷新 AST 查看器（格式化后 astRoot_ 已更新）
    astViewer_->setAst(astRoot_.get());

    // 恢复光标位置和滚动位置（尽可能）
    if (savedCursor.position() <= codeEditor_->document()->characterCount()) {
        codeEditor_->setTextCursor(savedCursor);
    }
    codeEditor_->verticalScrollBar()->setValue(scrollPos);
}

void Ide::onShowBytecode() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // L15 fix: 清除编辑器中残留的错误行标记
    codeEditor_->clearErrorLines();
    // GUI-10 fix: 清除调试执行行高亮
    codeEditor_->clearCurrentLine();

    // 词法分析
    try {
        lastTokens_ = lexer_.scan(source);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("字节码生成失败 - 词法错误: %1").arg(e.what()));
        return;
    }

    // 语法分析
    try {
        astRoot_ = parser_.parse(lastTokens_);
    } catch (const std::exception& e) {
        bytecodeList_->clear();
        bytecodeList_->addItem(QString("字节码生成失败 - 解析异常: %1").arg(e.what()));
        return;
    }
    if (parser_.hasErrors()) {
        bytecodeList_->clear();
        for (const auto& diag : parser_.getDiagnostics().all()) {
            bytecodeList_->addItem(QString::fromStdString(
                "[编译] " + diag.format()));
        }
        return;
    }

    if (!astRoot_) return;

    // D3 fix: 刷新 AST 查看器
    astViewer_->setAst(astRoot_.get());

    // 编译
    try {
        runCompiler();
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

    // 重置 VM 状态（确保单步模式从头开始）
    vm_.resetState();
    isVmInitialized_ = false;

    // 切换到字节码 Tab
    rightTabWidget_->setCurrentIndex(2);
}

void Ide::onVmStep() {
    if (isVmRunning_) return;
    if (lastCompileResult_.mainChunk.code.empty()) return;

    // 首次点击：初始化 VM 执行环境
    if (!isVmInitialized_) {
        vm_.initExecution(lastCompileResult_);
        isVmInitialized_ = true;
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
        Diagnostic diag(DiagLevel::Error, vm_.getLastError(), vm_.getLastErrorLine(), 0, DiagSource::VM);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        if (vm_.getLastErrorLine() > 0) {
            QSet<int> errorLines;
            errorLines.insert(vm_.getLastErrorLine());
            codeEditor_->setErrorLines(errorLines);
        }
        vmStackPanel_->clearAll();
        isVmInitialized_ = false;
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
    // GUI-11 fix: 清除字节码列表当前行高亮
    bytecodeList_->setCurrentRow(-1);

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

    bytecodeList_->setUpdatesEnabled(false);  // P6 fix: 批量填充时禁用重绘
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
    bytecodeList_->setUpdatesEnabled(true);  // P6 fix: 恢复重绘
}

void Ide::runLexer(const std::string& source) {
    lastTokens_ = lexer_.scan(source);

    // 更新 Token 列表
    tokenTable_->setRowCount(static_cast<int>(lastTokens_.size()));
    tokenTable_->setUpdatesEnabled(false);  // P6 fix: 批量填充时禁用重绘
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
        }
    }
    tokenTable_->setUpdatesEnabled(true);   // P6 fix: 恢复重绘
    tokenTable_->resizeColumnsToContents();

    // 使用统一诊断显示词法错误
    displayDiagnostics(lexer_.getDiagnostics());
}

void Ide::runParser(const std::vector<Token>& tokens) {
    astRoot_ = parser_.parse(tokens);

    // 使用统一诊断显示解析错误
    displayDiagnostics(parser_.getDiagnostics());

    // GUI-08 fix: 解析失败时清空旧 AST 视图
    if (astRoot_) {
        astViewer_->setAst(astRoot_.get());
    } else {
        astViewer_->clearAst();
    }
}

void Ide::runCompiler() {
    if (!astRoot_) return;

    lastCompileResult_ = compiler_.compile(*astRoot_);

    // 使用统一诊断显示编译错误
    displayDiagnostics(compiler_.getDiagnostics());
}

void Ide::updateDebugInfo() {
    auto vars = debugger_->getVariableSnapshot();
    debugPanel_->updateVariables(vars);

    auto stack = debugger_->getCallStack();
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
                // 使用 0 表示"从列到行尾"，setErrorRanges 会处理
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
    bool isDebug = isDebugRun_ && running;  // GUI-01 fix: 普通运行时禁用单步按钮
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
        return !isDirty_; // onSave 失败时 isDirty_ 仍为 true
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
