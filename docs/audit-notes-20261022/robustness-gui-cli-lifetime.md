# 审计笔记：gui/app 线程安全与生命周期 · capi/cli · §9.2 DeleteFiber 专项

> 日期：2026-10-22 ｜ 性质：**只读审计**（纪律约束：未构建、未运行 GUI，全部结论为静态代码审读，无实测时序验证）
> 置信度口径：所有发现均为「推演」（代码证据充分，未运行复现）；「实读」指对应代码行已逐行核对。
> 勿重复报告项已核实：协程跨线程恢复防御（7b4ec10）在、PkgCli 测试 lockfile 隔离（4586759）在、saveLockfile 原子写已由 pkg_core.cpp:401 `writeFile`（临时文件+rename）落地，saveManifest(:270)/saveLockfile(:1523) 均走该路径——本笔记不再报告。

---

## 0. 审计范围与方法

- **模块**：gui/ + app/（线程面、UI 禁用完整性、原子标志、Worker 清理、停止/超时回收、性能扫查）；capi/（错误传播与生命周期边界）；cli/（saveLockfile 以外的非原子写/半截写）；interpreter/ 协程实现（§9.2 专项）。
- **方法**：rg/只读读取逐函数审读 + 跨文件生命周期追踪；未运行任何构建/测试/GUI。
- **深度声明**：app/DebugCoordinator.cpp、app/VmStepper.cpp、app/PipelineRunner.cpp、app/IdeController 停止/超时路径为**快速扫查**（抽样而非逐行），结论见 §2-F6；gui/ 其余面板按守卫模式 grep 全量核对。

---

## 1. 专项调查（审计报告 §9.2 登记项）：协程值跨线程析构的 DeleteFiber 边界

### 结论一句话
DeleteFiber 的调用线程 = **最后一个释放 `CoroutineData` 引用（使 `CoroutineFiberState` 析构）的线程**，在 IDE 宿主场景下可以是主线程而 fiber 创建于 worker/async 线程；后果是**未文档化的跨线程 DeleteFiber + fiber 栈泄漏**（Windows）或泄漏 + ASan fake-stack 未关闭（Linux），**无 UAF**（只命中 parked fiber）；语言层不可达，**IDE 嵌入层可达**，capi/cli 不可达。定性 **P2**。

### 1.1 证据链（实读）
1. `interpreter/InterpreterCoroutine.cpp`：`CoroutineFiberState` 析构函数调用 `minilang_fiber::deleteFiber(fiber)`；`CoroutineData::interpreterFiber` 为 `shared_ptr<void>`，自定义 deleter `delete CoroutineFiberState`——即 **CoroutineData 最后一次释放的线程执行 DeleteFiber**。
2. `interpreter/RefCounted.h`：refCount 为 `atomic<int>`，release 归零者在最后一次 release 的线程执行 `delete`——析构线程由最后一次释放决定，与创建线程无关。
3. `ownerThread` 在首次 `.next()` 懒构造 fiber 时登记；恢复路径有 §3.8 防御（commit 7b4ec10，「协程不可跨线程恢复」）——**该防御只保护 resume，不保护析构**。
4. 完成态/失败态协程在 owner 线程 `callCoroutineNext` 内联 reset（interpreterFiber 在调用线程释放）——不命中跨线程析构；跨线程析构只命中**挂起（parked）fiber**。
5. `CoroutineFiberState::cd` 为裸指针，靠调用方 coroVal 引用计数保活——析构顺序本身自洽，问题仅在「哪个线程执行」。

### 1.2 可达路径（IDE 中全部成立）
- **路径 A｜REPL future Value 主线程析构**：`gui/ReplPanel.cpp` `executeLine`（~476-560）`std::async(std::launch::async, ...)` 返回 `std::future<Value>`；`pollReplFuture`（~560-620）在**主线程** `future.get()` → `result.toString()` → `result` 析构于主线程。REPL 行结果为已启动且仍挂起的协程时（如 `fun start(){var g=gen();g.next();return g;} start();`），主线程析构 owner 线程（async 线程）创建的 `CoroutineData` → 主线程 DeleteFiber。
- **路径 B｜Run(F5) 结束 / IDE 关闭**：cleanupWorker 在主线程调 `interpreter_->restoreReplState()`（Interpreter.cpp ~188 / ~1745+）→ 丢弃 run-scoped `globalEnv_` → worker 线程创建且仍挂起的协程在主线程析构 → 主线程 DeleteFiber。IDE 关闭时主线程析构 interpreter 同构。

### 1.3 后果分析
- **Windows fiber**：DeleteFiber 为**未文档化的跨线程行为**；fiber 栈（约 4MB 保留地址空间/协程）不会被回收，**C++ 栈上对象的析构被遗忘**（与 §9.2 原判断一致）。IDE 长会话（反复运行含挂起协程的程序）可累积泄漏。无 UAF——parked fiber 不在任何线程上运行。
- **Linux ucontext**（`MINILANG_CORO_FIBER=1`）：释放为普通堆 delete（本身线程安全），但同样泄漏栈内存，且 ASan fake-stack 未按平台标注关闭（`InterpreterCoroutine.cpp` 五函数对称接口已有 ASan fiber 标注，析构侧跨线程路径未覆盖）。
- **不发生**：UAF/崩溃/语言层可见语义错误。

### 1.4 可达性结论
| 层 | 可达性 | 理由 |
|---|---|---|
| MiniLang 语言层 | 不可达 | 单线程执行模型；跨线程恢复已被 §3.8 防御拒绝 |
| IDE 嵌入层（gui/app） | **可达**（路径 A/B） | 见 §1.2 |
| capi | 不可达 | 每次 eval 独立引擎、单线程使用（见 §3-F7 的并发问题独立存在，但 eval 内无跨线程协程析构） |
| cli | 不可达 | CLI REPL/脚本单线程 |

### 1.5 最小修复设计
1. **引擎侧（根治）**：`~CoroutineFiberState` 加 ownerThread 比对（std::this_thread::get_id()）：
   - 同线程 → 照常 `deleteFiber`；
   - 跨线程 → **有意弃置**（不调用 deleteFiber，记录诊断日志；与 ADR-007 Known Limitation #2 的弃置语义对齐），更新 ADR-007 文档明确「跨线程弃置 parked fiber 会泄漏其栈」。
   - 关键点：只跳过 `deleteFiber`，其余 C++ 侧成员照常析构（cd 保活语义不变）。
2. **宿主侧（消除触发面）**：`ReplPanel` 改为在 async 线程内完成 `result.toString()` 后仅向主线程投递 QString（与其余信号纪律一致），杜绝 Value 在主线程析构；路径 B 的 restoreReplState 同理可改为 worker 线程收尾时自行保存/恢复。

### 1.6 回归测试思路
- `MINILANG_CORO_FIBER` 门控测试（参照既有 `CrossThreadResumeRejected` 的 std::thread 驱动模式）：
  1. 线程 A 创建并启动生成器（一次 `.next()`），join 后主线程销毁持有协程的 Value；
  2. 断言：不崩溃；随后主线程新建第二个协程并完整跑完（验证 GcManager/registry 状态未损坏）；
  3. 可选：测试钩子记录 deleteFiber 调用线程，断言析构未跨线程调用 deleteFiber（修复生效判定）。

---

## 2. gui/ + app/ 发现

### F1（P1）GcManager 单槽回调覆盖 + 错误根集 collectCycle：七面板无守卫放大为存活容器误回收
- **位置**：
  - `common/BackendExecutionService.cpp:212`（`BackendExecutionService::execute` 内栈构造 `Interpreter interp;`）；
  - `interpreter/Interpreter.cpp:59`（构造注册回调，单槽覆盖）、`:69`（析构 `clearGcTriggerCallbackIfOwner`）；
  - `interpreter/GcManager.cpp:81`（阈值置 pending）、`:86-107`（`checkIncrementalGc`，:96 有空回调保护）、sweep Phase 2 ~386-470（对**所有未标记槽位**清空子元素，无 refCount==1 过滤；GcOnly 模式直接 delete）；
  - 无守卫面板（主线程直接调 execute/executeWithDetail）：`BackendParallelPanel.cpp:205-207`（onRunAllBackends 仅查 src.empty():195）、`BackendComparePanel.cpp:156/166/177`、`BugHuntPanel.cpp:522/535/660/672/684`、`CoroutineVisualizerPanel.cpp:308/319/328`、`ExerciseGraderPanel.cpp:812/817/822`、`PerformanceRacePanel.cpp:242/248/254`、`ProfileDashboardPanel.cpp:418/440/451`。
- **严重性**：P1 ｜ **类型**：竞态/错误根集回收（数据损坏面）/生命周期
- **根因**：GcManager 为进程级单例 + **单槽** `gcTriggerCallback_`，设计假设「同一时刻至多一个活跃 Interpreter 收集完整根集」。AUDIT-R3 P2-9 的 owner 令牌只修了「先构造者析构误清后构造者回调」，**未修「后构造者覆盖后以前者根集执行 sweep」**。`execute()` 每次新建 Interpreter 即覆盖常驻 Worker 解释器的回调；sweep 阶段对所有未标记槽位动手，未标记集合 = 「临时实例根集不可达」= 常驻解释器 globalEnv_ / Worker 存活容器的超集。
- **触发链**（推演）：
  1. **并发链**：F5 Worker 运行中（容器持续登记、可越过阈值），用户在主线程点击任一上述面板 → 临时 Interpreter 覆盖回调 → 面板脚本执行达到阈值 → 以临时实例的 globalEnv_/callStack 为根 collectCycle → Worker 存活容器 unmarked → 默认模式被 move 清空子元素（数据丢失/级联释放），GcOnly 模式直接 delete（Worker 线程随后访问 → UAF）。sweep 结构由 recursive_mutex 保护（无结构性数据竞争），但**根集语义错误不受锁保护**。
  2. **非并发链**（更易复现）：上轮运行的持久变量容器仍被常驻解释器 `globalEnv_` 引用（saveReplState/restoreReplState 生命周期）→ 面板执行触发临时实例 GC → 这些容器相对临时根集 unmarked → 被清空 → 下一轮 F5 运行读到空容器/错值。
  3. **附带失效**：临时实例析构按 owner 令牌清空回调（owner==临时）→ 常驻解释器此后**增量 GC 永久失效**（:96 空回调保护使其静默跳过，不崩溃），循环垃圾持续累积至会话结束。
- **佐证（项目自身不变量）**：`GcVisualizerPanel.cpp:655-658/674-680` 明确以 `isRunning()||isVmRunning()` 拒绝运行期 GC 操作，注释直言「破坏运行中的对象跟踪（可能提前回收 UAF）」——七面板违反同一不变量。
- **建议**：
  - 最小修复：七面板入口统一加 `controller_->isRunning()/isVmRunning()` 守卫（对齐 GcVisualizerPanel.cpp:658 模式），运行期拒绝执行并提示。
  - 彻底修复（引擎侧）：GcManager 回调改多槽（owner→callback 注册表），collectCycle 仅合并「全部活跃 owner」的根集；或 `execute()` 全程 `CallbackSuppressor` 包裹并在确认无其他活跃 Interpreter 时才允许增量触发。
- **置信度**：推演（高：代码路径闭合，时序未实测）。

### F2（P1）REPL 跨行生成器恢复必然失败：线程模型与 §3.8 跨线程防御直接冲突
- **位置**：`gui/ReplPanel.cpp` `executeLine` ~476-560（每行新开 `std::async`，MSVC 实现为每 future 独立线程）+ `pollReplFuture` ~560。
- **严重性**：P1 ｜ **类型**：功能失效/设计冲突（防御误伤宿主用法）
- **根因/复现场景**（推演，MiniLang 代码）：
  ```minilang
  var g = gen();   // 行1：创建生成器
  g.next();        // 行2：fiber 创建于线程2，ownerThread=线程2
  g.next();        // 行3：在线程3恢复 → §3.8 防御拒绝：「协程不可跨线程恢复」
  ```
  REPL 每行独立 async 线程 ⇒ 任何跨行挂起/恢复生成器**必然**命中防御。§3.8 修复（7b4ec10）消除了崩溃面，但把 IDE 自身线程模型下的合法用法变成了功能失效。
- **建议**：REPL 会话绑定单一专用执行线程（如常驻 worker QThread 顺序消费行队列），使 ownerThread 恒定；或短期文档化「REPL 不支持跨行恢复协程」并在文档同步。此修复与 §1.5 宿主侧修复（worker 线程 toString）可合并设计。
- **置信度**：推演（高：线程模型与防御逻辑均已实读）。

### F3（P2）跨线程析构 parked fiber（§9.2 主体）
- 见 §1。位置：`interpreter/InterpreterCoroutine.cpp`（~CoroutineFiberState 析构）；类型：生命周期/未文档化 API 使用；建议与测试思路见 §1.5/§1.6；置信度：推演（高）。

### F4（P2/P3）性能观察（顺带扫查）
- **P3｜面板后端执行阻塞 UI**：BackendParallelPanel/PerformanceRacePanel/ProfileDashboardPanel 等在主线程同步跑全部后端（BackendExecutionService 注释自认「主线程串行避免引擎非线程安全」）——大脚本时 UI 冻结时长=脚本执行时长。属设计取舍；建议后续移入 worker 队列（需先解决 F1 的根集问题，二者耦合）。
- **P3｜execute 全量重跑管线**：每次 execute 从 Lexer 全量走到执行，无 AST/chunk 复用（对比 ide.cpp 主路径有 300ms/500ms 防抖与缓存）。教学面板低频触发，影响有限。
- **正面结论（无新发现）**：ide.cpp 防抖覆盖良好（syntaxCheckTimer 300ms:8141、completionTimer 500ms:8137、FindReplace/BugHunt/LearningPath 200-250ms、fileTreeFilter 200ms:5404、输出合批 30ms:3359-3362）；SyntaxHighlighter 走 QSyntaxHighlighter 增量重高亮，无每 keystroke 全量解析。

### F5（正面结论）Worker 状态清理与 UI 禁用完整性
- `app/WorkerManager.cpp`：QThreadDeleter quit+wait(5000)→terminate+wait(2000)→_Exit 兜底；shared_ptr 共享 Interpreter/AST；stopForClose/forceStop 路径完整；isRunning_ 原子。
- `app/InterpreterWorker.cpp`：run 尾部 + 析构双保险清理回调；跨线程信号仅传 QString，无 Value 跨线程（除 F3/路径 A 的 REPL future 例外）。
- 守卫清单核对：GcVisualizer:658/677、ReplPanel:422/429（reload/magic/Ctrl+L 亦拦截:301/335/341/838）、CallStack:300/314、VariableInspector:324/425/437/596/608、MemoryModel:826/1281、BreakpointCondition:367/373、Watchpoint:317/321、BytecodeTrace:462 均在——缺口即 F1 七面板。

### F6（正面结论）停止/超时路径快速扫查（非逐行，抽样深度）
- `app/DebugCoordinator.cpp:24-38`：RCU 优雅期（activeCallbackCount_）+ stop() 先唤醒 pauseCV_ 阻塞 worker（R54-9），防 UAF 链完整。
- `app/VmStepper.cpp:766-773`：stop() assertMainThread + 停 vmRunTimer_ + 条件求值快速中止标志；runBatch 各异常/完成/断点路径均 stop 定时器并复位 isVmRunning_（:677-761）。
- `ide.cpp` closeEvent：stopCloseAnimationsAndTimers（QUnifiedTimer 驱动动画显式 stop + removePostedEvents，:715-804）→ stopRunningWorkersForClose（stopForClose(3000)→forceStop→vmStop→processEvents 清挂起信号 :853-894）→ REPL requestReplStop/waitReplFuture——顺序与兜底齐备。
- 未发现新问题；PipelineRunner.cpp 未逐行（抽样未覆盖），如需可后续补查。

---

## 3. capi/ 发现

### F7（P1 候选）插件 DLL 卸载后宿主函数悬垂（跨 context UAF）
- **位置**：`capi/minilang_capi.cpp` `minilang_load_plugin` / `minilang_destroy`；`capi/HostFunctionRegistry.h:91`（注释声称「context 销毁时调用 clear()」，**实现未调用**）。
- **严重性**：P1 ｜ **类型**：UAF/生命周期边界（句柄契约）
- **根因**：插件加载成功后其注册的宿主函数（HostThunk + HostFunctionRegistry 全局单例条目）永久存活；`minilang_destroy` FreeLibrary 卸载 DLL 但不注销 registry 条目。ctx_A 加载插件 → destroy → ctx_B eval 调用同名函数 → 跳转已卸载代码页（崩溃）。
- **建议**：context 记录本 context 注册的函数名集合，destroy 时逐个注销（registry 需补 unregister API）；短期替代：文档化「destroy 前必须保证所有 context 不再调用插件函数」并延迟 FreeLibrary。
- **置信度**：推演（静态链路闭合；无双 context 实测）。

### F8（P2）沙箱配置 save/set/restore 非事务
- **位置**：`capi/minilang_capi.cpp` `minilang_eval` ~148-176（对全局 RuntimeConfig 单例 saved/set/restore 三步序列）。
- **严重性**：P2 ｜ **类型**：竞态（TOCTOU）/错误传播
- **根因**：字段虽为 atomic，但三步序列非原子；头文件宣称「不同线程各自 context 并发安全」→ 并发 eval 互相污染沙箱配置；且 `BackendExecutionService::execute` 抛异常时跳过 restore（非 RAII），异常路径沙箱配置泄漏到后续 eval。
- **建议**：改为 RAII guard（构造 save+set、析构 restore）；长期 per-context 配置下传，不碰全局单例。
- **置信度**：推演。

### F9（P3）杂项
- `minilang_register_function`：HostThunk `new` 永不释放（代码注释自认 MVP 不回收）；同名覆盖泄漏旧 thunk。
- `HostFunctionRegistry.h:12-13`：自述「MVP 假设注册与执行不并发」无锁——注册期间并发 eval 属**文档化限制**，不另立案。

---

## 4. cli/ 发现（saveLockfile 除外，已修复不再报告）

| # | 位置 | 严重性 | 类型 | 说明与建议 |
|---|---|---|---|---|
| F10 | `cli/fmt_core.cpp:101`（fmt -w） | P2 | 非原子写/半截写 | 就地截断回写**用户源文件**：进程崩溃/磁盘满 → 用户源码损毁。建议对齐 `pkg_core.cpp:401 writeFile` 模式（临时文件+rename 原子替换） |
| F11 | `cli/lint_core.cpp:336`（lint --fix） | P2 | 非原子写/半截写 | 同上，就地回写用户源文件，同建议 |
| F12 | `cli/doc.cpp:75`、`cli/main.cpp:295`（-o 导出） | P3 | 非原子写 | 导出产物可再生，价值低；可顺手统一原子写 |
| F13 | `cli/fuzz_core.cpp:1139`（crash-*.ml） | P3 | 非原子写 | 调试产物；fuzz 崩溃瞬间半截文件不影响主流程 |
| — | `cli/dap_core.cpp:57` | — | — | fopen "rb" 只读，无写路径问题 |

已核实原子：`cli/pkg_core.cpp:401 writeFile`（临时文件+rename），saveManifest(:270)/saveLockfile(:1523) 均经此路径。**cli/ 内除上述四点外未发现其他写路径。**

---

## 5. 汇总表

| # | 位置 | 严重性 | 类型 | 一句话 |
|---|---|---|---|---|
| F1 | BackendExecutionService.cpp:212 + 七面板 + GcManager 单槽 | P1 | 竞态/错误根集回收 | 面板执行覆盖 GC 回调→错误根集 sweep 误回收存活容器/永久失去增量 GC |
| F2 | ReplPanel.cpp executeLine | P1 | 功能失效/设计冲突 | 每行新线程使跨行恢复协程必然被 §3.8 防御拒绝 |
| F3 | InterpreterCoroutine.cpp ~析构 | P2 | 生命周期/未文档化 API | 跨线程析构 parked fiber：泄漏为主，无 UAF（§9.2 专项，可达性=IDE 嵌入层） |
| F4 | 三面板主线程同步执行 | P3 | 性能 | UI 冻结=脚本时长；与 F1 耦合 |
| F7 | minilang_capi.cpp load_plugin/destroy | P1 | UAF/生命周期 | FreeLibrary 后 registry 条目悬垂，跨 context 跳转已卸载代码 |
| F8 | minilang_capi.cpp eval 沙箱三步 | P2 | 竞态 | 非事务 save/set/restore + 异常跳过恢复 |
| F9 | capi HostThunk | P3 | 泄漏 | MVP 自认；registry 无锁为文档化限制 |
| F10/F11 | fmt_core.cpp:101 / lint_core.cpp:336 | P2 | 非原子写 | 用户源文件就地截断回写 |
| F12/F13 | doc.cpp:75 / main.cpp:295 / fuzz_core.cpp:1139 | P3 | 非原子写 | 导出/调试产物 |

**全部发现置信度：推演**（只读纪律禁构建，无实测复现）；代码行号均已实读核对。
