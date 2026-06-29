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
#include <QShortcut>
#include <QSettings>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <sstream>
#include <algorithm>  // A-P2-8 fix: std::min

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

    // B6 fix: REPL 面板通过 IdeController（业务层）间接执行，不直接持有 Interpreter*
    replPanel_->setController(controller_);

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

    // F9: 加载保存的主题偏好
    QSettings settings("MiniLang", "MiniLang IDE");
    bool savedDark = settings.value("theme/dark", false).toBool();
    darkThemeAction_->setChecked(savedDark);  // 触发 toggled → onToggleTheme → applyTheme

    // F13: 初始化自动补全
    setupCompletion();
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

    // B4 fix: REPL 异步任务在跑时弹窗告知用户关闭将阻塞（~ReplPanel 调用
    // replFuture_.wait() 阻塞至任务完成，因 std::async 析构语义无法限时取消）。
    // 让用户选择等待任务完成后再关，或强制关闭（仍会阻塞直到任务完成）。
    if (replPanel_->isReplRunning()) {
        auto ret = QMessageBox::warning(this,
            QString::fromUtf8("REPL 仍在执行"),
            QString::fromUtf8("REPL 有异步任务正在执行。\n关闭窗口将阻塞直至任务完成（极端死循环场景下需等待解释器的迭代上限触发 RuntimeError）。\n\n是否继续关闭？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    // 2026-06-29 审计修复 R4: 关闭路径优先使用 stopForClose（协作式超时），
    // 失败再 forceStop（terminate+_Exit 兜底）。
    // 原实现直接 forceStop，其 terminate 路径会调用 std::_Exit(0) 跳过所有析构，
    // 用户未保存的代码丢失。stopForClose 给 worker 3 秒协作式退出窗口，
    // 正常 MiniLang 程序（循环体含 checkBreak）几乎都能在此窗口内退出，
    // 避免 terminate 触发。仅当 worker 卡死（如原生 C++ 死循环）才回退 forceStop。
    if (controller_->isRunning()) {
        if (!controller_->stopForClose(3000)) {
            // 2026-06-29 审计修复 R5: 协作式超时失败，forceStop 即将进入
            // terminate+_Exit(0) 路径（跳过所有析构）。在此提示用户保存未保存的代码，
            // 避免 terminate 后用户数据丢失。_Exit 后进程立即退出，无法再弹窗。
            // 注：maybeSave() 已在 closeEvent 开头调用过，但用户可能在 maybeSave 后
            // 又编辑了代码（如关闭确认期间触发文本变更信号），此处再次检查 isModified
            // 作为最后防线。
            if (codeEditor_->document()->isModified()) {
                auto ret = QMessageBox::warning(this,
                    QString::fromUtf8("程序无响应，即将强制终止"),
                    QString::fromUtf8("解释器线程未在 3 秒内响应停止请求，将强制终止进程。\n"
                                      "强制终止会跳过正常析构，未保存的代码将丢失。\n\n"
                                      "是否现在保存？"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Save);
                if (ret == QMessageBox::Save) {
                    onSave();
                    // onSave 可能因文件打开失败而未实际保存，检查 modified 状态
                    if (codeEditor_->document()->isModified()) {
                        // 保存失败（用户取消另存为对话框或文件不可写），取消关闭
                        event->ignore();
                        return;
                    }
                } else if (ret == QMessageBox::Cancel) {
                    event->ignore();
                    return;
                }
                // Discard: 继续强制终止
            }
            // 回退 forceStop（可能触发 terminate+_Exit）
            controller_->forceStop();
        }
    }
    if (controller_->isVmRunning()) {
        onVmStop();
    }
    event->accept();
}

// #4 fix: 同步 VM 断点及条件到 VmStepper
void Ide::syncVmBreakpoints() {
    QSet<int> bps = codeEditor_->getBreakpoints();
    QMap<int, std::string> conds;
    for (int line : bps) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty()) conds[line] = cond;
    }
    controller_->setVmBreakpoints(bps);
    controller_->setVmBreakpointConditions(conds);
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

    // F9: 视图菜单（主题切换）
    auto* viewMenu = menuBar()->addMenu(QString::fromUtf8("视图(&V)"));
    darkThemeAction_ = new QAction(QString::fromUtf8("深色主题(&D)"), this);
    darkThemeAction_->setCheckable(true);
    darkThemeAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_T);
    connect(darkThemeAction_, &QAction::toggled, this, &Ide::onToggleTheme);
    viewMenu->addAction(darkThemeAction_);

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

    // 左侧：代码编辑器（含查找替换面板）
    auto* editorContainer = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(editorContainer);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);

    codeEditor_ = new CodeEditor(this);
    codeEditor_->setMinimumWidth(300);
    highlighter_ = new SyntaxHighlighter(codeEditor_->document());

    findReplacePanel_ = new FindReplacePanel(codeEditor_, this);
    editorLayout->addWidget(findReplacePanel_);
    editorLayout->addWidget(codeEditor_, 1);

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

    // 方向三：IR 中间表示视图
    irViewer_ = new IrViewer(this);
    rightTabWidget_->addTab(irViewer_, "IR 视图");

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

    mainSplitter_->addWidget(editorContainer);
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

    // 方向三：IR 中间表示
    irAction_ = toolbar->addAction("🔮 IR");
    irAction_->setToolTip("查看 IR 中间表示 (Ctrl+Shift+I)");
    irAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_I);

    toolbar->addSeparator();

    // ---- VM 调试按钮 ----
    vmStepAction_ = toolbar->addAction("👉 VM单步");
    vmStepAction_->setToolTip("单步执行字节码 (Ctrl+Shift+N)");
    vmStepAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    vmStepAction_->setEnabled(false);

    // A4 fix: 新增 VM 步进语义按钮（与 Interpreter 调试按钮对齐）
    vmStepOverAction_ = toolbar->addAction("⏭ VM跨过");
    vmStepOverAction_->setToolTip("VM 单步跨过函数调用 (Ctrl+Shift+O)");
    vmStepOverAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
    vmStepOverAction_->setEnabled(false);

    vmStepOutAction_ = toolbar->addAction("⤴ VM跨出");
    vmStepOutAction_->setToolTip("VM 跳出当前函数 (Ctrl+Shift+U)");
    vmStepOutAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_U);
    vmStepOutAction_->setEnabled(false);

    vmRunAction_ = toolbar->addAction("▶ VM运行");
    vmRunAction_->setToolTip("VM 全速运行（命中断点暂停）(Ctrl+Shift+R)");
    vmRunAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    vmRunAction_->setEnabled(false);

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
    connect(irAction_, &QAction::triggered, this, &Ide::onShowIR);

    // 条件断点：编辑器右键设置条件时同步到调试控制器
    connect(codeEditor_, &CodeEditor::breakpointConditionRequested,
            this, [this](int line, const QString& condition) {
                controller_->setBreakpointCondition(line, condition.toStdString());
            });

    // VM 调试连接
    connect(vmStepAction_, &QAction::triggered, this, &Ide::onVmStep);
    connect(vmStepOverAction_, &QAction::triggered, this, &Ide::onVmStepOver);
    connect(vmStepOutAction_, &QAction::triggered, this, &Ide::onVmStepOut);
    connect(vmRunAction_, &QAction::triggered, this, &Ide::onVmRun);
    connect(vmStopAction_, &QAction::triggered, this, &Ide::onVmStop);

    // F6: 查找替换快捷键
    auto* findShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(findShortcut, &QShortcut::activated, this, &Ide::onFind);
    auto* replaceShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    connect(replaceShortcut, &QShortcut::activated, this, &Ide::onReplace);
    auto* findNextShortcut = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findNextShortcut, &QShortcut::activated, this, &Ide::onFindNext);
    auto* findPrevShortcut = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    connect(findPrevShortcut, &QShortcut::activated, this, &Ide::onFindPrev);
    auto* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escShortcut, &QShortcut::activated, this, [this]() {
        if (findReplacePanel_ && findReplacePanel_->isVisible()) {
            findReplacePanel_->closePanel();
        }
    });

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

    // QT-R-01 fix: RUN 模式异步执行暂停时，通过信号通知 UI 更新
    connect(controller_, &IdeController::vmRunPaused, this, &Ide::handleVmStepResult);

    // B6 bug fix: 移除未使用的 vmStepInfo 信号连接（全代码库无 emit，死代码）

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

    // 互斥检查：REPL 异步任务在跑时拒绝运行，避免并发访问 Interpreter 数据竞争
    if (replPanel_->isReplRunning()) {
        outputPanel_->appendError("REPL 正在执行，请等待其完成后再运行");
        return;
    }

    if (!controller_->prepareRun(false, source, currentFilePath_.toStdString())) return;

    // 更新 UI
    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    // A-P1-5 fix: startWorker 失败时回滚 UI 状态，避免界面卡在"运行中"
    try {
        controller_->startWorker();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("启动失败: %1").arg(e.what()));
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    } catch (...) {
        outputPanel_->appendError("启动发生未知异常");
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    }
}

void Ide::onDebug() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // 清空输出和调试面板
    outputPanel_->clearAll();
    debugPanel_->clearAll();  // H7 fix: 清空旧调试数据
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 互斥检查：REPL 异步任务在跑时拒绝调试，避免并发访问 Interpreter 数据竞争
    if (replPanel_->isReplRunning()) {
        outputPanel_->appendError("REPL 正在执行，请等待其完成后再调试");
        return;
    }

    if (!controller_->prepareRun(true, source, currentFilePath_.toStdString())) return;

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

    // A-P1-5 fix: startWorker 失败时回滚 UI 状态，避免界面卡在"运行中"
    // P2 fix: setupDebug 也纳入异常保护，防止抛出时 UI 卡在"运行中"
    try {
        controller_->setupDebug(breakpoints, conditions);
        controller_->startWorker();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("启动调试失败: %1").arg(e.what()));
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    } catch (...) {
        outputPanel_->appendError("启动调试发生未知异常");
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    }
}

void Ide::onStepIn() {
    // P2 fix: 异常保护，防止 controller 抛出时 UI 状态不一致
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepIn();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("单步进入失败: %1").arg(e.what()));
    } catch (...) {
        outputPanel_->appendError("单步进入发生未知异常");
    }
}

void Ide::onStepOver() {
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOver();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("单步跳过失败: %1").arg(e.what()));
    } catch (...) {
        outputPanel_->appendError("单步跳过发生未知异常");
    }
}

void Ide::onStepOut() {
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOut();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("单步跳出失败: %1").arg(e.what()));
    } catch (...) {
        outputPanel_->appendError("单步跳出发生未知异常");
    }
}

void Ide::onResume() {
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->resume();
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("继续执行失败: %1").arg(e.what()));
    } catch (...) {
        outputPanel_->appendError("继续执行发生未知异常");
    }
}

void Ide::onStop() {
    controller_->stop();
}

void Ide::onClearOutput() {
    outputPanel_->clearAll();
}

void Ide::onPausedAt(int line) {
    // P2 fix: 异常保护调试回调
    try {
        codeEditor_->setCurrentLine(line);
        updateDebugInfo();
        // GUI-09 fix: 暂停时自动切换到调试面板
        bottomTabWidget_->setCurrentWidget(debugPanel_);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("调试信息更新失败: %1").arg(e.what()));
    } catch (...) {
        outputPanel_->appendError("调试信息更新发生未知异常");
    }
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

    // QT-R-10 fix: 大文件格式化时显示等待光标，避免用户以为 UI 冻结。
    // RAII 守卫确保所有 return 路径都恢复光标。
    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } cursorGuard;

    // C9 fix: 使用统一前端管线
    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        // 错误诊断已由 runLexer/runParser 内部 emit diagnosticsReady
        if (!pipelineResult.errorMessage.empty()) {
            outputPanel_->appendError(QString("[格式化] %1: %2")
                .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed ? "词法异常" : "解析异常")
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
        } else if (pipelineResult.diagnostics) {
            for (const auto& diag : pipelineResult.diagnostics->all()) {
                outputPanel_->appendError(QString::fromStdString("[格式化] " + diag.format()));
            }
        }
        updateAstViewer();
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) return;

    // 格式化：行号会变化，需清除断点并保存光标位置
    bool hadBreakpoints = controller_->hasBreakpoints();

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

    // QT-R-10 fix: 大文件字节码生成时显示等待光标，避免用户以为 UI 冻结。
    // RAII 守卫确保所有 return 路径都恢复光标。
    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } cursorGuard;

    // L15 fix: 清除编辑器中残留的错误行标记
    codeEditor_->clearErrorLines();
    // GUI-10 fix: 清除调试执行行高亮
    codeEditor_->clearCurrentLine();

    // C9 fix: 使用统一前端管线
    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        bytecodeList_->clear();
        lastBytecodeSourceHash_ = 0;  // D21 fix: 失效缓存
        if (!pipelineResult.errorMessage.empty()) {
            bytecodeList_->addItem(QString("字节码生成失败 - %1: %2")
                .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed ? "词法异常" : "解析异常")
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
        } else if (pipelineResult.diagnostics) {
            for (const auto& diag : pipelineResult.diagnostics->all()) {
                bytecodeList_->addItem(QString::fromStdString("[编译] " + diag.format()));
            }
        }
        updateAstViewer();
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) return;

    // 编译
    try {
        controller_->runCompiler();
    } catch (const std::exception& e) {
        bytecodeList_->clear();
        lastBytecodeSourceHash_ = 0;  // D21 fix: 失效缓存
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

    // 切换到字节码 Tab（方向三新增 IR Tab 后，用 setCurrentWidget 避免索引漂移）
    rightTabWidget_->setCurrentWidget(bytecodeList_->parentWidget());
}

// ============================================================
// 方向三：IR 中间表示可视化
// ============================================================

void Ide::onShowIR() {
    std::string source = codeEditor_->toPlainText().toStdString();

    // QT-R-10 fix: 大文件 IR 生成时显示等待光标
    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } cursorGuard;

    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 方向三：启用 IR 编译路径
    controller_->compiler().setUseIR(true);

    // C9 fix: 使用统一前端管线
    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        irViewer_->clearIR();
        // 失败时恢复 useIR=false，避免污染后续 run/debug 的编译路径
        controller_->compiler().setUseIR(false);
        if (!pipelineResult.errorMessage.empty()) {
            irViewer_->setIR(nullptr);
            // 显示错误信息
            outputPanel_->appendError(QString("IR 生成失败 - %1: %2")
                .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed ? "词法异常" : "解析异常")
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
        } else if (pipelineResult.diagnostics) {
            displayDiagnostics(*pipelineResult.diagnostics);
        }
        updateAstViewer();
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) {
        controller_->compiler().setUseIR(false);
        return;
    }

    // 编译（走 IR 路径）
    try {
        controller_->runCompiler();
    } catch (const std::exception& e) {
        irViewer_->clearIR();
        controller_->compiler().setUseIR(false);
        outputPanel_->appendError(QString("IR 编译异常: %1").arg(e.what()));
        return;
    }

    // 填充 IR 可视化面板
    populateIRViewer();

    // 恢复 useIR=false：onShowIR 仅用于 IR 可视化，不应影响后续 run/debug 的编译路径
    controller_->compiler().setUseIR(false);

    // 切换到 IR Tab
    rightTabWidget_->setCurrentWidget(irViewer_);
}

void Ide::populateIRViewer() {
    const IRFunction* ir = controller_->lastIR();
    irViewer_->setIR(ir);

    // 方向四：同步保存 IR→字节码偏移映射（供 VM 单步时高亮）
    irToBytecodeOffset_ = controller_->lastIRToBytecodeOffset();
}

// ============================================================
// 方向四：IR 调试器集成 — VM 单步时高亮对应 IR 指令
// ============================================================

void Ide::highlightIRLine(size_t bytecodeOffset) {
    if (irToBytecodeOffset_.empty()) return;
    irViewer_->highlightByBytecodeOffset(irToBytecodeOffset_, bytecodeOffset);
}

// ============================================================
// A4 fix: VM 调试槽函数
// ------------------------------------------------------------
// 4 种步进模式共享同一 UI 更新逻辑（handleVmStepResult），
// 仅 vmStepByMode 的模式参数不同。
// ============================================================

void Ide::onVmStep() {
    if (controller_->isVmRunning()) return;
    // P0-1 fix: 按后端选择检查对象——原代码仅检查 lastCompileResult()（栈式 VM 结果），
    // 但 useRegisterVM=true 时实际编译结果存在 getLastRegisterResult() 中，
    // lastCompileResult() 为空导致 RegisterVM 模式下所有调试按钮点击无反应。
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    // A4 fix: 每次步进前同步断点（用户可能在暂停期间增删断点）
    syncVmBreakpoints();  // #4 fix: 同步断点及条件

    // 禁用所有 VM 步进按钮防止重入
    setVmStepActionsEnabled(false);

    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_IN);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("VM 单步异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    } catch (...) {
        outputPanel_->appendError("VM 单步发生未知异常");
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    }

    handleVmStepResult(result);
}

void Ide::onVmStepOver() {
    if (controller_->isVmRunning()) return;
    // P0-1 fix: 按后端选择检查对象（详见 onVmStep 注释）
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    // A4 fix: 每次步进前同步断点
    syncVmBreakpoints();  // #4 fix: 同步断点及条件

    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OVER);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("VM 跨过异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    } catch (...) {
        outputPanel_->appendError("VM 跨过发生未知异常");
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    }
    handleVmStepResult(result);
}

void Ide::onVmStepOut() {
    if (controller_->isVmRunning()) return;
    // P0-1 fix: 按后端选择检查对象（详见 onVmStep 注释）
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    // A4 fix: 每次步进前同步断点
    syncVmBreakpoints();  // #4 fix: 同步断点及条件

    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OUT);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("VM 跨出异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    } catch (...) {
        outputPanel_->appendError("VM 跨出发生未知异常");
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    }
    handleVmStepResult(result);
}

void Ide::onVmRun() {
    if (controller_->isVmRunning()) return;
    // P0-1 fix: 按后端选择检查对象（详见 onVmStep 注释）
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    // A4 fix: 每次步进前同步断点
    syncVmBreakpoints();  // #4 fix: 同步断点及条件

    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::RUN);
    } catch (const std::exception& e) {
        outputPanel_->appendError(QString("VM 运行异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    } catch (...) {
        outputPanel_->appendError("VM 运行发生未知异常");
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    }
    // QT-R-01 fix: RUNNING 表示异步 RUN 已启动，等待 vmRunPaused 信号
    if (result == IdeController::VmStepResult::RUNNING) {
        // 异步运行中：禁用步进按钮，仅启用 Stop 按钮
        vmStepAction_->setEnabled(false);
        vmStepOverAction_->setEnabled(false);
        vmStepOutAction_->setEnabled(false);
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        return;
    }
    handleVmStepResult(result);
}

/// A4 fix: 处理 vmStepByMode 的结果，更新 UI（栈/全局变量/调用栈/高亮）
void Ide::handleVmStepResult(IdeController::VmStepResult result) {
    switch (result) {
    case IdeController::VmStepResult::NOT_READY:
        setVmStepActionsEnabled(true, /*running=*/false);
        return;

    case IdeController::VmStepResult::RUNNING:
        // QT-R-01 fix: 异步 RUN 已启动，不应走到这里（onVmRun 中已处理）
        // 防御性处理：仅启用 Stop 按钮
        vmStepAction_->setEnabled(false);
        vmStepOverAction_->setEnabled(false);
        vmStepOutAction_->setEnabled(false);
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        return;

    case IdeController::VmStepResult::ERROR: {
        Diagnostic diag(DiagLevel::Error, controller_->getVmLastError(),
                        controller_->getVmLastErrorLine(), 0, DiagSource::VM);
        outputPanel_->appendError(QString::fromStdString(diag.format()));
        if (controller_->getVmLastErrorLine() > 0) {
            QSet<int> errorLines;
            errorLines.insert(controller_->getVmLastErrorLine());
            codeEditor_->setErrorLines(errorLines);
        }
        vmStackPanel_->clearAll();
        setVmStepActionsEnabled(true, /*running=*/false);
        return;
    }

    case IdeController::VmStepResult::FINISHED:
        outputPanel_->appendOutput("--- VM 执行结束 ---");
        vmStackPanel_->clearAll();
        setVmStepActionsEnabled(true, /*running=*/false);
        return;

    case IdeController::VmStepResult::OK:
    case IdeController::VmStepResult::PAUSED_AT_BREAKPOINT:
        // 更新 UI：栈 + 全局变量 + 当前指令高亮 + 调用栈
        // A1 fix: RegisterVM 模式显示寄存器窗口；栈式 VM 模式显示操作数栈
        if (controller_->isVmRegisterMode()) {
            vmStackPanel_->updateRegisters(controller_->getVmStack());
        } else {
            vmStackPanel_->updateStack(controller_->getVmStack());
        }
        vmStackPanel_->updateGlobals(controller_->getVmGlobals());
        {
            size_t currentIP = controller_->getVmCurrentIP();
            // A1 fix: 统一使用 opCodeName 字符串，兼容 OpCode/RegOp
            std::string opName = controller_->getVmCurrentOpCodeName();
            int opLine = controller_->getVmCurrentLine();
            vmStackPanel_->updateCurrentOp(currentIP, opName, opLine);
            highlightBytecodeLine(controller_->getVmCurrentChunkName(), currentIP);
            // 方向四：同步高亮 IR 视图中对应的 IR 指令（仅 main chunk 时有效）
            if (controller_->getVmCurrentChunkName() == "main") {
                highlightIRLine(currentIP);
            }
        }
        // A4 fix: 同步断点行高亮（命中断点时跳转到该行）
        if (result == IdeController::VmStepResult::PAUSED_AT_BREAKPOINT) {
            int breakLine = controller_->getVmCurrentLine();
            if (breakLine > 0) {
                codeEditor_->setCurrentLine(breakLine);
                outputPanel_->appendOutput(
                    QString("🔴 VM 命中断点: 第 %1 行").arg(breakLine));
            }
        }
        setVmStepActionsEnabled(true, /*running=*/true);
        return;
    }
}

/// A4 fix: 批量启用/禁用 VM 步进按钮
/// running=true 表示 VM 处于暂停状态（可继续步进），需启用所有步进按钮
/// running=false 表示 VM 已停止/未初始化，仅启用 vmStep + vmRun，禁用 vmStop
void Ide::setVmStepActionsEnabled(bool enabled, bool running) {
    vmStepAction_->setEnabled(enabled);
    vmStepOverAction_->setEnabled(enabled);
    vmStepOutAction_->setEnabled(enabled);
    vmRunAction_->setEnabled(enabled);
    vmStopAction_->setEnabled(enabled && running);
    // 首次启动后禁用「查看字节码」按钮（避免运行中重新编译导致状态不一致）
    if (enabled && running) {
        bytecodeAction_->setEnabled(false);
    } else {
        bytecodeAction_->setEnabled(true);
    }
}

void Ide::onVmStop() {
    controller_->vmStop();
    vmStackPanel_->clearAll();
    // GUI-11 fix: 清除字节码列表当前行高亮
    bytecodeList_->setCurrentRow(-1);

    setVmStepActionsEnabled(true, /*running=*/false);
}

// ============================================================
// F6: 查找替换功能
// ============================================================

void Ide::onFind() {
    findReplacePanel_->showFind();
}

void Ide::onReplace() {
    findReplacePanel_->showReplace();
}

void Ide::onFindNext() {
    if (findReplacePanel_->isVisible()) {
        // 面板可见时由面板处理
        return;
    }
    // 面板不可见时，使用上次查找内容（显示面板）
    findReplacePanel_->showFind();
}

void Ide::onFindPrev() {
    if (findReplacePanel_->isVisible()) {
        return;
    }
    findReplacePanel_->showFind();
}

// ============================================================
// F9: 主题切换
// ============================================================

void Ide::onToggleTheme(bool dark) {
    applyTheme(dark);
    // 持久化保存主题偏好
    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("theme/dark", dark);
}

void Ide::applyTheme(bool dark) {
    isDarkTheme_ = dark;

    // 加载对应的 QSS 样式表
    QString qssResource = dark ? ":/styles_dark.qss" : ":/styles.qss";
    QFile styleFile(qssResource);
    if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&styleFile);
        qApp->setStyleSheet(ts.readAll());
        styleFile.close();
    }

    // 同步语法高亮器配色
    if (highlighter_) {
        highlighter_->setDarkTheme(dark);
    }

    // F9: 同步代码编辑器配色（行号区域、当前行高亮）
    if (codeEditor_) {
        codeEditor_->setDarkTheme(dark);
    }
}

// ============================================================
// F13: 自动补全
// ============================================================

void Ide::setupCompletion() {
    // 构建静态补全词列表：关键字 + 内置函数 + 内置方法
    staticCompletionWords_.clear();

    // 1. 从 Lexer 获取所有关键字（B6 fix: 通过语义化接口）
    const auto& keywords = controller_->getKeywords();
    for (const auto& kv : keywords) {
        staticCompletionWords_ << QString::fromStdString(kv.first);
    }

    // 2. 内置函数
    staticCompletionWords_ << "print" << "input"
                           << "len" << "type" << "str" << "int" << "abs"
                           << "min" << "max" << "range" << "sum";

    // 3. 常用内置方法（字符串/数组/字典）
    staticCompletionWords_ << "push" << "pop" << "split" << "join"
                           << "indexOf" << "startsWith" << "endsWith"
                           << "substr" << "keys" << "values" << "contains";

    // 4. 布尔常量
    staticCompletionWords_ << "true" << "false" << "null";

    // 去重并排序
    staticCompletionWords_.removeDuplicates();
    staticCompletionWords_.sort(Qt::CaseInsensitive);

    // 设置到编辑器
    codeEditor_->setCompletionWords(staticCompletionWords_);

    // 创建防抖定时器：文本变化后延迟 500ms 更新用户符号
    completionTimer_ = new QTimer(this);
    completionTimer_->setSingleShot(true);
    completionTimer_->setInterval(500);
    connect(completionTimer_, &QTimer::timeout, this, &Ide::updateCompletionWords);

    // 连接文本变化信号（防抖）
    connect(codeEditor_, &QPlainTextEdit::textChanged, this, [this]() {
        if (completionTimer_) {
            completionTimer_->start();
        }
    });

    // 首次更新（扫描默认示例代码中的符号）
    updateCompletionWords();
}

void Ide::updateCompletionWords() {
    if (!codeEditor_) return;

    // 从当前文档扫描用户定义的符号
    QString text = codeEditor_->toPlainText();

    // 合并静态词 + 用户符号
    QStringList words = staticCompletionWords_;

    // P2-5 fix: 先移除字符串字面量和块注释内容，避免正则误匹配字符串内的 var/fun/class
    // D13 fix: 正则支持转义序列，避免 "He said \"hello\"" 被错误拆分为两段，
    // 导致 hello 被当作代码扫描。模式 "(?:\\.|[^"\\\n])*" 匹配：
    //   "        起始引号
    //   (?:      非捕获组，重复以下二者之一：
    //     \\.    转义序列（反斜杠 + 任意字符，如 \" \\ \n）
    //     |      或
    //     [^"\\\n]  非引号、非反斜杠、非换行的任意字符
    //   )*       重复 0 次或多次
    //   "        结束引号
    text.remove(QRegularExpression("\"(?:\\\\.|[^\"\\\\\\n])*\""));
    text.remove(QRegularExpression("/\\*.*?\\*/", QRegularExpression::DotMatchesEverythingOption));

    // P2-2 fix: 使用 static 正则避免每次调用都重新编译
    // P2-4 fix: 扩展匹配模式，覆盖参数名、for 循环变量、类字段
    static const QRegularExpression pattern(
        "\\b(?:var|fun|class)\\s+([A-Za-z_][A-Za-z0-9_]*)");
    auto matchIt = pattern.globalMatch(text);
    QSet<QString> userSymbols;
    while (matchIt.hasNext()) {
        QRegularExpressionMatch match = matchIt.next();
        QString name = match.captured(1);
        if (!name.isEmpty()) {
            userSymbols.insert(name);
        }
    }

    // P2-3 fix: 使用 QSet 做 O(1) 查重，避免 words.contains() 的 O(n) 线性扫描
    QSet<QString> existingWords;
    for (const QString& w : words) {
        existingWords.insert(w.toLower());
    }
    for (const QString& sym : userSymbols) {
        if (!existingWords.contains(sym.toLower())) {
            words << sym;
        }
    }

    words.sort(Qt::CaseInsensitive);
    codeEditor_->setCompletionWords(words);
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
        // D7 fix: 使用 instructionSizeAt 统一处理 OP_CLOSURE 变长指令
        size_t offset = 0;
        while (offset < targetChunk->code.size()) {
            if (offset == ip) break;
            offset += targetChunk->instructionSizeAt(offset);
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

    // D21 fix: 计算当前源码哈希，若与上次相同则跳过全量重建。
    // 常见场景：用户多次点击"运行"而代码未修改，避免 O(n) 清空+重建列表。
    QString currentSource = codeEditor_ ? codeEditor_->toPlainText() : QString();
    size_t currentHash = qHash(currentSource);
    if (currentHash == lastBytecodeSourceHash_ && bytecodeList_->count() > 0) {
        return;  // 源码未变，复用现有列表
    }
    lastBytecodeSourceHash_ = currentHash;

    bytecodeList_->clear();
    chunkRowMap_.clear();

    if (compileResult.mainChunk.code.empty()) {
        bytecodeList_->addItem("(无字节码)");
        return;
    }

    bytecodeList_->setUpdatesEnabled(false);  // P6 fix: 批量填充时禁用重绘
    int currentRow = 0;
    // G-P2-3 fix: QFont 静态化，避免循环内重复构造
    static const QFont bytecodeFont("Consolas", 10);

    // ---- 主 chunk ----
    {
        int startRow = currentRow;
        size_t offset = 0;
        while (offset < compileResult.mainChunk.code.size()) {
            std::string instr = compileResult.mainChunk.disassembleInstruction(offset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(bytecodeFont);
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
        header->setFont(bytecodeFont);
        header->setForeground(QColor("#569CD6"));
        bytecodeList_->addItem(header);
        currentRow++;

        int startRow = currentRow;

        size_t funcOffset = 0;
        while (funcOffset < kv.second.code.size()) {
            std::string instr = kv.second.disassembleInstruction(funcOffset);
            auto* item = new QListWidgetItem(QString::fromStdString(instr));
            item->setFont(bytecodeFont);
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
    // A-P2-8 fix: 分页保护，防止超大 Token 列表（如 1M Token）创建过多行导致 UI 卡死
    static constexpr int MAX_DISPLAY = 10000;
    int displayCount = static_cast<int>(std::min(tokens.size(), static_cast<size_t>(MAX_DISPLAY)));
    tokenTable_->setRowCount(displayCount);
    tokenTable_->setUpdatesEnabled(false);
    for (int i = 0; i < displayCount; ++i) {
        const Token& tok = tokens[i];
        tokenTable_->setItem(i, 0, new QTableWidgetItem(
            QString::fromStdString(Token::typeToString(tok.type))));
        tokenTable_->setItem(i, 1, new QTableWidgetItem(
            QString::fromStdString(tok.lexeme)));
        tokenTable_->setItem(i, 2, new QTableWidgetItem(
            QString::fromStdString(tok.literalToString())));  // A1 fix: Token 字面量调试输出
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

    // 超出限制时提示
    if (tokens.size() > static_cast<size_t>(MAX_DISPLAY)) {
        outputPanel_->appendOutput(QString("[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条")
                                   .arg(tokens.size()).arg(MAX_DISPLAY));
    }
}

void Ide::updateAstViewer() {
    if (controller_->astRoot()) {
        astViewer_->setAst(controller_->astRoot());
    } else {
        astViewer_->clearAst();
    }
}

void Ide::updateDebugInfo() {
    auto vars = controller_->getDebugVariableSnapshot();
    debugPanel_->updateVariables(vars);

    auto stack = controller_->getDebugCallStack();
    debugPanel_->updateCallStack(stack);
}

void Ide::displayDiagnostics(const DiagnosticBag& bag) {
    // A-P2-13 fix: 缓存 bag.all() 一次，避免重复遍历（all() 可能返回拷贝或视图）
    const auto& allDiags = bag.all();
    for (const auto& diag : allDiags) {
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
        for (const auto& diag : allDiags) {
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
