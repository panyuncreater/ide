# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-09 · 第五十五轮审计：Worker/调试器/GUI 未覆盖区域深度排查与修复（P1 × 2 + P2 × 6 + P3 × 7，共 15 项）

### 概述

本轮对前 54 轮未充分覆盖的区域进行四模块并行 agent 深度审计：Worker 线程层（InterpreterWorker/WorkerManager/PipelineRunner）、VM 执行层（VM/VMCalls/VMContainers）、调试器层（DebugController/DebugCoordinator/VmStepper）、未覆盖 GUI 面板（VmStackPanel/WelcomeWizard/MagicCommands/SyntaxHighlighter/BytecodeTracePanel/AstViewer 等）。P1 级修复 PipelineRunner 直接调用 runLexer/runParser 不失效缓存导致编辑器代码被执行为关卡代码、MagicCommands %disassemble 字节码解析未跳过操作数字节导致输出完全错误。P2 级修复 WorkerManager cleanupWorker 异常时回调未恢复/销毁顺序违反 Qt 线程亲和性/wait 无超时、VmStepper 步进暂停路径未重置 vmCrossedLine_ 导致断点重复触发、DebugController 步进模式 hitCount 过度递增、DebugCoordinator 析构未唤醒暂停的 worker、WelcomeWizard Esc 键绕过 onSkip 导致 completed_ 未置位。P3 级涵盖 DebugCoordinator 变量快照改用拷贝避免迭代器失效、AstViewer/VmStackPanel/BytecodeTracePanel 空状态提示、BytecodeTracePanel HTML 转义补全、SyntaxHighlighter 块注释深度编码溢出钳制等。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 缓存一致性 | `app/PipelineRunner.cpp` runLexer/runParser | **直接调用 runLexer/runParser 不失效前端管线缓存**——VmStackSandboxPanel 加载关卡源码时调用 runLexer/runParser，修改了 lexer_/parser_/astRoot_ 内部状态，但 cachedPipelineSource_ 仍为编辑器源码。后续 runFrontendPipeline 命中陈旧缓存，返回的 diagnostics 裸指针指向已被改写的 DiagnosticBag，astRoot_ 已被替换为关卡代码 AST。用户点击"运行"编辑器代码实际执行关卡代码。`invalidatePipelineCache()` 方法已存在但从未被调用。修复：runLexer/runParser 入口调用 invalidatePipelineCache()。 |
| 2 | P1 | 语义错误 | `gui/MagicCommands.cpp` handleDisassemble | **%disassemble 字节码解析未跳过操作数字节**——原实现循环只 `++offset`，未按指令实际长度前进，把操作数字节当作独立 opcode 解析，输出完全错误（如 OP_INT 的 2 字节常量索引显示为两个虚假 opcode）。修复：改用 chunk.disassembleInstruction(offset)，它按 kOpCodeInfo 元数据正确前进 offset（含变长 OP_CLOSURE），与 Bytecode.cpp 的 disassemble() 一致。 |
| 3 | P2 | 错误传播 | `app/WorkerManager.cpp` finished lambda | **cleanupWorker 异常时 interpreter 回调未恢复**——cleanupWorker 前半部分（restoreReplState/debugger->reset/workerThread quit+wait）可能抛异常，finished lambda 的 catch 块仅设 isRunning_=false 不调用 setupMainCallbacks()，interpreter 的 output/input 回调保持 worker 的 no-op，导致 REPL 输出静默丢失、input() 返回空串。修复：catch 块补充 setupMainCallbacks() 调用。 |
| 4 | P2 | 线程亲和性 | `app/WorkerManager.cpp` cleanupWorker + prepareRun catch | **worker_ 与 workerThread_ 销毁顺序错误**——cleanupWorker 和 prepareRun catch 先 reset workerThread_ 再 reset worker_，worker 析构时 Qt 内部可能访问 worker->thread()，此时 QThread 对象已销毁，指针悬垂 → 潜在 UAF。~WorkerManager 的顺序正确（先 worker 后 thread）。修复：统一三处为 quit+wait → reset worker_ → reset workerThread_。 |
| 5 | P2 | 死锁风险 | `app/WorkerManager.cpp` cleanupWorker + prepareRun catch | **workerThread_->wait() 无超时**——与 ~WorkerManager(5000)/stopForClose(timeout)/forceStop(5000) 不一致，线程终止异常时永久阻塞主线程导致 UI 冻结。修复：统一为 wait(5000) + 超时日志。 |
| 6 | P2 | 断点重复触发 | `app/VmStepper.cpp` stepByMode | **步进暂停路径未重置 vmCrossedLine_**——stepByMode 入口仅重置 vmCrossedDeeper_ 不重置 vmCrossedLine_，断点命中路径(L230)重置但步进暂停路径(L288-292)不重置。若上次步进跨行后暂停，vmCrossedLine_ 残留 true，下次步进首条指令若同行则不刷新(L221-224 仅行号变化时置 true)，断点检查(L226)因 vmCrossedLine_ 残留 true 立即重复触发，用户卡在当前行无法步进。修复：入口同步重置 vmCrossedLine_ = false。 |
| 7 | P2 | 三后端不一致 | `debug/DebugController.cpp` checkBreak | **步进模式 hitCount 过度递增**——原实现(M10+DBG-04 fix)在步进模式下经过断点行时即递增 hitCount，不区分是否暂停。STEP_OVER 进入更深帧期间经过断点行时 hitCount 被反复刷高，BreakpointConditionPanel 显示的命中次数远超实际暂停次数。与 VmStepper（仅在断点命中路径递增）和 RUN 模式（shouldPauseAtBreakpoint 仅在命中时递增）不一致。修复：仅在 shouldPause == true 时递增。 |
| 8 | P2 | UAF 风险 | `app/DebugCoordinator.cpp` ~DebugCoordinator | **析构未唤醒暂停的 worker**——原实现仅清空回调 + waitCallbacksIdle，但 worker 若阻塞在 pauseCV_.wait()，不在 callback 中（activeCallbackCount_==0），waitCallbacksIdle 立即返回。随后 interpreter_ 被释放，worker 醒来后访问已释放 interpreter → UAF。修复：清空回调前先调用 debugger_->stop() 设 stopped_=true 并 notify_one，worker 醒来后抛 DebugStopException 终止。 |
| 9 | P2 | 状态不一致 | `gui/WelcomeWizard.cpp` keyPressEvent | **Esc 键绕过 onSkip 导致 completed_ 未置位**——WelcomeWizard 未重写 keyPressEvent，按 Esc 走 QDialog 默认 reject() 不经过 onSkip()，completed_ 保持 false，下次启动再次弹出向导。点「跳过」按钮则正确设置 completed_=true。两种关闭方式语义不一致。修复：重写 keyPressEvent，Esc 调用 onSkip()。 |
| 10 | P3 | 线程安全 | `app/DebugCoordinator.cpp` variableCallback | **变量快照用 const 引用而非拷贝**——localVariables() 返回 const 引用，遍历期间 worker 线程修改 variables map 会导致迭代器失效。shared_ptr 防止 Environment 析构但不防止 map 内容被修改。修复：改用 snapshotLocalVariables() 返回拷贝。 |
| 11 | P3 | 空状态 | `gui/AstViewer.cpp` clearAst | **clearAst 后场景完全空白**——setAst(nullptr) 有占位提示(R52-6)但 clearAst() 无，两个语义相近的函数空状态行为不一致。修复：clearAst 末尾添加与 setAst(nullptr) 相同的占位文本。 |
| 12 | P3 | 空状态 | `gui/VmStackPanel.cpp` clearAll | **clearAll 后列表无占位提示**——stackList_ 清空后完全空白，用户无法确认面板是否正常。修复：插入禁用样式的「(已清空)」占位项，对齐 DebugPanel R53-6 模式。 |
| 13 | P3 | 空状态 | `gui/BytecodeTracePanel.cpp` onClearTrace | **onClearTrace 后 stackDetail_ 空白无占位**——用户无法区分「已清空」与「未选中行」。修复：setHtml 设置占位提示「(轨迹已清空，捕获后将显示栈快照)」。 |
| 14 | P3 | HTML 注入 | `gui/BytecodeTracePanel.cpp` onTraceRowSelected + showDoc | **opCodeName 及教学库多字段未 HTML 转义**——onTraceRowSelected 的 e.opCodeName 直接 .arg() 插入 HTML（对比栈快照已转义）；showDoc 的 d.opCodeName/d.category/d.operandFormat/d.stackEffect 同样未转义（仅 d.exampleCode 转义）。与 R52 批量转义修复不一致。修复：统一调用 toHtmlEscaped()。 |
| 15 | P3 | 编码溢出 | `gui/SyntaxHighlighter.cpp` highlightBlock | **块注释深度编码在 depth≥10 时溢出**——组合状态编码 `300 + interpBraceDepth*10 + blockCommentDepth`，blockCommentDepth 占个位(mod 10)，达 10 时溢出到 interpBraceDepth 导致状态损坏。修复：编码前 std::min(blockCommentDepth, 9) 钳制——10 层嵌套块注释极少见，钳制仅影响高亮不影响语义。 |

### 评估保留现状（3 项）

- **P3 VmStepper processEvents 未排除定时器事件**：ExcludeUserInputEvents 不排除定时器，500ms 轮询定时器可能在 STEP 中途触发。当前各面板已有 isVmRunning() 守卫防御，VmStepper 层面无强制保证但作为已知限制可接受。彻底修复需 sendPostedEvents 替代或引入 isVmStepping_ 标志，改动较大。保留现状。
- **P3 DebugController isPaused() 两次原子读非原子组合**：paused_ && !stopped_ 两次独立原子 load，极小窗口内可能返回不一致状态，仅一帧内视觉抖动无功能性后果。保持现状。
- **P2 VmStepper STEP_OUT 栈底使用 vmCrossedLine_ 而非 vmCrossedDeeper_**：注释与代码矛盾已更正。STEP_OUT 在栈底时无处可"跨出"，降级为"跨行后暂停"语义（使用 vmCrossedLine_）是合理设计，与 STEP_IN 行级粒度一致。保留现状。

### 测试影响

- 全部 15 项修复均无回归。全量 1763/1763 测试通过。

## 2026-07-09 · 第五十四轮审计：GC 栈溢出修复 + 未覆盖区域 Bug 排查 + 性能优化（P1 × 1 + P2 × 2 + P3 × 1 + Perf × 3，共 7 项）

### 概述

本轮聚焦前 53 轮未覆盖的 Bug 区域（GC 深嵌套栈溢出、模块路径 hash 一致性、REPL 多行历史）与性能优化机会。P1 级修复 GcManager markValue 纯递归遍历在深嵌套容器链（如 100000 层 `[[[[...]]]]`）下的栈溢出风险，改为显式 worklist 迭代式，同时消除递归调用开销（一举两得）。P2 级修复 ModuleIsolation pathHash 未折叠连续 `/` 导致同一模块产生不同 hash、ReplPanel 多行输入历史仅保存首行导致 Up 键无法恢复完整多行输入。P3 级修复 REPL 历史去重（连续相同命令不重复追加）。性能优化 3 项：DebugController RUN 快速路径移除冗余原子操作、IdeController notifyVmStateChanged 消除 vector 拷贝、Lexer string() 批量扫描连续普通字符。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 栈溢出 | `interpreter/GcManager.cpp` markValue | **markValue 纯递归遍历深嵌套容器链栈溢出**——原实现对 ARRAY/DICT/INSTANCE/CLOSURE 四种堆类型纯递归遍历子元素。MiniLang 的 MAX_LOOP_ITERATIONS=10000000 允许 while 循环构建极深嵌套的线性容器链（如 100000 层 `[[[[...]]]]`），这条链不是循环引用（每层不同对象），GC mark 阶段递归深度等于链长度，触发栈溢出崩溃。修复：改为显式 worklist 迭代式——`std::vector<const Value*> worklist`，将子元素 push_back 到 worklist 而非递归调用，栈深度恒定。同时解决递归调用开销（Perf 双重收益）。 |
| 2 | P2 | 路径一致性 | `ast/ModuleIsolation.cpp` pathHash | **pathHash 未折叠连续 `/`**——`"./a//b"` 与 `"./a/b"` 经 normalize（`\`→`/` + 去 `./` 前缀）后仍含 `//`，FNV-1a hash 产生不同值，导致同一模块被当作两个不同模块，`__mod_<hash>__` 前缀不一致引发重命名解析失败。修复：normalize 阶段后增加连续 `/` 折叠（保留单个 `/`）。 |
| 3 | P2 | 数据丢失 | `gui/ReplPanel.cpp` executeLine | **多行输入历史仅保存首行**——续行模式下用户输入 `fun foo() {` 后按 Enter 进入续行，首行被存入 history_。完整输入结束后执行 pendingInput_（含 `\n` 拼接的续行），但 history_ 仍为首行。按 Up 键恢复时只能恢复首行，无法恢复完整多行输入。修复：执行前用完整 pendingInput_ 替换之前存入的首行（仅当 historyIndex_ 指向末尾时，避免覆盖用户已浏览的历史位置）。 |
| 4 | P3 | UX | `gui/ReplPanel.cpp` executeLine | **历史不去重**——连续输入相同命令（如多次 `print(1)`）会在 history_ 中追加多条相同记录，按 Up 键需翻越多次才能到达上一条不同命令。修复：若 trimmedLine 与 history_.back() 相同则不追加，对齐主流 REPL（Python/Node/bash）行为。 |
| 5 | Perf | 原子操作 | `debug/DebugController.cpp` checkBreak | **RUN 快速路径冗余原子操作**——原 D-P1-1 快速路径（RUN 模式 + 无断点）在返回前调用 updateLineTracking，每节点执行 2 次原子 load（line/lastSeenLine_）+ 1-2 次原子 store（crossedLine_/lastSeenLine_）。紧密循环百万级节点累积可观开销。修复：移除快速路径的 updateLineTracking 调用。快速路径前提是无断点，crossedLine_/lastSeenLine_ 不被任何逻辑读取；用户添加断点后 hasBreakpoints_ 变 true 进入慢速路径，updateLineTracking 会重新计算。回退 D-P2-11 fix。 |
| 6 | Perf | vector 拷贝 | `app/IdeController.h` notifyVmStateChanged | **notifyVmStateChanged 每次拷贝整个 vector**——原实现 `auto listeners = vmStateChangedListeners_` 拷贝整个 vector（含 std::function）以防止回调期间 push_back 导致迭代器失效，但每次通知都拷贝 6 个 std::function 开销可观。修复：改用索引迭代 `for (size_t i = 0; i < n; ++i)` + 边界检查 `if (i < vmStateChangedListeners_.size())`，配合 addVmStateChangedListener 的 reserve 预留容量策略，避免拷贝。 |
| 7 | Perf | 逐字符处理 | `lexer/Lexer.cpp` string | **string() 普通字符逐字符 advance + value += char**——原实现字符串普通字符路径 `value += advance()` 逐字符调用 advance()（含函数调用开销 + 行号维护检查）+ value += char（可能触发多次 realloc）。对长字符串（如 1KB 文本）产生 1024 次 advance 调用。修复：批量扫描连续普通字符（直到遇 `\`、`{`、`"`、`\r`、`\n` 或 EOF），用 `value.append(source_.data() + runStart, runLen)` 一次性追加。多字节 UTF-8 字节（0x80-0xFF）不与特殊字符冲突，可安全批量扫描。 |

### 测试影响

- 全部 7 项修复均无回归。全量 1763/1763 测试通过。

## 2026-07-09 · 第五十三轮审计：app 层运行守卫与 UX 体验优化（P2 × 3 + P3 × 6 + 回归修复 × 1，共 10 项）

### 概述

本轮覆盖前 52 轮未审计的 app 层（ide.cpp ~6000 行）与 Worker 线程边界，并完成 UX 体验优化。P2 级聚焦运行态守卫前置：IdeController 跨线程裸指针访问、onRun/onDebug 缺 isRunning 守卫导致调试上下文被误清、onCompileAnalysis 缺运行守卫导致并发管线污染。P3 级涵盖 IrViewer/DebugPanel 空状态提示、编译分析状态栏进度反馈、关闭运行中标签告警、REPL Esc 中止续行 + Ctrl+L 清屏、CodeEditor Ctrl+Shift+D 复制行 + Alt+Up/Down 移动行等 UX 优化，以及 InterpreterModules 循环依赖检测后防御性 return。另回滚 AUDIT-P3-ROUND53 的"123. 报错"逻辑——该 fix 违反项目设计契约（TestLexer/LexerAudit 期望分词为 INT+DOT），导致 2 个测试回归。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P2 | 跨线程裸指针 | `app/IdeController.cpp` getReplScopeVariableNames | **跨线程裸 currentEnvironment() 访问**——该方法可能在 worker 线程 emit runtimeError 后被主线程 QueuedConnection 处理器调用，裸指针遍历 parent 链违反 Interpreter.h 跨线程契约。修复：改用 currentEnvironmentShared() 获取 shared_ptr，遍历 parent 链时通过赋值延长每个节点生命周期，对齐 DebugCoordinator 的 AUDIT-P1 修复模式。 |
| 2 | P2 | 运行守卫缺失 | `app/ide.cpp` onRun/onDebug | **isRunning/isVmRunning 守卫在破坏性清理之后**——原实现依赖 prepareRun 内部守卫，但 clearOutput/clearAll/clearErrorLines 在 prepareRun 之前执行，运行中误按 F5/F6 会先清空调试上下文（断点高亮、调用栈、变量快照、当前行高亮）再被 prepareRun 拒绝。F5/F6 工具栏 action 虽已禁用，但菜单 action 与键盘快捷键仍可能触发。修复：在 replPanel_ 检查后、clearOutput 之前增加 isRunning/isVmRunning/isDebugPaused 三态守卫。 |
| 3 | P2 | 并发管线污染 | `app/ide.cpp` onCompileAnalysis | **运行/调试中重跑前端管线**——原实现先 clearErrorLines/clearCurrentLine 再 runFrontendPipeline，会清掉调试暂停时的当前行高亮、并替换 pipeline_ 内部 lexer/parser/astRoot 状态——若 worker 线程正在使用同一 pipeline_ 解析模块源码（import 路径），将产生并发数据竞争（lexer/parser 非线程安全）。修复：入口增加运行守卫，运行中仅打开右侧可视化面板查看当前已编译结果，不重新执行管线。 |
| 4 | P3 | 防御性 | `interpreter/InterpreterModules.cpp` visitImportStmt | **循环依赖/深度超限检测后无 return**——当前 runtimeError 是 throw 语义，缺 return 不可达；但若未来 runtimeError 改为非抛出式错误处理（错误码/返回值），缺少 return 会继续执行后续加载流程，违反"循环依赖即停止"语义。修复：两处 runtimeError 后添加防御性 return。 |
| 5 | P3 | 空状态 | `gui/IrViewer.cpp` clearIR | **clearIR 后 browser 完全空白**——用户无法区分"尚未编译"与"编译产物为空"。修复：setText 设置占位提示"尚无 IR 输出。请先点击「编译分析」或运行程序，再切换到 IR 视图查看。"，后续 setIR/setHtml 会覆盖。 |
| 6 | P3 | 空状态 | `gui/DebugPanel.cpp` clearAll | **clearAll 后 variableTree/callStackList 完全空白**——QTreeWidget/QListWidget 无原生 placeholder。修复：插入禁用样式的占位项（灰色、不可选不可启用），populateXxx 入口先 clear() 不污染后续真实数据。 |
| 7 | P3 | 进度反馈 | `app/ide.cpp` onCompileAnalysis | **同步编译阶段仅 WaitCursor 鼠标反馈，状态栏无文字提示**——大文件编译时用户感知不到进度。修复：RAII guard 统一管理 cursor 与状态栏消息，开始时 showMessage("正在编译分析...")，结束（含所有 return 路径）时 clearMessage()。 |
| 8 | P3 | 运行守卫 | `app/ide.cpp` onEditorTabCloseRequested | **运行/调试中关闭标签会破坏调试上下文**——currentFilePath_ 切换、断点清空、editorTabs_ 重组，导致 worker 线程引用的 source/filePath 失配。原实现无任何守卫。修复：入口增加 isRunning/isVmRunning/isDebugPaused 守卫，运行中拒绝关闭并提示用户先停止。 |
| 9 | P3 | 键盘交互 | `gui/ReplPanel.cpp` eventFilter | **REPL 无 Esc 中止续行 + 无 Ctrl+L 清屏**——续行模式下用户只能继续输入完整代码，无法中止（多行结构如未闭合的 fun 定义一旦开始就必须完成）；清屏需输入 'clear' 命令。修复：Esc 在续行模式下中止 pendingInput_ 回到单行模式；Ctrl+L 清空 outputArea_（保留 pendingInput_/history/变量状态），运行中拒绝以避免与 worker 输出竞争。 |
| 10 | P3 | 编辑器增强 | `gui/CodeEditor.cpp` keyPressEvent | **缺 Ctrl+Shift+D 复制行 + Alt+Up/Down 移动行**——常见编辑器快捷键缺失，影响代码编辑/重组效率。修复：Ctrl+Shift+D 复制当前行（或选区）到下一行，支持选区副本保持原选区选中；Alt+Up/Down 移动当前行（或选区行块）上/下，自动重新选中移动后的块。与现有 Ctrl+D（选中下一个相同单词）、Ctrl+Shift+K（删除当前行）不冲突。 |
| 回归 | P2 | 回归修复 | `lexer/Lexer.cpp` number | **回滚 AUDIT-P3-ROUND53 的"123. 报错"逻辑**——该 fix 让 "1." 和 "123.foo" 报错"数字字面量小数点后需有数字"，但 TestLexer::Number_IntegerFollowedByDotAndIdentifier 和 LexerAudit::TrailingDecimalPoint 明确期望分词为 INT + DOT（+ IDENTIFIER），这是项目的设计契约——MiniLang 不支持方法调用语法 123.foo()，但 Lexer 应保持宽容分词，将语义判断交给 Parser。原 fix 改变了既定行为，导致 2 个测试回归。修复：删除该 fix 块，恢复"123. 后无数字/指数 → 分词为 TK_INT_LIT + TK_DOT"的行为。 |

### 评估保留现状（3 项）

- **P3-UX3 全局 Esc 仅关闭查找面板**：项目中主要的浮层就是 FindReplacePanel（已正确处理），QDialog/QMenu 自带 Esc 关闭，扩展到 QDockWidget 会改变用户预期。保留现状。
- **P3-UX4 断点跨会话持久化**：需 QSettings 序列化 + 文件路径键管理 + 文件移动/重命名处理，改动较大且重启 IDE 重新设置断点成本不高。保留现状。
- **P3-UX6 调试暂停期编辑器完全只读**：已实现（setRunningState true 时 setReadOnly，VM 模式 L5758-5760 BUG-ORCH-5 fix）。无需修改。

### 测试影响

- 全部 10 项修复均无回归。全量 1763/1763 测试通过（含回滚 AUDIT-P3-ROUND53 修复的 2 个回归测试）。


**历史变更**：更早的开发记录（第五十二轮及以前）已归档至 [docs/changelog/archive/](docs/changelog/archive/)。
