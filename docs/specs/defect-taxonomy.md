# MiniLang 缺陷分类学（Spec）

> 本文档是“经验”的唯一权威：每类缺陷只保留**触发面 → 不变量 → 审计入口 → 锁定测试**，不写轮次叙事。
> 审计前先按本表对号入座；新增一类须同时补一个可执行的检查（回归测试、编译期断言或脚本），否则不得入表。
> 语义细则见 `docs/specs/backend-consistency.md`；开放项见 `docs/BACKLOG.md`。

## K01 IR 路径栈不平衡

- **触发**：新增 IROp 或 emit 分支；表达式语句返回值未 POP；super 写回链条件分支。
- **不变量**：lowering 后操作数栈深度平衡；每条 IROp 的 (pops, pushes) 只有效应表一个事实源。
- **入口**：`compiler/ir/BytecodeIRBackend.cpp` 各 `visit*` 收尾、`compiler/ir/BytecodeIRBackendStackCheck.cpp`。
- **锁定**：`AuditBatch1IRBalance.*`、`EffectTableCoversAllIROps`、`PhiInstructionRejectedByLower`；新增 IROp 未补表即编译失败。

## K02 四后端语义不一致

- **触发**：错误消息文本、求值顺序、类型校验时机、边界条件（空/负数/NaN/深嵌套）。
- **不变量**：Interpreter / StackVM / RegisterVM 同语义；JIT 已支持场景与 StackVM 严格一致。
- **入口**：`tests/TestThreeEnginesConsistency.cpp`（6 组合矩阵）、`test_harness/diff_test.cpp`（含非 IR 直译口径）、`docs/specs/backend-consistency.md` §2。
- **锁定**：`ConsistencyDiff.*`、`VME2E*`、`TestThreeEnginesFuzz`。

## K03 闭包 / upvalue 生命周期

- **触发**：多层嵌套捕获、作用域关闭后写回、挂起窗口逃逸闭包、共享闭包 COW 克隆。
- **不变量**：关闭 = 快照（三后端一致）；挂起时开放 upvalue 快照式关闭，恢复时先写回再重挂；调用点读类型参数必须走 const 访问器，避免 `ensureUnique` 克隆丢写回。
- **入口**：`interpreter/Value.h` 的 `Upvalue`/`ClosureData`、`Compiler`/`AstIRBuilder` 闭包发射点。
- **锁定**：`AuditBatch6ClosureWriteback.*`、`ConsistencyDiff.AuditUpvalue_R2_*`、`CoroutineSuspension.SuspendedWindowClosureUpvalueSemantics`。

## K04 模块系统

- **触发**：路径规范化/大小写、循环依赖、预扫描遗漏（FunDecl/ExportStmt）、可变导出快照、预编译模块重定位。
- **不变量**：缓存去重键与 loader 请求路径分离；变量导出按快照、函数/类按名称共享；循环导入部分加载语义四路径一致。
- **入口**：`interpreter/InterpreterModules.cpp`、`compiler/core/Compiler.cpp` 的 `visitImportStmt`、`AstIRBuilder::handleImportStmt`、`common/ModulePath.h`。
- **锁定**：`VME2EImportSnapshot.*`、`VME2EImport.CaseInsensitiveDedupOnWindows`、循环导入四后端用例。

## K05 try / catch / finally

- **触发**：catch 变量作用域泄漏、栈残留、finally 内 break/continue/return、待处理异常覆盖、跨帧续跳目标。
- **不变量**：finally 的控制流转移丢弃待处理异常（Java 语义）；`pendingJumpStack_` 条目按帧归属，只消费本帧。
- **入口**：`emitCatchBlock`/`emitFinallyBlock`、StackVM/RegisterVM 的 `tryStack_`/`pendingJumpStack_`、解释器 `runFinallyBlock`。
- **锁定**：`AuditR6FinallyAbrupt`、`AuditR7Regression.AbruptReplaceAbruptNoStaleJumpTarget`、`CalleeFinallyDoesNotStealCallerJumpTarget`。

## K06 条件断点 / 调试器

- **触发**：断点对象持引用失效、沙箱状态未恢复、条件求值缓存与 slot 布局失配、watchpoint 三路径缺失。
- **不变量**：任何求值沙箱必须 RAII 恢复全部状态；调试器缓存不得跨 slot 布局复用。
- **入口**：`debug/DebugController`、`app/DebugCoordinator`、`app/VmStepper`、`common/ConstFunEval.h`。
- **锁定**：`VMConditionalBreakpoint*`、`TestWatchpoint`、`TestDebuggerReplStage2`。

## K07 REPL

- **触发**：续行判断、模块缓存刷新、异步执行超时、跨行状态恢复、跨行协程恢复。
- **不变量**：REPL 会话的执行线程与状态生命周期必须单一；跨行 `.next()` 不得跨线程。
- **入口**：`gui/ReplPanel`、`app/IdeController` 的 `saveReplState`/`restoreReplState`。
- **锁定**：REPL 阶段测试；跨行协程为开放项 `B-004`。

## K08 Formatter 往返等价

- **触发**：缩进双重计算、括号保留规则、注释归属、AST 结构不等价。
- **不变量**：parse → format → reparse 的 AST 等价，二次格式化幂等。
- **入口**：`formatter/`、`test_harness` 的 `astEqual`。
- **锁定**：`TestFormatterRoundTrip`（samples/mini 数据驱动 + 内联边界）。

## K09 类继承

- **触发**：字段默认值丢失、super 方法查找、方法回退链、`fieldSlotIndex` 写回、构造路径参数校验时序。
- **不变量**：继承链字段布局与方法查找在四路径一致；构造参数校验必须在类型参数合并之后。
- **入口**：`visitClassDecl`/`constructClassInstance`/`setupInitEnvironment`、`emitMethodCallWriteback`。
- **锁定**：`AuditBatch1Super.*`、继承/构造泛型回归。

## K10 GUI / 线程安全

- **触发**：Worker 状态清理不完整、运行期 UI 操作未禁用、原子标志竞态、面板无运行守卫、跨线程析构。
- **不变量**：引擎状态只允许单一执行线程访问；面板在 `isRunning()/isVmRunning()` 时拒绝重入；跨线程析构只允许弃置 fiber，不得跨线程 DeleteFiber。
- **入口**：`app/WorkerManager`、`app/IdeController`、七个执行面板、`gui/ReplPanel`。
- **锁定**：`GcIsolation.*`、`CoroutineSuspension.CrossThreadParkedFiberDestruction`、GUI smoke。

## K11 泛型注解擦除

- **触发**：新增注解形态（`T[]`/`T?`/`dict[..T..]`/`fun(T):T`）、构造路径 guard 时序、显式类型实参、JIT 原生 helper。
- **不变量**：擦除点共七处；判定统一用 `annotationReferencesTypeParam` 递归判定；JIT 通道单独回归。
- **入口**：`common/TypeChecker.h`、`Interpreter::checkType`/`visitEnumVariantExpr`、`Compiler::emitTypeCheck`、`AstIRBuilder::emitTypeCheckIR`、两 VM 的 enum 载荷校验、`jitBuildEnumVariant`。
- **锁定**：`GenericErasureCompletion.*`（含 runJIT 通道）。

## K12 作用域遮蔽与块内 fresh

- **触发**：顶层块内新声明、祖先块遮蔽、catch 变量遮蔽、解构绑定、块退出清理；块内 fresh 全局名被塞入局部槽映射（`-1` 哨兵）后误当槽位 255（B-010）。
- **不变量**：直译路径与 IR 路径必须同规格登记 fresh/shadow，块退出 `DELETE_VAR` 清理或保存/恢复；嵌套块重声明走保存/恢复；`currentLocals_` 只允许真实局部槽（≥0），块 fresh 全局名由独立集合跟踪（直译 `blockScopedGlobals_` / IR `topLevelBlockFreshGlobals_`）。
- **入口**：`Compiler::visitBlock`/`visitVarDecl`/`bindDestructureVar`、`AstIRBuilder::visitBlock`/`visitDestructureBinding`、`shadowedGlobalNames_`/`blockScopedGlobals_`/`topLevelBlockFreshGlobals_`。
- **锁定**：`ConsistencyDiff.BlockFresh*`、`CatchVarShadowNestedBlockRestored`、`BlockFreshDestructureScopeEndsAtBrace`（四路锁定）、`BacklogRegression.B010*`（五后端）。

## K13 协程挂起与 GC 根

- **触发**：快照不在根集、绝对索引未相对化、最外层帧 `returnIp` 未重定基、嵌套挂起上下文、失败态重抛、TCO 上下文跨挂起、跨线程恢复/析构。
- **不变量**：ADR-007 语义表 + `docs/specs/backend-consistency.md` §9 全部条款；快照内不携带绝对基线；挂起期间快照必须贡献根。
- **入口**：`interpreter/InterpreterCoroutine.cpp`、`compiler/backend-stack/VMCalls.cpp`、`compiler/backend-reg/RegisterVMCalls.cpp`、`CoroutineData`。
- **锁定**：`CoroutineSuspension.*`、`CoroutineTest`、`AsyncAwait`。

## K14 JIT 缓存与所有权

- **触发**：缓存键漏字段、codegen 变更未递增版本、共享块 owned 标志、同键重复 store、只读 helper 触发 COW；member-get PIC 缓存 `InstanceData*` 地址后实例被覆盖/删除释放（地址复用假命中）；槽位裸 bits 覆盖泄漏；无预分配槽全局名被编译期拒绝（B-010/B-013）。
- **不变量**：键覆盖全部影响机器码的声明；`kJitCodegenVersion` 递增纪律（当前 5）；缓存共享块实例 `owned=false`；只读 helper 接收者 const；执行期堆值零释放（槽位覆盖/删除/写回经 `retiredValues_` 延迟到下次 execute，PIC 存活期实例不得释放）；name-based 全局写回必须运行时解析。
- **入口**：`compiler/jit/BlockCache`、`compiler/jit/JIT.cpp`、`compiler/jit/JITRuntime.cpp`、`JITBackend::retiredValues_`/`storeJitSlotBits`/`emitStoreGlobalOwned`/`jitResolveGlobalPtr`。
- **锁定**：`BlockCache*`、`TestJIT*`、`TestJITCoverageGaps`、`BacklogRegression.B010*`/`B012*`/`B013*`。

## K15 宿主嵌入生命周期

- **触发**：临时 Interpreter 覆盖 GC 回调、面板并发执行、编译期/调试沙箱与常驻解释器并存、跨线程 Value 析构。
- **不变量**：短生命周期 Interpreter 必须 `GcTriggerPolicy::Isolated`；Isolated 生命周期内不得构造 Managed 实例；面板入口必须运行守卫。
- **入口**：`common/BackendExecutionService`、`app/IdeController` 四沙箱、`common/ConstFunEval.h`、`cli/dap_core.cpp`、`gui/ReplPanel`。
- **锁定**：`GcIsolation.*`、`CoroutineSuspension.CrossThreadParkedFiberDestruction`。
