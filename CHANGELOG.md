# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-08 · 第四十五轮：IR catch upvalue 关闭时机 + 沙箱 instSnaps 悬垂防御 + 类构造参数重校验 + 非 REPL GC roots + Worker 析构兜底

### 概述

继续对 MiniLang IDE 项目进行深度 Bug 排查，采用四模块并行 agent 审计策略（VM/IR 后端、Interpreter/GC、Parser/Lexer/Formatter/Module、Debugger/GUI 线程安全），共发现约 43 个问题。本轮修复 5 项明确低风险的真实可触发问题（P2 × 4，P3 × 1），涉及 IR 路径 catch 变量 upvalue 关闭时机不一致、条件断点沙箱 instSnaps Value* 悬垂风险、类构造期间重定义后参数签名未重新校验、非 REPL 模式 GC roots 为空集、InterpreterWorker 析构回调兜底。全量 1753/1753 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复 |
|---|------|--------|------|------|
| 1 | IR | P2 | IR 路径函数内 catch 变量 upvalue 关闭时机不一致——`visitTryStmt` 将 catch 变量 slot 记录到外层 BlockScope.localSlots，依赖外层块退出时 leaveBlockScope 统一关闭，而 Compiler.cpp 直接路径在 catch 块后立即发射 OP_CLOSE_UPVALUE。闭包捕获 catch 变量时 upvalue 快照时机晚于直接路径，三后端语义不一致 | catch 块编译后立即发射 `CLOSE_UPVALUE(slot)`，对齐 Compiler.cpp 行 2141-2144。catch 块内部 slot 已由其自身 leaveBlockScope 关闭，仅 catch 变量本身未关闭 |
| 2 | Interpreter | P2 | `evaluateCondition` 中 `instSnaps` 使用 `Value*` 裸指针作为 key——`inst` 来自 `getBoundInstance()` 指向 `variables["this"]` map 条目，条件求值期间 variables map rehash 会导致 `inst` 悬垂，SandboxGuard 析构时 UAF | 改用 `InstSnapEntry` 结构体存储 `Environment*`，析构时从 `env->getBoundInstance()` 重新获取 `inst` 指针，避免使用可能悬垂的快照指针 |
| 3 | Interpreter | P2 | `constructClassInstance` 类重定义后参数签名未重新校验——参数求值前用旧 initMethod 校验参数数量，求值后重新查找 initMethod 但未重新校验。类重定义后新 init 参数更少时多余参数被静默忽略，参数更多时缺失参数走 null 路径 | 重新查找 initMethod 后补充参数数量校验，与新 initMethod 的 `requiredParamCount`/`params.size()` 比较，不匹配时报错 |
| 4 | Interpreter | P2 | 非 REPL 模式 GC roots 为空集——`execute()` 中仅 REPL 模式收集 gcRoots，非 REPL 模式传入空根集。若 Interpreter 被复用（如测试场景），上一轮残留的循环引用容器在 tracked_ 中，空根集导致 Phase 2 误判所有 tracked 容器为不可达孤岛并清空子元素 | 非 REPL 模式下也收集 `globalEnv_` 顶层变量作为 GC roots，防御性保证复用场景的正确性 |
| 5 | InterpreterWorker | P3 | `~InterpreterWorker` 未兜底清空回调——run() 末尾清空 interp_ 回调，但若 worker 异常终止未走 run() 末尾路径，interp_ 仍持有捕获 this 的悬垂 lambda | 析构函数兜底清空 `setOutputCallback` 和 `setInputCallback`，防御未来回归 |

### 关键决策

1. **IR catch upvalue 用显式 CLOSE_UPVALUE 而非调整 BlockScope 归属**：catch 变量 slot 保留在 outer scope 的 localSlots 中（供 slot 回收和 varMap_ 清理），但在 catch 块编译后立即发射 CLOSE_UPVALUE(slot)。双重关闭是 no-op（isClosed 检查 + openUpvalues_.erase），安全且对齐直接路径
2. **instSnaps 改用 Environment* 而非 Value* 作为信息载体**：Environment 对象地址稳定（shared_ptr 管理），而 Value* 指向 unordered_map 条目可能因 rehash 悬垂。析构时从 env 重新获取 boundInstance_ 确保指针有效
3. **类重定义参数校验用 argValues.size() 而非重新求值**：参数已求值完毕（argValues 已收集），只需与新 initMethod 的参数范围比较即可。重新求值会导致副作用重复执行
4. **非 REPL GC roots 用 globalEnv_ 而非完整状态收集**：非 REPL 模式下 Interpreter 通常为新建（tracked_ 为空），防御性收集 globalEnv_ 变量即可覆盖复用场景。完整收集 callStack_/funRegistry_ 等不必要（GC 在 execute 入口触发，此时 callStack_ 为空）
5. **AUDIT-PLM-01（ModuleIsolation 默认参数作用域）经核实不是 Bug**：MiniLang 默认参数在闭包环境求值（非函数作用域），当前代码在 popScope 后处理 defaultValues 的行为正确

### 保留现状的审计发现

- **P1** break/continue finally 三后端不一致（需 finally 续跳机制）
- **P1** 条件断点求值无超时机制（VM 模式冻 UI）
- **P1** onContentsChange 多行插入/删除断点行号偏移错误
- **P1** super 上下文栈压入搜索起始类而非定义类
- **P1** 非 REPL 模式 GC roots 不完整（本轮仅收集 globalEnv_，未收集 callStack_/funRegistry_ 等，但因 GC 时机在 execute 入口，影响有限）
- **P2** writeBackCapturedVars 将局部重声明变量写回 capturedVars
- **P2** SandboxGuard 闭包 env 共享导致沙箱副作用泄漏
- **P2** typeMatch 不支持 dict[K:V]、函数类型、可选类型
- **P2** AST 节点缺少 endLine/endColumn（Formatter 插值注释错位）
- **P2** RegisterVM closeUpvaluesFrom slot 越界只打 Warning（对齐 StackVM 诊断口径）
- **P3** collectCycle 期间 callStack_ 不在 roots 中
- **P3** Formatter escapeString 未转义所有控制字符
- **P3** envPool_ 无界增长（REPL 模式）
- **P3** 空模块源码被当作加载失败
- **P3** \uXXXX 仅支持 BMP（设计限制）

### 验证

- MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` + `minilang_tests` 构建通过
- 全量 1753/1753 测试通过（1 个 minilang_perf_test_NOT_BUILT 占位符未运行）

## 2026-07-08 · 第四十四轮：REPL 状态对称性 + 条件断点沙箱完整性 + 多维数组门控 + 默认参数类型检查 + Formatter 转义去重

### 概述

补档此前一轮审计中已落地但未及时记录到 CHANGELOG 的 8 项修复（P1 × 1，P2 × 7，含 1 项预存在编译错误）。本轮审计聚焦 REPL 状态保存/恢复字段对称性、条件断点沙箱副作用隔离完整性、多维数组类型注解门控一致性、默认参数即时类型检查对齐、Formatter 字符串转义规则单一来源。修复均已在代码中落地并通过全量测试，本轮完成文档补录并重新应用一项被回退的真实残留 Bug（isClassTypeDeclStart 多维门控）。全量 1753/1753 测试通过（minilang_ide + minilang_tests 构建通过，1 个 minilang_perf_test_NOT_BUILT 占位符未运行）。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复 |
|---|------|--------|------|------|
| 1 | Interpreter | P1 | `saveReplState` 缺失 `moduleMtimes_` 保存——`restoreReplState` 读取 `savedModuleMtimes` 但 `saveReplState` 未写入，用空默认值覆盖 `moduleMtimes_`，导致后续 import 命中缓存时跳过 mtime 变更检查，磁盘修改被忽略（与 `savedModuleCache`/`savedModuleExports`/`savedModuleLoadingSet` 字段不对称） | `saveReplState` 添加 `replState_.savedModuleMtimes = moduleMtimes_;`，维持 save/restore 字段对称性 |
| 2 | BytecodeTracePanel | P2 | `captureCurrentState` 缺少 `isVmRunning()` 守卫——与 CallStackPanel/VariableInspectorPanel/BreakpointConditionPanel/MemoryModelPanel 4 个面板不一致，VM RUN 批之间捕获轨迹会混入用户未主动请求的中间状态数据，未来若 VmStepper 改为多线程会升级为数据竞争 | 添加 `isVmRunning()` 守卫，VM 运行中显示"暂停后可捕获"并早返回 |
| 3 | Parser | P2 | `isClassTypeDeclStart` 不支持多维数组类型注解 `ClassName[][]`——第四十三轮已修复 `parseTypeAnnotation` 的多维支持（while 循环），但此门控函数仍用单次 `[]` 检查，导致 `ClassName[][] paramName` 在函数参数处不被识别为类类型声明，落入"名字在前"分支引发解析错误。**注：此修复在第四十三轮后曾被回退，本轮重新应用** | 将单次 `if` 改为 `while` 循环消费连续 `[]` 后缀，与 `parseTypeAnnotation` 和 `classDecl` 保持一致 |
| 4 | Parser | P2 | `classDecl` 不支持多维数组字段 `ClassName[][] field`——单次 `[]` 检查遇到第二个 `[` 即回退，导致多维数组字段无法声明 | `classDecl` 字段解析将单次 `[]` 检查改为 `while` 循环消费连续 `[]` 后缀，安全回溯（仅在 `[` 后紧跟 `]` 时才消费） |
| 5 | TypeChecker | P2 | `visitAssignment` 缺少 `NullLiteral` 分支——与 `checkVarDecl` 不一致，`checkVarDecl` 有 `NullLiteral` 分支（null 兼容所有类型，actualType 设为 NULL_T），但 `visitAssignment` 遗漏，导致 `var x: string = null` 后续 `x = null` 赋值的类型推断行为不一致 | 添加 `NullLiteral` 分支，对齐 `checkVarDecl` 行为（actualType 设为 NULL_T，typeMatchLiteral 对 null 永远返回 true） |
| 6 | Interpreter | P2 | `evaluateCondition` 沙箱不完整——`SandboxGuard` 未保存/恢复 `loopFlow_` 和模块相关状态（`moduleCache_`/`moduleExports_`/`moduleMtimes_`/`exportedNames_`），条件断点求值包含 `import` 或循环语句时副作用泄漏到外层执行状态，破坏断点隔离语义 | `SandboxGuard` 新增 `savedLoopFlow` + `savedModuleCache` + `savedModuleExports` + `savedModuleMtimes` + `savedExportedNames` 成员，构造时保存、析构时恢复 |
| 7 | Interpreter | P2 | `callInstanceMethod` 默认参数类型检查时机——原实现评估默认值后立即 push 到 argValues 不做类型检查，与 `constructClassInstance` 不一致（constructClassInstance 在评估后立即 checkType），导致方法默认参数类型不匹配时不报错（如 `fun init(x: int = "abc")` 调用时不报错） | 默认参数评估后立即 `checkType`，对齐 `constructClassInstance` 行为 |
| 8 | Formatter | P2 | `escapeString` lambda 与 `formatStringLiteral` 转义规则重复——`formatInterpolatedString` 内部定义 `escapeString` lambda，与 `formatStringLiteral` 的转义 switch 独立维护，Lexer 新增转义序列时需同时修改两处，存在同步风险 | 提取公共函数 `escapeStringContent`，`formatStringLiteral` 和 `formatInterpolatedString` 共用，消除转义规则重复 |

### 关键决策

1. **saveReplState 字段对称性而非功能修复**：`saveReplState`/`restoreReplState` 的契约是"restore 读取的每个字段必须在 save 中写入"。`savedModuleMtimes` 在 restore 中被读取但 save 中未写入，违反契约导致空 map 覆盖。修复是一行添加，维持字段对称性是最小且正确的方案
2. **evaluateCondition 沙箱保存模块状态而非禁止 import**：条件断点求值中 import 的副作用（缓存写入、导出名注册）若不隔离会污染外层。深拷贝模块状态（map of shared_ptr）开销可接受（条件断点不频繁），且与 loopFlow_ 保存一并完成，沙箱完整性优于性能
3. **isClassTypeDeclStart 多维门控重新应用**：第四十三轮修复了 parseTypeAnnotation 的多维支持但遗漏了 isClassTypeDeclStart 门控函数。isClassTypeDeclStart 是类类型参数解析的入口谓词，返回 false 则 parseTypeAnnotation 永远不会被调用。三个函数（isClassTypeDeclStart/parseTypeAnnotation/classDecl）必须一致支持 while 循环消费连续 `[]`
4. **默认参数即时类型检查对齐 constructClassInstance**：callInstanceMethod 与 constructClassInstance 都处理默认参数，类型检查时机必须一致。即时 checkType（评估后立即检查）比延迟检查（全部评估后批量检查）更早暴露错误，且与 constructClassInstance 行为对齐
5. **Formatter escapeStringContent 提取为文件级 static 函数**：而非成员函数，因为转义规则是无状态的纯函数，不需要访问 Formatter 状态。文件级 static 限制作用域，避免污染头文件

### 保留现状的审计发现

- **P1** try-finally break/continue 三后端不一致（需 finally 续跳机制，中等规模改动）
- **P2** ClosureData 未注册 GcManager（纯闭包循环引用泄漏，需完整 GC 重构）
- **P2** formatClassDecl/visitTryStmt 注释丢失（需 ClassDecl 节点记录 closingBraceLine）
- **P2** formatInterpolatedString 多行插值注释错位（需 InterpolatedString 添加 endLine 字段）
- **P2** formatBlock closingBraceLine 错误恢复越界（仅在错误恢复路径触发）
- **P2** CodeEditor 断点行号替换偏移（边缘场景）
- **P2** REPL try/catch 顺序检查宽松（Parser 是最终防线）
- **P2** visitVarDecl 自动构造守卫顺序（无实际 bug 触发）

### 验证

- MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` + `minilang_tests` 构建通过
- 全量 1753/1753 测试通过（1 个 minilang_perf_test_NOT_BUILT 占位符未运行）

## 2026-07-08 · 第四十三轮：RuntimeError finally 语义修复 + 多维数组类型注解 + GC vmClosure upvalues + 前导零检测 + 断点同步 + stale 信号防御

### 概述

继续对 MiniLang IDE 项目进行深度 Bug 排查，采用四模块并行 agent 审计策略（VM/IR 后端、Interpreter/Value、Parser/Lexer/Formatter、Debugger/GUI 线程安全），共发现约 46 个问题（P1 × 6 含 3 项已知保留，P2 × 19，P3 × 16+）。本轮修复 10 项明确低风险的真实可触发问题（P1 × 2，P2 × 8，含 1 项预先存在的编译错误）。全量 1753/1753 测试通过（minilang_ide + minilang_tests 构建通过）。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复 |
|---|------|--------|------|------|
| 1 | Interpreter | P1 | RuntimeError 绕过 finally 块——`visitTryStmt` 外层 `catch (const ThrowException&)` 仅捕获 throw 语句异常，RuntimeError（数组越界/类型不匹配/除零等）不被捕获直接传播跳过 finally，三后端不一致（VM 路径 OP_TRY_BEGIN 捕获所有运行时错误会执行 finally） | 新增 `catch (const ReturnException&)` + `catch (const DebugStopException&)` + `catch (...)` 三层捕获；ReturnException/DebugStopException 直接 re-throw（return 三后端一致跳过 finally，中止信号不执行 finally），catch(...) 执行 finally 后 re-throw（RuntimeError 等对齐 VM 路径）。注：BreakException/ContinueException 在 Interpreter 中用 loopFlow_ 状态标志实现，不会以异常形式传播 |
| 2 | Debugger/GUI | P1 | closeEvent 期间 pending pausedAt 信号导致 stale UI 更新——`doPause` 中 `emit pausedAt(line)` 通过 QueuedConnection 投递到主线程，`stopForClose` 唤醒 worker 退出后 Ide 析构时处理 pending 事件访问已析构成员（codeEditor_/debugPanel_），try/catch 无法捕获 UAF | `onPausedAt` 入口检查 `!controller_->isRunning() && !controller_->isDebugPaused()` 直接返回，丢弃 stale 信号 |
| 3 | Interpreter | P2 | `visitTryStmt` L1634 注释错误——注释"return/break/continue：不执行 finally（与 VM 路径一致，已知限制）"是错误的，break/continue 在 Interpreter 中会执行 finally（loopFlow_ 状态标志路径） | 修正注释：return 不执行 finally（三后端一致）；break/continue Interpreter 执行 finally，VM 跳过 finally，三后端不一致为已知限制 |
| 4 | Lexer | P2 | 前导零 `007` 静默接受——`number()` 对 `0` 后跟数字直接消费并用 from_chars 解析，前导零在 C 中被误解为八进制、Python3 中报错，教学型语言静默接受误导学习者 | 在 0x/0b/0o 检测后添加前导零检测：`source_[start_]=='0' && isAsciiDigit(peek())` 报错并消费整个数字序列，提供等价合法值建议 |
| 5 | Parser | P2 | 二维数组类型注解 `int[][]` 不支持——`parseTypeAnnotation` 用单次 `if` 仅消费一对 `[]`，遇到第二个 `[` 即返回，导致 `int[][]` 中第二个 `[]` 留在 token 流引发后续解析错误 | 将单次 `if` 改为 `while` 循环消费连续 `[]` 后缀，直到不再是 `[]` 模式 |
| 6 | Interpreter | P2 | `typeMatch` 多维数组类型注解未识别——`annotation[size-2]=='['` 仅匹配单维 `int[]`，对 `int[][]`（末尾两字符 `]]`）不匹配导致报错 | 改用 `annotation.substr(size-2)=="[]"` 后缀比较，剥离末尾 `[]` 后递归 typeMatch 每个元素，天然支持多维（int[][] → int[] → int） |
| 7 | GcManager | P2 | GC roots 未覆盖 vmClosure upvalues——`markValue` 的 VAL_CLOSURE 分支仅标记 capturedVars，遗漏 vmClosure->upvalues，仅通过 VM 闭包 upvalues 可达的循环容器被误判为不可达孤岛而误回收 | 在 VAL_CLOSURE 分支增加 vmClosure->upvalues 遍历，对每个 isClosed=true 的 upvalue 的 value 字段调用 markValue（open 状态值在栈上是 GC roots，value 字段不持有有效值，markValue 安全跳过） |
| 8 | Debugger | P2 | `waitCallbacksIdle` 无超时 spin-wait——`DebugController::waitCallbacksIdle` + `DebugEvaluator::waitCallbackIdle` 无限 spin-wait，若 callback 进入死循环（如条件断点求值包含无限循环），activeCallbackCount_ 永不归零，析构永久阻塞，进程挂死 | 两处均添加 3 秒超时上限，超时后记录警告并继续析构（接受可能的 UAF 风险，但优于永久阻塞）；与 P1-1（条件断点求值超时）配合 |
| 9 | CodeEditor/Debugger | P2 | 断点切换在 Interpreter 调试会话期间不同步——`LineNumberArea::mousePressEvent` 修改 breakpoints_ 后不发射信号，Interpreter 调试暂停期间新增/删除断点不同步到 DebugController.breakpoints_，新断点不生效，已删除断点仍触发 | CodeEditor 新增 `breakpointsChanged()` 信号，mousePressEvent 断点切换后发射；Ide 连接此信号，在 `isDebugPaused()` 状态下同步到 DebugController（VM 模式每次步进前 syncVmBreakpoints 会同步，无需处理） |
| 10 | BuiltinMethods | P2 | `executeBuiltinType` 参数名 `col` 与函数体 `column` 不一致——预先存在的编译错误，参数名是 `col` 但函数体用 `column`（未声明的标识符），因 .obj 缓存未暴露，头文件变更触发重编译暴露 | 统一参数名为 `column`，与其他 executeBuiltinXxx 函数一致 |

### 关键决策

1. **RuntimeError finally 修复用 catch(...) 而非逐个捕获**：RuntimeError 类型多（数组越界/类型不匹配/除零等），逐个捕获易遗漏。catch(...) 捕获所有异常，但在之前用 catch (const ReturnException&) 和 catch (const DebugStopException&) 排除不应执行 finally 的异常（return 三后端一致跳过 finally，中止信号不执行 finally）。BreakException/ContinueException 在 Interpreter 中用 loopFlow_ 状态标志实现，不会以异常形式传播，故 catch(...) 不会捕获它们
2. **多维数组类型注解用递归剥离而非扁平检查**：typeMatch 用 `substr(size-2)=="[]"` 后缀比较剥离末尾 `[]` 后递归调用，天然支持任意维数（int[][]、int[][][]等），比扁平检查所有维数更简洁且可扩展
3. **GC vmClosure upvalues 仅标记 isClosed=true 的 upvalue**：open 状态的 upvalue 值在栈上（栈是 GC roots），value 字段不持有有效值，markValue 安全跳过（null/非指针类型直接返回）。仅 isClosed=true 的 upvalue 的 value 字段持有值（可能为容器引用），需标记
4. **断点同步用 isDebugPaused 守卫而非无条件同步**：Interpreter 调试暂停期间才需同步到 DebugController（会话启动时已同步快照）。VM 模式每次步进前 syncVmBreakpoints 会同步，无需在 breakpointsChanged 中处理。无条件同步会增加非调试状态下的开销
5. **waitCallbacksIdle 超时 3 秒而非更长**：callback 正常执行通常 <100ms，3 秒超时足够覆盖慢路径。超时后继续析构的风险是 worker 线程可能仍在执行 callback 访问已析构的 interpreter_ shared_ptr（但 shared_ptr 安全，真正危险的是非线程安全的 Environment/Value）。若 callback 真的挂死，forceStop 的 terminate() 是唯一出路
6. **stale 信号防御用状态检查而非 removePostedEvents**：`QCoreApplication::removePostedEvents` 需要事件类型参数且 API 复杂。状态检查 `!isRunning() && !isDebugPaused()` 简单直接，worker 已停止且非暂停时说明是 stale 信号。try/catch 无法捕获 UAF，状态检查是更可靠的防御

### 保留现状的审计发现

- **P1** break/continue finally 三后端不一致（需 finally 续跳机制，中等规模改动）
- **P1** 条件断点求值无超时机制（VM 模式冻 UI，推荐方案 A 步数上限 + 方案 C stopped_ 检查）
- **P1** onContentsChange 多行插入/删除断点行号偏移错误（含详细修复方案，待实施）
- **P1** super 上下文栈压入搜索起始类而非定义类（多层继承 super 解析错误，需 findMethod 返回定义类，中等规模改动）
- **P2** SandboxGuard 闭包 env 共享导致沙箱副作用泄漏（需深拷贝 closureEnv）
- **P2** writeBackCapturedVars 将局部重声明变量写回 capturedVars（需 computeFreeVariables 记录遮蔽）
- **P2** Formatter 插值字符串内部注释错位（需 AST endLine 字段）
- **P2** constructClassInstance 类重定义期间参数签名不一致
- **P2** formatBlock closingBraceLine `<=` 比较导致 `} // comment` 注释位置变化（已知妥协）
- **P2** needsParens 对 UnaryOp 嵌套的括号保留过度（不影响等价性，仅影响美观）
- **P3** collectCycle 期间 callStack_ 不在 roots 中（潜在，当前不触发）
- **P3** Formatter escapeString 未转义所有控制字符
- **P3** envPool_ 无界增长（REPL 模式）
- **P3** SandboxGuard 未保存/恢复模块相关状态
- **P3** 空模块源码被当作加载失败

### 验证

- MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` + `minilang_tests` 构建通过
- 全量 1753/1753 测试通过（1 个 minilang_perf_test_NOT_BUILT 非真实测试）

## 2026-07-08 · 第四十二轮：GC 不变量修复 + break upvalue 关闭 + COW 异常安全 + UI 禁用遗漏 + QSettings 持久化

### 概述

继续对 MiniLang IDE 项目进行深度 Bug 排查，聚焦三后端语义一致性、GC 不变量、COW 异常安全、UI 线程安全与进程退出持久化。共修复 9 项问题（P1 × 2，P2 × 6，P3 × 1）。全量 1753/1753 测试通过（minilang_ide + minilang_tests 构建通过）。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | interpreter/GcManager.cpp | P1 | `collectCycle` Phase 3 用 survivors 重建 `tracked_` 后，`aliveSet_` 未同步重建。下一轮 Phase 2 将 survivors 误判为"已销毁"跳过 sweep，survivors 变为不可达循环孤岛时永久泄漏 | Phase 3 后用 `tracked_`（= survivors）重建 `aliveSet_`，维持 `aliveSet_ = tracked_ 中仍存活的对象集合` 不变量 |
| 2 | parser/Parser.cpp | P1 | `synchronize` 错误恢复缺少 `TK_FROM` 同步点，import 语句 `TK_FROM` 缺失时解析器跳过 from 关键字继续解析后续语句，导致结构信息丢失 | 两处 switch 各添加 `case TK_FROM: return;` |
| 3 | compiler/Compiler.cpp + IR.cpp | P2 | `visitBreakStmt` 直接发 OP_JUMP 跳到 breakTarget（在 OP_CLOSE_UPVALUE 之后），跳过 upvalue 关闭，导致 break 跳出循环时闭包捕获变 by-reference（三后端不一致） | LoopContext 增加 `bodySlotBase` + `needCloseUpvalue` 字段；visitBreakStmt 在 OP_JUMP 前发射 OP_CLOSE_UPVALUE（Compiler.cpp 直接路径 + IR.cpp IR 路径同步修复） |
| 4 | interpreter/Value.h | P2 | `ensureUnique` 先 `ptr->release()` + `cloned.release()` 再 `registerTracked(raw)`，若 registerTracked 抛 bad_alloc 则 raw 泄漏（已脱离 unique_ptr）且 box_ 仍指向旧 ptr（release 已执行→双重释放） | 将 `registerTracked` 移到 `ptr->release()` 之前，若抛异常 unique_ptr 自动 delete cloned，旧引用计数不变 |
| 5 | interpreter/GcManager.cpp | P2 | `registerTracked` 中 `aliveSet_.insert` 在 `tracked_.push_back` 成功后抛 bad_alloc 时，tracked_ 含 obj 但 aliveSet_ 不含，破坏不变量，collectCycle 将 obj 误判为"已销毁"跳过 sweep → 永久泄漏 | `aliveSet_.insert` 用 try/catch 包裹，失败时 `tracked_.pop_back()` 回滚 |
| 6 | app/ide.cpp | P2 | VM 执行路径（onVmRun/handleVmStepResult RUNNING 分支）未禁用 `formatAction_`，用户在 VM 运行期间格式化代码导致字节码陈旧 | RUNNING 分支 + `setVmStepActionsEnabled` 添加 `formatAction_->setEnabled(false)` |
| 7 | app/ide.cpp | P2 | VM 执行路径未禁用 `replPanel_` 输入，用户在 VM 运行期间提交 REPL 输入并发访问 Interpreter/VM 状态 | RUNNING 分支 + `setVmStepActionsEnabled` 添加 `replPanel_->setInputEnabled(false)` |
| 8 | app/WorkerManager.cpp | P2 | `forceStop` 两处 `std::_Exit(0)` 前只 flush Logger，未调用 `QSettings::sync()`，_Exit 跳过析构导致最近工作区/窗口布局/引导标记等未刷盘丢失 | 两处 `_Exit(0)` 前添加 `QSettings("MiniLang", "MiniLang IDE").sync()` |
| 9 | compiler/Compiler.cpp | P3 | visitTryStmt 注释声称"break/continue/return 不会执行 finally（三后端一致）"，实际 break/continue 时 Interpreter 执行 finally 而 StackVM/RegisterVM 跳过，三后端不一致 | 注释修正为准确描述：break/continue 时 Interpreter 执行 finally、VM 跳过（不一致），return 三后端均跳过（一致） |

### 关键决策

1. **break upvalue 关闭用 bodySlotBase 单次发射**：OP_CLOSE_UPVALUE bodySlotBase 关闭 slot >= bodySlotBase 的全部 open upvalues，含循环体内嵌套块声明的变量（嵌套块 slot >= bodySlotBase），无需逐层块关闭
2. **COW ensureUnique 将 registerTracked 前移而非回滚**：在 release 旧引用前注册，若失败 unique_ptr 自动清理 cloned，旧引用计数不变，是最简洁的事务性保护
3. **registerTracked 用 try/catch 回滚而非更换容器**：push_back + insert 顺序保持，仅添加异常回滚，最小改动
4. **UI 禁用统一到 setVmStepActionsEnabled**：formatAction_ 和 replPanel_ 与 compileAnalysisAction_ 一起管理，避免散布在各分支

### 保留现状

- break/continue 时 finally 块三后端不一致（P1，需 finally 续跳机制，中等规模改动）
- 条件断点求值无超时机制（P1，VM 模式冻 UI）
- onContentsChange 多行插入/删除断点行号偏移错误（P1）
- Formatter 插值字符串内部注释错位（P2，需 AST endLine 字段）
- waitCallbacksIdle 无超时 spin-wait（P2）

## 2026-07-08 · 第四十一轮：教学模块第三轮深度审计与修复（教学内容一致性 + 防重复守卫 + 错误友好化 + 搜索高亮 + Markdown 占位符）

### 概述

对全部教学模块面板进行第三轮深度 Bug 审计，采用四模块并行 agent 策略，共发现约 38 个问题（P1 × 9，P2 × 17，P3 × 12）。本轮修复 P1 全部 + P2 主要项 + P3 关键项共 16 项。核心修复包括：7 处教学内容错误（Point.new()→Point()、fun new→fun init、0 or "default" 注释错误）、4 面板浏览即完成映射缺失、op-priority-challenge 同步缺失、BugHunt 全题完成才发射 challengeSolved、3 面板防重复提交守卫（BugHunt/TokenPuzzle/LabManual）、LabManualPanel controller 为空友好提示、TeachingTreePanel 搜索过滤高亮失效、MarkdownRenderer 行内代码 `**` 误处理、LearnerProgress 负数字段校验。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | CallStackPanel/IRTransformPanel/MemoryModelPanel/BytecodeTracePanel/VariableInspectorPanel/SyntaxProductionLibrary/BreakpointConditionPanel | P1 | 7 处教学内容使用 `Point.new()`、`fun new`、`0 or "default"` 注释错误等不符合 MiniLang 构造方法语义（必须命名为 `init`，`Point(...)` 直接调用类名触发构造）与短路语义（左操作数为假时返回右值）的示例代码 | 修正为 `Point()`、`fun init`、修正 `or`/`and` 短路语义注释 |
| 2 | ide.cpp | P1 | 4 个面板（BytecodeTracePanel/CallStackPanel/VariableInspectorPanel/BreakpointConditionPanel）首次浏览未映射到 LearningPath 完成活动 | 在 ensureTeachingPanelCreated 中补 4 面板浏览即完成映射 |
| 3 | op-priority-challenge | P1 | op-priority-challenge 关卡未与 LearningPath 同步 | 补 markActivityCompleted 同步调用 |
| 4 | BugHuntPanel | P1 | onTripleVerify 在未全部完成时即发射 challengeSolved | 改为全部题目完成才发射 challengeSolved |
| 5 | BugHuntPanel | P2 | onRunVerify/onTripleVerify 无防重复守卫，processEvents 让渡控制权期间快速重复点击触发多次 runStringCaptureOutput + 多次 save() 磁盘 I/O + recordFailure 计数虚高 | 添加 busy_ 守卫 + 按钮禁用/恢复 |
| 6 | TokenPuzzlePanel | P2 | onCheckAnswer/onSkipLevel 无防重复守卫 | 添加 busy_ 守卫 + 按钮禁用/恢复 |
| 7 | LabManualPanel | P2 | onSubmitChoiceExercise/onSubmitExpectedOutputExercise 无防重复守卫 | 添加 submitting_ 守卫 + 答对后禁用提交按钮 + setText("已通过") |
| 8 | LabManualPanel | P2 | controller 为空时暴露 `!ERROR: No controller` 给学员 | 友好提示"运行环境未就绪，请先在主界面运行一次程序"，且仅在 controller 可用时才记录失败 |
| 9 | TeachingTreePanel | P2 | setCurrentPanel 在搜索过滤激活时直接 setCurrentItem(hidden item)，选中态被设置但用户不可见——高亮"失效" | 跳转前重置搜索过滤（QSignalBlocker 避免 clear() 重入 onSearchChanged） |
| 10 | MarkdownRenderer | P3 | renderInline 中行内代码 `code` 先替换为 `<code>...</code>`，但随后的粗体正则 `\*\*([^*]+)\*\*` 仍会匹配 `<code>` 标签内的 `**`，导致 `` `a **b** c` `` 被错误渲染 | 占位符法：先用控制字符占位替代直接 replace，让粗体/斜体/链接正则在纯净文本上执行，最后统一替换占位符为 `<code>` |
| 11 | LearnerProgress | P3 | load() 中从 JSON 读取的数值字段无范围校验，手动篡改的 JSON 可能含负数，破坏排序语义 | 为所有数值字段（attemptCount/levelStars/score/bestStars/spentMinutes/failCount）添加范围校验 |

### 关键决策

1. **构造方法必须命名为 init**：MiniLang 三后端（Interpreter/StackVM/RegisterVM）均查找名为 `init` 的方法作为类构造函数，`Point.new()` 不被识别为构造调用，`fun new` 会被识别为普通方法。教学示例必须使用 `Point()` 直接调用类名触发构造 + `fun init` 命名构造方法
2. **防重复守卫用早返回 + busy_ 标志而非 Qt::BlockQueuedConnection**：processEvents 让渡控制权期间用户可点击其他按钮触发槽函数重入。Qt::BlockQueuedConnection 会冻结 UI 事件循环，违背 processEvents 的设计意图。`if (busy_) return;` + `busy_ = true; btn->setEnabled(false);` 在 early return 之后设置，函数末尾恢复，是最轻量的重入防护
3. **答对后禁用提交按钮而非依赖守卫**：LabManualPanel 答对后即使有 submitting_ 守卫，用户仍可重复点击触发 recordScore 多次写入。禁用按钮 + setText("已通过") 提供视觉反馈，且从根因消除重复提交
4. **controller 为空友好提示且不记录失败**：教学场景下学员不应看到 `!ERROR: No controller` 等内部错误。改为"运行环境未就绪，请先在主界面运行一次程序以初始化引擎，再回来验证此题"的友好提示，且仅在 controller 可用时才记录失败（避免误判污染学情）
5. **跳转即重置搜索过滤**：TeachingTreePanel setCurrentPanel 在搜索过滤激活时直接 setCurrentItem(hidden item) 导致高亮失效。跳转即重置过滤符合用户心智模型（跨面板导航应跳出搜索上下文），且避免树仍只显示过滤结果与实际面板不一致
6. **占位符法解决行内代码 `**` 误处理**：MarkdownRenderer renderInline 中行内代码先替换为占位符，让后续粗体/斜体/链接正则在无 `<code>` 标签的纯净文本上执行，最后统一替换占位符为 `<code>`。与第三十七轮关键字腐蚀修复同一思路
7. **负数字段校验与 recordXxx 截断逻辑一致**：LearnerProgress load() 中所有数值字段添加范围校验，与 recordScore（截断到 [0,100]）、addSpentMinutes（拒绝负数）等写入路径的校验逻辑保持一致

### 修改文件清单

- 修改：`gui/CallStackPanel.cpp`、`gui/IRTransformPanel.cpp`、`gui/MemoryModelPanel.cpp`、`gui/BytecodeTracePanel.cpp`、`gui/VariableInspectorPanel.cpp`、`gui/SyntaxProductionLibrary.cpp`、`gui/BreakpointConditionPanel.cpp`（P1 教学内容错误：Point.new()→Point()、fun new→fun init、0 or "default" 注释）
- 修改：`app/ide.cpp`（P1 4 面板浏览即完成映射 + op-priority-challenge 同步 + BugHunt 全题完成才发射 challengeSolved）
- 修改：`gui/BugHuntPanel.{h,cpp}`（P2 onRunVerify/onTripleVerify 防重复守卫）
- 修改：`gui/TokenPuzzlePanel.{h,cpp}`（P2 onCheckAnswer/onSkipLevel 防重复守卫）
- 修改：`gui/LabManualPanel.{h,cpp}`（P2 onSubmitChoiceExercise/onSubmitExpectedOutputExercise 防重复守卫 + controller 为空友好提示 + 答对后禁用提交按钮）
- 修改：`gui/TeachingTreePanel.cpp`（P2 setCurrentPanel 跳转前重置搜索过滤 + QSignalBlocker）
- 修改：`gui/MarkdownRenderer.cpp`（P3 renderInline 占位符法解决行内代码 `**` 误处理）
- 修改：`gui/LearnerProgress.cpp`（P3 load() 负数字段校验）

### 保留现状的审计发现

- BugHuntPanel e.what() 用于教学调试场景（合理保留）
- ProfileDashboardPanel errorMessage 用于诊断后端兼容性（设计意图保留）
- BytecodeTracePanel/CallStackPanel/VariableInspectorPanel connect 缺 context（连接自身子控件，面板销毁时子控件同步销毁，风险极低）
- GlossaryPanel relatedPanelFor 映射表完整（30 术语→面板映射）
- BackendComparePanel source 参数未使用（设计意图，使用 controller AST）
- BreakpointConditionPanel modeText 逻辑正确
- VmStackSandboxPanel 步数上限已有多层保护（STEP_IN 语义/MAX_STEP_LOOP 100 万/kMaxSteps 10 万/processEvents 让出/traceRunning_ 守卫）
- VmStackSandboxPanel 页切换无 QTimer 且 VM 监听器已检查页索引

## 2026-07-08 · 第四十轮：教学模块第二轮深度审计与修复（BoxedIntData 崩溃 + 教学作弊防护 + 状态机 + 异常防护 + 性能）

### 概述

对全部教学模块面板进行第二轮深度 Bug 审计，采用四模块并行 agent 策略，共发现约 40 个问题（P1 × 1，P2 × 15，P3 × ~15）。本轮修复 P1 全部 + P2 主要项共 15 项。核心修复包括：VariableInspectorPanel valueToBitsHex 对 BoxedIntData 触发 std::abort 崩溃、BugHuntPanel 三后端空输出作弊路径、BugHuntPanel 提示状态机幂等失效、TokenPuzzlePanel 跳过关卡误标记完成、AstBuilderToyPanel 纯表达式包装教学误导、LabManualPanel exercisePassed_ 索引错位 + applyFolding 状态泄漏、PipelineViewer 动画冲突 + O(n²) 性能、BytecodeTrace/DebugPanel toString 异常防护、MemoryModelPanel 悬垂指针、WelcomeWizard 重复 UI 重建、ProfileDashboard 硬编码坐标、VmStackSandboxPanel 重入风险。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | VariableInspectorPanel | P1 | `valueToBitsHex` 对 BoxedIntData（超大整数装箱）调用 `NaNBox::fromInt` 触发 `canEncodeInt` 失败 → `std::abort()` 崩溃 | `case VAL_INT` 分支前判断 `v.isPointer()`（BoxedIntData 标志），走堆指针占位符路径而非 fromInt 重编码 |
| 2 | TokenPuzzlePanel | P2 | `onSkipLevel` 跳过关卡仍发射 `activityCompleted`，LearningPathPanel 误标记为已完成 | 移除 `emit activityCompleted`（跳过≠完成，unlockNextLevel 基于星数解锁不依赖此信号） |
| 3 | BugHuntPanel | P2 | `onTripleVerify` 三后端均无输出时 `outputConsistent` 平凡为 true，删除所有 print 即可作弊通关 | 增加 `hasOutput` 检查（至少一个后端有非空输出），`outputConsistent && allClean && hasOutput` 才发射 challengeSolved |
| 4 | BugHuntPanel | P2 | `onItemSelected` 幂等保护条件含 `hintLevel_==0`，查看提示后再次点击同一题目导致 hintLevel_ 重置、提示状态混乱 | 移除 `hintLevel_==0` 约束，同一题目无论提示状态都直接 return |
| 5 | AstBuilderToyPanel | P2 | `onVerifyWithRealParser` 纯表达式包装为 `var __toy_tmp = <expr>;`，真实 AST 根节点是 VarDecl 而学员搭的是表达式树，造成教学误导 | 输出中明确标注包装行为："纯表达式已包装为 var __toy_tmp = <expr>; 才能解析，VarDecl 的子树才是目标表达式" |
| 6 | LabManualPanel | P2 | `onSubmitChoiceExercise` 用 `choiceIndex`（CHOICE 内序号）索引 `exercisePassed_`（全局练习索引），CHOICE/EXPECTED_OUTPUT 混排时标记错题 | 遍历 exercises 找到第 choiceIndex 个 CHOICE 时记录 `globalIdx`，用 `exercisePassed_[globalIdx]` 替代 `exercisePassed_[choiceIndex]` |
| 7 | LabManualPanel | P2 | `applyFolding` 折叠区内切换 `inCodeBlock`，未闭合代码块导致 `inCodeBlock` 恒为 true，后续所有行（含标题）被当作代码块跳过，文档剩余部分消失 | 折叠区内不切换 `inCodeBlock`（内容已跳过，状态不应泄漏到折叠区外） |
| 8 | BytecodeTracePanel | P2 | `captureCurrentState` 中 `v.toString()` 无 try/catch，与 CallStack/VariableInspector 不一致，异常传播到 QTimer 槽 | 对齐 try/catch 防护：`try { s = v.toString(); } catch (...) { s = "<error>"; }` |
| 9 | DebugPanel | P2 | `updateVariables` 中 `v.value.toString()` 无 try/catch，异常传播到 DebugCoordinator RCU 回调链 | 同上 try/catch 防护 |
| 10 | PipelineViewer | P2 | `populateSource` 每次循环调用 `os.str().back()` 返回值拷贝 O(n)，n 个 Token 总 O(n²) | 改用 `lastChar` 变量跟踪最后写入字符，降为 O(n) |
| 11 | PipelineViewer | P2 | `switchToStep` 未停止前一个 QPropertyAnimation，快速连续切换导致 `finalPos` 依赖动画中间值，页面卡在偏移位置 | 同一步骤重复点击 early return + findChildren 停止已有 pos 动画 |
| 12 | MemoryModelPanel | P2 | `nanBoxExamples` 用栈上 `int dummy` 作为指针示例，IIFE 返回后 dummy 出作用域，`b.bits` 持有悬垂栈地址 | `int dummy` → `static int dummy`，保证生命周期，展示真实有效指针值 |
| 13 | WelcomeWizard | P2 | Step4 按钮 `emit learningPathRequested()` 同步触发 `showTeachingPanel`（第一次），`accept()` 后又无条件调用（第二次），LearningPathPanel 被重建两次 | 移除 `learningPathRequested → showTeachingPanel` 连接，统一在 `exec()` 返回后调用一次 |
| 14 | ProfileDashboard | P2 | `paintEvent` 硬编码 `QRect(220, 200, width()-240, 200)`，splitter 拖动/窗口缩放时柱状图位置错位 | 改为基于 `resultTable_->geometry().bottom()` + widget 边界动态计算 chartRect |
| 15 | VmStackSandboxPanel | P2 | `onTraceRunAll` 长循环期间 `processEvents` 让渡控制权，用户可点击 step/reset 触发重入，与正在跑的循环共享 VM 状态导致崩溃 | 新增 `traceRunning_` 成员守卫 + 运行期间禁用 stepBtn_/resetTraceBtn_/runAllBtn_；onTraceStep/onTraceReset 检查守卫 |

### 关键决策

1. **BoxedIntData 用 isPointer() 判断而非 getType()**：BoxedIntData 的 `getType()` 返回 `VAL_INT`（与内联 int 相同），但 `box_.isPointer()` 为 true（堆对象标志）。在 `case VAL_INT` 分支内检查 `v.isPointer()` 区分两种编码，对 BoxedIntData 走堆指针占位符路径避免 fromInt abort
2. **跳过不发射 activityCompleted 而非发射 0 星**：activityCompleted 信号语义是"活动已完成"，LearningPathPanel 据此标记完成。跳过≠完成，0 星持久化由 markLevelStars(0) 单独处理，不应触发完成通知
3. **空输出作弊用 hasOutput 而非 expectedOutput 匹配**：BugHuntItem 结构中 expectedOutput 未结构化为可比较字段（仅文字描述），hasOutput（至少一个后端有非空输出）是最低成本修复，排除退化解
4. **exercisePassed_ 用 globalIdx 而非重构为 choiceIdx 索引**：exercisePassed_ 对所有练习类型追加（全局索引），choiceGroups_ 仅对 CHOICE 追加（CHOICE 内序号）。在 onSubmitChoiceExercise 中遍历找到 CHOICE 的全局索引，保持 exercisePassed_ 语义不变
5. **applyFolding 折叠区内不切换 inCodeBlock**：折叠区内容已被跳过，切换 inCodeBlock 会让状态泄漏到折叠区外。折叠区外的代码块仍正常跟踪 inCodeBlock，互不影响
6. **switchToStep 同步骤 early return 而非重新动画**：同一步骤重复点击时 page->pos() 依赖前一个动画的中间值，finalPos 错误。early return 避免此问题，仅刷新内容
7. **traceRunning_ 守卫 + 禁用按钮而非停止定时器**：onTraceRunAll 是同步循环，processEvents 期间用户可点击其他按钮。守卫标志 + 禁用按钮双重防护，确保 VM 状态不被并发修改

### 修改文件清单

- 修改：`gui/VariableInspectorPanel.cpp`（valueToBitsHex BoxedIntData 崩溃修复）
- 修改：`gui/TokenPuzzlePanel.cpp`（onSkipLevel 移除 activityCompleted 发射）
- 修改：`gui/BugHuntPanel.cpp`（onTripleVerify hasOutput + onItemSelected 幂等）
- 修改：`gui/AstBuilderToyPanel.cpp`（onVerifyWithRealParser 标注包装行为）
- 修改：`gui/LabManualPanel.cpp`（exercisePassed_ globalIdx + applyFolding inCodeBlock 隔离）
- 修改：`gui/BytecodeTracePanel.cpp`（captureCurrentState toString try/catch）
- 修改：`gui/DebugPanel.cpp`（updateVariables toString try/catch）
- 修改：`gui/PipelineViewer.cpp`（populateSource lastChar O(n) + switchToStep 动画守卫）
- 修改：`gui/MemoryModelPanel.cpp`（nanBoxExamples static dummy）
- 修改：`gui/ProfileDashboardPanel.cpp`（paintEvent 动态 chartRect）
- 修改：`gui/VmStackSandboxPanel.{h,cpp}`（traceRunning_ 重入守卫）
- 修改：`app/ide.cpp`（WelcomeWizard 移除重复 showTeachingPanel 连接）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

### 保留现状的审计发现

以下 P2/P3 审计发现因需架构改动或风险较高保留现状：
- SyntaxExplorerPanel onRunSample 同步阻塞（P1，需 QtConcurrent 架构改动）
- LabManualPanel onSubmitExpectedOutputExercise 同步阻塞（P2，同上）
- VmStackSandboxPanel onTraceRunAll 步数上限（P2，提高上限意义不大）
- VmStackSandboxPanel 页切换按钮非互斥（P2，改用 QButtonGroup 需重构）
- BugHuntPanel onTripleVerify 空输出与 expectedOutput 匹配（P2，需结构化字段）
- TeachingTreePanel setCurrentPanel 搜索过滤下高亮失效（P2）
- GlossaryPanel relatedPanelFor 映射不完整（P3）
- MarkdownRenderer 行内代码内 `**` 误处理为粗体（P3）
- MarkdownRenderer 表格中间分隔行被当作数据行（P3）
- LearnerProgress load 不校验负数字段值（P3）
- BackendComparePanel runXxx source 参数未使用（P3）
- IRTransformPanel reloadCurrentIR 死代码（P3）
- BreakpointConditionPanel modeText VM STEP 间歇期显示"未运行"（P3）

## 2026-07-08 · 第三十九轮：continue 方向性错误修正 + VM 面板数据竞争 + 闭包沙箱隔离 + Lexer 代理码点 + VmStepper hitCount

### 概述

继续对 MiniLang IDE 项目进行深度 Bug 排查，采用四模块并行 agent 审计策略。**最关键的发现**：第三十八轮的 continue 修复存在方向性错误——`continueTarget` 应在 `OP_CLOSE_UPVALUE` **之前**而非之后，导致 continue 仍然跳过 upvalue 关闭。本轮修正此错误并修复另外 8 项问题。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 严重性 | 问题 | 修复 |
|---|--------|------|------|
| 1 | P1 | 第三十八轮 continue 方向性错误（while+for×Compiler+IR=4 处） | continueTarget/continueLabel 移到 OP_CLOSE_UPVALUE/leaveBlockScope **之前** |
| 2 | P1 | VariableInspectorPanel VM 模式数据竞争 | 添加 isVmRunning() 守卫 |
| 3 | P1 | CallStackPanel VM 模式数据竞争 | 添加 isVmRunning() 守卫 |
| 4 | P2 | deepCloneForSandbox 闭包浅拷贝导致 capturedVars 变异泄漏 | capturedVars COW detach + 递归深拷贝 |
| 5 | P2 | Lexer `\uXXXX` 接受 Unicode 代理码点产生 WTF-8 | 拒绝 0xD800-0xDFFF 代理码点 |
| 6 | P2 | VmStepper hitCount 在 RUN/STEP 模式下过度递增 | checkBreakpointHit 改为纯查询，hitCount 递增移到过滤条件通过后 |
| 7 | P2 | ~DebugController 未调用 waitCallbacksIdle | 析构函数补 waitCallbacksIdle() |
| 8 | P2 | ~DebugEvaluator 未调用 waitCallbackIdle | 析构函数补 waitCallbackIdle() |
| 9 | P2 | VariableInspectorPanel onVariableSelected VM 模式数据竞争 | 添加 isVmRunning() 守卫 |

### 关键决策

1. **continue 跳转目标必须在 upvalue 关闭之前**：字节码执行是顺序的——continue 跳到 continueTarget 后，从该点继续执行后续指令。如果 continueTarget 在 OP_CLOSE_UPVALUE 之后，continue 会跳过 OP_CLOSE_UPVALUE；如果在之前，continue 落到 OP_CLOSE_UPVALUE 上，执行关闭后再走 OP_LOOP。第三十八轮的逻辑"放在之后确保关闭"是颠倒的——栈式 VM 中跳转目标是"落地后从该点继续执行"，不是"执行该点之前的代码"
2. **VM 面板数据竞争用 isVmRunning() 守卫而非加锁**：VM 的 globalSlots_/frames_ 是 hot path（每条指令都可能修改），加锁会拖慢 VM 执行。isVmRunning() 守卫复用已有的运行状态标志，仅在 VM 暂停时才读取快照，对齐 Interpreter 调试路径的 isDebugPaused() 守卫模式
3. **deepCloneForSandbox 闭包用 COW detach 而非深拷贝整个 ClosureData**：closure 的 body（AST 指针）和 closureEnv（weak_ptr）是只读/弱引用，共享安全。仅 capturedVars 需要 COW detach（非 const capturedVars() 触发 ensureUnique）+ 递归深拷贝容器值，隔离 writeBackCapturedVars 的副作用
4. **VmStepper hitCount 拆分查询与计数职责**：原 checkBreakpointHit 将"查询断点匹配"与"递增命中计数"耦合，C++ 短路求值导致过滤条件失败时 hitCount 已被递增。改为纯查询（isBreakpointHit 语义），hitCount 递增移到调用方过滤条件通过后，对齐 DebugController::shouldPauseAtBreakpoint 的"先过滤后计数"模式
5. **Lexer 代理码点拒绝而非合并**：`\uD83D\uDE00` 代理对合并为 U+1F600 需要跨转义序列状态，风险较高。本轮采用保守方案——拒绝代理码点（0xD800-0xDFFF）并报错，避免产生非标准 WTF-8。astral plane 支持留作后续增强

### 修改文件清单

- 修改：`compiler/Compiler.cpp`（visitWhileStmt + visitForStmt：continueTarget/updateStart 移到 OP_CLOSE_UPVALUE 之前）
- 修改：`compiler/IR.cpp`（visitWhileStmt + visitForStmt：continueLabel 移到 leaveBlockScope 之前）
- 修改：`gui/VariableInspectorPanel.cpp`（refreshLive + onVariableSelected：VM 模式添加 isVmRunning() 守卫）
- 修改：`gui/CallStackPanel.cpp`（refreshLive：VM 模式添加 isVmRunning() 守卫）
- 修改：`interpreter/Interpreter.cpp`（deepCloneForSandbox：闭包 capturedVars COW detach + 递归深拷贝）
- 修改：`lexer/Lexer.cpp`（case 'u'：拒绝 0xD800-0xDFFF 代理码点）
- 修改：`app/VmStepper.cpp`（checkBreakpointHit 改为纯查询；runBatch + stepByMode hitCount 递增移到过滤条件通过后）
- 修改：`debug/DebugController.cpp`（析构函数补 waitCallbacksIdle()）
- 修改：`debug/DebugEvaluator.h`（析构函数补 waitCallbackIdle()）

### 审计发现但保留现状的问题

- **break/continue 时 finally 块三后端不一致**（P1）：Interpreter 执行 finally，VM/RegisterVM 跳过。修复需 finally 续跳机制（中等规模改动），保留现状
- **try-finally return 语义**（P2）：三后端一致跳过 finally，设计决策，保留现状
- **Formatter 插值注释错位**（P2）：正确修复需 AST 注释附着（架构改动），保留现状
- **break 跳过 upvalue 关闭**（P2）：breakTarget 涉及多处回填，风险较高，保留现状

### 验证"保留现状"项实际不需要架构改动的结论

- **Parser C 风格数组 `[]` 后缀**：已完整修复（isClassTypeDeclStart + parseTypeAnnotation + 前瞻验证 + 测试覆盖），不需要架构改动
- **Formatter Assignment 不加括号**：实际上不是 Bug——Parser 右结合 assignment 是最低优先级，`a = b = c` 往返等价
- **UOP_UNKNOWN 处理**：已修复（输出 "+" 保持 AST 可重新解析）
- **nan/inf 处理**：已修复（输出 "0.0/* nan */" 占位注释）

## 2026-07-08 · 第三十八轮：三后端语义一致性深度审计（while continue + upvalue 关闭 + 自动构造 + 调试器断点重复触发 + 跨线程数据竞争）

### 概述

继续对 MiniLang IDE 项目进行深度 Bug 排查，采用四模块并行 agent 审计策略，重点排查三后端语义一致性、闭包/upvalue 生命周期、调试器状态机、跨线程数据竞争等方面。共发现约 35 个问题，本轮修复 7 项明确、低风险、真实可触发的问题（P1 × 5，P2 × 2）。核心修复包括：while 循环 continue 跳过 OP_CLOSE_UPVALUE 导致闭包捕获变 by-reference（三后端不一致）、Interpreter visitVarDecl 自动构造路径缺失默认参数绑定和 closeCapturedVariables、DebugController checkBreak 中 updateLineTracking 顺序错误导致同行断点重复触发、DebugCoordinator callStackCallback 跨线程 const 引用数据竞争。全量 1753/1753 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | Compiler + IR | P1 | while 循环 `continue` 跳转目标在 `OP_CLOSE_UPVALUE` 之前，跳过 upvalue 关闭，闭包捕获的循环局部变量变为 by-reference 而非 by-value 快照，三后端语义不一致（Interpreter 在 continue 时仍调用 closeCapturedVariables） | `compiler/Compiler.cpp` 新增 `continueTarget` 标记放在 OP_CLOSE_UPVALUE 之后，continue 跳转回填改为跳到 continueTarget；`compiler/IR.cpp` 新增 `continueLabel` 放在 `leaveBlockScope` 之后，对齐 for 循环的 continue 跳转目标（updateStart，在 OP_CLOSE_UPVALUE 之后） |
| 2 | Interpreter | P1 | `visitVarDecl` 自动构造路径（`var f: Foo` 无初始化表达式但类型注解为类名）缺失 init 默认参数绑定，init 体引用参数时穿透到父环境 | `interpreter/Interpreter.cpp` 补充 init 默认参数绑定循环（对齐 `constructClassInstance`），包含 EnvGuard 临时切换到 closureEnv 求值默认值 + checkType 类型注解校验 |
| 3 | Interpreter | P1 | 同上自动构造路径缺失 `closeCapturedVariables` 调用，闭包捕获变量丢失 | `interpreter/Interpreter.cpp` `PrevEnvGuard` 改为 `InitEnvGuard`（析构时调用 `closeCapturedVariables`，对齐 `constructClassInstance` 的 InitEnvGuard） |
| 4 | DebugController | P1 | `checkBreak` 中 `updateLineTracking` 在 `shouldPause` 之后执行，`shouldPauseAtBreakpoint` 命中时设 `crossedLine_=false` 被随后的 `updateLineTracking` 覆盖为 true（line != lastSeenLine_），导致 resume 后同行下一个 AST 子表达式断点重复触发 | `debug/DebugController.cpp` 将 `updateLineTracking` 移到 `shouldPause` 之前执行 |
| 5 | DebugCoordinator | P1 | `callStackCallback` 使用 `getCallStack()` 返回 const 引用，跨线程遍历期间 worker 线程 push_back/pop_back 导致迭代器失效 | `interpreter/Interpreter.{h,cpp}` 新增 `getCallStackSnapshot()` 返回值拷贝；`app/DebugCoordinator.cpp` 改用 `getCallStackSnapshot()` |
| 6 | Interpreter | P2 | `callInstanceMethod` 默认参数路径手动 save/restore `currentEnv_`，evaluate 抛异常时残留为 cachedParentEnv | `interpreter/Interpreter.cpp` 改用 RAII guard（对齐 `constructClassInstance`/`callNamedFunction`/`callClosureValue` 的 EnvGuard 模式） |
| 7 | DebugController | P2 | `resume`/`stepIn`/`stepOver`/`stepOut` 清除 `stopped_` 可在 worker 响应 stop 的窗口内取消正在进行的 stop，导致状态机不一致 | `debug/DebugController.cpp` 4 方法添加 `if (stopped_) return;` 守卫，移除 `stopped_ = false;`（stopped_ 的清除应由 `reset()` 新调试会话负责） |

### 关键决策

1. **continue 跳转目标对齐 for 循环**：while 循环的 continue 跳到 loopStart（条件检查点）在 OP_CLOSE_UPVALUE 之前，而 for 循环的 continue 跳到 updateStart 在 OP_CLOSE_UPVALUE 之后。修复将 while 的 continue 跳转目标移到 OP_CLOSE_UPVALUE 之后，与 for 循环行为一致
2. **自动构造路径对齐 constructClassInstance**：visitVarDecl 的自动构造路径（`var f: Foo`）应与 constructClassInstance 保持完全一致的初始化流程，包括默认参数绑定、类型检查、closeCapturedVariables
3. **updateLineTracking 顺序修复而非标志位重置**：原实现 shouldPauseAtBreakpoint 命中时设 crossedLine_=false，但 updateLineTracking 随后覆盖为 true。修复方式是将 updateLineTracking 移到 shouldPause 之前，而非在 shouldPause 之后再次重置 crossedLine_
4. **getCallStackSnapshot 值拷贝而非加锁**：跨线程访问调用栈时，值拷贝比在 worker 线程加锁更轻量，避免 worker 线程在 push_back/pop_back 时阻塞等待 UI 线程遍历完成
5. **stopped_ 粘性标志**：stopped_ 一旦由 stop() 设置，应保持到 reset() 清除。resume/step 清除 stopped_ 可在 worker 响应 stop 的窗口内取消正在进行的 stop

### 修改文件清单

- 修改：`compiler/Compiler.cpp`（while continue 跳转目标移到 OP_CLOSE_UPVALUE 之后）
- 修改：`compiler/IR.cpp`（while continueLabel 移到 leaveBlockScope 之后）
- 修改：`interpreter/Interpreter.{h,cpp}`（visitVarDecl 自动构造默认参数绑定 + InitEnvGuard + callInstanceMethod RAII + getCallStackSnapshot）
- 修改：`debug/DebugController.cpp`（checkBreak updateLineTracking 顺序 + resume/step stopped_ 守卫）
- 修改：`app/DebugCoordinator.cpp`（callStackCallback 改用 getCallStackSnapshot）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

### 保留现状的审计发现

- P2 try-finally 在 return 时不执行 finally 块（Interpreter.cpp visitTryStmt）
- P2 deepCloneForSandbox 闭包浅拷贝导致 capturedVars 变异泄漏
- P2 Formatter 不对 Assignment 节点加括号（往返不等价）
- P2 Parser declaration() 的 `[]` 后缀检查导致 C 风格数组声明解析失败
- P2 Formatter formatInterpolatedString 静默丢弃插值表达式内部注释
- P2 VmStepper checkBreakpointHit hitCount 递增时机错误
- P2 VariableInspectorPanel worker 执行期间读取 Environment map 内容数据竞争

## 2026-07-08 · 第三十七轮：教学模块系统审计与修复（GuidedTour UAF + listener 反注册 + Markdown 腐蚀 + autoTimer 数据竞争 + P2 视觉/性能）

### 概述

对全部教学模块面板（40+ 面板文件）进行系统性 Bug 审计，采用四模块并行 agent 策略，共发现约 45 个问题（P0 × 2，P1 × 10，P2 × ~30，P3 × 1）。本轮修复 P0 全部 + P1 关键项 + P2 主要项共 17 项。核心修复包括：GuidedTour bubble_ UAF（QPointer）、IdeController VM 状态监听器无反注册导致 6 面板析构后 UAF、MarkdownRenderer 关键字 "class" 腐蚀已生成 span 属性、LabManualPanel 选择题索引越界、调试 resume 期间 autoTimer 与 worker 线程数据竞争、多处 P2 视觉/性能/状态机问题。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | GuidedTour | P0 | `bubble_` 裸指针，IDE 析构期 / 多次 start-hideOverlay 累积泄漏 UAF | `QWidget*` → `QPointer<QWidget>`；hideOverlay 中 `deleteLater()` + 置 null 替代 `hide()`；onPanelGuidedTourRequested 连接 `finished → deleteLater` |
| 2 | IdeController | P0 | `vmStateChangedListeners_` 无反注册机制，6 面板 setController 注册捕获裸 this 的 lambda，析构后 notifyVmStateChanged 调用 UAF | `addVmStateChangedListener` 增加 `void* owner` 参数 + `removeVmStateChangedListener(owner)` 延迟清除；6 面板（BytecodeTrace/CallStack/VariableInspector/BreakpointCondition/MemoryModel/VmStackSandbox）析构函数反注册 |
| 3 | MarkdownRenderer | P1 | 关键字 "class" 腐蚀已生成 `<span class="...">` 的属性；关键字正则每次循环重新编译 | 占位符法重写 highlightMiniLang：先收集 span 片段用 `\x01{idx}\x02` 占位，最后统一替换；预编译关键字正则数组 |
| 4 | LabManualPanel | P1 | `onSubmitChoiceExercise` 全局 exerciseIdx vs 选择题内序号混用导致越界 | `choiceGroups_` 按 CHOICE 出现顺序追加，choiceIdx 为选择题内序号；feedbackLabel objectName 改为 `feedback_choice_{idx}` |
| 5 | MemoryModelPanel | P1 | showEvent 中 `start(500)` 覆盖 `setInterval(2000)` 的降频优化 | `start(500)` → `start()`（使用已设的 2000ms 间隔） |
| 6 | LearnerProgress | P1 | load() 校验 `s <= 4` 拒绝合法的 currentStage=5（全通关状态） | `s <= 4` → `s <= LearningPathData::stageCount()` |
| 7 | CallStackPanel | P1 | isVmInitialized 优先级导致 Interpreter 调试显示陈旧 VM 状态；autoTimer resume 期间数据竞争；toString 无 try/catch | 交换 if 优先级；增加 `isDebugPaused()` 检查；toString/typeName 包裹 try/catch |
| 8 | VariableInspectorPanel | P1 | 同 CallStackPanel + onVariableSelected 恒真条件 `!v.isNull() \|\| v.getType()==VAL_NULL` | 同上修复模式 + 改用 `found` 标志 |
| 9 | DebugCoordinator | P1 | 缺少 isPaused() 转发，面板无法判断调试是否处于暂停状态 | 新增 `bool isPaused() const { return debugger_->isPaused(); }` + IdeController::isDebugPaused() |
| 10 | LearningPathPanel | P2 | rgba 格式错误（hex 字符串作首参）+ markActivityCompleted 不幂等 | `rgba(%1, 0.18)` → `rgba(r, g, b, 46)` 数字参数；markActivityCompleted 开头检查已完成则 return |
| 11 | GlossaryPanel | P2 | 链接跳转到新术语时不发射 termActivated（第三十六轮移除 onTermSelected 发射后遗留） | `setCurrentRow(i)` 后添加 `emit termActivated(entries_[i].id)` |
| 12 | TeachingTreePanel | P2 | 搜索清空后被折叠的分类保持折叠状态 | 清空搜索分支添加 `if (anyVisible) tree_->expandItem(top)` |
| 13 | BackendComparePanel | P2 | 三后端串行执行无 processEvents 导致 UI 冻结 | 三个 runXxx 调用之间插入 processEvents + 状态文字更新 |
| 14 | DebugPanel | P2 | currentStack_ 存储完整调用栈含全部 Value 拷贝（仅显示 200 帧） | `currentStack_ = stack` → 截断到 `MAX_CALL_STACK_STORE = 200` |
| 15 | VmStackSandboxPanel | P2 | RegisterVM 模式标题/空状态文本不符；onTraceRunAll 100000 步同步阻塞 UI | 动态设置 GroupBox 标题 + 空状态文本；每 1000 步 processEvents |
| fix | VmStackSandboxPanel | build | onTraceRunAll 使用 QApplication::processEvents 缺 `#include <QApplication>` | 补充 include |

### 关键决策

1. **QPointer 替代裸指针而非 shared_ptr**：GuidedTour 的 bubble_ 是 QWidget 子对象，由 Qt 对象树管理生命周期。QPointer 是 Qt 原生弱引用，对象销毁时自动置 null，比 shared_ptr 更适合 Qt 对象生命周期模型。hideOverlay 用 deleteLater + 置 null 确保 Qt 事件循环安全删除
2. **listener 延迟清除而非 erase**：`removeVmStateChangedListener` 标记 `cb.fn = nullptr` 而非 vector erase，避免在 notifyVmStateChanged 遍历期间修改 vector 导致迭代器失效。notifyVmStateChanged 跳过空 cb。owner 参数用 `void*` 避免 C 风格转换
3. **占位符法解决关键字腐蚀**：MarkdownRenderer 先用控制字符 `\x01{idx}\x02` 占位替代直接 replace，让所有正则匹配在"无 span 标签"的纯净文本上执行，最后统一替换占位符为 span。避免关键字 "class" 匹配到已生成 `<span class="...">` 中的 class 属性
4. **isDebugPaused 转发而非面板直接访问 DebugController**：面板只持有 IdeController 指针，通过 `IdeController::isDebugPaused() → DebugCoordinator::isPaused() → DebugController::isPaused()` 链式转发，保持 Facade 模式封装
5. **autoTimer 数据竞争用 isDebugPaused 守卫而非停止定时器**：调试 resume 期间 autoTimer 仍需为下一步暂停刷新视图，不能停止。改为 autoTimer 触发时检查 isDebugPaused()，仅在暂停状态才调用快照接口，避免与 worker 线程并发访问

### 修改文件清单

- 修改：`gui/GuidedTour.{h,cpp}`（QPointer + deleteLater + finished→deleteLater）
- 修改：`app/IdeController.h`（VmStateChangedListener 结构 + removeVmStateChangedListener + isDebugPaused）
- 修改：`app/DebugCoordinator.h`（isPaused 转发）
- 修改：`gui/{BytecodeTrace,CallStack,VariableInspector,BreakpointCondition,MemoryModel,VmStackSandbox}Panel.{h,cpp}`（析构反注册 + autoTimer isDebugPaused 守卫 + 优先级修复）
- 修改：`gui/MarkdownRenderer.cpp`（占位符法重写 highlightMiniLang）
- 修改：`gui/LabManualPanel.cpp`（choiceIdx 索引修复）
- 修改：`gui/MemoryModelPanel.cpp`（showEvent start() 修复）
- 修改：`gui/LearnerProgress.cpp`（load 校验 stageCount）
- 修改：`gui/{CallStack,VariableInspector}Panel.cpp`（autoTimer 数据竞争 + toString try/catch + 优先级）
- 修改：`gui/LearningPathPanel.cpp`（rgba 数字参数 + markActivityCompleted 幂等）
- 修改：`gui/GlossaryPanel.cpp`（anchorClicked 发射 termActivated）
- 修改：`gui/TeachingTreePanel.cpp`（搜索清空恢复展开）
- 修改：`gui/BackendComparePanel.cpp`（三后端 processEvents）
- 修改：`gui/DebugPanel.cpp`（currentStack_ 截断 200）
- 修改：`gui/VmStackSandboxPanel.cpp`（RegisterVM 标题/空状态 + onTraceRunAll processEvents + #include QApplication）
- 修改：`app/ide.cpp`（onPanelGuidedTourRequested 连接 finished→deleteLater）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

### 保留现状的审计发现

以下 P2/P3 审计发现因需架构改动或风险较高保留现状：
- BugHuntPanel onTripleVerify 空输出一致性误判（P2 语义歧义）
- AstBuilderToyPanel onVerifyWithRealParser 纯表达式包装为 VarDecl（P2 教学误导）
- SyntaxExplorerPanel onRunSample 同步阻塞（P1，需架构改动）
- ProfileDashboard paintEvent 硬编码坐标（P2 视觉）
- MemoryModelPanel nanBoxExamples 悬垂指针值（P2 潜在 UAF）
- LabManualPanel applyFolding inCodeBlock 状态不一致（P2 Markdown）
- VariableInspectorPanel valueToBitsHex 堆类型占位符（P3 教学不完整）

## 2026-07-08 · 第三十六轮：IDE UI/UX 七项优化（折叠按钮 + 输出面板 + 背景色 + 运行修复 + 切换动画）

### 概述

本轮完成 7 项 IDE 界面与交互优化，覆盖代码折叠按钮视觉、错误下划线配色、示例代码兼容性、输出面板 VSCode 风格改造、术语表跳转修复、教学模块切换动画流畅度、三面板布局运行修复、背景色完整覆盖。修复了一个阻断构建的 `DebugController::waitCallbacksIdle` 访问权限问题（private→public）。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 模块 | 问题 | 修复方案 |
|---|------|------|----------|
| 1 | CodeEditor | 折叠按钮为方框+/-样式，错误下划线用 Qt::red 过于刺眼 | 折叠标记改为 VSCode 风格雪佛龙箭头（▷/▽）+ 抗锯齿 + 主题感知配色；错误下划线改为 Solarized 红 #DC322F + 淡红背景 |
| 2 | samples | closure_patterns.mini / higher_order.mini 使用匿名函数表达式 `fun(){...}` 不被 MiniLang 语法支持 | 改为命名内部函数声明 + 引用返回 |
| 3 | Ide/Output | 输出面板用 Unicode 图标（▶ℹ✓⚠✗）+ 时间戳，不够 VSCode 风格 | 改为文本级别前缀 `[Info]/[Done]/[Warn]/[Error]` + Plain 级无前缀无时间戳 + Solarized 配色 |
| 4 | GlossaryPanel | 单击切换术语条时 `onTermSelected` 发射 `termActivated` 导致跳转关联面板 | 移除 `onTermSelected` 中的 `emit termActivated`，仅保留双击/链接点击触发 |
| 5 | Ide/Teaching | 教学模块切换卡顿——dock 显隐在动画进行中触发 layout 重绘、refresh 阻塞、slideInWidget 220ms 偏长 | dock 显隐移到 setCurrentIndex 之前、refresh 移到之后、slideInWidget 时长降为 150ms |
| 6 | Ide/Editor | `loadCodeIntoMainEditor` 创建新标签后未 `switchToTab`，导致 `codeEditor_` 为 nullptr，`onRun` 直接返回；`bottomPanelHeight_` 默认 600px 导致输出面板"全屏覆盖" | 补充 `switchToTab(newIdx)`；默认高度从 600 改为 220 + 历史>500 迁移 |
| 7 | Ide/Style | 背景色覆盖不完全——centerStack/centerSplitter/welcomePage/replPanel 等缺失背景色；CodeEditor 浅色模式 Base=#ffffff 与行号区 #EEE8D5 割裂 | applyFluentStyle 补全覆盖 7 个 widget；CodeEditor 浅色 Base 改为 #FDF6E3 |
| fix | DebugController | `waitCallbacksIdle()` 声明为 private 但 `DebugCoordinator` 析构需调用 → LNK2019 | 提升为 public（MSVC 将 access specifier 编入 mangled name） |

### 关键决策

1. **折叠标记用雪佛龙箭头而非方框+/-**：VSCode 风格雪佛龙（▷ 折叠/▽ 展开）视觉更轻盈，无方框背景干扰。抗锯齿绘制 + 主题感知配色（折叠态用主题蓝 #268BD2 强调，展开态用次要灰 #586E75）
2. **输出面板 Plain 级无时间戳**：用户程序输出（print 结果）是纯净数据，添加时间戳和级别前缀会干扰阅读。仅系统消息（编译进度/错误/警告）才显示 `[HH:mm:ss] [Level]` 前缀，对齐 VSCode 终端行为
3. **`loadCodeIntoMainEditor` 补 `switchToTab`**：`createNewEditorTab` 返回新标签索引但不设置 `codeEditor_` 成员，导致后续 `onRun()` 的 `if (!codeEditor_) return;` 直接返回。这是"加载代码后点击运行无反应"和"LabManual 触发示例无法运行"的根因
4. **`bottomPanelHeight_` 默认 600→220**：600px 在 800px 高度窗口下占 75% 垂直空间，编辑器区被挤压到 140px，表现为"输出面板全屏覆盖"。220px 对齐 VSCode 默认输出面板高度。对历史已保存的>500px 值做一次性迁移
5. **背景色用 objectName 匹配而非 setPalette**：`applyFluentStyle` 已有 `findChildren<QWidget*>()` + objectName 匹配的框架，为 centerStack_/centerSplitter_/replPanel_ 补设 objectName 后复用此框架，比逐个 setPalette 更一致
6. **CodeEditor 浅色 Base 从 #ffffff 改为 #FDF6E3**：Solarized 主题下行号区是 base2 (#EEE8D5)，编辑区应为 base3 (#FDF6E3)，原 #ffffff 是 VSCode 默认色而非 Solarized，导致"编辑区白色 vs 行号区米黄"割裂
7. **`waitCallbacksIdle` 提升为 public**：MSVC 将成员访问说明符编入 mangled name（A=private, Q=public），从 private 改为 public 后符号名变化，必须重编译 `DebugController.cpp` 才能链接成功

### 修改文件清单

- 修改：`gui/CodeEditor.cpp`（折叠标记雪佛龙箭头 + 错误下划线 Solarized 红 + 浅色 Base #FDF6E3）
- 修改：`samples/mini/closure_patterns.mini`（匿名函数→命名函数）
- 修改：`samples/mini/higher_order.mini`（匿名函数→命名函数）
- 修改：`gui/GlossaryPanel.cpp`（移除 onTermSelected 的 termActivated 发射）
- 修改：`app/ide.cpp`（loadCodeIntoMainEditor 补 switchToTab + restoreLayout 默认 220 + showTeachingPanel 重构 + appendOutput VSCode 风格 + applyFluentStyle 背景覆盖 + outputTextEdit_ 调色板 + centerStack/centerSplitter/replPanel objectName）
- 修改：`debug/DebugController.h`（waitCallbacksIdle private→public）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

## 2026-07-08 · 第三十五轮：后续 28 项待处理问题修复（跨线程 UAF + GC roots + 一致性 + Lexer Unicode 转义 + LRU 缓存）

### 概述

继续处理第三十二轮系统审计标记的 28 项后续待处理问题。本轮修复其中 9 项明确、可独立修复的问题（P1 × 4，P2 × 5），覆盖跨线程 UAF（3 处）、GC roots 不完整、三后端一致性（3 处）、Lexer Unicode 转义缺失、condAstCache 伪 LRU 等。其余 19 项（boundInstance_ COW 矛盾、Formatter 往返等价性、性能优化 ×3、DebugPanel 调用栈拷贝等）因需架构改动或风险较高保留现状。新增 1 个回归测试（String_HexAndUnicodeEscapes）。全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | GUI | P1 | ReplPanel 异步 lambda 裸 `self`，invokeMethod(QueuedConnection) 投递的 lambda 在析构后 dispatch → UAF | `QPointer<ReplPanel>` 替代裸指针 + `qApp` 作为 invokeMethod context + lambda 内 null 检查 |
| 2 | Interpreter | P1 | `currentEnvironment()` 返回裸指针，GUI 线程遍历期间 worker 修改 currentEnv_ → UAF | 新增 `currentEnvironmentShared()` 返回 shared_ptr 副本，variableCallback 改用此方法并链式持有 parent shared_ptr |
| 3 | Debug | P1 | `~DebugCoordinator` 清空 callback 后 worker 线程可能仍在锁外调用 cb() → UAF | DebugController/DebugEvaluator 新增 `activeCallbackCount_` 原子计数 + `waitCallbacksIdle()` spin-wait（RCU 优雅期模式） |
| 4 | Interpreter | P1 | GC roots 遗漏 `savedClassRegistry.fields` + `savedModuleCache`，循环引用类字段/模块变量被误回收 | execute() GC roots 追加遍历 savedClassRegistry.fields + closureEnv + savedModuleCache |
| 5 | RegVM | P2 | `closeUpvaluesFrom` slot 越界静默不拷贝值无 Warning（StackVM 有 Warning） | 补 Warning 日志对齐 StackVM 诊断口径 |
| 6 | ModuleIsolation | P2 | `pathHash` 不 normalize 分隔符，依赖调用者自行 normalize（三处重复） | normalize 逻辑下沉到 pathHash 内部（`\`→`/` + 去 `./` 前缀） |
| 7 | Parser | P2 | 尾逗号两种模式（模式 A `if(check)break` + 模式 B `&& !check`），维护风险不同 | 数组/字典/导入名称列表 3 处统一为模式 B |
| 8 | Lexer | P2 | 不支持 `\uXXXX`/`\xNN` Unicode 转义 | 转义 switch 新增 `case 'u'`（4位十六进制→UTF-8）和 `case 'x'`（2位十六进制→字节） |
| 9 | IdeController | P2 | `condAstCache` 用 `unordered_map::erase(begin())` 淘汰，非 LRU 非 FIFO（伪随机） | 改为 `list + unordered_map<key, list::iterator>` 经典 LRU，命中 move_to_front，超上限 pop_back |

### 关键决策

1. **ReplPanel 用 QPointer + qApp 而非 QSharedPointer**：ReplPanel 是 QWidget 不能用 std::shared_ptr 管理；QPointer 是 Qt 原生的弱引用 QObject 指针，对象销毁时自动置 null。qApp 作为 invokeMethod context 确保 lambda 在主线程执行且 qApp 永远存活，lambda 内检查 QPointer 是否为 null 避免悬垂访问
2. **currentEnvironmentShared() 而非修改 currentEnvironment() 返回类型**：保留原 `Environment* currentEnvironment()` 向后兼容（同线程调用方不受影响），新增 `shared_ptr` 版本供跨线程调用方使用。variableCallback 持有 shared_ptr 副本遍历 parent 链，每个节点通过 shared_ptr 赋值延长生命周期
3. **RCU 优雅期模式解决析构竞态**：DebugController/DebugEvaluator 的 callback 采用 copy-then-call-outside-lock 模式（锁内拷贝 std::function，锁外调用），清空 callback 无法等待锁外 cb() 完成。引入 `activeCallbackCount_` 原子计数，cb() 期间 fetch_add，完成后 fetch_sub（RAII guard），析构清空 callback 后 spin-wait 直到计数归零。比 BlockingQueuedConnection 更轻量（不需要事件循环），比 mutex 更安全（不会死锁）
4. **GC roots 追加 closureEnv**：savedClassRegistry 中每个 ClassInfo 除了 fields（字段默认值）外，还有 closureEnv（类定义时捕获的环境），两者都可能持有堆对象引用。仅遍历 fields 不够，还需遍历 closureEnv 的 snapshotLocalVariables
5. **pathHash normalize 下沉而非提取公共函数**：将 normalize 逻辑放在 pathHash 内部比提取 `ast::normalizeModulePath` 公共函数更彻底——即使未来新增调用者忘记 normalize，pathHash 也能保证 hash 一致性。三后端现有的 normalize 代码保留（幂等操作，不影响正确性）
6. **Parser 尾逗号统一为模式 B**：模式 A（`if(check(CLOSE)) break` 在 do 体内开头）比模式 B（`while(match(COMMA) && !check(CLOSE))`）更脆弱——do 体内的 break 容易被误删。模式 B 的继续条件集中在一处，更内聚
7. **condAstCache 用 list + map 而非 LinkedHashMap**：C++ 标准库没有 LinkedHashMap，`std::list` + `std::unordered_map<key, list::iterator>` 是经典 LRU 实现。list 的 splice 操作 O(1) 实现 move_to_front，unordered_map 提供 O(1) 查找。32 条上限对调试场景足够

### 关键教训

1. **Qt::QueuedConnection 的生命周期陷阱**：`QMetaObject::invokeMethod(self, lambda, Qt::QueuedConnection)` 投递的 lambda 在接收对象的事件队列中，`~ReplPanel` 的 `wait()` 只等待 future 完成，不处理事件队列。析构后回到事件循环才 dispatch → UAF。所有跨线程投递的 lambda 必须用 QPointer 或 shared_ptr guard，context 用 qApp（永远存活）而非可能被销毁的对象
2. **shared_ptr 链式持有保证 parent 链安全**：遍历 Environment parent 链时，仅持有当前节点的 shared_ptr 不够——parent 可能在遍历期间被析构。通过 `currentShared = currentShared->parent` 赋值，每个节点都持有 shared_ptr 副本，确保整条链不被析构
3. **copy-then-call-outside-lock 模式的析构竞态**：锁内拷贝 callback、锁外调用的模式无法被析构时的清空操作等待。任何采用此模式的 callback 系统都需要 activeCallbackCount_ + spin-wait 或类似 RCU 优雅期机制，否则析构期间 worker 线程仍在锁外调用 cb() → UAF
4. **GC roots 收集必须覆盖所有 REPL 暂存状态**：saveReplState 暂存的不只是 savedGlobalEnv，还有 savedClassRegistry（含 fields + closureEnv）和 savedModuleCache。每类暂存状态都可能持有堆对象引用，必须全部作为 GC roots 传入，否则 GC 误判循环引用孤岛并清空 elements
5. **unordered_map erase(begin()) 不是 LRU**：unordered_map 的迭代顺序由哈希函数决定，与插入/访问顺序无关。需要 LRU 时必须用 `list + unordered_map<key, list::iterator>`，命中时 splice move_to_front，淘汰时 pop_back

### 修改文件清单

- 修改：`gui/ReplPanel.h`（+QPointer/QCoreApplication include）
- 修改：`gui/ReplPanel.cpp`（QPointer 替代裸 self + invokeMethod context 改 qApp + null 检查）
- 修改：`interpreter/Interpreter.h`（+currentEnvironmentShared 声明）
- 修改：`interpreter/Interpreter.cpp`（+currentEnvironmentShared 实现；GC roots 追加 savedClassRegistry/savedModuleCache 遍历）
- 修改：`app/DebugCoordinator.cpp`（variableCallback 改用 currentEnvironmentShared；析构补 waitCallbacksIdle）
- 修改：`debug/DebugController.h`（+activeCallbackCount_ 成员 + waitCallbacksIdle 声明）
- 修改：`debug/DebugController.cpp`（getVariableSnapshot/getCallStack 增减计数 + waitCallbacksIdle 实现 + `<thread>` include）
- 修改：`debug/DebugEvaluator.h`（+activeCallbackCount_ + waitCallbackIdle + evaluate 增减计数 + `<atomic>/<thread>` include）
- 修改：`compiler/RegisterVM.cpp`（closeUpvaluesFrom slot 越界补 Warning）
- 修改：`ast/ModuleIsolation.cpp`（pathHash normalize 下沉）
- 修改：`parser/Parser.cpp`（3 处尾逗号统一为模式 B）
- 修改：`lexer/Lexer.cpp`（转义 switch 新增 case 'x' / case 'u'）
- 修改：`app/IdeController.cpp`（condAstCache 改 list + unordered_map LRU + `<list>` include）
- 修改：`tests/TestLexer.cpp`（String_UnknownEscapeRejected 改用 \q；+String_HexAndUnicodeEscapes 回归测试）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

### 保留现状（19 项）

- **boundInstance_ COW 矛盾**：最终一致性已通过 writeBack 补偿，改用 const_cast 绕过 COW 风险高
- **Formatter 往返等价性（3 项）**：UOP_UNKNOWN/nan/inf 需 Lexer 扩展 nan/inf 字面量，涉及多模块改动
- **性能优化（3 项）**：BIN_AND/OR tempSlot 累积、OP_MEMBER_GET internConcat、DebugPanel 调用栈全量拷贝——纯性能优化无正确性问题，改动复杂度高

## 2026-07-08 · 第三十四轮：7 项教学面板与 IDE 体验问题修复（AST 构建器 + BugHunt 崩溃 + 性能仪表盘 + 代码旅程 + 学习中心返回）

### 概述

针对用户反馈的 7 项教学面板与 IDE 体验问题做集中修复，覆盖 AST 构建器节点关系、BugHunt 模式崩溃、性能仪表盘三后端失败与数据量过大、布局留白、代码生命旅程完成条件、学习中心返回编辑器等。核心收益：(1) 修复 BugHuntPanel 构造期间 refreshBugChips 无限递归栈溢出（P0 崩溃）；(2) 修复性能仪表盘类实例化场景三后端全部失败（构造方法名错误）；(3) 降低性能仪表盘重场景数据量避免卡死；(4) 修复 AST 构建器叶子节点错误嵌套；(5) 学习中心新增「← 返回编辑器」按钮解决迷路问题。全量 1762/1762 测试通过。

### 问题与修复对应表

| # | 问题 | 修复方案 | 修改文件 |
|---|------|---------|---------|
| 1 | AST 构建器添加 Number 等叶子节点后焦点切换到新节点，导致后续添加形成错误嵌套而非兄弟节点 | `addNodeWithLabel` 根据 label 前缀判断是否为叶子节点（Number:/Identifier:），叶子节点添加后保持父节点选中（`tree_->setCurrentItem(current)`），非叶子节点保持原行为（切换到新子节点） | `gui/AstBuilderToyPanel.cpp` |
| 2 | 点击 Bug 狩猎模式时程序崩溃（P0 栈溢出） | `refreshBugChips` 中将 `isVisible()` 判定改为难度筛选条件判定（与 firstVisible 口径一致），避免构造期间父 widget 未 show() 时 isVisible() 恒返回 false 导致 refreshBugChips → onItemSelected → refreshBugChips 无限递归；同时 `onItemSelected` 添加幂等保护（相同索引且 hintLevel_==0 时直接 return） | `gui/BugHuntPanel.cpp` |
| 3 | 性能仪表盘类实例化循环三个后端都失败 | 源码 `fun new(px, py)` 改为 `fun init(px, py)`——MiniLang 三后端均查找名为 "init" 的构造方法，"new" 会被识别为普通方法导致实例化后字段未初始化 | `gui/ProfileDashboardPanel.cpp` |
| 4 | 性能仪表盘斐波那契递归和字典访问循环数据量过大运算太慢 | fib-recursion 从 fib(20) 降为 fib(15)；dict-access 从 10000 次降为 2000 次；对应 title 和 description 同步更新 | `gui/ProfileDashboardPanel.cpp` |
| 5 | 性能仪表盘每个循环的解释占满左下角空间，留下一小截空白 | 移除 `scenarioDesc_->setMaximumWidth(220)`；左栏布局改为 `addWidget(scenarioList_, 0)` + `addWidget(scenarioDesc_, 1)`，scenarioDesc_ 拉伸填满左下角剩余空间 | `gui/ProfileDashboardPanel.cpp` |
| 6 | 代码生命旅程描述与实际不符（原称"30s 小动画"实际为静态信息图），完成条件需改为点进去观看即完成 | `CodeJourneyInfoPanel` 移除 6 阶段访问跟踪（visitedStages_/kTotalStages/markStageVisited），新增 `journeyCompleted_` bool 成员 + `markCompleted()` 方法 + `showEvent` override，首次显示即标记完成并 emit journeyCompleted；`LearningPathData.cpp` 标题从"🚀 代码生命旅程动画"改为"🚀 代码生命旅程"，描述从"一段 30 秒小动画..."改为"一张静态信息图...点进去看即算完成"；`app/ide.cpp` 连接 journeyCompleted → markActivityCompleted("code-journey") | `gui/CodeJourneyInfoPanel.{h,cpp}`、`gui/LearningPathData.cpp`、`app/ide.cpp` |
| 7 | 学习中心打开后回不到代码编辑区（唯一返回入口是 TeachingTreePanel 顶部树节点，隐蔽） | `TeachingPanelHeader` 新增「← 返回编辑器」按钮（backBtn_，objectName="teachingBackBtn"）+ `returnToEditorRequested` 信号；`app/ide.cpp` `wrapTeachingPanel` 中连接 returnToEditorRequested → showEditorArea；QSS 浅色背景（#FDF6E3）+ Solarized 风格 hover（#EEE8D5 + #268BD2 边框） | `gui/TeachingPanelHeader.{h,cpp}`、`app/ide.cpp` |

### 关键决策

1. **AST 构建器叶子节点焦点保持而非切换**：原实现无论添加什么节点都 `setCurrentItem(child)`，导致连续添加叶子节点时焦点累积下沉形成错误嵌套。叶子节点（Number:/Identifier:）无子节点，添加后应保持父节点选中以便继续添加兄弟节点；非叶子节点保持原行为（切换到新节点便于为其添加子节点）
2. **BugHuntPanel isVisible() 在构造期间不可用**：`QWidget::isVisible()` 在父 widget 未 show() 时恒返回 false，构造期间调用 refreshBugChips 用 isVisible() 判定芯片是否被难度筛选隐藏会得到错误结果，触发 onItemSelected → refreshBugChips 无限递归。改用难度筛选条件判定（与 firstVisible 计算口径一致）从根因消除递归；同时给 onItemSelected 添加幂等保护作为防御
3. **MiniLang 构造方法必须命名为 "init"**：三后端（Interpreter/StackVM/RegisterVM）均查找名为 "init" 的方法作为类构造函数，"new" 会被识别为普通方法。性能仪表盘 class-instantiation 场景用 `fun new(px, py)` 导致实例化后字段未初始化，三后端全部失败。改为 `fun init(px, py)` 对齐三后端语义
4. **性能仪表盘数据量平衡**：fib(20) 在三后端串行执行时耗时过长导致 UI "未响应"；fib(15) 耗时约 fib(20) 的 1/15，仍能体现递归性能差异但不会卡死。dict-access 10000 次 → 2000 次同理
5. **代码生命旅程完成条件简化**：原 6 阶段访问跟踪要求用户点击 6 个跳转按钮才算完成，过于繁琐且与"点进去观看"的用户预期不符。改为 showEvent 首次显示即标记完成——用户点进面板观看信息图即算完成，跳转按钮仅作为深入探索的入口
6. **学习中心返回入口显式化**：原唯一返回入口是 TeachingTreePanel 顶部树节点（"📝 代码编辑器"），隐蔽且需先切回树导航。在每个教学面板的 TeachingPanelHeader 添加显式「← 返回编辑器」按钮，统一连接到 showEditorArea()，用户在任何教学面板都能一键返回

### 关键教训

1. **QWidget::isVisible() 在构造期间不可用**：`isVisible()` 依赖 widget 已被 show() 的运行时状态，构造期间（父 widget 未 show()）恒返回 false。任何在构造期间调用的刷新逻辑都不能用 isVisible() 判定子 widget 的显隐状态，应改用业务条件（如筛选条件）直接计算
2. **递归刷新函数必须有终止条件**：refreshBugChips 调用 onItemSelected，onItemSelected 又调用 refreshBugChips，若无幂等保护或终止条件会无限递归栈溢出。递归刷新函数应添加状态比较（如"当前索引未变则不刷新"）作为终止条件
3. **MiniLang 构造方法命名是三后端共同约定**：三后端均硬编码查找 "init" 作为构造方法名，任何其他名称（如 "new"/"constructor"）都会被识别为普通方法。教学场景的示例代码必须遵守此约定
4. **完成条件应匹配用户预期**：教学活动的完成条件应反映用户的实际学习行为，而非强制要求遍历所有交互元素。"点进去观看即完成"比"点击 6 个跳转按钮才算完成"更符合用户对"浏览型活动"的预期
5. **返回入口应在用户视线焦点处**：教学面板的返回入口应在 TeachingPanelHeader（用户视线焦点）而非依赖切回树导航。每个面板的 header 都应有统一的返回按钮，避免用户在面板间迷路
6. **性能基准场景的数据量需平衡教学价值与响应性**：数据量过大导致 UI 卡死违背教学目的（用户无法观察三后端差异），数据量过小无法体现性能差异。fib(15) 和 dict 2000 次是兼顾两者的平衡点

### 修改文件清单

- 修改：`gui/AstBuilderToyPanel.cpp`（issue 1：叶子节点焦点保持）
- 修改：`gui/BugHuntPanel.cpp`（issue 2：refreshBugChips 递归栈溢出修复 + onItemSelected 幂等保护）
- 修改：`gui/ProfileDashboardPanel.cpp`（issue 3/4/5：fun new → fun init；fib(20)→fib(15)；dict 10000→2000；布局 stretch 修复）
- 修改：`gui/CodeJourneyInfoPanel.h`（issue 6：移除 6 阶段访问跟踪，新增 journeyCompleted_/markCompleted/showEvent）
- 修改：`gui/CodeJourneyInfoPanel.cpp`（issue 6：showEvent 首次显示即完成；refreshProgress 基于 journeyCompleted_；onJumpTo* 移除 markStageVisited）
- 修改：`gui/LearningPathData.cpp`（issue 6：code-journey 活动标题/描述更新）
- 修改：`gui/TeachingPanelHeader.h`（issue 7：新增 backBtn_ 成员 + returnToEditorRequested 信号）
- 修改：`gui/TeachingPanelHeader.cpp`（issue 7：新增「← 返回编辑器」按钮 + QSS 样式）
- 修改：`app/ide.cpp`（issue 6/7：连接 journeyCompleted → markActivityCompleted；连接 returnToEditorRequested → showEditorArea）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

## 2026-07-08 · 第三十二轮：全项目性能与 Bug 系统审计（四模块并行审计 + 14 项修复）

### 概述

对 MiniLang IDE 项目进行系统性性能检测与 Bug 排查。采用四模块并行 agent 审计策略（VM/IR 后端、Interpreter/Value、Parser/Lexer/Formatter、Debugger/GUI 线程安全），共发现 42 个问题（P1 × 17，P2 × 25）。本轮修复其中 14 项明确、低风险、真实可触发的问题（P1 × 8，P2 × 6），覆盖栈平衡、COW 语义、类型检查、作用域隔离、线程安全、IR 完整性等。其余 28 项（需架构改动或深入评估的 GC roots、跨线程 UAF、Formatter 边界 AST、Lexer Unicode 转义等）标记为后续处理。全量 1125/1125 测试通过。

### 问题与修复对应表

| # | 模块 | 严重性 | 问题 | 修复方案 |
|---|------|--------|------|----------|
| 1 | VM | P1 | `executeMethodCall` 默认参数错误路径栈残留（3 处 return 未 popN） | 3 处 `return runtimeError` 前加 `popN(1+fieldCount+argCount)` |
| 2 | VM | P1 | `executeCall` 类构造 `extraSlots<0` 错误路径栈残留 | `return runtimeError` 前加 `popN(1+fieldCount+args.size())` |
| 3 | IR | P1 | `visitBinaryOp` default 分支未设 `hasError_`，Release 下静默产坏代码 | 新增 `hasError_ = true`，调用方检测后中止 IR 构建 |
| 4 | Interpreter | P1 | `writeBackCapturedVars` 触发 COW（未套用 `closeCapturedVariables` 的 const_cast 模式） | 应用 `const_cast<const Value&>` 绕过 COW，与 B1 fix 一致 |
| 5 | Interpreter | P1 | `constructClassInstance` init 参数缺类型检查（三后端不一致） | 参数绑定循环中加 `checkType`，与 `callClosureValue` P1-4 fix 对齐 |
| 6 | ModuleIsolation | P1 | finally 块未 `pushScope`，局部变量污染外层作用域 | 与 catch 块对齐加 `pushScope/popScope` |
| 7 | Debug | P1 | `stop()` 设 `running_=false` 使 `checkBreak` 静默 return 而非 throw | 移除 `running_=false`，由 `reset()`/`prepareRun` 负责重置 |
| 8 | Worker | P1 | `~WorkerManager`/`forceStop` 成功路径未 disconnect finished 信号 → UAF | 补充 `workerThread_->disconnect(this)`，与 `stopForClose` 对齐 |
| 9 | IR | P2 | `TYPE_CHECK` lowering 不更新 `vregStackDepth_` | 添加 `vregStackDepth_[src_vreg] = code.size()`，与 DUP/LOAD_MUTATED 一致 |
| 10 | RegVM | P2 | `vregToReg` 溢出后 `lowerInstruction` 继续写入损坏字节码 | `lowerInstruction` 入口加 `if (hasError_) return false` 快速失败 |
| 11 | Interpreter | P2 | super 调用 `set("this", std::move(obj))` 未检查返回值 | 检查返回值，false 时 `runtimeError` |
| 12 | Lexer | P2 | 插值片段列号用字节偏移而非 UTF-8 码位 | 改用 `columnAt(start_)`，与其他路径一致 |
| 13 | Formatter | P2 | 注释排序用 `std::sort`（非稳定），同行多注释顺序未定义 | 改 `std::stable_sort` + 列号次要排序键 |
| 14 | Parser | P2 | `peek()` 无边界检查依赖 TK_EOF 哨兵 | 与 `previous()` P0-14 fix 对齐加边界检查 |

### 关键决策

1. **四模块并行审计**：VM/IR、Interpreter/Value、Parser/Lexer/Formatter、Debugger/GUI 四个 search agent 并行覆盖全项目，每个 agent 深度审读源码并验证行号。比顺序审计快 4 倍，且各 agent 专注单一模块能发现更深层问题
2. **P1 优先修复真实可触发问题**：14 项修复中 8 个 P1 均为正常 MiniLang 源码可触发或运行时常见路径（方法默认参数、类构造、闭包写回、super 调用、调试停止）。需外部构造 AST 才能触发的 Formatter 边界问题（UOP_UNKNOWN/nan/inf/空 value）标记为后续处理，因正常路径无影响
3. **writeBackCapturedVars 复用 closeCapturedVariables 的 const_cast 模式**：`capturedVars()` 非 const 重载触发 `ensureUnique` COW，导致修改写到副本而非原件。`Environment::closeCapturedVariables`（B1 fix）已用 `const_cast<const Value&>` 绕过，但 `writeBackCapturedVars` 未同步。两处实现同一不变量（直接修改共享 ClosureData），应统一模式
4. **DebugController.stop() 不重置 running_**：原 `running_=false` 有双重语义（"已停止"与"未启动"），`checkBreak` 行 33 `if (!running_) return` 在 `stopped_=false` 且 `running_=false` 时静默 return 而非 throw `DebugStopException`。移除后 `running_` 由 `reset()` 行 472 和 `prepareRun` 系列负责，`stop()` 后 `checkBreak` 优先检查 `stopped_` 抛异常
5. **WorkerManager 三路径 disconnect 对齐**：`stopForClose`（BUG-IDE-15 fix）已 disconnect，但 `~WorkerManager` 析构和 `forceStop` 成功路径未同步。`QThread::finished` 在 `wait()` 返回前发射，QueuedConnection lambda 已投递主线程队列，`reset()` 销毁 QThread 对象但 lambda 仍持 `this`。三路径统一 `disconnect(this)` 消除 UAF
6. **ModuleIsolation finally 块作用域**：原注释"finally 块不引入新作用域（无 catch 变量）"是误解——finally 块内可包含 VarDecl，应隔离。不 pushScope 会导致 finally 内局部变量污染外层作用域，`resolveToModuleTopLevel` 误判，模块非导出顶层名引用不被重命名

### 关键教训

1. **错误路径必须保持栈平衡**：`executeMethodCall`/`executeCall` 的默认参数填充路径有 4 处 `return runtimeError` 未清理栈上已推入的 this+fields+args。虽然 `hasError_=true` 后主循环退出不崩溃，但栈残留会在 REPL 错误恢复或调试器栈检查时报告错误状态。每个 `push` 路径的错误返回点都应配对 `popN`
2. **COW 绕过模式必须全局一致**：`capturedVars()`/`fields()` 等 const 重载触发 COW 的方法，在直接修改共享数据时必须用 `const_cast<const Value&>` 绕过。`closeCapturedVariables`（B1 fix）和 `executeCall` 类构造（性能修复）已应用此模式，但 `writeBackCapturedVars` 遗漏。同一不变量的所有写入点应统一模式
3. **assert 在 Release 构建中被剥离不能作为唯一防线**：`visitBinaryOp` default 分支仅有 `assert(false)` + `Logger::Error`，Release 下两者都不阻断控制流，悬空 vreg 被返回污染 IRModule。必须同时设置 `hasError_` 等可检测的错误标志，让调用方能中止处理
4. **信号语义不能有双重含义**：`running_=false` 既表示"已停止"又表示"未启动"，导致 `checkBreak` 在 `stop()` 后静默 return 而非 throw。每个标志应有单一语义，状态转换由明确的方法负责（`stop` 只设 `stopped_`，`reset`/`prepareRun` 管 `running_`）
5. **Qt 跨线程 QueuedConnection 的生命周期陷阱**：`QThread::finished` 在 `wait()` 返回前已发射，QueuedConnection lambda 已投递主线程队列。即使 `workerThread_.reset()` 销毁 QThread 对象，已投递的 lambda 仍持 `this` 指针。所有销毁 worker 的路径（析构/stopForClose/forceStop）都必须先 `disconnect(this)`，不能仅依赖 `stopForClose` 的修复

### 修改文件清单

- 修改：`compiler/VMCalls.cpp`（问题1/2：executeMethodCall + executeCall 类构造错误路径栈平衡）
- 修改：`compiler/IR.cpp`（问题3：visitBinaryOp hasError_；问题9：TYPE_CHECK vregStackDepth_）
- 修改：`compiler/RegisterBytecodeBackend.cpp`（问题10：lowerInstruction 入口 hasError_ 检查）
- 修改：`interpreter/InterpreterCalls.cpp`（问题4：writeBackCapturedVars const_cast；问题5：constructClassInstance checkType）
- 修改：`interpreter/Interpreter.cpp`（问题11：super set 返回值检查）
- 修改：`ast/ModuleIsolation.cpp`（问题6：finally 块 pushScope/popScope）
- 修改：`lexer/Lexer.cpp`（问题12：插值列号改用 columnAt）
- 修改：`formatter/Formatter.cpp`（问题13：stable_sort + 列号）
- 修改：`parser/Parser.cpp`（问题14：peek 边界检查）
- 修改：`debug/DebugController.cpp`（问题7：stop 不重置 running_）
- 修改：`app/WorkerManager.cpp`（问题8：析构 + forceStop 补充 disconnect）
- 文档同步：`CHANGELOG.md` + `docs/development.md` + `project_memory.md`

### 后续待处理（28 项，需架构改动或深入评估）

- **Interpreter GC roots 不完整**：遗漏 `savedClassRegistry_.fields` 与 `savedModuleCache_`，需扩展根集收集
- **跨线程 UAF（3 项）**：ReplPanel 异步 lambda 裸 self、`currentEnvironment()` 跨线程裸指针、DebugCoordinator 析构清空 callback 竞态，需 mutex/BlockingQueuedConnection/shared_ptr guard
- **Formatter 往返等价性（3 项）**：UOP_UNKNOWN/nan/inf/空 value 输出不可解析，需外部构造 AST 触发，正常路径无影响
- **Lexer Unicode 转义**：不支持 `\uXXXX`/`\xNN`，需扩展词法分析
- **性能优化（5 项）**：BIN_AND/OR tempSlot 累积、OP_MEMBER_GET internConcat、OP_BUILD_ARRAY 堆分配、condAstCache 伪 LRU、DebugPanel 调用栈全量拷贝
- **一致性（多 项）**：closeUpvaluesFrom 两 VM 实现差异、boundInstance_ COW 引用语义矛盾、pathHash 跨平台分隔符、Parser 尾逗号处理不一致等



## 2026-07-08 · 第三十三轮：IDE 三栏布局与教学面板 UI 6 项优化（面板切换动画 + 输出窗口全宽 + 欢迎页编辑器展开 + BugHunt 代码加载动画 + BugHunt 芯片栏 + LabManual 芯片栏）

### 概述

针对用户反馈的 6 项 IDE 三栏布局与教学面板 UI 问题做集中修复：(1) 左侧面板切换不流畅——用 slideInWidget 替代 fadeInWidget；(2) 输出窗口仅覆盖代码编辑区——确认 bottomContainer_ 已在 mainLayout 全宽布局，新增平滑高度展开/收起动画；(3) 欢迎页打开文件编辑器未完全展开——修复 ensureEditorVisible 动画触发条件；(4) BugHunt 加载代码到编译器交互——教学模式下编辑器从右侧 45/55 分栏滑入，关闭时教学区平滑延展恢复；(5) BugHunt 目录区过大——大目录列表替换为水平芯片栏（参考 TokenPuzzlePanel 关卡芯片），4 栏 splitter 简化为 2 栏；(6) LabManual 目录区过大——章节列表替换为章节芯片栏，3 栏 splitter 简化为 2 栏。全量 1762/1762 测试通过。

### 问题与修复对应表

| # | 问题 | 修复方案 | 修改文件 |
|---|------|---------|---------|
| 1 | 面板切换不流畅且逻辑有误 | `onActivityChangedById` 中 `fadeInWidget` → `slideInWidget`（O(1) pos 动画替代 O(n) QGraphicsOpacityEffect） | `app/ide.cpp` |
| 2 | 输出窗口仅覆盖代码编辑区且把教学内容顶掉 | 确认 `bottomContainer_` 已在 `mainLayout` 垂直布局（全宽不挤压教学）；`showBottomPanel`/`hideBottomPanel` 新增 `QPropertyAnimation` 驱动 `maximumHeight` 平滑展开/收起；`fadeInWidget` → `slideInWidget` | `app/ide.cpp` |
| 3 | 欢迎界面打开文件后代码编辑区未完全展开 | `ensureEditorVisible` 移除 `centerStack_->isHidden()` 前置条件，改为 `editorTabWidget_` wasHidden 即触发动画；动画结束后延迟 `centerStack_->hide()` | `app/ide.cpp` |
| 4 | BugHunt 加载代码到编译器交互流程不佳 | `loadCodeIntoMainEditor` 教学模式下不调用 `ensureEditorVisible`（保留教学面板），改为 45%/55% 分栏动画；`onEditorTabCloseRequested` 教学模式下关闭最后一个标签时教学区平滑延展恢复（不回欢迎页） | `app/ide.cpp` |
| 5 | BugHunt 目录区域占比过大，功能区域重叠 | 4 栏 splitter（列表|变体|描述|代码）→ 芯片栏 + 2 栏 splitter（描述|代码）；15 个 Bug 芯片 + 7 个变体芯片水平排列，Solarized QSS 三态（默认/当前/难度色边框）；搜索框移至芯片栏右侧 | `gui/BugHuntPanel.{h,cpp}` |
| 6 | 实验手册内容区域拥挤，目录占比过大 | 3 栏 splitter（章节|内容|代码）→ 章节芯片栏 + 2 栏 splitter（内容|代码）；8 个章节芯片水平排列，Solarized QSS 二态（默认/当前）；释放 130px 目录宽度给内容阅读区 | `gui/LabManualPanel.{h,cpp}` |

### 关键决策

1. **slideInWidget 替代 fadeInWidget 全面铺开**：QGraphicsOpacityEffect 的 O(n) 离屏 pixmap 合成在第二十九轮已定位为系统性卡顿根因。本轮将 `showBottomPanel`/`showRightPanel`/`onActivityChangedById` 三处剩余 fadeInWidget 调用全部替换为 slideInWidget（O(1) pos 动画），与第二十九轮的 LearningPathPanel 修复形成完整闭环
2. **bottomContainer_ 平滑高度动画用 maximumHeight 而非 fixedHeight**：`QPropertyAnimation` 驱动 `maximumHeight` 属性从 0 到目标值，动画结束后恢复 `setFixedHeight` 约束允许用户手动拖拽调整。比直接 `setFixedHeight + show()` 的瞬时弹出更柔和，对齐 VS Code 面板动效节奏
3. **BugHunt 芯片栏复用 TokenPuzzlePanel 关卡芯片模式**：`QPushButton#bugChip` + QSS 动态属性 `[current='true']` / `[diff='0|1|2']`（难度色边框：绿/黄/红）+ `unpolish/polish` 触发重评估。芯片文本两行（编号 + 严重性），tooltip 显示完整标题与类别。比 QListWidget 目录更紧凑，释放 140px 水平空间给描述与代码区
4. **LabManual 章节芯片编号而非标题**：8 章节用编号 1-8 芯片（tooltip 显示完整标题），比 QListWidget 130px 目录列更紧凑。3 栏→2 栏 splitter 释放 130px 给内容阅读区，提升长文阅读体验
5. **ensureEditorVisible 动画触发条件用 wasHidden 而非 centerStack_->isHidden()**：从欢迎页打开文件时 centerStack_ 显示欢迎页（非 hidden），原条件跳过动画导致编辑器未完全展开。改为检测 editorTabWidget_ 是否 wasHidden 即触发动画，覆盖欢迎页场景

### 关键教训

1. **QPropertyAnimation 驱动 maximumHeight 后需恢复约束**：动画期间 `setFixedHeight(0)` + 动画 `maximumHeight` 0→target；动画结束后须 `setFixedHeight(bottomPanelHeight_)` 恢复固定约束，否则用户拖拽后面板高度不固定
2. **芯片栏搜索过滤用 setVisible 而非 setHidden**：QPushButton 的 `setVisible(false)` 与 `setHidden(true)` 等效，但与 QLayout 的间距计算配合更好（隐藏后布局自动收紧）
3. **教学模式下编辑器关闭应恢复教学区而非回欢迎页**：`onEditorTabCloseRequested` 在 `!centerInEditorMode_` 时应 `centerStack_->show()` + 动画延展教学区到全宽，而非 `centerStack_->setCurrentWidget(welcomePage_)` 跳回欢迎页（破坏教学上下文）
4. **芯片点击 lambda 捕获索引而非用 Qt::UserRole**：QListWidget 模式需 `item->data(Qt::UserRole)` 间接取索引（因难度筛选打乱顺序），芯片模式直接 lambda 捕获 `const int itemIdx = i` 即可，简化数据流

### 修改文件清单

- 修改：`app/ide.cpp`（issue 1/2/3/4：slideInWidget 替换 + 平滑高度动画 + ensureEditorVisible 修复 + loadCodeIntoMainEditor/onEditorTabCloseRequested 教学模式动画）
- 修改：`gui/BugHuntPanel.{h,cpp}`（issue 5：芯片栏替代大目录列表，4 栏→2 栏 splitter，新增 populateBugChips/refreshBugChips/populateVariantChips/refreshVariantChips）
- 修改：`gui/LabManualPanel.{h,cpp}`（issue 6：章节芯片栏替代大目录列表，3 栏→2 栏 splitter，新增 populateChapterChips/refreshChapterChips）

## 2026-07-08 · 第三十一轮：教学面板 11 项问题排查与修复（Glossary/SyntaxExplorer 自动加载 + GuidedTour 跨页定位 + BugHunt 调试闭环 + ProfileDashboard 状态增强 + fadeInWidget 全局清理）

### 概述

针对用户反馈的教学面板 11 项问题做集中排查与修复，覆盖自动加载缺失、指引框定位混乱、调试交互不足、状态提示不醒目、后端兼容性、布局调整、切换卡顿、完成判断标准缺失等。核心收益：(1) 消除 fadeInWidget 设计缺陷引发的系统性切换卡顿与内容空白；(2) GuidedTour 跨页定位缺陷修复，3 面板指引框定位正确；(3) BugHunt 增加「预测-观察-修复-验证」四步调试闭环，明确教学目标；(4) ProfileDashboard 三后端兼容性 + 性能优化 + 状态动画；(5) CodeJourney 完成判断标准（6 阶段访问跟踪）。全量 1762/1762 测试通过。

### 问题与修复对应表

| # | 问题 | 修复方案 | 修改文件 |
|---|------|---------|---------|
| 1 | 术语表切换功能异常 | 修复 GlossaryPanel 切换逻辑（上一会话完成） | `gui/GlossaryPanel.cpp` |
| 2 | 语法浏览器首次进入内容未加载 | 实现首次进入自动加载（上一会话完成） | `gui/SyntaxExplorerPanel.cpp` |
| 3 | 变量检测器/调用栈检查器/条件断点指引框定位混乱且页面空白 | 重写 createGuidedTour：只高亮始终可见的页切换按钮，概念步骤用 nullptr（居中气泡）+ 内嵌完整示例代码 | `gui/{VariableInspector,CallStack,BreakpointCondition}Panel.cpp` |
| 4 | Bug 狩猎模式缺乏有效交互 | 增加「预测→观察→修复→验证」四步调试闭环 + 检查清单 UI | `gui/BugHuntPanel.{h,cpp}` |
| 5 | 异常流与 IR 优化回放切换失效 | 移除回放页 fadeInWidget 调用（QGraphicsOpacityEffect 导致 opacity 卡 0） | `gui/IRTransformPanel.cpp` |
| 6 | 性能仪表盘"运行中"状态不醒目 | 橙色背景 + QTimer 循环圆点动画 + 进度指示 [1/3] [2/3] [3/3] | `gui/ProfileDashboardPanel.{h,cpp}` |
| 7 | 性能仪表盘数据量大导致卡死，仅 interpret 后端可运行 | 后端间 processEvents 避免 UI 阻塞 + 失败错误信息显示 + 降低迭代次数（fib/loop 5→3） | `gui/ProfileDashboardPanel.cpp` |
| 8 | 实验手册"本章练习"模块位置不当 | 练习区从中间列迁移至右侧样例代码下方，中间列改为全高 contentBrowser_ | `gui/LabManualPanel.cpp` |
| 9 | 各模块切换卡顿 | fadeInWidget 全部替换为 slideInWidget 或移除（上一会话+本会话完成） | `gui/{IRTransformPanel,VmStackSandboxPanel}.cpp` |
| 10 | 类型教学库未实现自动加载 | 修复为进入即加载（上一会话完成） | 相关教学面板 |
| 11 | 代码生命旅程动画缺少完成判断标准 | 6 阶段访问跟踪 + 进度提示（✅/⬜）+ journeyCompleted 信号 | `gui/CodeJourneyInfoPanel.{h,cpp}` |

### 关键决策

1. **GuidedTour 跨页定位用 nullptr 居中气泡替代高亮不可见控件**：根因为 GuidedTour 的 `bubble_` 定位依赖 `target->mapTo(host_, QPoint(0,0))`，当 target 位于 QStackedWidget 不可见页时 mapTo 返回错误坐标导致气泡定位混乱。修复策略：只高亮始终可见的页切换按钮（pageLiveBtn_/pageLibraryBtn_），概念性步骤用 `target=nullptr` 让气泡居中显示，配合内嵌完整示例代码（`<pre>` 块）演示典型用例。比「强制切换到目标页再高亮」更稳健，避免改变用户当前所在页
2. **BugHunt 四步调试闭环而非自由交互**：教学目标是培养系统化调试能力，"预测-观察-修复-验证"四步是软件工程经典调试方法论。用 `debugStepsLabel_` 检查清单（✅/⬜）显式呈现进度，`predictionEdit_` 让用户先写下预期，`onCodeModified()` 检测代码修改标记修复步，`onTripleVerify()` 标记验证步。比自由探索式交互更有教学引导性
3. **ProfileDashboard 状态动画用 QTimer 而非 QPropertyAnimation**：状态文字"运行中."→"运行中.."→"运行中..."循环圆点只需 400ms 周期更新文本，QTimer + setText 比 QPropertyAnimation 更直接。橙色背景（#F5A623）+ 白字 + bold 让"运行中"状态视觉醒目，避免误认为卡死
4. **ProfileDashboard 三后端兼容性修复用 processEvents 而非多线程**：三后端串行执行时每个后端之间调用 `QApplication::processEvents()` 让 UI 重绘状态文字与进度，避免长时间阻塞导致"未响应"。多线程会引入 QThread 生命周期管理复杂度，processEvents 是 Qt 单线程长任务的轻量级 UI 响应方案
5. **fadeInWidget 全局清理而非局部修复**：fadeInWidget 使用 QGraphicsOpacityEffect，连续切换时 opacity 属性可能卡在 0 导致内容永久空白。这是系统性设计缺陷（第二十九轮已定位），因此所有教学面板的 fadeInWidget 调用全部清理——QListWidget/QTextBrowser 等内容刷新无需动画，复杂容器改用 slideInWidget（O(1) pos 属性）
6. **LabManual 练习区迁移至样例代码下方而非独立列**：用户反馈"本章练习"模块位置不当，原位于中间列与 contentBrowser_ 平级。迁移到右侧列样例代码下方形成"代码-练习"垂直对照布局，中间列改为全高 contentBrowser_ 提升阅读体验。三栏布局保持（章节导航 | 教学内容 | 样例代码+练习）
7. **CodeJourney 完成判断标准用 6 阶段访问跟踪**：6 个阶段按钮（编辑器/Token/AST/IR/字节码/输出）全部点击访问即为完成。用 `QHash<QString,bool> visitedStages_` 跟踪，`progressLabel_` 显示 ✅/⬜ 进度，全部访问后变绿色"🎉 旅程完成！"+ `journeyCompleted` 信号。比"观看动画时长"等模糊标准明确可判定

### 关键教训

1. **GuidedTour 高亮目标必须在调用时可见**：`target->mapTo(host_, ...)` 依赖目标的几何信息，目标在 QStackedWidget 不可见页、被 setVisible(false) 隐藏、或父窗口未显示时均返回错误坐标。跨页引导应改用 nullptr 居中气泡 + 文字描述
2. **QGraphicsOpacityEffect 不适合任何需要连续切换的内容容器**：不仅是 100+ 子 widget 的滚动区域（第二十九轮教训），QListWidget/QTextBrowser 等简单容器连续切换时 opacity 也可能卡 0。fadeInWidget 应仅用于一次性显示（如 bottomStack_ 首次弹出），内容刷新场景一律移除
3. **教学模块需明确的"完成"判断标准**：用户无法自行判定是否完成会降低学习闭环体验。完成标准应可量化（访问 N 个阶段 / 答对 N 道题 / 修复 N 个 Bug）且视觉显式呈现（进度条/✅标记/完成提示）
4. **单线程长任务 UI 响应用 processEvents 而非多线程**：当任务可在 200-500ms 分段完成时，每段间 `QApplication::processEvents()` 是 Qt 单线程长任务的轻量级 UI 响应方案，避免 QThread 生命周期管理复杂度。但需注意 processEvents 可能触发重入，关键状态需用 guard 保护
5. **教学调试模块应引导方法论而非自由探索**：BugHunt 原"展示问题代码+错误原因"被动模式缺乏教学引导，用户看完即走无法形成调试能力。增加"预测-观察-修复-验证"四步检查清单显式引导系统化调试方法论，比"提供更多按钮让用户自由操作"更有教学价值

### 修改文件清单

- 修改：`gui/GlossaryPanel.cpp`（问题1：切换逻辑修复）
- 修改：`gui/SyntaxExplorerPanel.cpp`（问题2：首次进入自动加载）
- 修改：`gui/VariableInspectorPanel.cpp`（问题3：重写 createGuidedTour，nullptr 居中气泡 + 示例代码）
- 修改：`gui/CallStackPanel.cpp`（问题3：同模式重写 createGuidedTour，递归 fib 示例）
- 修改：`gui/BreakpointConditionPanel.cpp`（问题3：同模式重写 createGuidedTour，for 循环+条件断点示例）
- 修改：`gui/BugHuntPanel.h`（问题4：+debugStepsLabel_/predictionEdit_/submitPredictionBtn_ 成员 + 4 步状态标志 + refreshDebugSteps 声明）
- 修改：`gui/BugHuntPanel.cpp`（问题4：+四步调试闭环 + refreshDebugSteps + onSubmitPrediction/onCodeModified + showCurrentItem/showCurrentVariant 重置状态 + onRunVerify/onTripleVerify 标记步）
- 修改：`gui/IRTransformPanel.cpp`（问题5：移除回放页 3 处 fadeInWidget 调用）
- 修改：`gui/ProfileDashboardPanel.h`（问题6/7：+QTimer include + statusAnimTimer_/statusAnimDots_/statusRunningBase_ 成员 + startStatusAnimation/stopStatusAnimation 声明）
- 修改：`gui/ProfileDashboardPanel.cpp`（问题6：橙色背景+循环圆点动画+进度指示；问题7：processEvents+失败错误显示+降低迭代次数 5→3）
- 修改：`gui/LabManualPanel.cpp`（问题8：练习区迁移至右侧样例代码下方，中间列改全高 contentBrowser_）
- 修改：`gui/VmStackSandboxPanel.cpp`（问题9：移除 refreshStackView 的 fadeInWidget）
- 修改：`gui/CodeJourneyInfoPanel.h`（问题11：+QHash include + journeyCompleted 信号 + progressLabel_/visitedStages_ 成员 + markStageVisited/refreshProgress 声明）
- 修改：`gui/CodeJourneyInfoPanel.cpp`（问题11：+progressLabel_ 进度提示 + markStageVisited 6 阶段跟踪 + refreshProgress ✅/⬜ 显示 + 6 个 jump handler 调用 markStageVisited）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）+ `project_memory.md`（本章节）

## 2026-07-08 · 第三十轮：5 教学面板新手指引（GuidedTour 集成）+ 教学入口库视觉显著性提升（TeachingTreePanel）

### 概述

本轮完成两项子任务：(A) 为 5 个教学面板（BytecodeTrace / CallStack / VariableInspector / BreakpointCondition / BugHunt）接入新手指引 GuidedTour，让新手点进面板即可获得 5 步导览；(B) 提升教学入口库 TeachingTreePanel 的视觉显著性，新增渐变标题横幅、搜索框、Solarized 树样式、默认展开前两个分类、叶子节点 tooltip。GuidedTour 基础设施不变。MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` 构建通过。

### 修改内容

1. **5 面板 createGuidedTour 方法 — `gui/{BytecodeTrace,CallStack,VariableInspector,BreakpointCondition,BugHunt}Panel.{h,cpp}`**
   - 每个面板 `.h` 前向声明 `class GuidedTour;` + 公有方法 `GuidedTour* createGuidedTour(QWidget* host)`
   - 每个面板 `.cpp` 新增 `#include "gui/GuidedTour.h"` + 实现：`new GuidedTour(host, host)` + 5 步 `addStep(target, title, desc)`
   - 步骤目标取面板关键 widget：子页切换按钮 / 自动刷新复选框 / 主数据视图 / 教学库按钮 / 加载样例按钮等（具体见各面板实现）
   - **BytecodeTracePanel**：pageTraceBtn_ / autoCaptureCheck_ / traceTable_ / pageLibraryBtn_ / loadCodeBtn_
   - **CallStackPanel**：pageLiveBtn_ / autoRefreshCheck_ / stackTree_ / pageLibraryBtn_ / loadCodeBtn_
   - **VariableInspectorPanel**：pageLiveBtn_ / autoRefreshCheck_ / varTree_ / pageLibraryBtn_ / loadCodeBtn_
   - **BreakpointConditionPanel**：pageLiveBtn_ / breakpointTable_ / pageLibraryBtn_ / loadSampleBtn_ / nullptr（第 5 步无目标居中收尾）
   - **BugHuntPanel**：itemList_ / diffBeginnerBtn_ / codeEditor_ / tripleVerifyBtn_ / hintBtn_
   - 引导文案用 `QString::fromUtf8(...)` 构造中文，避免 MSVC 源码编码问题；不硬编码条目计数（如「18 条指令」改为「每条指令的语义说明」），避免数据源变化时文案失真

2. **TeachingPanelHeader 新增「新手引导」按钮 — `gui/TeachingPanelHeader.{h,cpp}`**
   - `.h` 新增信号 `void guidedTourRequested(const QString& panelId);` + 私有成员 `QPushButton* tourBtn_;`
   - `.cpp` 构造函数在 helpBtn_ 与 learningPathBtn_ 之间新增 `tourBtn_`（文本「新手引导」，objectName="teachingTourBtn"），点击 emit `guidedTourRequested(panelId_)`
   - QSS：`#teachingTourBtn` 用主题色 #268BD2 填充 + 白字 + #1E6FA3 边框 + 4px 圆角 + 600 字重，hover #1E6FA3 / pressed #1A6090，区别于普通 helpBtn 的 hover 浅蓝

3. **Ide 连接信号 + 首次访问自动触发 — `app/ide.{h,cpp}`**
   - `ide.h` private slots 新增 `void onPanelGuidedTourRequested(const QString& panelId);`
   - `ide.cpp` `wrapTeachingPanel` 中连接 `guidedTourRequested → onPanelGuidedTourRequested`
   - `ide.cpp` 新增 `onPanelGuidedTourRequested` 实现：先 `ensureTeachingPanelCreated(panelId)`（懒加载），再按 panelId 分派到 5 个面板的 `createGuidedTour(this)` 并 `start()`
   - `showTeachingPanel` 末尾新增首次访问自动触发：`kAutoTourPanels`（5 个目标 ID）+ QSettings `guidedTour/shown_<panelId>` 标记，首次进入时 `QTimer::singleShot(0, ...)` 延迟一帧启动引导（确保 widget 几何就绪后高亮定位准确），标记后不再自动弹出

4. **TeachingTreePanel 视觉显著性提升 — `gui/TeachingTreePanel.{h,cpp}`**
   - **B1 顶部标题横幅**：新增 `headerBanner_`（QWidget）+ `bannerTitle_`（🎓 学习中心）+ `bannerSubtitle_`（点击下方任意主题开始学习），渐变背景 QSS（#268BD2 → #1E6FA3）白字 padding 12px；🎓 emoji 用 UTF-8 字节序列 `\xF0\x9F\x8E\x93` 构造
   - **B2 树样式美化**：`#teachingTree` QSS —— #FDF6E3 背景 + 无边框 + item padding 6px 4px + 4px 圆角 + hover `rgba(38,139,210,0.08)` + 选中 #268BD2 填充白字 + branch 透明
   - **B3 搜索框**：新增 `searchEdit_`（QLineEdit，objectName="teachingSearchEdit"，placeholder「搜索面板...」，clearButton）+ `onSearchChanged` 槽：遍历分类节点按文本包含匹配（不区分大小写）隐藏/显示叶子，有匹配则展开分类无匹配则折叠并隐藏整个分类，空搜索恢复全部可见
   - **B4 默认展开前两个分类**：`buildTree` 末尾从「仅展开第一个 category」改为「展开前两个 category」（入门导览 + 编译前端），让新手最相关面板立即可见
   - **B5 叶子节点 tooltip**：`buildTree` 创建叶子时 `setToolTip(0, "点击进入 <label>")`（PanelCatalog 无 description 字段，用简单入口提示）

### 关键决策

1. **createGuidedTour 返回 new 对象由调用方持有**：GuidedTour 析构时恢复高亮目标样式，bubble_ 作为 host 子 widget 随 host 释放。Ide 调用 `createGuidedTour(this)->start()` 后不显式持有指针——tour 的生命周期由 host（MainWindow）的子 widget 关系托管，引导结束/跳过时 hideOverlay 清理，符合 GuidedTour 既有设计
2. **首次访问用 QSettings 持久化 + singleShot 延迟一帧**：QSettings 标记避免每次进入重复弹出；`QTimer::singleShot(0)` 确保面板完成布局后再启动，widget 几何就绪后高亮边框与气泡定位才准确（showTeachingPanel 末尾 centerStack_ 已 setCurrentIndex 但布局可能未完成）
3. **引导文案不硬编码条目计数**：第二十四轮 P2-C 已确立「文案计数从数据源派生」原则，本轮引导文案同样避免「18 条指令 / 6 个场景」等硬编码，改用「每条指令的语义说明 / 典型调用栈形态」等不随数据变化的措辞
4. **tourBtn 用主题色强调区别于 helpBtn**：helpBtn 是「这是什么？」帮助文档入口（hover 浅蓝），tourBtn 是「新手引导」操作入口（#268BD2 填充白字），视觉权重更高，引导新手优先点击
5. **搜索过滤隐藏无匹配分类**：非空搜索时若无任何叶子匹配，隐藏整个分类标题避免空分类残留；有匹配则展开分类让结果可见。编辑器项与分隔符项始终可见不参与过滤
6. **B4 展开前两个分类而非全部**：全部展开会让 20+ 叶子铺满首屏造成视觉过载，展开前两个（入门导览 + 编译前端）覆盖新手最常用入口，其余折叠减少噪声
7. **不修改 GuidedTour.h/.cpp 与 IdeController**：遵守任务约束，GuidedTour 既有 addStep/start API 满足需求，IdeController 不涉及

### 修改文件清单

- 修改：`gui/BytecodeTracePanel.h`（+前向声明 `class GuidedTour;` + `createGuidedTour` 声明）
- 修改：`gui/BytecodeTracePanel.cpp`（+`#include "gui/GuidedTour.h"` + `createGuidedTour` 实现 5 步）
- 修改：`gui/CallStackPanel.h` / `.cpp`（同上模式）
- 修改：`gui/VariableInspectorPanel.h` / `.cpp`（同上模式）
- 修改：`gui/BreakpointConditionPanel.h` / `.cpp`（同上模式，第 5 步 target=nullptr 居中收尾）
- 修改：`gui/BugHuntPanel.h` / `.cpp`（同上模式）
- 修改：`gui/TeachingPanelHeader.h`（+`guidedTourRequested` 信号 + `tourBtn_` 成员）
- 修改：`gui/TeachingPanelHeader.cpp`（+tourBtn_ 创建 + connect + QSS #teachingTourBtn 主题色强调）
- 修改：`app/ide.h`（+`onPanelGuidedTourRequested` 槽声明）
- 修改：`app/ide.cpp`（wrapTeachingPanel 连接 guidedTourRequested + `onPanelGuidedTourRequested` 实现 + showTeachingPanel 首次访问自动触发）
- 修改：`gui/TeachingTreePanel.h`（+`headerBanner_`/`bannerTitle_`/`bannerSubtitle_`/`searchEdit_` 成员 + `onSearchChanged` 槽）
- 修改：`gui/TeachingTreePanel.cpp`（构造函数横幅+搜索框+树 QSS + buildTree 展开两分类+tooltip + onSearchChanged 实现）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）

## 2026-07-08 · 第二十九轮：学习路径地图章节切换卡顿 + 偶发崩溃修复（QGraphicsOpacityEffect 移除 + 轻量滑入动画）

### 概述

本轮针对用户反馈的学习路径地图两个稳定性问题做集中修复：(1) 点击章节时页面切换卡顿、无法顺畅切换；(2) 窗口偶发性崩溃。根因定位为 `LearningPathPanel::refresh()` 末尾调用的 `PanelAnimator::fadeInWidget(scrollContent_)` —— 该函数对含 100+ 子 widget 的 `scrollContent_` 施加 `QGraphicsOpacityEffect`，每帧 O(n) 离屏 pixmap 合成导致卡顿；`deleteLater()` 延迟删除旧 widget 时 effect 的 pixmap 缓存仍引用已销毁子 widget → QPainter use-after-free 崩溃。叠加 6 处冗余双重 `refresh()` 调用与搜索框逐字符触发重建，放大了动画堆积与崩溃概率。修复方案：移除 `fadeInWidget` 改用 `slideInWidget`（仅驱动 pos 属性，O(1) 复杂度）、搜索框 250ms 防抖、`fadeInWidget` 自身增加旧动画停止逻辑、清除 6 处冗余 `refresh()`。全量 1762/1762 测试通过。

### 根因分析

| 问题 | 根因 | 机制 |
|------|------|------|
| 章节切换卡顿 | `QGraphicsOpacityEffect` 对 100+ 子 widget 离屏合成 | 每帧将 scrollContent_ 及全部子 widget 渲染到 off-screen pixmap，O(n) 复杂度，28 活动 × 多标签 = 100+ widget |
| 偶发崩溃 | `QGraphicsOpacityEffect` + `deleteLater()` 竞态 | refresh() 用 deleteLater 延迟删除旧 widget，effect 的 pixmap 缓存仍引用已销毁子 widget，QPainter 绘制时 use-after-free |
| 动画堆积 | `fadeInWidget` 不停止旧动画 | 快速触发 refresh()（搜索框/双重调用）时多个 QPropertyAnimation 同时写入 opacity 属性 |
| 双重重建 | 6 处 `showTeachingPanel("learning-path")` 后跟冗余 `refresh()` | showTeachingPanel 内部已调用 refresh()，外部再调一次触发两次 deleteLater + 两次 fadeInWidget |

### 修改内容

1. **`gui/LearningPathPanel.cpp` — 移除 fadeInWidget + 搜索框防抖**
   - `refresh()` 末尾移除 `PanelAnimator::fadeInWidget(scrollContent_)` 调用，添加详细注释说明崩溃机制
   - 构造函数中搜索框 `textChanged` 信号改为 250ms 防抖定时器（`searchDebounceTimer_`），避免逐字符触发 refresh 重建
   - 移除 `#include "gui/PanelAnimator.h"`（不再使用），新增 `#include <QTimer>`

2. **`gui/LearningPathPanel.h` — 新增防抖定时器成员**
   - 新增 `QTimer* searchDebounceTimer_ = nullptr` 成员

3. **`gui/PanelAnimator.h` — fadeInWidget 安全加固 + 新增 slideInWidget**
   - `fadeInWidget`：调用前先 `findChildren<QPropertyAnimation*>()` 查找并停止该 widget 上正在运行的 opacity 动画，防止多动画堆积
   - 新增 `slideInWidget(QWidget*, int)`：轻量子页面滑入动画，通过 QPropertyAnimation 驱动 widget 的 `pos` 属性（从右侧 24px 偏移滑入到目标位置）
     - 不使用 QGraphicsOpacityEffect，无离屏 pixmap 合成（O(1) vs O(n)）
     - 不受子 widget 数量影响，适合复杂面板（含滚动区域/列表/表格）的过渡
     - 动画结束后确保位置精确归位（防止动画中途被 resize 打断）
     - 同样先停止旧 pos 动画防堆积

4. **`app/ide.cpp` — showTeachingPanel 添加滑入动画 + 清除 6 处冗余 refresh**
   - `showTeachingPanel`：`centerStack_->setCurrentIndex(idx)` 后调用 `PanelAnimator::slideInWidget(newPanel)` 添加轻量过渡动画
   - 清除 6 处 `showTeachingPanel("learning-path")` 后的冗余 `learningPathPanel_->refresh()` 调用：
     - 首次启动 WelcomeWizard 完成回调（2 处）
     - 「再次显示欢迎向导」菜单项回调
     - `onActivityRequested` 的 welcome 分支回调
     - `wrapTeachingPanel` 的 header `learningPathRequested` 回调
     - `GuidedTour::finished` 回调
   - `onActivityRequested` 的 canonical 分支移除 `learning-path` 的冗余 refresh（showTeachingPanel 已处理）

### 关键决策

1. **`slideInWidget` 而非修复 `fadeInWidget`**：QGraphicsOpacityEffect 的根本问题是 O(n) 离屏合成复杂度，即使加固动画停止逻辑也无法解决卡顿。`slideInWidget` 仅驱动 `pos` 属性，O(1) 复杂度，且 QStackedWidget 不通过 layout 管理子 widget 位置（直接 setGeometry），因此临时 move 偏移不会与布局系统冲突

2. **保留 `deleteLater()` 而非改用 `delete`**：refresh() 可能从 row->clicked 信号槽调用链中触发（onActivityClicked→onActivityRequested→showTeachingPanel→markActivityCompleted→refresh），立即删除信号发送者会在 Qt 信号分发期间引发 use-after-free。移除 fadeInWidget 后，deleteLater 的旧 widget 从 layout 移除后不可见，在事件循环返回时安全清理，无渲染风险

3. **搜索框 250ms 防抖**：用户输入「ast」会触发 3 次 textChanged→refresh 重建。防抖后仅在停止输入 250ms 后重建一次，减少 66% 无效重建。选择 250ms 是体感「即时」与「减少重建」的平衡点（VS Code 搜索框约 200-300ms）

4. **`fadeInWidget` 保留但加固而非删除**：fadeInWidget 仍被 `bottomStack_`/`rightStack_`/guidedTour 等 3 处使用，这些场景的 widget 子树较小（底部面板标签内容），QGraphicsOpacityEffect 性能可接受。加固动画停止逻辑即可，无需冒险重构

### 关键教训

1. **QGraphicsOpacityEffect 不适用于子 widget 数量多的容器**：该 effect 强制将整个 widget 子树渲染到 off-screen pixmap 再应用透明度，复杂度 O(n)。对含 100+ 子 widget 的滚动区域每帧合成 = 卡顿。复杂容器应改用 `pos`/`geometry`/`maximumHeight` 等属性动画，O(1) 复杂度

2. **QGraphicsOpacityEffect + deleteLater 是崩溃高危组合**：effect 会缓存源 widget 的 pixmap，deleteLater 延迟删除子 widget 时，effect 的缓存可能仍引用已销毁 widget。在动态增删子 widget 的容器上应完全避免 QGraphicsOpacityEffect

3. **「信号触发回调 + 外部显式调用」的双重执行模式需全局排查**：showTeachingPanel 内部已调用 refresh()，但 6 处外部调用者仍重复调用。此类模式应通过 grep 全局排查并清除冗余，而非依赖开发者记忆

4. **搜索框输入必须防抖**：QLineEdit::textChanged 每次按键都触发，若处理函数含重建/网络请求等重操作，必须用 QTimer 防抖。250ms 是体感即时与性能的平衡点

### 修改文件清单

- 修改：`gui/LearningPathPanel.h`（+`QTimer* searchDebounceTimer_` 成员）
- 修改：`gui/LearningPathPanel.cpp`（移除 fadeInWidget 调用 + 搜索框 250ms 防抖 + 移除 PanelAnimator.h include + 新增 QTimer include）
- 修改：`gui/PanelAnimator.h`（fadeInWidget 加固动画停止 + 新增 slideInWidget 轻量滑入动画）
- 修改：`app/ide.cpp`（showTeachingPanel 添加 slideInWidget + 清除 6 处冗余 refresh + onActivityRequested 清除冗余 refresh）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）+ `project_memory.md`（本章节）

## 2026-07-08 · 第二十八轮：TeachingPanelHeader 帮助窗口尺寸统一调大 + 自适应内容高度 + Solarized QSS 美化

### 概述

本轮针对教学面板「这是什么？」帮助信息窗口（`TeachingPanelHeader::showHelpDialog`）做可读性与视觉一致性升级。原实现正常情况仅设 `setMinimumSize(480, 360)`、兜底情况 `setMinimumSize(400, 200)`，无最大尺寸约束、无自适应内容高度，长文案需手动拖拽窗口才能完整阅读；兜底分支用 `BodyLabel` 不支持滚动。升级为「统一调大窗口尺寸 + 自适应内容高度 + Solarized 亮色 QSS + 兜底也用 QTextBrowser」方案。MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` 构建通过。

### 修改内容

1. **统一调大窗口尺寸 — `showHelpDialog()` 正常路径**
   - `setMinimumSize(640, 480)` + `setMaximumSize(900, 700)`（限制最大避免占满屏幕）+ 初始 `resize(680, 520)`
   - 原 `setMinimumSize(480, 360)` 无最大尺寸，长文案窗口初始接近 minimum 需手动拖拽

2. **自适应内容高度 — 新增 `adjustHelpDialogSize(QDialog*, QTextBrowser*)` 私有方法**
   - `QTextDocument::setTextWidth(viewportWidth)` 设定文本换行宽度后用 `size().height()` 获取文档实际高度
   - `desiredHeight = docHeight + 80`（padding + 标题/按钮行 + 边距），用 `qBound(minHeight, desiredHeight, maxHeight)` 钳制到 `[minimum, maximum]` 区间
   - 宽度保持当前值不变（仅调高度），避免破坏用户拖拽的宽度
   - 兜底：dialog 未 show 时 viewport 宽度可能无效，用 `dlg->width() - 40 - 24 - 2`（布局边距 + padding + 边框）推算；最终兜底 600px
   - 调用时机：`setHtml` 后 `exec` 前，`layout()->activate()` 强制布局计算确保 viewport 有效宽度

3. **兜底情况改用 QTextBrowser — `showHelpDialog()` 未知 panelId 分支**
   - 原 `BodyLabel`（不支持滚动）改为 `QTextBrowser`，长文本可滚动
   - `setMinimumSize(500, 300)` + `setMaximumSize(800, 600)` + `resize(540, 340)`
   - `setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded)` 显式设置滚动条策略
   - 同样应用 Solarized QSS + 调用 `adjustHelpDialogSize` 自适应高度

4. **视觉美化 — Solarized 亮色 QSS**
   - `QDialog#helpDialog`：背景 `#FDF6E3`（Solarized base3）
   - `QTextBrowser#helpBrowser`：背景 `#FDF6E3` + 文字 `#002B36`（Solarized base03）+ `#93A1A1` 边框 + 6px 圆角 + 12px padding
   - 「知道了」按钮保持 `PrimaryPushButton` 自带 Fluent 主色 `#268BD2` 样式，不覆盖（避免 QSS 冲突）
   - 窗口标题加 📖 emoji（UTF-8 字节序列 `\xF0\x9F\x93\x96` 构造，避免 MSVC 源码编码问题）
   - 窗口标题格式：「📖 帮助 · <面板标题>」（兜底为「📖 帮助」）

5. **内容增强 — HTML 末尾提示行**
   - 正常 + 兜底路径均在 HTML 末尾追加虚线分隔 + 「💡 提示：按 Esc 关闭，拖拽角落可调整窗口大小」
   - 💡 emoji 用 UTF-8 字节序列 `\xF0\x9F\x92\xA1` 构造
   - 提示文字 12px `#839496`（Solarized base01）弱化视觉权重
   - QDialog 默认支持 Esc 关闭（触发 `reject`），无需额外代码

6. **预存 Qt6 兼容性 Bug 修复 — `gui/PanelAnimator.h`**
   - `QPropertyAnimation::target()` 在 Qt6 中已移除（Qt5 起标记 deprecated），改为 `targetObject()`
   - 两处修改：`fadeInWidget`（L144）+ `slideInWidget`（L170）
   - 根因：原代码为 Qt5 API，PanelAnimator.h 是 inline 头文件，ninja 未检测到头文件依赖变化导致 .obj 未重编，bug 一直潜伏；本轮修改 TeachingPanelHeader.h 触发依赖文件重编暴露此 bug

### 关键决策

1. **`adjustHelpDialogSize` 在 `exec` 前调用而非 `contentsChanged` 信号**：帮助文案是静态的（`setHtml` 一次性加载），不需要监听 `contentsChanged` 动态调整。`exec` 前调用 `layout()->activate()` + 读取 `document()->size()` 即可获得准确高度，避免信号回调的复杂性
2. **viewport 宽度兜底推算**：dialog 未 show 时 `viewport()->width()` 可能为 0，用 `dlg->width() - 布局边距 - padding - 边框` 推算文本换行宽度。最终兜底 600px 确保不会因宽度为 0 导致文档高度计算异常（宽度 0 会让文本变成单列窄高，高度严重高估）
3. **兜底也用 QTextBrowser**：原 BodyLabel 不支持滚动，长「暂无此面板的帮助文档」文案在小窗口中被截断。改用 QTextBrowser 与正常路径一致，确保任何分支下长文本均可滚动
4. **按钮保持 PrimaryButton 不覆盖 QSS**：PrimaryButton 自带 Fluent 主色 #268BD2 样式，若用 `QPushButton#helpCloseBtn` QSS 覆盖会与 Fluent 全局样式冲突。保持 PrimaryButton 默认样式既满足「Fluent 主色 #268BD2」要求又避免 QSS 优先级问题
5. **emoji 用 UTF-8 字节序列**：MSVC 源码编码不一致时直接写 `📖`/`💡` 字面量可能损坏，用 `QString::fromUtf8("\xF0\x9F\x93\x96")` 显式字节序列保证可移植性（与第二十五/二十六轮决策一致）
6. **不修改 helpDocs() 文案**：20 个面板的帮助文案保持不变，仅修改 HTML 模板（加提示行）和窗口容器（尺寸/QSS/自适应）

### 修改文件清单

- 修改：`gui/TeachingPanelHeader.h`（+前向声明 `QDialog`/`QTextBrowser` + `adjustHelpDialogSize` 私有方法声明）
- 修改：`gui/TeachingPanelHeader.cpp`（+`#include <QTextDocument>` + `showHelpDialog` 重构：统一调大尺寸 + Solarized QSS + 兜底改用 QTextBrowser + HTML 末尾提示行 + 窗口标题 📖 emoji + 调用 `adjustHelpDialogSize` + `adjustHelpDialogSize` 实现）
- 修改：`gui/PanelAnimator.h`（Qt6 兼容性修复：`QPropertyAnimation::target()` → `targetObject()` 两处，L144 + L170）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）

## 2026-07-08 · 第二十七轮：VM 栈沙盒面板新增「真实字节码追踪」子页（教学透明化）

### 概述

本轮针对 VM 栈沙盒面板（VmStackSandboxPanel）做教学价值增强，从「单页黑盒栈状态机」升级为「双子页透明教学」结构。原面板仅有栈沙盒页（前端模拟栈状态机 + 末尾黑盒式 StackVM 验证），学习者无法观察真实字节码的逐指令执行过程。新增「真实字节码追踪」子页，通过 IdeController 的 VM 单步 API（vmStep/vmReset/getVmStack/getVmCurrentIP/isVmRegisterMode 等）调用真实 StackVM/RegisterVM 引擎，让学习者透明观察字节码指令序列、IP 推进、操作数栈变化、寄存器状态，把抽象的 push/pop 概念与真实 VM 执行过程对齐。MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` 构建通过。

### 修改内容

1. **双子页 QStackedWidget 结构 — `gui/VmStackSandboxPanel.{h,cpp}` 构造函数重构**
   - 顶部新增子页切换按钮栏：`pageSandboxBtn_`（栈沙盒）+ `pageTraceBtn_`（真实字节码追踪），`setCheckable(true)` 互斥高亮，QSS 选中态 #268BD2 主题色填充 + 白字 + bold
   - 原「栈沙盒」单页内容完整保留，包装为 `sandboxPage` QWidget 加入 `pageStack_`（index 0）
   - 新增 `tracePage_` QWidget（index 1），由 `buildTracePage()` 构建

2. **追踪页 UI 布局 — `buildTracePage()`**
   - 顶部工具条：「📦 编译并加载」按钮 + IP 指示器 QLabel（`IP: N OpCode: OP_XXX`）+ 状态标签
   - 中部水平 QSplitter：左侧 `bytecodeList_`（QListWidget，每行 `[IP] OpCode operand`，Consolas 字体）+ 右侧垂直 QSplitter（操作数栈 `traceStackView_` / 寄存器视图 `registerTable_` / 程序输出 `traceOutputEdit_`）
   - 底部按钮栏：「▶ 单步执行」/「⏩ 运行到底」/「🔄 重置」
   - QSS：`#bytecodeList` 选中项 #268BD2 蓝色背景白字；`#registerTable` Fluent 风格 #EEE8D5 表头 + #93A1A1 网格线

3. **真实字节码加载 — `loadBytecodeFromCurrentLevel()`**
   - 通过 `controller_->runLexer(source) + runParser() + runCompiler()` 编译当前关卡源码（复用 `levelToMiniLangSource()` 映射）
   - `runCompiler()` 内部同步 VmStepper 编译结果（栈式 CompileResult + 寄存器 RegisterCompileResult）
   - StackVM 模式：遍历 `lastCompileResult().mainChunk`，调用 `disassembleInstruction(offset)` 填充列表（offset 自动推进），`bytecodeOffsets_` 记录每行对应的字节偏移
   - RegisterVM 模式：遍历 `compiler().getLastRegisterResult().mainChunk`，用 `regOpName(op)` + 操作数字节构造行文本，`instructionSizeAt(offset)` 推进
   - 鼠标悬停 tooltip：StackVM 模式从 `BytecodeTraceLibrary::opCodeDocs()` 查找 OpCode 语义说明（栈效果/样例代码）

4. **单步执行可视化 — `onTraceStep() / onTraceRunAll() / onTraceReset()`**
   - `onTraceStep()`：调用 `controller_->vmStep()`，根据 `VmStepResult`（OK/FINISHED/ERROR/NOT_READY）更新状态标签
   - `onTraceRunAll()`：循环 `vmStep()` 直到非 OK 结果（上限 100000 步防死循环），显示总步数
   - `onTraceReset()`：调用 `controller_->vmReset()` 重置 VM 状态，IP 归零，清空输出
   - 注册 `addVmStateChangedListener` 回调：VM 状态变更时若当前在追踪页自动调用 `refreshTraceViews()`

5. **指令级追踪高亮 — `refreshTraceViews()`**
   - IP 指示器实时显示 `IP: N  OpCode: OP_XXX`（从 `getVmCurrentIP()` + `getVmCurrentOpCodeName()`）
   - 字节码列表三态高亮：已执行指令浅绿背景 `QColor(220,240,220)` / 当前 IP 指令浅蓝背景 `QColor(200,220,255)` + bold / 未执行默认白底
   - `bytecodeOffsets_` 二分查找 currentIp_ 对应行号，`scrollToItem(PositionAtCenter)` 自动滚动到当前指令
   - 操作数栈视图：`getVmStack()` 返回 `vector<Value>`，反向遍历显示（栈顶在上方标注「← 栈顶」）；RegisterVM 模式显示 `R{n} = value` 格式

6. **寄存器视图 — `refreshRegisterTable()`**
   - QTableWidget 2 列（寄存器名 / 值），RegisterVM 模式显示 32 行（R0-R31），值从 `getVmStack()`（寄存器模式返回寄存器窗口）读取并 `Value::toString()` 格式化
   - StackVM 模式显示 1 行提示「（栈式 VM）无寄存器，操作数在栈顶」，tooltip 提示切换引擎
   - 表头 Fluent 风格 QSS（#EEE8D5 背景 + #93A1A1 边框 + 4px padding）

7. **沙盒页教学联动增强**
   - `executeOp()` 末尾新增 OpCode 对应提示：`sandboxOpHintLabel_` QLabel 显示「→ 沙盒 PUSH_INT 对应真实 OpCode: OP_INT」等映射（`buildSandboxToRealOpMap()` 静态 QHash，10 条 SandboxOpType → OpCode 名称映射：PUSH_INT→OP_INT / PUSH_STRING→OP_STRING / ADD→OP_ADD / SUB→OP_SUBTRACT / MUL→OP_MULTIPLY / DIV→OP_DIVIDE / MOD→OP_MODULO / NEG→OP_NEGATE / PRINT→OP_PRINT / HALT→OP_RETURN）
   - `onVerifyWithRealStackVM()` 末尾启用 `gotoTraceBtn_`「📊 查看追踪」按钮（初始禁用，验证后激活），点击调用 `switchToTracePage()` 跳转到追踪页

8. **IdeController 绑定 — `setController(IdeController*)`**
   - 新增成员 `controller_` + `setController()` 方法（对齐 BytecodeTracePanel/MemoryModelPanel 模式）
   - `app/ide.cpp` `registerLazyPanel("vm-sandbox")` lambda 中增加 `vmStackSandboxPanel_->setController(controller_)` 调用
   - 不修改 IdeController.h（任务约束），仅消费已有 API：vmStep/vmReset/getVmStack/getVmCurrentIP/getVmCurrentOpCodeName/isVmRegisterMode/isVmInitialized/getVmLastError/lastCompileResult/compiler().getLastRegisterResult()

### 关键决策

1. **保留栈沙盒状态机不破坏**：原沙盒页（前端模拟栈状态机 + 关卡游戏 + 真实 StackVM 黑盒验证）完整保留为子页 1，新增追踪页作为子页 2，QStackedWidget 切换。栈沙盒的教学价值（亲手 push/pop 理解栈语义）与追踪页的教学价值（观察真实 VM 执行）互补
2. **复用 BytecodeTraceLibrary::opCodeDocs()**：指令 tooltip 不重复造轮子，直接从现有 OpCode 教学库查找语义说明，与 BytecodeTracePanel 的 OpCode 教学库子页共用数据源
3. **双后端兼容**：追踪页同时支持 StackVM 与 RegisterVM 模式——根据 `controller_->isVmRegisterMode()` 分派字节码加载路径（`disassembleInstruction` vs `regOpName` 手动格式化），寄存器视图在 StackVM 模式显示提示而非空表
4. **不修改 IdeController.h**：任务约束。所有 VM 状态访问通过 IdeController 已暴露的 inline 转发方法（vmStep/vmReset/getVmStack 等），RegisterCompileResult 通过 `compiler().getLastRegisterResult()` 间接访问
5. **`gotoTraceBtn_` 初始禁用**：避免用户未验证就跳转追踪页看到空状态，验证后激活形成「验证 → 查看追踪」的教学引导链
6. **VM 状态变更监听自动刷新**：`setController()` 注册 `addVmStateChangedListener` 回调，仅在追踪页可见时（`pageStack_->currentIndex() == 1`）触发 `refreshTraceViews()`，避免沙盒页操作时的无谓刷新

### 修改文件清单

- 修改：`gui/VmStackSandboxPanel.h`（+`#include <QStackedWidget>/<QTableWidget>/<QHash>` + `setController` 方法 + 11 个追踪页 UI 成员 + `controller_`/`bytecodeOffsets_`/`currentIp_` 状态 + `buildTracePage/loadBytecodeFromCurrentLevel/refreshTraceViews/refreshRegisterTable/buildSandboxToRealOpMap/switchToTracePage` 方法声明 + `gotoTraceBtn_`/`sandboxOpHintLabel_`/`pageSandboxBtn_`/`pageTraceBtn_`/`pageStack_`/`traceOutputEdit_` 成员）
- 修改：`gui/VmStackSandboxPanel.cpp`（构造函数重构为 QStackedWidget 双页 + `buildTracePage` 实现 + `setController` 实现 + `loadBytecodeFromCurrentLevel` 双后端字节码加载 + `onTraceStep/onTraceRunAll/onTraceReset` 单步控制 + `refreshTraceViews` 三态高亮 + `refreshRegisterTable` 32 寄存器填充 + `buildSandboxToRealOpMap` 静态映射 + `executeOp` 末尾 OpCode 提示 + `onVerifyWithRealStackVM` 末尾启用 gotoTraceBtn_ + `switchToTracePage` 跳转）
- 修改：`app/ide.cpp`（`registerLazyPanel("vm-sandbox")` lambda 增加 `vmStackSandboxPanel_->setController(controller_)` 调用）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）

## 2026-07-08 · 第二十六轮：教学游戏面板关卡切换控件视觉升级（芯片栏 + QSS 美化）

### 概述

本轮针对 3 个教学游戏面板（TokenPuzzlePanel / AstBuilderToyPanel / VmStackSandboxPanel）的关卡切换控件做视觉识别度与可操作性升级。原实现三面板均用裸 QComboBox（无 objectName、无 QSS、无悬停反馈），关卡状态（锁定/当前/已完成）缺乏视觉区分。升级为「QComboBox 保留 + 上方关卡芯片按钮栏」双轨方案：芯片栏提供一键切换与状态可视化（locked 🔒 / unlocked 关卡号 / current 主题色填充+⭐），QComboBox 应用 Solarized 风格 QSS 美化，两者双向同步。MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` 构建通过。

### 修改内容

1. **关卡芯片按钮栏（level chip row）— 三面板 buildUi()**
   - 在 QComboBox 上方新增独立一行 QHBoxLayout，每个关卡一个 QPushButton（`objectName="levelChip"`，`setFixedSize(48, 32)`，字号 11px）
   - 芯片点击 → `levelCombo_->setCurrentIndex(i)` 触发 `onLevelChanged → loadLevel → refreshLevelChips` 完成双向同步（QComboBox 切换 → loadLevel → refreshLevelChips 同步芯片高亮）
   - `setToolTip` 显示关卡标题与目标/教学点，悬停即可预览

2. **QSS Solarized 风格美化 — 三面板构造函数 setStyleSheet**
   - `QComboBox#levelCombo`：#FDF6E3 背景 + #93A1A1 边框 + 4px 圆角，hover 态 #268BD2 边框
   - `QPushButton#levelChip`：#EEE8D5 背景 + #93A1A1 边框 + 4px 圆角，hover 态 #268BD2 边框 + #E5F3FB 背景
   - `QPushButton#levelChip[current='true']`：#268BD2 主题色填充 + 白字 + #1E6FA3 边框 + bold
   - `QPushButton#levelChip[locked='true']`：#EDEDED 灰色背景 + #AAA 灰字 + #CCC 边框
   - 动态属性选择器通过 `setProperty` + `style()->unpolish()/polish()` 触发 QSS 重新评估

3. **三态语义区分 — refreshLevelChips() 新方法**
   - **TokenPuzzlePanel**（有解锁机制）：locked 状态读取 `QStandardItemModel::item(i)->isEnabled()`，locked 显示 🔒 + 不可点击；current 且已获星显示「关卡号 ⭐」；unlockNextLevel() 末尾调用 refreshLevelChips 同步解锁
   - **AstBuilderToyPanel**（所有题目解锁）：locked 恒 false；current 且已完成（`completedLevels_` 含该 level）显示「关卡号 ⭐」；markCurrentCompleted() 末尾调用 refreshLevelChips
   - **VmStackSandboxPanel**（所有关卡解锁）：locked 恒 false；current 且已完成（查询 `LearnerProgressStore::getLevelStars("level-N") >= 0`）显示「关卡号 ⭐」；onCheck() 通关后调用 refreshLevelChips

4. **emoji 用 UTF-8 字节序列构造**
   - 🔒 = `QString::fromUtf8("\xF0\x9F\x94\x92")`（U+1F512）
   - ⭐ = `QString::fromUtf8("\xE2\xAD\x90")`（U+2B50）
   - 避免 MSVC 源码编码不一致导致 emoji 损坏（与第二十五轮 PipelineViewer 决策一致）

### 关键决策

1. **保留 QComboBox + 增加芯片栏（双轨）**：不破坏现有 `currentIndexChanged` 信号接线与解锁逻辑（TokenPuzzlePanel 的 QStandardItemModel enable/disable 机制），芯片栏作为视觉增强层叠加，双向同步确保两种切换方式一致
2. **dynamic property + polish/unpolish 触发 QSS 重评估**：Qt 的 QSS 动态属性选择器 `[current='true']` 在 `setProperty` 后不会自动重评估，必须 `style()->unpolish(widget); style()->polish(widget);` 强制刷新
3. **三面板 locked 语义差异化**：TokenPuzzlePanel 有真实解锁机制（前置关完成才解锁），AstBuilderToyPanel/VmStackSandboxPanel 所有关卡默认解锁，locked 属性恒 false 但保留 QSS 规则以备未来扩展
4. **已完成标记读取数据源不同**：TokenPuzzlePanel 用内存 `levelStars_`，AstBuilderToyPanel 用内存 `completedLevels_`，VmStackSandboxPanel 查询持久化 `LearnerProgressStore`——均复用各面板既有数据源，不引入冗余状态

### 修改文件清单

- 修改：`gui/TokenPuzzlePanel.h`（+`QList<QPushButton*> levelChips_` 成员 + `refreshLevelChips()` 声明）
- 修改：`gui/TokenPuzzlePanel.cpp`（+`#include <QStyle>` + buildUi 芯片栏构建 + levelCombo objectName + QSS + loadLevel/unlockNextLevel 调用 refreshLevelChips + refreshLevelChips 实现）
- 修改：`gui/AstBuilderToyPanel.h`（+`#include <QList>` + `levelChips_` 成员 + `refreshLevelChips()` 声明）
- 修改：`gui/AstBuilderToyPanel.cpp`（+`#include <QStyle>` + 构造函数芯片栏 + levelCombo objectName + QSS + loadLevel/markCurrentCompleted 调用 refreshLevelChips + refreshLevelChips 实现）
- 修改：`gui/VmStackSandboxPanel.h`（+`#include <QList>` + `levelChips_` 成员 + `refreshLevelChips()` 声明）
- 修改：`gui/VmStackSandboxPanel.cpp`（+`#include <QStyle>` + 构造函数芯片栏 + levelCombo objectName + QSS + loadLevel/onCheck 调用 refreshLevelChips + refreshLevelChips 实现）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）

## 2026-07-08 · 第二十五轮：编译管线可视化面板 UI 美化（PipelineViewer）

### 概述

本轮针对编译管线可视化面板（PipelineViewer）做全面 UI 美化，把原本「无样式 Qt 默认按钮 + 单层信息 + 仅 220ms 淡入」的扁平展示，升级为「5 阶段 Solarized 主题色 + emoji 图标导航 + header 标题条 + 淡入叠加水平滑动」的结构化视觉叙事。核心收益是管线 5 阶段（源码→Token→AST→IR→字节码）的视觉区分度提升 + 信息层级清晰化（每页 header 标题+说明）+ 切换动效更优雅（淡入+方向感滑动）。MSVC 19.51 + Qt 6.10.3 + Ninja `minilang_ide` 构建通过。

### 修改内容

1. **5 阶段主题色体系 — gui/PipelineViewer.{h,cpp}**
   - 新增静态助手 `stageColor(step)` 返回 5 阶段 Solarized 主题色：源码石墨灰 #586E75 / Token 海蓝 #268BD2 / AST 森林绿 #859900 / IR 紫色 #6C71C4 / 字节码橙色 #CB4B16
   - 配套 `stageIcon(step)` 返回 Unicode emoji 图标（📄/🔢/🌳/⚙/📦，用 UTF-8 字节序列构造避免源码编码问题）
   - 配套 `stageTitle(step)` / `stageDesc(step)` 返回阶段中文标题与简短描述（用于 header 条）

2. **按钮样式优化 — PipelineViewer.cpp 构造函数**
   - `makeStepBtn` lambda 增加阶段主题色 QSS：未选中态用 #FDF6E3 浅色背景 + 1.5px 阶段色边框 + 阶段色文字；选中态用阶段色填充 + 白字；hover 态用 #EEE8D5；pressed 态同选中态
   - 按钮文本自动前置 `stageIcon(step)` emoji（📄 1. 源码 / 🔢 2. Token / 🌳 3. AST / ⚙ 4. IR / 📦 5. 字节码）
   - 每个按钮设置 `objectName=stepBtnN`（N=0..4）便于 QSS 选择器定位
   - 按钮最小宽度 80→96，增加 padding 与 font-weight: 600，cursor 设为 PointingHandCursor

3. **布局优化 — 步骤导航条圆角容器 + chevron 箭头**
   - 步骤导航条容器从 QWidget 改为 QFrame，应用 QSS 圆角背景（#EEE8D5 base2 + #93A1A1 边框 + 8px 圆角）
   - 阶段间箭头从「→」改为更优雅的 chevron「›」（U+203A），22pt 字号，颜色取目标阶段主题色（暗示流向）
   - 内容区（QStackedWidget）外层加 1px 边框 + 6px 圆角 + #FDF6E3 背景；每页内边距 8px

4. **动画过渡增强 — switchToStep 增加水平滑动**
   - 保留原有 `PanelAnimator::fadeInWidget` 220ms 淡入（QGraphicsOpacityEffect 0→1）
   - 新增 `QPropertyAnimation` 驱动 `pos` 属性水平滑动：根据切换方向（step>=oldStep 前进 / step<oldStep 后退）决定起始偏移 ±24px，220ms OutCubic 缓动回到原位
   - 利用 QStackedLayout 在 setCurrentIndex 后子控件 geometry 不再被覆盖的特性，安全叠加位移动画

5. **信息层级展示 — 每阶段页面 header 条**
   - `wrapPage` lambda 把每个内容控件（sourceBrowser/tokenTable/astSummary/irBrowser/bytecodeBrowser）包装到「header + content」垂直布局容器
   - header 是 QLabel 富文本：左侧 4px 阶段主题色竖线 + #EEE8D5 背景 + 「emoji 标题   描述」三段式布局（标题 14px 600 主题色，描述 11px #657B83）
   - 内容控件保持原有 populate* 数据源不变，仅套了一层 #FDF6E3 背景 + #93A1A1 边框 + 4px 圆角的 QSS

6. **状态条美化 — updateStatusBar 新方法**
   - 新增 `updateStatusBar()` 私有方法集中管理状态条样式与文本（reloadCurrentStep / onCursorPositionChanged 都改调用此方法）
   - 状态条加 📌 emoji 图标前缀，左侧 4px 当前阶段主题色竖线（随阶段切换变色），#EEE8D5 背景 + 4px 圆角 + #93A1A1 边框
   - 替代原 reloadCurrentStep / onCursorPositionChanged 中重复的 stepNames 内联数组，改为 static const QStringList

### 关键决策

1. **emoji 用 UTF-8 字节序列构造**：直接写 `QString::fromUtf8("📄")` 在 MSVC 源码编码不一致时可能损坏，改用 `QString::fromUtf8("\xF0\x9F\x93\x84")` 显式字节序列保证可移植性
2. **水平滑动用 `pos` 属性而非 maximumWidth/maximumHeight**：PanelAnimator 提供的 animateShowHorizontal 是宽度展开动画（适合面板弹出），不适合页面切换；页面切换需要的是位移效果，故用 QPropertyAnimation 驱动 `pos` 属性
3. **方向感动画**：前进方向从右侧滑入（dx=+24），后退方向从左侧滑入（dx=-24），让用户感知到「在管线中前进 / 后退」的方向感
4. **header 用 QLabel 富文本而非自定义 QFrame**：QLabel 的 richText 已能渲染「图标+标题+描述」三段式布局，配合 QSS border-left 实现左竖线，比自定义 paintEvent 简洁
5. **每页内容控件 QSS 不破坏原有逻辑**：sourceBrowser/tokenTable 等仅加背景/边框/圆角 QSS，populate* 数据填充逻辑、anchorClicked 信号、右键菜单、Ctrl+C 快捷键全部保持不变
6. **stageColor 与 TeachingTheme::learningStageColor 共用 Solarized 色板但映射不同**：learningStageColor 用于学习路径 5 阶段（绿/黄/蓝/紫/红），stageColor 用于管线 5 阶段（灰/蓝/绿/紫/橙），两者独立维护避免语义混淆

### 修改文件清单

- 修改：`gui/PipelineViewer.h`（+`#include <QColor>/<QString>` + 4 个静态助手声明 + `updateStatusBar()` 声明）
- 修改：`gui/PipelineViewer.cpp`（+`#include <QFrame>/<QPropertyAnimation>/<QEasingCurve>/<QAbstractAnimation>` + 4 个静态助手定义 + 构造函数 QSS 美化 + emoji 图标 + wrapPage header 包装 + switchToStep 水平滑动动画 + updateStatusBar 方法 + reloadCurrentStep/onCursorPositionChanged 改调用 updateStatusBar）
- 文档同步：`CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）

## 2026-07-08 · 第二十三轮：IDE 体验优化 5 项（启动性能 + 视觉一致性 + 工具栏修复）

### 概述

本轮针对 IDE 使用中暴露的 5 个体验问题做集中修复：启动慢、欢迎页字体未居中、视图菜单残留教学面板子菜单、字节码调试结束后工具栏按钮重复、断点圆点/条件断点弹窗/代码补全弹窗样式粗糙。核心收益是启动时延下降（懒加载 20 个教学面板）+ 工具栏状态机正确性（调试/VM 按钮互斥）+ 编辑器视觉语言统一（Fluent 风格断点与补全弹窗）。全量 1762/1762 测试通过。

### 修改内容

1. **IDE 启动慢优化（双管齐下） — app/ide.{h,cpp}**
   - 移除 `Ide` 构造函数末尾冗余的 `applyFluentStyle()` 调用：`Theme::setThemeMode(Light)` 已触发 `onThemeModeChanged` → `applyFluentStyle()`，原二次调用白白重新生成 ~510 行 QSS 并全量 setStyleSheet
   - **教学面板懒加载工厂**：`ide.h` 新增 `QMap<QString, std::function<void()>> teachingPanelFactories_` + `ensureTeachingPanelCreated(panelId)` 方法；`ide.cpp` 用 `registerLazyPanel` lambda 替换原 20 个 `makeTeachingPanel` 直接构造调用，面板构造（含信号接线）延迟到首次访问 `showTeachingPanel()` 时执行
   - 收益：启动时不再一次性构造 20 个教学面板的 widget 树与数据加载，仅构造 `TeachingTreePanel` 导航树；用户点击导航项时才创建对应面板

2. **欢迎页字体未居中对齐 — app/ide.cpp**
   - `TitleLabel`/`CaptionLabel` 的 `setAlignment(Qt::AlignCenter)` 调用移到 `setFont()` 之后：`setFont` 触发的 `fontChangeEvent` 可能重置 alignment，原调用顺序导致首屏字体未居中
   - 兜底：通过 QSS `qproperty-alignment: 'AlignCenter'` 确保即便运行时 font change 也能保持居中

3. **视图菜单移除教学面板子菜单 — app/ide.cpp**
   - 删除 4 个教学子菜单：`teachBeginnerMenu` / `teachFrontendMenu` / `teachEngineMenu` / `teachAdvancedMenu`（共 ~60 个 toggleAction 项，菜单层级深且与左侧 `TeachingTreePanel` 树形导航功能重复）
   - 保留全部 19 个教学面板的键盘快捷键：改用 `registerTeachingShortcut` lambda 注册 `QShortcut`（Ctrl+1~9 / Ctrl+Shift+1~9 / F5~F12 等），快捷键触发时若已在对应面板则切回编辑器（与原 toggle 行为一致）
   - 视图菜单的「学习中心」入口与 ActivityBar「学习」图标统一收敛到 `TeachingTreePanel` 树形导航

4. **字节码调试结束后按钮重复修复 — app/ide.cpp**
   - `onVmStop()` 末尾新增 `showVmButtons(false)`：原实现仅清理 VM 状态（`vmStackPanel_->clearAll()` 等）和动作 enabled 状态，未隐藏 VM 按钮容器，导致退出字节码调试后工具栏仍残留「VM 单步/步过/步出/运行/停止」5 个按钮 + 分隔线；后续再进入树遍历调试会出现两套按钮并存
   - **调试/VM 按钮互斥**：`showDebugButtons(true)` 进入时主动隐藏 VM 按钮组（`vmButtonContainer_` + `vmSepAction_`），`showVmButtons(true)` 进入时主动隐藏调试按钮组（`debugButtonContainer_` + `debugSepAction_` + 重置 `debugButtonsVisible_`），确保工具栏任一时刻只显示一套调试按钮

5. **断点圆点 / 条件断点弹窗 / 代码补全弹窗样式重设计 — gui/CodeEditor.cpp**
   - **断点圆点**：`LineNumberArea::paintEvent` 中断点绘制改为 `painter.save()` + `setRenderHint(Antialiasing)` 局部开启抗锯齿；三层绘制——(a) 半透明大圆外发光晕（60 alpha）(b) 径向渐变主圆点（中心 lighter(140) 高光 → 边缘 coreColor）+ 1.2px ringColor 描边 (c) 条件断点附加 1px 金色外环（间隙 2px）；色值从纯 `Qt::red`/`QColor(255,165,0)` 改为更柔和的 `#E43B44`/`#F5A623`；半径 6→7 增强可见性
   - **条件断点弹窗**：`contextMenuEvent` 中用自定义 `QDialog`（420×180）替代 `QInputDialog::getText`；Fluent 色板 QSS（`#FDF6E3` 背景 + `#268BD2` 主色按钮 + 4px 圆角 + Consolas 等宽输入框）；布局含标题/提示/输入框/警告/取消+确定按钮行；移除 `#include <QInputDialog>`（已无引用）
   - **代码补全弹窗**：`CodeEditor` 构造函数中 `completer_->popup()->setStyleSheet()` 应用 Fluent 风格 QSS——`#FDF6E3` 背景 + `#93A1A1` 边框 + 6px 圆角 + 4px 10px item padding + 4px item 圆角 + `#268BD2` 选中色（白字）+ 12% alpha hover 色 + 8px 自定义滚动条；与 IDE 整体 Solarized 亮色 + Fluent 圆角风格一致

### 关键决策

1. **懒加载工厂 map 而非 lazy singleton**：`teachingPanelFactories_` 集中注册所有面板的构造闭包（含信号接线），`ensureTeachingPanelCreated` 首次访问时调用闭包构造并写入 `panelToStackIndex_`，后续访问直接命中 map 跳过。比每个面板单独写 `if (!panel_) create()` 更统一，且避免散落的 lazy 初始化代码
2. **`setAlignment` 移到 `setFont` 之后 + QSS 兜底**：仅改调用顺序不够稳健（不同 Qt 版本/Style 的 fontChangeEvent 行为可能不同），叠加 QSS `qproperty-alignment` 作为运行时属性强制覆盖，双重保险
3. **`QShortcut` 替代 `QAction` 菜单项**：原菜单项既承担菜单显示又承担快捷键，删除菜单后快捷键也丢失。改用 `QShortcut` 解耦——快捷键逻辑独立于菜单存在，菜单可按需增删而不影响快捷键
4. **`showDebugButtons`/`showVmButtons` 互斥而非各自独立**：两套按钮功能重叠（都是调试控制），任一时刻只应显示一套。在 show 函数内部主动隐藏对方，比在每个调用点单独 hide 更不易遗漏
5. **断点圆点抗锯齿局部开启**：`painter.setRenderHint(Antialiasing)` 会影响后续所有绘制（含行号文字），因此用 `painter.save()`/`restore()` 包裹仅对断点圆点开启，避免行号文字抗锯齿后模糊
6. **条件断点用自定义 `QDialog` 而非 QSS 美化 `QInputDialog`**：`QInputDialog` 在 Windows 原生主题下样式难以覆盖（按钮/输入框由系统绘制），自定义 `QDialog` 可完全控制布局与 QSS，且可扩展（未来添加「验证条件语法」按钮等）

### 修改文件清单

- 修改：`app/ide.h`（+`#include <functional>` + `teachingPanelFactories_` 成员 + `ensureTeachingPanelCreated` 声明）
- 修改：`app/ide.cpp`（移除冗余 `applyFluentStyle` + 懒加载工厂 20 个 `registerLazyPanel` + 欢迎页 `setAlignment` 顺序 + QSS qproperty-alignment + 删除 4 个教学子菜单 + 19 个 `registerTeachingShortcut` + `showDebugButtons`/`showVmButtons` 互斥 + `onVmStop` 调用 `showVmButtons(false)`）
- 修改：`gui/CodeEditor.cpp`（断点圆点三层绘制 + 条件断点自定义 `QDialog` + 补全弹窗 Fluent QSS + `#include <QHBoxLayout>/<QPushButton>` + 移除 `#include <QInputDialog>`）
- 文档同步：`README.md`（测试徽章 1754→1762）+ `CHANGELOG.md`（本章节 + 头部计数 1754→1762）+ `docs/development.md`（摘要表头插入新行）+ `project_memory.md`（本章节）

## 2026-07-08 · 第二十四轮：教学模块 P2 项（复核版收尾）

### 概述

本轮依据【MiniLang教学模块分析_复核与优化建议.md】完成 P2 三项收尾，覆盖帮助文案魔法数字、长章节折叠、错误码优先匹配三条主线。核心是把「文案/提示与数据源解耦、UI 减负、错误匹配从子串升级到稳定 code」三件事补齐，让教学模块的「最后一公里」落地。全量 1762/1762 测试通过（+8 个 ErrorHintEngineCodeMatching 新测试）。

### 修改内容

1. **P2-C 帮助文案魔法数字 — gui/TeachingPanelHeader.cpp**
   - 引入 `gui/LearningPathData.h`，新增 `learningPathSummaryText()` 从 `LearningPathData::stageCount()` + `activities().size()` 动态派生「N 个阶段 M 个活动」文案
   - `learning-path` 帮助条目的 purpose 字段改为调用 `learningPathSummaryText()`，删除硬编码「5 个阶段 21 个活动」（实际 28 个活动）
   - `bug-hunt` 帮助条目改为「三档分级挑战（初阶 / 中阶 / 高阶）」，删除与档位数无关的「7 类历史 Bug」描述
   - 文件头注释统一为「内置教学面板的帮助文案」，删除「15 个 / 20 个」自相矛盾的计数

2. **P2 长章节折叠 — gui/LabManualPanel.{h,cpp}**
   - 新增 `foldBtn_`「📂 折叠次要章节」按钮 + `foldMinorSections_` 状态成员 + `onToggleFold()` 槽
   - 新增 `isMinorSection(headingText)` 静态方法：识别 14 个次要章节关键字（进阶/思考题/常见错误/延伸/拓展/参考/扩展/挑战/习题/常见问题/FAQ/补充/附录/深入）
   - 新增 `applyFolding(markdown)` 方法：状态机扫描 Markdown，识别 `##` 标题（仅 level==2），折叠模式下保留次要章节标题（让 TOC 链接仍可点击）+ 紧跟「▶ 此节已折叠」提示行，跳过后续行直到同级或更高级标题
   - 代码块内的 `#` 不算标题（围栏 ```/~~~ 状态跟踪）
   - `showCurrentChapter()` 在生成 TOC 前调用 `applyFolding`，折叠模式下 TOC 仍含次要章节标题（点击跳转后看到「已折叠」提示）

3. **P2 错误码优先匹配 — common/Diagnostic.h + gui/ErrorHintEngine.{h,cpp} + app/ide.cpp**
   - **Diagnostic** struct 新增 `std::string code` 字段（默认空），与 `ErrorHintEngine::errorPatterns()` 表 tag 对应
   - 新增 6 参构造重载 `Diagnostic(lv, msg, ln, col, src, diagCode)`，向后兼容现有 5 参构造
   - `DiagnosticBag` 新增带 code 的 `addError/addWarning/addInfo` 4 参重载
   - **ErrorHintEngine** 新增 4 参 `enrichErrorMessage(msg, code, category, scopeVars)` 重载：code 非空时优先查 `kCodeToHint` 表（9 条简单 tag → 固定教学提示映射）附加返回；code 空/未知/需 msg 提取变量名的 tag（undefined-variable/undefined-function/type-mismatch）回退到现有 3 参子串匹配版本
   - `app/ide.cpp` `displayDiagnostics()` 循环内改为调用 4 参版本，从 `diag.code` 读取诊断码传入
   - 核心价值：未来引擎模块逐步迁移到带 code 的 Diagnostic 后，即使消息文案变化（如英文/本地化），code 仍能精确匹配并附加中文教学提示

4. **测试 — tests/TestErrorHintEngine.cpp**
   - 新增 `ErrorHintEngineCodeMatching` 测试套件 8 用例：EmptyCodeFallsBackToSubstring / KnownCodeMissingSemicolon / KnownCodeUnbalancedParen / KnownCodeDivisionByZero / KnownCodeRecursionDepth / UnknownCodeFallsBackToSubstring / UndefinedVariableCodeFallsBackForSpelling / LocalizedMsgStillMatchesViaCode
   - 覆盖：code 空回退 / 已知 code 精确匹配 / 未知 code 回退 / 需 msg 提取变量的 tag 回退 / 英文消息通过 code 仍匹配（核心价值验证）

### 关键决策

1. **applyFolding 保留次要章节标题而非整体删除**：折叠模式下保留 `## 标题` 让 TOC 链接仍可点击，点击跳转后看到「▶ 此节已折叠」提示行。整体删除会让 TOC 缺少次要章节链接，学员不知道有这节存在
2. **isMinorSection 关键字表而非正则**：14 个关键字用 `contains` 简单匹配，避免正则编译开销。主章节（目标/概念/步骤/验证/概述/简介/示例/原理）始终展开
3. **applyFolding 仅识别 `##`（level==2）可折叠**：`#` 一级标题（章标题）始终展开，`###` 三级标题不视为折叠边界（避免过度折叠导致内容碎片化）
4. **Diagnostic code 字段为可选（默认空）**：保持现有 5 参构造向后兼容，引擎模块逐步迁移到 6 参带 code 构造。未迁移时 4 参 enrichErrorMessage 自动回退到子串匹配，行为完全兼容
5. **kCodeToHint 表只放「不需要 msg 提取变量名」的简单 tag**：undefined-variable/undefined-function/type-mismatch 需要从 msg 提取变量名做拼写建议，逻辑较复杂，已在 3 参版本实现，4 参版本对这些 tag 直接回退以复用逻辑，避免重复实现
6. **ide.cpp displayDiagnostics 是唯一接入 diag.code 的点**：runtime 异常路径（runtimeError/VM ERROR/ReplPanel catch）的异常对象 `e.what()` 不带 code，保持 3 参调用不变。displayDiagnostics 处理的是 DiagnosticBag 中的 Diagnostic 对象（parse/compile 阶段产生），能读到 `diag.code`

### 修改文件清单

- 修改：`gui/TeachingPanelHeader.cpp`（+include LearningPathData.h + learningPathSummaryText() + learning-path/bug-hunt 文案改为派生）
- 修改：`gui/LabManualPanel.h`（+foldBtn_ / foldMinorSections_ / onToggleFold / isMinorSection / applyFolding）
- 修改：`gui/LabManualPanel.cpp`（+include QRegularExpression + foldBtn_ 创建 + showCurrentChapter 调用 applyFolding + isMinorSection/applyFolding/onToggleFold 实现）
- 修改：`common/Diagnostic.h`（Diagnostic +code 字段 + 6 参构造重载 + DiagnosticBag addError/addWarning/addInfo 4 参重载）
- 修改：`gui/ErrorHintEngine.h`（+4 参 enrichErrorMessage 重载声明）
- 修改：`gui/ErrorHintEngine.cpp`（+4 参 enrichErrorMessage 实现 + kCodeToHint 表 9 条映射）
- 修改：`app/ide.cpp`（displayDiagnostics 改为 4 参 enrichErrorMessage 传 diag.code）
- 修改：`tests/TestErrorHintEngine.cpp`（+ErrorHintEngineCodeMatching 套件 8 用例）
- 新增归档：`docs/changelog/archive/2026-07-07-round20-teaching-panel-p0.md`（第二十轮归档）
- 文档同步：`README.md`（测试徽章 1754→1762）+ `CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）+ `project_memory.md`（本章节）

## 2026-07-08 · 第二十二轮：教学模块 P0+P1 项（复核版）

### 概述

本轮依据【MiniLang教学模块分析_复核与优化建议.md】完成复核版 P0 三项 + P1 四项落地。核心是打通教学面板之间的信号接线、活动→面板映射、错误速查联动。修复两处 Qt6 兼容性预存 Bug（QRegExp 已移除），全量 1754/1754 测试通过。

### 修改内容

1. **P0-A 术语表跳转信号接线 — gui/GlossaryPanel.{h,cpp} + app/ide.cpp**
   - `GlossaryPanel` 新增 `relatedPanelFor(term)` 静态方法 + `termActivated(term, relatedPanel)` 信号
   - 30 条术语映射到相关教学面板（如 NaN-boxing→memory-model / closure→closure-inspector / BytecodeChunk→bytecode-trace 等）
   - `app/ide.cpp` 连接 `termActivated` → `onJumpToPanel(relatedPanel)`，实现术语→关联面板一键跳转

2. **P0-B Bug-Hunt 三后端判分与回写 — gui/BugHuntPanel.{h,cpp}**
   - 新增 `challengeSolved(challengeId)` 信号
   - `onTripleVerify()` 末尾：当三后端输出一致且匹配 `expectedBehavior` 时发射 `challengeSolved`
   - `app/ide.cpp` 连接 `challengeSolved` → `markActivityCompleted("bug-hunt")`，打通 Bug 狩猎通关回写

3. **P0-D LabManual EXPECTED_OUTPUT 自动判分 — gui/LabManualPanel.cpp（审读确认已实现）**
   - 经审读，`controller_->runStringCaptureOutput` 同步比对 stdout 与 `expectedOutput` 的逻辑已在第二十轮 P0-1 闭环中实现（`runSampleRequested` → 运行 → 输出对照）。本轮仅验证，不修改

4. **P1-E rightDock 白名单保留 — app/ide.cpp**
   - `showTeachingPanel()` 新增 `kKeepRightDockPanels` 白名单：pipeline / ir-transform / bytecode-trace / memory-model / backend-compare 等需配套右侧编译分析面板的浏览型面板显示时不再隐藏 rightDock_，保留配套视图

5. **P1-F2/F 多面板活动映射 + 浏览型面板 visited-* 活动 — gui/LearningPathData.cpp + app/ide.cpp + gui/PanelCatalog.cpp**
   - `LearningPathData` 阶段零末尾新增 6 个浏览型活动：visited-glossary / visited-pipeline / visited-memory-model / visited-bytecode-trace / visited-exception-flow / visited-closure-inspector（共 28 个活动，原 22 + 6）
   - `app/ide.cpp` `showTeachingPanel()` 末尾添加 `kBrowsePanelToActivity` 映射，浏览型面板进入即调用 `markActivityCompleted(visited-*)`
   - `gui/PanelCatalog.cpp` `canonicalPanelId()` 新增 6 个 visited-* → 真实面板 ID 的映射（visited-glossary→glossary 等），确保 `LearningPathActivitiesMapToValidPanels` 测试通过

6. **P1-F12 常见错误速查与 ErrorHintEngine 联动 — gui/ErrorHintEngine.{h,cpp} + gui/LabManualPanel.cpp + tests/TestErrorHintEngine.cpp**
   - `ErrorHintEngine` 新增 `ErrorPattern` 结构体（tag / title / buggyCode / category）+ `errorPatterns()` 静态方法返回 12 条错误模式（missing-semicolon / unbalanced-paren / unbalanced-brace / undefined-variable / undefined-function / division-by-zero / index-out-of-bounds / type-mismatch / unexpected-token / arity-mismatch / recursion-depth / null-access）
   - `LabManualPanel` 处理 `buggy:tag` URL scheme：点击章节末尾「触发示例」链接 → 查 errorPatterns 表 → emit `runSampleRequested(buggyCode)` 加载运行错误代码
   - `showCurrentChapter()` 每章 Markdown 末尾动态追加「🔧 常见错误速查（ErrorHintEngine 联动）」段：从 errorPatterns() 生成表格 + `[▶ 触发](buggy:tag)` 链接，替代原 LabManualContent 静态死表
   - `tests/TestErrorHintEngine.cpp` 新增 `ErrorHintEnginePatternsTable` 测试套件（4 用例）：PatternsAreNonEmpty / TagsAreUnique / KnownTagsPresent / CategoriesAreValid

7. **P1-F7 细粒度压扁修复 — app/ide.cpp**
   - 三游戏面板（TokenPuzzle / AstBuilderToy / VmStackSandbox）的 `activityCompleted` connect 改为调用 `LearnerProgressStore::areAllLevelsCompleted({子关卡 ID 列表})`，仅当全部子关卡完成时才 `markActivityCompleted(父活动 ID)`
   - 避免原「单关完成即标记整体完成」的过度乐观进度

8. **Qt6 兼容性预存 Bug 修复（构建阻塞）**
   - **gui/IRTransformPanel.cpp**：`#include <QRegExp>` → `<QRegularExpression>`，`irTextToClickableHtml` 内 `QRegExp::indexIn` → `QRegularExpression::match` + `hasMatch` + `captured`
   - **gui/PipelineViewer.cpp**：同上模式，`irTextToClickableHtml` + `bytecodeTextToClickableHtml` 两处 QRegExp → QRegularExpression
   - 根因：QRegExp 在 Qt6 中已被移除（Qt5 起标记 deprecated），原代码在 Qt 6.10.3 下编译失败 `fatal error C1083: 无法打开包括文件: "QRegExp"`

### 关键决策

1. **visited-* 活动为「轻量打卡」而非「学习目标」**：浏览型面板（术语表/管线/内存模型等）只需进入即可完成，不设关卡/得分。这类活动用于补全 LearningPathData 的活动→面板映射完整性，让「学习路径地图」中每个推荐面板都有对应活动可标记
2. **`buggy:` URL scheme 复用 runSampleRequested 信号**：不引入新信号，复用第二十轮 P0-1 已接线的 `runSampleRequested → loadCodeIntoMainEditor + onRun` 闭环。错误代码加载后用户可直接运行观察错误信息，配合 ErrorHintEngine 增强提示形成「看错误示例 → 运行 → 看增强错误提示」教学链
3. **errorPatterns 静态表集中管理**：12 条错误模式集中在 `ErrorHintEngine::errorPatterns()` 单一数据源，LabManualPanel 动态生成「常见错误速查」段从该表读取。新增错误模式只需修改一处，所有章节末尾自动同步
4. **三游戏面板 areAllLevelsCompleted 而非单关触发**：原 connect 监听 levelId 信号后直接 `markActivityCompleted(父活动)`，导致「Token 拼图第 1 关完成」即标记整体完成，进度失真。改为 `areAllLevelsCompleted` 检查全部 5/6/5 子关卡完成才标记父活动
5. **canonicalPanelId 显式映射而非前缀匹配**：visited-* 不是面板 ID 而是「活动别名」，不能用 `startsWith("visited-")` 前缀匹配（会误判未来新增的 visited-foo 等未注册活动）。显式 `if (id == "visited-glossary") return "glossary";` 6 条映射，与 lab-*/bug-hunt-* 别名处理风格一致

### 修改文件清单

- 修改：`gui/GlossaryPanel.h`（+relatedPanelFor 静态方法声明 + termActivated 信号）
- 修改：`gui/GlossaryPanel.cpp`（+relatedPanelFor 实现 30 术语映射 + emit termActivated）
- 修改：`gui/BugHuntPanel.h`（+challengeSolved 信号声明）
- 修改：`gui/BugHuntPanel.cpp`（onTripleVerify 末尾发射 challengeSolved）
- 修改：`gui/LearningPathData.cpp`（+6 visited-* 活动，阶段零末尾）
- 修改：`gui/PanelCatalog.cpp`（canonicalPanelId +6 visited-* 映射）
- 修改：`gui/ErrorHintEngine.h`（+ErrorPattern struct + errorPatterns() 声明）
- 修改：`gui/ErrorHintEngine.cpp`（+errorPatterns() 实现 12 条模式）
- 修改：`gui/LabManualPanel.cpp`（+#include ErrorHintEngine.h + buggy: scheme 处理 + showCurrentChapter 动态错误速查段）
- 修改：`gui/IRTransformPanel.cpp`（QRegExp → QRegularExpression）
- 修改：`gui/PipelineViewer.cpp`（QRegExp → QRegularExpression 两处）
- 修改：`app/ide.cpp`（+termActivated connect + challengeSolved connect + kKeepRightDockPanels 白名单 + kBrowsePanelToActivity 自动标记 + 三游戏面板 connect 改 areAllLevelsCompleted）
- 修改：`tests/TestLearningPathAudit.cpp`（活动数 22→28 + StageProgressCalculation 追加 6 visited-* markCompleted）
- 修改：`tests/TestErrorHintEngine.cpp`（+ErrorHintEnginePatternsTable 套件 4 用例）
- 新增归档：`docs/changelog/archive/2026-07-07-round19-teaching-panel-fixes.md`（第十九轮归档）
- 文档同步：`README.md`（测试徽章 1728→1754）+ `CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表头插入新行）+ `project_memory.md`（本章节）

## 2026-07-07 · 第二十一轮：教学模块 P2-3 学情画像多维度进度数据

### 概述

本轮依据【MiniLang教学模块分析与改进建议.md】完成 P2-3 优先级改进（功能 9），将学习路径进度从「打卡式」升级为「学情画像」：`LearnerProgress` 新增 perActivity 的 `score`/`bestStars`/`spentMinutes`/`failCount` 四维字段；`LearningPathPanel` 新增「薄弱点提示」与「预计剩余时间」汇总；`LabManualPanel` 章节练习接入得分/失败记录。同时修复 P2-2 引入的 MarkdownRenderer heading `id` 属性导致 6 个测试失败的问题。全量 1728/1729 测试通过（1 perf 占位符 Not Run）。

### 修改内容

1. **P2-3a LearnerProgress 多维进度数据 — gui/LearnerProgress.{h,cpp}**
   - `LearnerProgress` struct 新增 4 个字段：`score`（0-100 分值）/ `bestStars`（历史最佳星级）/ `spentMinutes`（累计耗时）/ `failCount`（失败次数）
   - 新增 `WeakPoint` struct：`activityId` / `attempts` / `fails` / `score`，用于薄弱点检测
   - 新增 10 个 API：`recordScore(activityId, score, stars)`（截断 [0,100]，保留历史最佳）/ `getScore` / `getBestStars` / `addSpentMinutes`（累加，负数忽略）/ `getSpentMinutes` / `recordFailure` / `getFailCount` / `getWeakPoints(all, minAttempts, maxCount)`（筛选未完成且尝试≥3 或失败≥1，按 failCount→attemptCount 降序）/ `estimatedRemainingMinutes(all)`（未完成且已解锁活动的 estimatedMinutes 之和）/ `totalSpentMinutes`
   - `load()` 读取 4 个新字段 JSON 对象，`save()` 写入，`reset()` 同步清空

2. **P2-3b LearningPathPanel 薄弱点与剩余时间 — gui/LearningPathPanel.{h,cpp}**
   - 新增 `weakPointsLabel_`（warning 色）与 `remainingLabel_`（success 色）两个 QLabel
   - `refresh()` 调用 `store.getWeakPoints(all)` 显示第 1 个薄弱点标题 + 失败次数，多个时 tooltip 列出全部
   - `refresh()` 显示「⏱ 剩余 ~X 分钟 · 已用 Y 分钟」（estimatedRemainingMinutes + totalSpentMinutes）
   - `buildActivityRow()` 增强时间显示（有实际耗时时「~X 分钟 / 已用 Y」）+ 得分/星级徽章（score>0 或 stars>0 时）+ 已解锁活动 rich tooltip（尝试/失败/得分/星级/耗时）

3. **P2-3c LabManualPanel 练习接入得分记录 — gui/LabManualPanel.cpp**
   - `onSubmitChoiceExercise()`：答对时 `recordScore(ch.id, 100, 1)` + save；答错时 `recordFailure(ch.id)` + save
   - `checkAllExercisesPassed()`：全章通过时升级为 3 星 `recordScore(ch.id, 100, 3)` + save

4. **P2-3d 测试套件 4：LearningPathRichProgressAudit — tests/TestLearningPathAudit.cpp**
   - 新增 8 个测试：`RecordScoreKeepsBest`（保留历史最佳）/ `RecordScoreClampsOutOfRange`（截断 [0,100]）/ `SpentMinutesAccumulates`（累加 + totalSpentMinutes）/ `RecordFailureIncrementsCount` / `WeakPointsDetection`（筛选/排序/maxCount/排除已完成）/ `EstimatedRemainingMinutesExcludesCompletedAndLocked` / `LoadSaveRoundTripPreservesRichFields`（JSON 往返）/ `ResetClearsRichFields`

5. **MarkdownRendererAudit 测试适配 P2-2 heading id 属性 — tests/TestMarkdownRendererAudit.cpp**
   - P2-2 为标题渲染添加 `id="anchor"` 属性（`<h1 id="...">text</h1>`），导致 6 个测试的 `contains("<hN>text</hN>")` 精确匹配失败
   - 新增 `headingMatches(html, level, text)` 辅助函数，使用 `QRegularExpression` 匹配 `<hN(?:\s+[^>]*)>text</hN>`，接受可选 id 属性
   - 更新 6 个测试：Heading1/2/3、StdStringOverload、CompositeLabManualSnippet、FragmentStripsWrapper

### 关键决策

1. **score 截断 [0,100] 且保留历史最佳**：recordScore 仅当新分 ≥ 旧分时覆盖 bestStars，避免重玩低分覆盖历史高分；score 截断到 [0,100] 防止越界
2. **薄弱点筛选策略**：未完成且 (attemptCount >= minAttempts=3 或 failCount >= 1)，按 failCount 降序 → attemptCount 降序排序，maxCount=5 限制显示数量。失败次数高优先，尝试次数高但未通过次之
3. **estimatedRemainingMinutes 排除已完成和未解锁**：仅统计「未完成且已解锁」活动的 estimatedMinutes，避免高估（已完成不需要再做）和低估（未解锁活动暂不计入）
4. **MarkdownRendererAudit 用正则而非子串匹配**：heading 现带 `id` 属性，`contains("<h1>text</h1>")` 精确匹配会失败。用 `QRegularExpression` 匹配 `<h1(?:\s+[^>]*)>text</h1>` 既严格（仍验证标签层级与文本）又兼容可选属性

### 修改文件清单

- 修改：`gui/LearnerProgress.h`（LearnerProgress 新增 4 字段 + WeakPoint struct + 10 API 声明）
- 修改：`gui/LearnerProgress.cpp`（load/save/reset 同步 4 字段 + 10 API 实现）
- 修改：`gui/LearningPathPanel.h`（新增 weakPointsLabel_ / remainingLabel_ 成员）
- 修改：`gui/LearningPathPanel.cpp`（refresh 薄弱点 + 剩余时间 + buildActivityRow 徽章/tooltip）
- 修改：`gui/LabManualPanel.cpp`（onSubmitChoiceExercise / checkAllExercisesPassed 接入 recordScore/recordFailure）
- 修改：`tests/TestLearningPathAudit.cpp`（+8 测试：套件 4 LearningPathRichProgressAudit）
- 修改：`tests/TestMarkdownRendererAudit.cpp`（headingMatches 辅助函数 + 6 测试适配 id 属性）
- 修改：`gui/AstBuilderToyPanel.h`（前向声明 `class IdeController;` 修复 setController 编译错误）
- 文档同步：`README.md`（测试徽章更新）+ `CHANGELOG.md`（本章节）+ `docs/development.md`（摘要表插入新行）+ `project_memory.md`（追加本轮关键决策）
- 归档：`docs/changelog/archive/2026-07-07-round18-parser-sync-type-annotation.md`（第十八轮归档）

## 2026-07-07 · 第二十轮：教学模块 P0 三项改进

### 概述

本轮依据【MiniLang教学模块分析与改进建议.md】完成 P0 优先级三项改进，覆盖教学闭环打通、游戏进度持久化、错误信息友好化三条主线。修复后全量 1730/1730 测试通过，minilang_ide 与 minilang_tests 双目标构建成功。

### 修改内容

1. **P0-1 打通「看教学 + 写代码 + 看运行结果」同屏（F5/F13）— app/ide.cpp + gui/LabManualPanel.{h,cpp}**
   - `showTeachingPanel()` 引入白名单策略：lab-manual / bug-hunt / syntax-explorer / backend-compare / ir-transform / profile-dashboard 等需底部 dock 的面板显示时不再隐藏 bottomDock_，仅 rightDock_ 隐藏
   - LabManualPanel 新增 `runSampleRequested` 信号 + `runBtn_`「▶ 运行示例」按钮 + `onRunSample()` 槽
   - app/ide.cpp 连接 `runSampleRequested` → `loadCodeIntoMainEditor(code)` + `onRun()`，实现「看教学样例 → 一键运行 → 底部输出」闭环

2. **P0-2 细化游戏进度粒度并持久化星级（F7）— gui/LearnerProgress.{h,cpp} + 三个游戏面板 + app/ide.cpp**
   - `LearnerProgress` struct 新增 `std::map<std::string, int> levelStars` 字段（key="token-puzzle-N"/"ast-toy-level-N"/"level-N"，value=-1 未完成 / 0 跳过 / 1-3 星级）
   - `LearnerProgressStore` 新增 3 个 API：`markLevelStars(levelId, stars)`（保留历史最佳）、`getLevelStars(levelId)`、`areAllLevelsCompleted(levelIds)`
   - `load()` 读取 levelStars JSON 对象，`save()` 写入，`reset()` 同步清空
   - **TokenPuzzlePanel**：构造时从 store 加载已记录星级，按已完成关卡解锁下一关 UI；`onCheckAnswer()`/`onSkipLevel()` 写入 store 并保存
   - **AstBuilderToyPanel**：构造时从 store 加载已完成关卡列表；`markCurrentCompleted()` 写入 store（AST 玩具无星级评分，统一记 3 星）
   - **VmStackSandboxPanel**：`onCheck()` 通关时写入 store（VM 沙盒无星级评分，统一记 3 星）
   - **app/ide.cpp** 三个 connect 简化：移除原 `levelId.section('-', 0, 1)` 的复杂映射逻辑，统一改为只转发粗粒度活动 ID 到 LearningPathPanel（细粒度星级由面板自身直接持久化到 LearnerProgressStore）

3. **P0-3 主编译/运行路径接入 ErrorHintEngine（F10/F11）— app/IdeController.{h,cpp} + app/ide.cpp + gui/ReplPanel.cpp**
   - **IdeController** 新增 `getReplScopeVariableNames()` 公共接口：返回当前 Interpreter 作用域链上所有可见变量名（去重，子作用域优先），供 GUI ErrorHintEngine 拼写建议使用
   - **app/ide.cpp** 三处 appendError 调用点接入 ErrorHintEngine：
     - `runtimeError` 信号处理器：收集 scopeVars → `enrichErrorMessage(msg, "runtime", scopeVars)` → diag.format()
     - `displayDiagnostics()` 循环内：对 Error 级诊断调用 `enrichErrorMessage(msg, "parse", {})`（parse 阶段无 scopeVars）
     - VM ERROR case：调用 `enrichErrorMessage(msg, "runtime", {})`（VM 路径无法直接访问 Interpreter 作用域）
   - **gui/ReplPanel.cpp** 异步 worker 线程的 RuntimeError/std::exception 捕获点：在 worker 线程上下文（此时主线程在 pollReplFuture 阻塞等待）调用 `ctrl->getReplScopeVariableNames()` 收集 scopeVars，再 `enrichErrorMessage` 增强消息后投递到 REPL 输出区。原有 emit runtimeError/genericError 信号保留原始 msg，由主线程 handler 二次增强（避免双重增强）

### 关键决策

1. **白名单而非黑名单**：P0-1 的 dock 可见性策略用白名单（kKeepBottomDockPanels），新增需底部 dock 的教学面板需显式加入白名单。比黑名单更安全，避免新增面板时遗忘导致默认被隐藏
2. **markLevelStars 保留历史最佳**：相同关卡的更高星级才覆盖，避免用户重玩低星覆盖历史高星记录
3. **AST 玩具/VM 沙盒统一记 3 星**：这两个面板无星级评分机制（只有结构/序列比对），用 3 星表示"完美通过"，与 TokenPuzzle 的 1-3 星语义区分
4. **ReplPanel 双路径增强避免双重增强**：worker 线程捕获点增强后投递到 REPL 输出区；emit 信号携带原始 msg，由主线程 handler 二次增强后显示到主错误面板。两条路径各自增强一次，避免重复附加教学提示
5. **parse 阶段 scopeVars 为空**：MiniLang 的未定义变量/函数错误是运行时错误（解释器求值阶段），parse 阶段仅做语法检查。故 displayDiagnostics 的 parse 错误 scopeVars 为空，拼写建议在 runtime 错误路径生效

### 修改文件清单

- 修改：`app/IdeController.h`（新增 getReplScopeVariableNames 声明）
- 修改：`app/IdeController.cpp`（新增 Interpreter.h + unordered_set include + getReplScopeVariableNames 实现）
- 修改：`app/ide.cpp`（顶部新增 ErrorHintEngine.h include；runtimeError/displayDiagnostics/VM ERROR 三处接入；P0-1 showTeachingPanel 白名单；P0-1 LabManualPanel runSampleRequested connect；P0-2 三游戏面板 connect 简化）
- 修改：`gui/LabManualPanel.h`（新增 runSampleRequested 信号 + runBtn_ 成员 + onRunSample 槽）
- 修改：`gui/LabManualPanel.cpp`（新增 runBtn_ 创建 + onRunSample 实现）
- 修改：`gui/LearnerProgress.h`（LearnerProgress 新增 levelStars 字段 + 3 个 API 声明）
- 修改：`gui/LearnerProgress.cpp`（load/save/reset 同步 levelStars + 3 个 API 实现）
- 修改：`gui/TokenPuzzlePanel.cpp`（构造加载持久化星级 + onCheckAnswer/onSkipLevel 写入 store + include LearnerProgress.h）
- 修改：`gui/AstBuilderToyPanel.cpp`（构造加载已完成关卡 + markCurrentCompleted 写入 store + include LearnerProgress.h）
- 修改：`gui/VmStackSandboxPanel.cpp`（onCheck 通关写入 store + include LearnerProgress.h）
- 修改：`gui/ReplPanel.cpp`（worker 线程 RuntimeError/std::exception 捕获点收集 scopeVars + ErrorHintEngine 增强）
- 文档同步：`README.md` + `CHANGELOG.md` + `docs/development.md` + `project_memory.md`

**历史变更**：更早的开发记录（第十九轮 7 项教学面板修复与优化、第十八轮 Parser 错误恢复 + 三后端类型注解一致性、第十七轮寄存器分配 P0 修复 + 模块状态泄漏 + Formatter 往返等价性、第十六轮架构优化、第十五轮审计批次 6、第十四轮教学面板布局重构、工程配置与文档一致性修复等）已归档至 [docs/changelog/archive/](docs/changelog/archive/)。
