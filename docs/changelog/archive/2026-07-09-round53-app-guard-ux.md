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
