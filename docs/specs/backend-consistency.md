# MiniLang 多后端一致性约定（Spec）

> 本文档定义**新增/修改语言特性时**的三后端同步强制 checklist 与语义细则对照。
> 不变量本身见 `AGENTS.md` §3；验证方法论与套件矩阵见 `docs/testing.md`；设计依据见 `docs/adr/ADR-002-triple-backend.md`。本文档只写「做什么」，不重复「怎么做验证」。

## 1. 核心不变量

- 同一 MiniLang 源码在 **Interpreter / StackVM / RegisterVM** 三条路径必须产生相同语义结果。
- **JIT**（x86-64，`MINILANG_USE_JIT` 默认 ON）对已支持场景与 StackVM 严格一致；未支持场景**优雅降级**（不崩溃、不错值）。
- IR 路径与非 IR 路径共享 VM，中间表示不同（BytecodeChunk vs RegBytecodeChunk），修改 IR 后端时必须同时核对两条 IR 出口（`BytecodeIRBackend` / `RegisterBytecodeBackend`）。

## 2. 语义细则对照表（三后端必须逐项一致）

| 语义 | 强制行为 | 边界关注 |
|------|---------|---------|
| 整数除法 | 截断向零 | 负数、`-0`、除零错误路径 |
| `and` / `or` | 短路且**返回操作数原值**（非布尔） | 非布尔操作数、链式短路 |
| 类型注解 | 强制校验，注解不匹配必须报错 | 运行时注解 vs 编译期注解行为一致 |
| `super` 调用 | 方法查找走父类链 | 多级继承、字段默认值、fieldSlotIndex |
| 运算符重载 | dunder 分派（`__add__` 等） | 缺失回退、内建类型 vs 用户类型 |
| `?` 错误传播 | 错误沿调用链传播 | 嵌套调用、finally 交互 |
| `async`/`await` | 调度语义一致 | 并发交错、异常跨 await |
| 闭包捕获 | upvalue 快照/共享语义三后端一致 | 多闭包共享变量、逃逸后写入（见 `docs/testing.md` AUDIT-R5 R2 快照案例） |
| try/catch | catch 变量作用域隔离、栈残留清零 | 嵌套 finally、break/return 穿 finally |
| 闭包索引调用写回 | 经数组/字典索引调用的闭包对 capturedVars 的变异跨调用持久 | `AuditBatch6ClosureWriteback.Array/DictIndexCallMutationPersists` |
| 多闭包捕获语义 | **快照语义**（非 JS/Lua 共享单元格）：作用域关闭后各闭包持有独立 capturedVars | `AuditBatch6ClosureWriteback.SharedCaptureViaIndexCallsSnapshotSemantics`、R2 回归锁 |
| catch 变量重声明 | catch 块内 `var` 重声明 catch 变量名：块内可见、全局原值不受污染、重声明后可再 throw | `AuditBatch6CatchRedecl.*` |
| super 非方法上下文 | 顶层/嵌套普通函数中 `super` 为**运行时错误**（"未定义的变量: this"），不崩溃不静默 | `AuditBatch1Super.TopLevelSuperRuntimeError`、`ConsistencyDiff.H7b` |
| 无限循环防护 | 循环回边注入迭代计数，超限报错（对齐 Interpreter） | R166、`compiler/jit/JIT.cpp` OP_LOOP safepoint |
| 生成器挂起语义 | `MINILANG_CORO_FIBER=1` 平台三执行后端真挂起（副作用只执行一次），`=0` 统一重放；yield 表达式恢复值 = 自身 yield 值，最终 yield 即 done | 单边启用禁令、快照相对化/returnIp 重定基/嵌套上下文 checklist、`CoroutineSuspension.*` 锁定测试——见 §9 与 [ADR-007](../adr/ADR-007-coroutine-suspension.md) |

> 表中每项的新增/修改都必须在三后端同步实现（含 IR 双出口），并以一致性测试锁定（§5）。

## 3. 新增语言特性：三后端同步 checklist

1. **语义定义先行**：在本文档 §2 或对应 ADR 中明确目标行为（含边界），三后端以此为唯一依据，禁止各自解释。
2. **三后端实现核对**：Interpreter（`interpreter/`）→ StackVM（`compiler/core/Compiler.cpp` + `compiler/backend-stack/VM.cpp`）→ RegisterVM（`compiler/backend-reg/RegisterBytecodeBackend.cpp` + `compiler/backend-reg/RegisterVM.cpp`），逐个确认实现存在且行为一致。
3. **IR 双出口核对**：若特性走 IR 路径（`AstIRBuilder` → IRModule），核对 `BytecodeIRBackend` 与 `RegisterBytecodeBackend` 两个出口都正确 emit。
4. **JIT 判定**：明确特性在 JIT 的支持状态（见 §4），锁定「支持=一致」或「降级=不崩溃不错值」。
5. **一致性测试**：按 §5 规则补测试，覆盖三后端 × 边界值。
6. **文档同步**：更新 `docs/testing.md` 特性覆盖表与本文档 §2 对照表。

## 4. JIT 支持判定与降级断言

- **判定表**：新特性按「JIT 已支持场景清单」（`tests/TestJIT.cpp`、`TestJITCoverageGaps.cpp` 14 用例锁定：闭包、模块系统、并发原语、字符串方法、enum variant 等当前为「StackVM 正确 + JIT 优雅降级」）。
- **降级断言写法**：已支持场景断言「与 StackVM 严格一致」；未支持场景断言「不崩溃、不错值」（运行结果正确或显式降级信号），**不得断言 JIT 支持未实现特性**。
- JIT 补全实现后，`TestJITCoverageGaps` 对应断言自动收紧为一致性断言（该文件设计意图即「缺口清单即降级断言清单」）。
- 涉及 JIT 上下文/邮箱模式（`compiler/JIT*.cpp`：`JitContext` 持有 `r12` 上下文指针）的改动必须同步验证 JIT 与 VM 停靠/反优化路径。

## 5. 一致性测试编写规则

1. **矩阵覆盖**：每个语义最少覆盖三后端 × 边界值（0、负数、空容器、NAN/Infinity、深嵌套、超长字符串）。
2. **命名**：三后端差分 → `ConsistencyDiff.*` / `TestThreeEngines*`；VM 端到端 → `VME2E*`；JIT → `TestJIT*`（见 `docs/specs/testing-conventions.md`）。
3. **锁定回归**：修复一致性缺陷必须携带能捕获旧缺陷的回归测试（改回 Bug 代码应红）。
4. **组合开关**：RegisterVM 路径测试覆盖 `setVM(true)` + `setUseRegisterVM(true)` + `setIROptimize(true)` 组合（`docs/testing.md` L19-20）；JIT 路径用 `MINILANG_USE_JIT=ON` 构建（x86-64 默认）。

## 6. 单边修复禁令

- **禁止**仅修改单条路径来"对齐"测试：三后端行为差异必须定位到语义根源，同步修正所有路径。
- 历史教训：upvalue 快照语义曾出现「Interpreter 快照 vs VM 共享」的误判，实测证明三后端本就一致，单边修改反破坏一致性（`docs/testing.md` AUDIT-R5 R2，由 `ConsistencyDiff.AuditUpvalue_R2_MultiClosureSharedCounterAfterClose` 回归锁定）。若需对齐主流语言（Lua/JS 共享语义），必须三后端同步引入架构级改动（共享 upvalue cell）。

## 7. IR lowering 栈平衡不变量（D1 校验器，2026-10-03 落地）

- **不变量**：IR 指令流在 lowering 后的操作数栈深度必须平衡——每条 IROp 的 (pops, pushes) 由效应知识单一来源描述（D1 第二步表驱动：`compiler/ir/BytecodeIRBackendStackCheck.cpp` 的 `kFixedStackEffect` 数据表 + 公式型分支），编译期 `static_assert(tableIsComplete())` 强制全部枚举值登记（新增 IROp 未补表/补公式即编译失败）。
- **校验分级**（`checkStackBalance`，debug 构建启用，Release 零开销）：
  - 负深度（弹出多于可用）→ **拒绝 lowering**（运行时必然 peek/pop 错位）；
  - LABEL 合并点深度冲突 → **报告但不拒绝**——已知合法形态为 AUDIT-R6 "break 丢弃挂起 return 值"（try 内 return 压栈后 finally 中 break 直接跳出），运行时由 OP_RETURN 帧回收清理；
  - 未登记效应（PHI 等）→ **拒绝 lowering**。
- **peek 型语义易错点**：`STORE_LOCAL`/`STORE_UPVALUE`/`JUMP_IF_FALSE`/`TYPE_CHECK` 不消费栈顶（值留栈，由后续显式 POP 消费）；六条 `WRITEBACK_*` 零操作数栈效应；catch 入口栈高 = TRY_BEGIN 时高 + 1（异常值）。
- **诊断**：`MINILANG_IR_STACK_DEBUG=1` 输出逐指令栈深轨迹。
- **字节码截断校验（2026-10-04 上移）**：两 VM 的字节码截断检查在 `initExecution` 加载期一次性预扫描完成——`VM::initExecution` 校验主 chunk 与全部 `functionChunks_`（`validateChunkInstructionBoundaries`），`RegisterVM::initExecution` 同构（`validateRegChunkInstructionBoundaries`，基于 `RegBytecodeChunk::instructionSizeAt`）。主循环 release 不再逐指令做 `instructionSize`/`instructionSizeAt` 越界检查，debug 保留逐指令校验双保险。**不变量**：新增变长指令时，预扫描的展开逻辑（栈式 VM 内联 OP_CLOSURE 展开 / 寄存器 VM `instructionSizeAt`）与主循环消费的长度必须保持同一事实源，否则加载期校验与运行时推进错位。
- 该校验器落地即捕获一处真实缺陷（顶层 super 写回多发 POP，见 CHANGELOG 2026-10-03）。

## 8. JIT 语义判定补充（2026-10-03 复核）

- JIT 硬编码 NaN-boxing tag 常量与 `NaNBox.h` 编码的一致性由 `verifyNanBoxConstants()` 在 `JITBackend::execute` debug 入口校验（含 `JitContext` 30 个字段偏移的 static_assert 编译期护栏）。
- JIT 遇不支持场景为 **fail-fast**（整体编译失败显式报错，无静默降级继续执行）；`genericXXX` 路径为运行时类型分发兜底，与审计报告 D4 中"FIXME/TODO 标记"无关（该说法为 `XXX` 子串误扫，勘误见报告内注）。

## 9. 协程真挂起一致性（2026-10-05 落地，ADR-007）

生成器 `.next()` / `await` 的执行模型由重放模式升级为真挂起模式（yield 保存执行位置切回调用方，下次恢复），**三执行后端由同一宏 `MINILANG_CORO_FIBER`（[interpreter/CoroutineFiber.h](../../interpreter/CoroutineFiber.h)）门控同步启用**——设计依据与实现细节见 [ADR-007](../adr/ADR-007-coroutine-suspension.md)，本节只写 checklist。

- **单边启用禁令**：真挂起/重放是平台级整体开关，禁止单后端先行切换（副作用执行次数会成为跨后端分歧点）。新增第四执行路径（如未来 WASM 后端）时必须同步实现挂起或显式回退重放。
- **双路径核对义务**：`MINILANG_CORO_FIBER=0` 平台的构建中重放路径与挂起路径并存于 `callCoroutineNext`/`visitYieldExpr`/`executeCoroutineOps` 的 `#if` 双分支。修改协程相关代码时必须双路径核对语义等价（挂起路径的语义基线见 ADR-007"语义选择"表）。
- **快照字段相对化**：帧/try 处理器/upvalue 上的一切绝对索引（StackVM `basePointer`/`stackBase`/`stackSlot`，RegisterVM 寄存器 upvalue 编号，两 VM 的 `frameIndex`）入快照必须转为相对偏移、恢复时按新入口基线转回。**新增帧字段或 try 处理器字段时，若引入新的绝对索引，必须同步纳入快照相对化**，否则调用方深度变化后读写错位。
- **最外层生成器帧 `returnIp` 重定基**：自然完成可发生在任意一次 `.next()` 中，恢复时必须把最外层生成器帧的 `returnIp` 重定基到本次调用点；漏做 = 自然完成后调用方从历史调用点重执行。
- **嵌套挂起上下文**：`callCoroutineNext` 可嵌套（await drain、生成器调用生成器），登记自身上下文前保存外层值、全部退出路径恢复。
- **语义基线（挂起 = 重放的逐点对齐，唯一差异为副作用执行一次）**：yield 表达式恢复值 = 自身 yield 值；最终 yield（`currentYieldId >= yieldCount`）即 done、尾部不执行；动态 yieldCount（`kDynamicYieldCount`）仅在函数体自然结束时 done；错误封送重抛、done 不置位、下次 `.next()` 全新重建。
- **GC 根义务**（Interpreter fiber）：挂起时快照环境链根 + callStack 后缀（帧持 env）；新增随挂起存活的解释器成员引用时必须纳入快照。
- **锁定测试**：`CoroutineSuspension.*` 5 项（副作用一次/恢复值/状态连续/try 跨挂起/长序列）为四后端一致性断言；`CoroutineTest` 22 项 + `AsyncAwait` 11 项在双模式下均须通过。
