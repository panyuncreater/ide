# ADR-007: 协程真挂起架构（重放模式升级）

## Status

Accepted. 2026-10-05 落地，三执行后端同步启用（`MINILANG_CORO_FIBER=1` 平台，当前为 Windows）。全量回归 4123/4123 通过。

## Background

R164（2026-07-23，见 [docs/development.md](../development.md) 协程四后端一致性条目）为四后端引入生成器（`fun*` / `yield` / `.next()` / `.done()`）与 `await` 支持时，采用的是**重放模式**：每次 `.next()` 从头重新执行生成器函数体，用运行时 yield 执行计数器（`currentYieldExecutionCount_`）跳过已消费的 yield，命中目标时抛 `YieldSignal`/`VMYieldSignal` 返回。四后端一致采用重放是该轮的明确设计（"实现四后端语义一致性（重放模式）"）。

> **历史勘误**：`InterpreterCoroutine.cpp` 头注释曾声称"StackVM/RegisterVM 用真挂起模式（D.5 实现），保存帧快照精确恢复执行点"——与代码不符。四后端均为重放模式，不存在 Interpreter 与 VM 的语义分歧，只存在四后端共同的重放限制。

重放模式的两个结构性代价：

1. **前缀副作用重复执行**。第 k 次 `.next()` 会完整重执行函数体前缀（含其中全部副作用）：`fun* g() { while (true) { print("x"); yield 1; } }` 的每次 `.next()` 都会再次 print。重放实现自身注释已承认（"用户不应在 yield 表达式中放置有副作用的操作——这是重放模式的已知限制"）。
2. **O(n²) 总代价**。n 次 `.next()` 的总执行量为 1+2+…+n = O(n²)。生成器在循环中产出长序列（教学场景常见：数列生成、遍历器）时，代价随序列长度平方增长；`await` 的同步 drain（循环 `.next()` 到完成）按相同曲线劣化。

### 为什么必须三后端同步升级

[ADR-002](ADR-002-triple-backend.md) 的核心不变量（同一源码三后端语义等价）禁止单后端升级：若仅 Interpreter 真挂起而 VM 保持重放，副作用执行次数成为跨后端分歧点；反之亦然。`async`/`await` 调度语义在 AGENTS.md §3 与 [docs/specs/backend-consistency.md](../specs/backend-consistency.md) §2 中被列为三后端必须逐项一致的行为。因此本 ADR 的范围是**三执行后端协同改造**，任何单后端先行落地都被禁止（见 Consquences 的单边启用禁令）。

## Decision

三执行后端同步升级为**真挂起模式**：yield 时保存执行位置并切回调用方，下次 `.next()` 从挂起点精确恢复。副作用只执行一次，n 次 `.next()` 总代价 O(n)。升级由平台宏 `MINILANG_CORO_FIBER` 门控（[interpreter/CoroutineFiber.h](../../interpreter/CoroutineFiber.h)），当前仅 Windows（`=1`）启用；其他平台三后端**统一**保留重放路径——同一平台上要么全部挂起、要么全部重放，跨平台行为差异属已记录的既定限制（与 [ADR-006](ADR-006-jit-backend.md) 的 JIT 平台降级同模式）。

### 各后端实现形态

| 后端 | 挂起载体 | 恢复机制 |
|------|---------|---------|
| Interpreter | Windows fiber（`ConvertThreadToFiberEx`/`CreateFiberEx`/`SwitchToFiber`），递归树遍历解释器的 C++ 栈即延续 | fiber 切换回生成器栈，`visitYieldExpr` 的 `switchToFiber` 调用点之后继续 |
| StackVM | `VMCoroutineSuspension`（VMCalls.cpp）：帧链 + 栈切片（`VMStack` 定长数组的 `[baseStack, top)` 区间）+ try 栈 + 开放 upvalue，存入 `CoroutineData::vmSuspension` | `callCoroutineNext` 恢复分支回推切片、重挂帧链与 upvalue、按 `resumeIp` 继续指令循环 |
| RegisterVM | `RegVMCoroutineSuspension`（RegisterVMExec.cpp）：帧链（寄存器窗口 `std::array<Value,32>` 内嵌于帧，无独立值栈）+ try 栈 + 开放 upvalue | 同构恢复分支 |
| JIT | 无改动 | 含生成器的程序经 CompileError 整体回退 VM（既有行为），自动继承 VM 挂起语义 |

`CoroutineData`（[interpreter/Value.h](../../interpreter/Value.h)）扩展两个不透明持有字段：`interpreterFiber`（Interpreter fiber 状态）与 `vmSuspension`（VM 快照，两 VM 互斥使用）；克隆路径（`cloneImpl` 的 `VAL_COROUTINE` 分支）有意不复制——克隆得到未启动的独立协程（与既有"迭代器单消费者"语义一致）。

### 语义选择（与重放模式逐点对齐）

升级的原则是**除副作用执行次数外，一切可观察行为保持不变**：

| 语义点 | 重放模式行为 | 真挂起行为 | 一致性 |
|--------|-------------|-----------|--------|
| yield 表达式的值 | 被跳过的 yield 求值为自身 yield 值（VM `push(yieldValue)`、Interpreter `lastValue_ = yieldValue`） | 恢复后 yield 表达式求值为自身 yield 值 | 逐点一致 |
| 最终 yield 后的 done | `currentYieldId >= yieldCount` 即置 done，尾部语句不执行 | 同规则置 done，快照弃用，尾部不执行 | 一致（静态 yieldCount）；动态 yieldCount（循环内 yield，`kDynamicYieldCount`）两端均仅在函数体自然结束时置 done |
| yield 表达式的求值次数 | 前缀中每次重放都重新求值（副作用重复） | 仅挂起点求值一次 | **唯一有意变更** |
| 错误传播 | RuntimeError/ThrowException/DebugStopException 穿透 `.next()`，done 不置位，可再次 `.next()` 重试 | exception 封送（`std::exception_ptr`）后重抛，done 不置位，下次 `.next()` 丢弃死 fiber 全新重建（等价于重试重执行） | 一致 |
| 闭包/环境状态 | 每次重放重建 env | 挂起期间 env 链经快照持活，变量变异跨 `.next()` 连续 | 值层面一致（重放重建后也是同值） |

### 实现不变量（快照/恢复正确性）

1. **绝对索引重定基**。帧的 `basePointer`（StackVM）、try 处理器的 `stackBase`、开放 upvalue 的 `stackSlot`/`owningFrameIdx`、寄存器 upvalue 编号（RegisterVM，`frameIdx*32+regIdx`）均为绝对索引，而两次 `.next()` 的调用方栈深/帧深可以不同。快照按当次 `.next()` 入口基线转**相对偏移**存储，恢复时按新入口基线转回绝对。漏做重定基 = 调用方深度变化后读写错位栈槽。
2. **最外层生成器帧 `returnIp` 重定基**。生成器帧的 `returnIp` 在首次 `.next()` 时固定为该次调用点；真挂起下自然完成（OP_RETURN/REG_RETURN 以 `returnIp` 恢复调用方 ip，再由 dispatch 的 `ip += instrLen` 推进）可能发生在**任意一次** `.next()` 中。恢复时必须将最外层生成器帧的 `returnIp` 重定基到本次调用点（`frames_[baseFrames].returnIp = frames_[baseFrames-1].ip`）。漏做 = 自然完成后主脚本从历史调用点重执行（症状：输出令牌数多于 print 语句数）。帧链内部帧的 `returnIp` 指向生成器体内调用点，与调用方无关，保持不变。
3. **嵌套挂起上下文保存/恢复**。生成器体内的 `await`（OP_AWAIT/REG_AWAIT 的 drain 循环）与生成器调用生成器会嵌套进入 `callCoroutineNext`。嵌套调用登记自身挂起上下文（`currentSuspendingCoro_`/`coroSuspendBase*`）前必须保存外层值，全部退出路径（挂起返回、异常穿透、正常返回尾）恢复——否则外层执行体随后的 yield 会落回重放分支或挂错对象。Interpreter 侧对应 `activeCoroutineFiber_` 的 prevActive 保存/恢复。
4. **挂起状态即 GC 根**（Interpreter）。挂起时快照环境链根（`currentEnv_`）与 callStack 后缀（帧持 env）——`suspendedEnvPool` 复用与 mark-sweep 循环收集都以这些强引用为根。VM 侧快照全在堆容器中，随 `CoroutineData` 所在的协程值自然生根。
5. **开放 upvalue 摘除重挂**。挂起清理时**不得**对生成器栈区执行 `closeUpvaluesFrom`（关闭会把 upvalue 冻结为快照，恢复后写入与闭包读取分叉）；应将生成器区的开放条目从 `openUpvalues_` 摘除存入快照，恢复时按重定基后的槽位重挂。

### 语义锁定测试

`tests/TestCoroutine.cpp` 的 `CoroutineSuspension.*` 组（`#if MINILANG_CORO_FIBER` 门控，四后端一致断言）：副作用只执行一次（`"bbb"` vs 重放的 `"bbbbbb"`）、yield 表达式恢复值、跨挂起点局部状态连续性、try/catch 跨挂起点、300 次 `.next()` 长序列值正确。既有 `CoroutineTest` 22 项与 `AsyncAwait` 11 项在挂起模式下原样通过（值语义不变）。

## Known Limitations（有意记录）

1. **平台覆盖**：非 Windows 平台三后端统一为重放模式。POSIX ucontext 路径为后续方向，落地时需配套 ASan fiber 标注（`__sanitizer_start_switch_fiber`/`__sanitizer_finish_switch_fiber`）避免备用栈误报。
2. **fiber 栈析构不执行**：`DeleteFiber` 直接释放栈内存，其上未析构的 C++ 对象（含 env `shared_ptr`）引用计数随栈内存一并遗忘——仅发生在协程弃用（done 后弃置/解释器销毁）路径的有界一次性泄漏。
3. **生成器自恢复禁用**：生成器体内对自身调用 `.next()` 在挂起模式下显式报错（fiber 不可重入激活；重放模式下该写法本就趋于无限重放，无合法用途）。
4. **GC 边缘（理论性）**：挂起 fiber 的 C++ 栈上值临时对象对 mark-sweep 不可见；因所有用户可见状态（局部变量/闭包捕获）都经 Environment 链且已被快照生根，仅含非循环临时值的 parked 栈不受影响，循环引用临时值不可达该状态（自引用需要命名绑定），当前不做额外处理。

## Consequences

**正向**：
- 生成器/await 的副作用语义与主流语言（Python/JS/C#）一致，消除"yield 前不能有副作用"的教学陷阱与四后端共同的行为怪异。
- O(n²) → O(n)：循环产出长序列的生成器程序与 await drain 的代价恢复线性。
- 重放模式代码（yield 计数器 gating、逐次重执行）在启用平台上整体退役，`visitYieldExpr`/`OP_YIELD`/`REG_YIELD` 的挂起路径成为唯一路径，分支复杂度净减。

**代价与风险**：
- 三后端各引入一套快照/恢复路径（`#if` 双分支并存于非启用平台的构建中），修改协程相关代码时必须双路径核对。
- 绝对索引重定基是隐性不变量（§实现不变量 1-2），新增帧字段或 try 处理器字段时若引入新的绝对索引，必须同步纳入快照相对化——已列入 [backend-consistency.md §9](../specs/backend-consistency.md) 的 checklist。
- fiber 使解释器的 C++ 栈深度分析多一层（生成器体在 4MB fiber 栈上执行，受 `MAX_RECURSION_DEPTH` 保护）。

## References

- [ADR-002: 三后端一致性](ADR-002-triple-backend.md)（单边启用禁令的依据）
- [ADR-006: JIT 后端](ADR-006-jit-backend.md)（平台降级模式先例）
- [backend-consistency.md §9](../specs/backend-consistency.md)（协程挂起一致性 checklist）
- [docs/development.md](../development.md) R164 条目（重放模式的原始设计记录）
- `tests/TestCoroutine.cpp`（`CoroutineTest` 22 项一致性 + `CoroutineSuspension` 5 项语义锁定）
