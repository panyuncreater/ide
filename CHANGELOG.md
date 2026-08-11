# Changelog

本文件记录 MiniLang IDE 的版本演进，遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 规范，分类说明每版本对用户可见的变更：**Added**（新增功能）/ **Changed**（对已有功能的修改）/ **Deprecated**（即将移除）/ **Removed**（已移除）/ **Fixed**（缺陷修复）/ **Security**（安全相关）。

条目以版本快照粒度组织，不重复 git log。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)，以 [CMakeLists.txt](CMakeLists.txt) `project(MiniLangIDE VERSION ...)` 为基线。

## [v1.3.0] - 2026-07-31

插件系统与沙箱模式落地，教学板块面向初学者做六维体验优化，JIT 修复 Linux 下 SysV ABI 违规导致的 SIGSEGV。

### Added

- **插件系统**：C API 新增 `minilang_load_plugin`，插件经 DLL 回调表 ABI 调用 `minilang_register_function` 注册原生能力；测试插件 DLL 端到端覆盖（[tests/plugin/test_plugin.cpp](tests/plugin/test_plugin.cpp)）。
- **沙箱模式**：`minilang_set_sandbox` 暴露 `sandboxEnabled` / `AllowInput` / `AllowImport` 原子开关——禁 input、禁文件模块导入（`std/` 内建模块仍放行）、禁插件加载；13 用例覆盖三后端 × 三类拦截。
- **HostFunctionRegistry**：宿主函数全局注册表（[common/HostFunctionRegistry.h](common/HostFunctionRegistry.h)），三后端在"未定义函数"回退路径统一查表调用，语义天然一致。
- **async/await 阶段 4 测试补全**：新增 [tests/TestAsyncAwait.cpp](tests/TestAsyncAwait.cpp) 21 用例，覆盖 await/yield 混合、`std/async` 调度器（runAll round-robin / runTask）、四后端一致性。
- **教学板块六维体验优化**：
  - 4 → 6 分类重组（[gui/PanelCatalog.cpp](gui/PanelCatalog.cpp)）：原「执行引擎」23 项平铺拆分为「执行引擎 / 内存与优化 / 调试与观测」三类，按编译原理学习曲线递进排序。
  - 难度分级：`PanelEntry` 新增 `level` 字段（1 入门 / 2 进阶 / 3 高级），分类内按难度非递减排序。
  - 新手引导覆盖 12 → 42/42：通用兜底引导从 `TeachingPanelHeader::helpDocFor()` 派生 3 步 GuidedTour，无专属引导的面板不再弹"暂无引导"。
  - 帮助文案层次化：弹窗新增"🎯 难度：X · 分类：Y"提示行，8 条薄文案补一段加粗核心概念讲解。
- **教学库内容扩充**：OpCode 文档 46 → 75、变量类型示例 17 → 23、闭包场景 12 → 16。

### Changed

- 教学面板标题栏 / 帮助弹窗 / 横幅 / 搜索框 / 树样式共 20+ 处硬编码颜色迁移到 `TeachingTheme` 语义色，主题色变更自动跟随 QFluentKit。
- 3 个手写子页切换面板（VmStackSandboxPanel / JitVisualizerPanel / BytecodeTracePanel）统一改用 `TeachingSubPageBar` 组件，采用组件的面板从 3 → 6 个。

### Fixed

- **JIT SysV ABI 违规（P0，Linux SIGSEGV）**：[compiler/JITCodeGen.cpp](compiler/JITCodeGen.cpp) / [compiler/JITCodeGenHelpers.cpp](compiler/JITCodeGenHelpers.cpp) 中 6 处运行时回调硬编码 Win64 参数寄存器 rcx/rdx，Linux SysV ABI 应为 rdi/rsi，导致 ctx 指针传入垃圾值，TestJIT OSR/Deopt 16 项测试 SIGSEGV。按平台 `#ifdef` 选择参数寄存器修复。
- **教学面板 closing_ UAF（P0）**：`ensureTeachingPanelCreated` / `showTeachingPanel` / `showQuickPanelJumpDialog` 补 `closing_` 检查，避免关闭流程中 `maybeSave` 模态事件循环派发挂起回调时懒构造"孤儿"面板。
- **reverse-timeline 懒加载工厂未注册（P1）**：面板已登记且实现完整但工厂从未注册、源文件未入 CMake，教学树点击静默回退编辑器。补 [cmake/minilang_core.cmake](cmake/minilang_core.cmake) 源列表 + [app/ide.cpp](app/ide.cpp) 工厂注册。
- **教学样例语法错误（P1）**：ClosureInspector 全部 16 个样例 + VariableInspector 引导样例长期使用无括号 `print counter();`，但 `print` 强制要求 `(`，加载样例后必然 parse 失败。全部修正为 `print(...)`。
- **CI Linux GCC -Werror 全量补全**：18 文件 sign-compare / unused-function / warn_unused_result / `__forceinline` 平台守卫 / asmjit SYSTEM 抑制第三方 -Wpedantic / Ubuntu lrelease 单横线 `-version`（Qt 不接受 `--version`）/ app/ide.cpp Qt 6.8 弃用 API / StepExplainerPanel switch 缺 case 等。
- 新手引导 `kAutoTourPanels` 12 → 37 面板首访自动引导；`onPanelGuidedTourRequested` 补 `watchpoint` 路由分支。
- 帮助文案 `watchpoint` / `reverse-timeline` 两条条目补全至 42/42。

## [v1.2.0] - 2026-07-29 ~ 2026-07-30

七特性 MVP 阶段性收尾：宏模板、trait/mixin、async/await、`?` 错误传播四项语言能力三后端全链路落地；运算符重载（dunder）与 const fun 编译期求值补齐。

### Added

- **宏模板（语言）**：新增 [ast/MacroExpander.h](ast/MacroExpander.h) / .cpp——`macro name(p1,p2) { <expr> }` 表达式模板宏，`name!(args)` 调用 `cloneWithSubstitution` 克隆宏 body 并替换形参 VarRef（同 C 宏语义，同一形参多次出现共享实参子树），递归深度上限保护；parse 期展开后三后端零改动。
- **trait/mixin（语言）**：新增 `TK_TRAIT` / `TK_WITH` 词法 + `TraitDecl` AST 节点，`trait T { fun m() {...} }` + `class C with T1, T2`（可与 extends 并存），trait 方法在 parse 期合入混入类（四后端零改动）。
- **async/await 阶段 4（语言）**：`TK_ASYNC` / `TK_AWAIT` 词法，复用 R164 协程重放模式；调度器以纯 MiniLang 实现于内建模块 `std/async`（`runAll` / `runTask`）。
- **`?` 错误传播（语言）**：`?` 脱糖为共享内联 `__qmark_unwrap`（三后端自动一致），`Ok` / `Some` 解包、`Err` / `None` 可捕获传播；配套 `std/result` 泛型 enum 模块。
- **运算符重载 / dunder 分派（语言）**：`__add` / `__sub` / `__mul` / `__div` / `__mod` instance 算术分派——Interpreter 在 numericBinaryOp 报错前分派（含继承链查找与 1 参数校验），StackVM 在 executeArithOps 帧注入，RegisterVM 在 executeArith 经 executeCallImpl 注入（含 methodCache 缓存），JIT 适配。无 dunder 时回退原"算术运算需要数值类型"报错。
- **const fun 编译期求值（性能）**：新增 [common/ConstFunEval.h](common/ConstFunEval.h)（header-only 三后端共享），折叠条件为「直接名称调用 + 实参全字面量 + arity 匹配 + 独立 Interpreter 沙箱求值成功无副作用 + 结果原始类型」；任一条件不满足返回 `nullopt` 回退运行时调用。

### Fixed

- **构建：tests/CMakeLists.txt PCH 缺失**：为 `minilang_app_tests` / `minilang_gui_smoke` 补挂 PCH，修复 fresh 重编译时 GcManager 单例 LNK2005 / LNK1169（unity-build ODR 预存缺陷清偿）。
- **CI 5 个失败任务**：ubuntu `lrelease6` 缺失（补装 `qt6-tools-dev`，三名兜底）；macOS Clang 4 项编译错误（含 `Interpreter.h` `atomic<shared_ptr>` → `mutex`）；Windows `/WX` C4189；Docker 补 `-DMINILANG_BUILD_TEST_HARNESS=OFF -DMINILANG_BUILD_CLI=OFF`；89 文件 clang-format 就地格式化；弃用 actions 升级（`upload-artifact@v6`、`msvc-dev-cmd@v1.13.0`）。
- **CrashHandler.cpp Unity build 污染**：`windows.h` 必须先于 `dbghelp.h` 包含。

## [v1.1.0] - 2026-07-26 ~ 2026-07-28

已知限制清偿批次（B1 Interpreter TCO 蹦床 + B1-Shadow 三 VM 死循环 + E3 channel.recvTimeout + E1 stale obj 自动修复），AUDIT-R2/R3 系列审计修复收尾，ARCH-10 架构缺陷修复（GUI 层解耦 + Unity Build + Facade 瘦身）。

### Added

- **Interpreter 尾调用蹦床（TCO）**：消除三后端最后一个已知设计差异（testing.md 限制 #10）。`visitReturnStmt` 用与 VM 共享的 `TCO::identifyTailCall` 识别自尾调用，求值实参后抛 `TailCallSignal`；`callNamedFunction`（SelfFunction）与 `invokeMethod`（SelfMethod）的蹦床循环捕获信号后重建环境重新执行函数体，C++ 递归深度恒定。10 万层深尾递归 / 方法尾递归 / 互递归 / 闭包逐轮捕获均通过。
- **channel.recvTimeout(ms)**：四后端单点共享分发（[interpreter/BuiltinMethods.cpp](interpreter/BuiltinMethods.cpp) `handleSyncObjectMethod`），等待至多 ms 毫秒；有消息返回消息，超时或已关闭且无消息返回 null；参数校验：非整数 / 负数 / 超 60s 上限报错。`MAX_CHANNEL_TIMEOUT_MS = 60000`。
- **scripts/fix_stale_objs.bat**：定向删除四个核心 OBJECT 子库 obj + `minilang_core.lib`，自动修复 Ninja stale unity obj 链接失败（无需全量重建）。[scripts/build.bat](scripts/build.bat) / [scripts/run_tests.bat](scripts/run_tests.bat) 内置 LNK2019/2001/1120 自动清理 + 重试一次。
- **L18 Interpreter 状态快照与回滚**：为反向调试提供基础设施。`StateSnapshot` 捕获环境链 / 调用栈 / 注册表 / 模块状态 / 控制状态；`captureStateSnapshot` / `restoreFromSnapshot` 配套；`ExecutionTraceRecorder::TraceSnapshot` 新增 `interpreterState` 类型擦除字段。
- **L21 DAP `pause` 请求同步暂停**：`doContinue` 循环每 `kPausePollInterval=256` 步检查 `pauseRequested_` 标志 + 调用 `stdinPollCallback_` 探测 stdin。跨平台非阻塞探测：Windows `PeekNamedPipe` / `_kbhit`，Unix `poll(stdin, 0)`。

### Changed

- **GUI 层与引擎层解耦（ARCH-10 P1）**：新增 [common/MemoryInspectionAPI.h](common/MemoryInspectionAPI.h) / .cpp（`NaNBoxSnapshot` / `HeapObjectSnapshot` / `GcStatsSnapshot` 三个纯数据快照），消除 MemoryModelPanel / GcVisualizerPanel 对 `interpreter/NaNBox.h` / `RefCounted.h` / `GcManager.h` 的直接依赖。新增 [common/BackendExecutionService.h](common/BackendExecutionService.h) / .cpp 中间层，封装 `Lexer → Parser → Compiler → Backend` 完整流程，ProfileDashboardPanel / PerformanceRacePanel / BackendParallelPanel / BugHuntPanel / ExerciseGraderPanel / CoroutineVisualizerPanel / BackendComparePanel 等 7+ 面板迁移到此服务。
- **IdeController Facade 瘦身（ARCH-10 P2）**：新增 6 个子组件访问器（`pipelineRunner()` / `workerManager()` / `debugCoordinator()` / `vmStepper()` / `interpreter()` / `debugController()`），新面板优先通过访问器获取子组件，避免在 Facade 层堆砌纯透传方法；老面板透传 API 保持向后兼容。
- **Unity Build 默认可启用**：[cmake/minilang_core.cmake](cmake/minilang_core.cmake) 用 `SKIP_UNITY_BUILD_INCLUSION ON` 将含 `windows.h` 的 `CrashHandler.cpp` 独立编译，避免污染同 batch 的 `lexer/Token.h`；`minilang_frontend` / `minilang_backend` 子库 `AUTOMOC OFF`。冷构建加速 30-50%。
- **BytecodeCache 格式版本 1 → 2（AUDIT-R3 P2-4/P2-5）**：optFlags（irOptimize / irSSAOptimize 位图）入键，切优化配置后旧缓存失效；`moduleExports` 段无条件读写（移除 `remaining()>=4` 启发式）。

### Fixed

- **B1-Shadow：三 VM 路径 TCO 遮蔽误判导致死循环（P1）**：`fun f(n) { var f = helper; return f(n + 1); }` 局部变量遮蔽函数名，`TCO::identifyTailCall` 仅按名称匹配误判自递归，TCO 跳回自身入口形成死循环直至指令预算耗尽。修复为 `CompilerStmt.cpp` / `AstIRBuilder.cpp` 用 `currentLocals_` / `varMap_` 检查遮蔽，命中时回退普通调用。
- **AUDIT-R3 系列（VM / Interpreter / GcManager / IROptPasses / BytecodeCache / AstIRBuilder）**：
  - `push` / `pop` / `popN` 栈溢出 / 下溢改用不可捕获 `fatalError`——原 `runtimeError` 在活动 try 上下文转 `throwException` 改写 `frame.ip`，void 语义调用方随后 `ip += n` 使执行点错位。
  - `openUpvalues_` 悬垂清理改用 `lower_bound(newStackSize)` 定位尾部区间。
  - `OP_ADD/SUB/MUL_INT_SPEC` 类型特化路径补溢出检测，与通用路径 `computeArith` 的 IntOverflow 语义对齐。
  - finally 求值前保存并清除 `loopFlow_`——try 块内 break 后标志残留使 finally 块只执行第一条语句即中断。
  - 增量 GC 根集收集改为环境链全层遍历（含模块缓存环境 / REPL 暂存模块缓存）。
  - `markValue` / `collectCycle` 根遍历补 `VAL_COROUTINE`（args / currentValueBox / vmClosureBox）——挂起协程仅可达的循环容器原被误判不可达孤岛。
  - `loopUnroll` 回溯验证计数器初值为常量 0 + 排除体内嵌套控制流，拒绝非法展开。
  - `peekRef` 错误哨兵改 `thread_local` 并逐次重置——原函数级 static 可写哨兵跨 VM 实例共享，spawn 子线程构成写竞争。
  - `writeBackReceiver` 全局槽写回补 slot 范围校验。
  - `needsPopForExprStmt` 以节点类型枚举清单为准覆盖全部表达式语句（防再次遗漏栈泄漏）。
  - 生成器重放的调用栈收缩并入 RAII 守卫——原弹帧循环在 try/catch 之后，RuntimeError / ThrowException 逃逸时 `callStack_` 残留陈旧帧。
  - GC 触发回调新增 owner 令牌 + `clearGcTriggerCallbackIfOwner`——多 Interpreter 实例并存时先构造者析构不再误清存活实例的回调。
  - 模块路径规范化三处同步循环剥 `./` + 折叠 `/./`。
- **AUDIT-R2 系列（教学执行子系统）**：BugHuntPanel `QButtonGroup` id 显式分配、`verifying_` 守卫 RAII 化、变体索引双侧上界检查、递进提示 ≥3 条不变量扩展到全题库；BackendExecutionService 不再 reset 全局 GcManager 单例（保护并发 worker）、微秒精度耗时、三后端计时基线不含编译时间。
- **构建阻塞**：[interpreter/GcManager.cpp](interpreter/GcManager.cpp) `markValue` VAL_COROUTINE case C4457 变量遮蔽（循环变量 `v` 遮蔽参数 `const Value& v`）；stale unity obj 链接失败（LNK2019 `setGcTriggerCallback` 签名变更后未重编）；`LoopUnrollVregRenamingCorrectness` 测试适配 P1-9 计数器初值验证收紧。
- **spawn 异常传播三后端不一致（P3-A1）**：spawn join 延迟执行模式下，闭包内 throw 触发的 `throwException` 弹出闭包帧并穿透 `invokeClosureSync` 边界，破坏"调用前后 frames_/stack_/ip 不变"不变量——StackVM `invokeClosureSync` 末尾 `result = pop()` 误吞 thrownValue，RegisterVM `result = reg(dstReg)` 误读 savedReg0。修复为两后端在调用前后比较 `tryStack_.size()`，缩小时返回 `VM_EXCEPTION_THROW` 不操作 ip/栈/寄存器。
- **文档过时项清理（实证后更新）**：A1 匿名函数（faq.md Q5 / language-comparison.md 从"不支持"更正为 R98 实现现状）、A2 多行字符串（faq.md Q6 更正为"普通双引号字面量允许跨行"）、C1 V-P1-6（TestJIT R161Closure* 陈旧"暂时禁用"注释）、B1（testing.md 已知限制 #10 标记已修复）。

[v1.3.0]: https://github.com/MiniLang/ide/releases/tag/v1.3.0
[v1.2.0]: https://github.com/MiniLang/ide/releases/tag/v1.2.0
[v1.1.0]: https://github.com/MiniLang/ide/releases/tag/v1.1.0
