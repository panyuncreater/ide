# Changelog

本文件记录 MiniLang IDE 的版本演进，遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 规范，分类说明每版本对用户可见的变更：**Added**（新增功能）/ **Changed**（对已有功能的修改）/ **Deprecated**（即将移除）/ **Removed**（已移除）/ **Fixed**（缺陷修复）/ **Security**（安全相关）。

条目以版本快照粒度组织，不重复 git log。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)，以 [CMakeLists.txt](CMakeLists.txt) `project(MiniLangIDE VERSION ...)` 为基线。

## [Unreleased]（2026-10-03）

### Added

- **IR lowering 栈平衡校验器（D1 第一步）**：[compiler/ir/BytecodeIRBackend.cpp](compiler/ir/BytecodeIRBackend.cpp) 新增 debug 构建专用校验（`#ifndef NDEBUG`，Release 零开销）——每条 IROp 的 (pops, pushes) 效应表（`irStackEffect`，逐条对齐 StackVM 运行时实现）+ `lower()` 指令循环内的操作数栈深度模拟（`checkStackBalance`），替代"发现一个补一个"的人工保证模式（对应 2026-08-03 审计报告 D1 的第一步：先校验后重构）。失败分级：负深度（弹出多于可用，运行时必然错位）拒绝 lowering；LABEL 合并点深度冲突报告但不拒绝（兼容 AUDIT-R6 "break 丢弃挂起 return 值"的合法不平衡——运行时由 OP_RETURN 帧回收清理）。peek 型指令（STORE_LOCAL/JUMP_IF_FALSE/TYPE_CHECK 不消费栈顶）与 catch 标签深度协议（TRY_BEGIN 高度 + 1）已正确建模；`MINILANG_IR_STACK_DEBUG=1` 可输出逐指令栈深轨迹。回归测试 `AuditBatch1IRBalance.*` 5 用例。**校验器落地过程中顺带捕获并修复一个真实 lowering 缺陷**：顶层/嵌套普通函数中的 `super.method()` 写回链多发一个 POP（BUG-INH-4 的条件对齐 isVarRef 分支遗漏——`this` 回退到 GLOBAL 时 OP_SET_GLOBAL 已 pop），此前被"未定义的变量： this"运行时错误掩盖，见 Fixed 条目。

- **审计剩余项复核与测试补强（D3/D4/D7/D8/D15 复核）**：三路并行复核 2026-08-03 审计报告剩余项的现状——D3 条件断点三个子问题（入口快照未初始化/沙箱标志跨求值/D1 求值缓存与 slot 失配）均已被既有修复覆盖或属误报（SandboxGuard 16 类状态 RAII 恢复、VM 路径每求值全新临时 Interpreter、缓存为按条件文本键控的 AST 而非 BytecodeChunk）；D4 的"4 处 FIXME + 1 处 TODO"不存在（`genericXXX` 标识符被 XXX 子串扫描误命中，审计报告已加勘误注记），NaN-boxing tag 检查与编码严格一致。落地补强：
  - **D15**：astEqual AST 等价比较器自 test_harness 提升进常规回归（[tests/TestFormatterRoundTrip.cpp](../tests/TestFormatterRoundTrip.cpp)），samples/mini 全部 .mini 样例数据驱动「parse → format → reparse → AST 等价 + 二次格式化幂等」，另含右/左嵌套括号、成员链、try/catch/finally+闭包等内联边界用例。
  - **D7**：闭包经数组/字典索引调用的 capturedVars 写回持久性回归锁（此前仅覆盖读取路径）；多闭包快照语义（对齐 R2）锁定为 "1102" 四后端一致。
  - **D8**：catch 块内 var 重声明 catch 变量的三后端语义锁（含顶层遮蔽 + DELETE_VAR 清理交互、重声明后 rethrow）。
  - **D4**：`verifyNanBoxConstants()` 接入 `JITBackend::execute` debug 调用点（原实现无任何调用点，注释声称"首次 execute 时校验"但从未接线）。
  - **D10**：gui/ 全部手工 `blockSignals(true/false)` 配对（4 文件 8 处）替换为 `QSignalBlocker` RAII；`Interpreter::stopRequested_` 协作中止标志 relaxed → release/acquire（GUI 线程写 / Worker 线程读建立同步关系）。
  - **W4 警告豁免全量退役（审计问题 5 闭环）**：CMakeLists.txt 的 gui/ 五类豁免（C4456/C4458/C4189/C4100/C4996）全部移除——C4189/C4100/C4996 复核为零警告（原豁免为陈旧保护），C4456/C4458 遮蔽 38 处集中于 AstVisualizerPanel.cpp 的 else-if 链条件声明与循环变量，逐处重命名清偿；gui/ 全部编译单元现与 core 同处 /W4 + /WX 零豁免基线。
  - **BaseTeachingPanel 面板基类（审计问题 3 第一步）**：新增 [gui/TeachingPanelBase.h](../gui/TeachingPanelBase.h)/.cpp——主题切换接线收敛为 `applyTheme()` 虚钩子（原 6 处 `Theme::onThemeModeChanged` 手工 lambda 样板），CodeJourneyInfoPanel / BreakpointConditionPanel / IRTransformPanel 三个面板先行迁移验证，其余面板按家族分批迁移（迁移路径见基类头注释）。
  - **Fuzz 语料扩充**：[tests/TestThreeEnginesFuzz.cpp](../tests/TestThreeEnginesFuzz.cpp) 新增 3 个生成器类别（每次运行 100 个随机程序，固定种子可复现）——异常处理（有条件 throw/catch 变量/finally/嵌套/重抛链，对齐已知 Bug 模式 #5）、继承与 super（super.init/方法动态派发/字段查找链，模式 #9）、字典与 COW（索引读写/副本写别名隔离，模式 #1-4）。
  - **MSVC ASan CI 门禁提案**：新增 [docs/ci-msvc-asan-proposal.md](ci-msvc-asan-proposal.md)——Windows MSVC 侧 `/fsanitize=address` 阻断式测试 job 的可直接合入定义（现有 ASan 门禁仅覆盖 Linux，主力开发平台 Windows 存在内存错误检测盲区；平台差异风险已有 v1.3.0 SysV ABI P0 先例）。CI 配置属只读范围，故以提案形式交付；job 命令已在本地 ASan 构建树验证。

### Changed

- **D1 第二步（表驱动 + 文件拆分）**：栈效应知识迁移为数据表——[compiler/ir/BytecodeIRBackendStackCheck.cpp](../compiler/ir/BytecodeIRBackendStackCheck.cpp)（自 BytecodeIRBackend.cpp 拆分，1929→1493+456 行）承载 `kFixedStackEffect` constexpr 效应表（64 条固定效应）+ 10 条公式型分支，`static_assert(tableIsComplete())` 在编译期强制全部 IROp 枚举值登记（新增 IROp 未补表即编译失败，关闭"静默跳过校验"缺口）；PHI 等不可 lower 操作的未登记路径由保守跳过改为显式拒绝；新增 `AuditBatch1IRBalance.EffectTableCoversAllIROps` / `PhiInstructionRejectedByLower` 测试。
- **compiler/ 子目录拆分（审计问题 1 落地）**：`compiler/` 41 个文件按职责迁入五个子目录——`core/`（Compiler 全家族、Bytecode、BytecodeCache、ExprStmtPop、GlobalSlotAllocator 共享基础设施）、`ir/`（IR、IRSSA、IROptPasses、AstIRBuilder、BytecodeIRBackend + 栈平衡校验器）、`backend-stack/`（VM、VMCalls、VMContainers）、`backend-reg/`（RegisterBytecode、RegisterBytecodeBackend、RegisterVM 全家族）、`jit/`（JIT 全家族，含 JITA64）。仓库内 include 均为根相对路径，批量更新为 `compiler/<子目录>/<文件>.h`；`cmake/minilang_core.cmake` 源列表同步；`docs/changelog/archive/` 历史条目保持时点原貌不回写。4109 用例全量回归验证通过。

### Fixed

- **IR 路径顶层/嵌套函数 super 调用栈不平衡（D1 校验器捕获）**：[compiler/ir/AstIRBuilder.cpp](compiler/ir/AstIRBuilder.cpp) `emitMethodCallWriteback` 的 super 分支无条件补 `POP` 消费 `LOAD_MUTATED` 残留（BUG-INH-4 注释假设 `this` 是 LOCAL 槽 0），但非方法上下文（顶层、嵌套普通函数）中 `resolveVar("this")` 回退到 GLOBAL——`OP_SET_GLOBAL` 为 pop 语义，多发一个 `POP` 使 `var x = super.foo()` 的 `DEFINE_GLOBAL` 欠栈。此前被"未定义的变量： this"运行时错误先触发而掩盖（静态字节码不平衡从未被执行到）。修复为条件对齐 isVarRef 分支：仅 LOCAL/UPVALUE（peek 语义）补 `POP`。方法体内合法 super 调用的发射序列不变。
- **Environment 弱指针悬挂风险收口（D2）**：`Value::makeClosure` 工厂内统一执行 `env->markClosureEnvRef()`（[interpreter/Value.cpp](interpreter/Value.cpp)，实现随 D2 fix 移出 Value.h——Environment 仅有前向声明，避免循环包含）。原先该标记由 Interpreter 唯一闭包创建点手工调用（AUDIT-BUG-I1），属"人工保证"不变量：新增创建点漏标即静默语义损坏（envPool_ 回收复用后 `resetForReuse` 清空闭包 weak_ptr 仍指向的 variables）。收口后不变量结构性成立。回归测试 `AuditBatch1ClosureEnv.*` 3 用例（含 >64 次 env 池回收压力下的循环闭包行为回归）。
- **TeachingPanelBase 双布局冲突隐患（审计问题 3 迁移修正）**：基类构造函数原先无条件 `new QVBoxLayout(this)`，而已迁移的三个面板构造函数仍自建顶层布局——QWidget 仅允许一个顶层布局，子类二次创建会被 Qt 拒绝（运行时告警 + 子控件失去布局管理，面板显示错乱；此前的编译/测试验证未覆盖 GUI 运行时布局路径）。修复为 `rootLayout()` 惰性创建/采纳：子类已自建布局则直接采纳，否则才创建标准零边距容器。同步迁移调试家族三面板（DebugPanel / WatchPanel / WatchpointPanel）到基类：主题切换接线收敛为 `applyTheme()` override，gui/ 内手工 `Theme::onThemeModeChanged` 样板清零（审计问题 3 按家族迁移第二步）。
- **JIT 异常分派 use-after-pop 容器溢出（MSVC ASan 门禁首捕获）**：[compiler/jit/JITRuntime.cpp](compiler/jit/JITRuntime.cpp) `jitThrow` 命中 handler 后先 `tryStack_.pop_back()` 销毁栈顶 `JitTryHandler`，再经 `back()` 引用读 `handler.catchAddr` 返回——MSVC STL 容器注解将 [size, capacity) 标记为毒化区，ASan 以 container-overflow 拦截（TestJIT.R162\* 12 项 + L14RuntimeErrorCatch.\* 6 项；非插桩构建因旧槽位尚未被覆写而侥幸通过，属潜伏 UB）。修复为 pop 前按值复制 `catchAddr`；修复后 R162/L14 全家族 26 项在 ASan 下零报告，非插桩全量回归 4109/4109 通过。缺陷由本次落地的本地 MSVC ASan 验证流程首次捕获（CI 合入提案见 [docs/ci-msvc-asan-proposal.md](ci-msvc-asan-proposal.md)）。
- **Windows 增量构建依赖跟踪失效根治（debug 构建树）**：`out/build/debug` 编译器检测缓存（`CMakeFiles/<版本>-msvc1/CMake{C,CXX}Compiler.cmake`）存有历史乱码 `msvc_deps_prefix`（代码页不一致的 configure 遗留），导致每次 reconfigure 都把乱码写回 `rules.ninja`、ninja 依赖解析丢失（改头文件不重编），此前只能每次手工注入 GBK 字节。对照实验确认真因后（正常 936 代码页下全新 configure 的探测与生成本就正确、VSLANG=1033 因未装英文语言包无效），将缓存中的前缀修正为规范 UTF-8 文本——此后 regen 自动产出与构建期 cl（GBK）匹配的前缀，**无需任何手工注入**。端到端验证：`ninja -t deps` 记录有效、touch 头文件正确触发重编、全量 4109/4109 通过。排查指引已更新至 [docs/getting-started.md](getting-started.md)（原"修正 rules.ninja"指引属治标，已标注）
- **构建入口脚本在中文控制台下失效修复（`scripts/`，经用户授权的一次性修改）**：`_common.bat` / `configure.bat` / `build.bat` / `run_tests.bat` 内的 UTF-8 中文注释在默认 GBK（代码页 936）控制台下被 cmd 逐行误解析为命令（REM 行同样会被分词，全角标点的 UTF-8 字节按 GBK 配对错位后暴露命令碎片，如 `'锛孧SVC' 不是内部或外部命令`），导致 configure.bat 在运行 cmake 之前即异常退出（实测日志止于环境检测 INFO、exit 1）。修复：全部 .bat 注释转写为 ASCII 英文（逻辑零改动，对任意控制台代码页免疫），顺修 configure.bat 的 `>/dev/null` 重定向（cmd 无此语义，改 `>nul`——此前重定向失败会令 windows-msvc-\* 预设的 QTDIR 校验被静默跳过）；另为 5 个含中文且无 BOM 的 .ps1 补 UTF-8 BOM（Windows PowerShell 5.1 无 BOM 时按 ANSI 误读）。端到端验证：configure.bat 完整跑通 cmake（exit 0、零乱码），reconfigure 后 deps 前缀保持正确 GBK，build.bat / run_tests.bat 全流程通过（4109/4109）。

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

- **JIT SysV ABI 违规（P0，Linux SIGSEGV）**：[compiler/jit/JITCodeGen.cpp](compiler/jit/JITCodeGen.cpp) / [compiler/jit/JITCodeGenHelpers.cpp](compiler/jit/JITCodeGenHelpers.cpp) 中 6 处运行时回调硬编码 Win64 参数寄存器 rcx/rdx，Linux SysV ABI 应为 rdi/rsi，导致 ctx 指针传入垃圾值，TestJIT OSR/Deopt 16 项测试 SIGSEGV。按平台 `#ifdef` 选择参数寄存器修复。
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
