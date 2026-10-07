# ADR-008: JIT per-chunk 编译架构与编译块复用

## Status

Accepted。2026-10-10/11 分阶段落地：阶段 1a（per-chunk 独立代码块）、阶段 1b（tiering 真单 chunk 编译）、阶段 2（进程内块缓存，共享存活设计）均已实施，全量回归通过（debug 树 4132/4132，含 BlockCache 5 项新回归）；阶段 3（磁盘持久化，含重定位记录层）仅记录设计，未实施。

## Background

[ADR-006](ADR-006-jit-backend.md) 落地的第四执行后端采用**单体编译**：`JITBackend::compileAllChunks` 把 mainChunk + 全部 functionChunks 发射进**一个** `asmjit::CodeHolder`，经一次 `runtime_.add` 产出唯一入口（原 `JITCodeGen.cpp:33-2346`）。该布局带来三类结构性代价：

1. **tiering 全量重编译**。lazy compilation / INT 特化 / FLOAT 特化 / OSR 迁移 / 反优化（R157/R158/R159 系列）全部以"重新编译所有 chunk 到新 CodeHolder → 提取目标 chunk 入口 → 丢弃其余"实现（`compileSingleChunkLazy`、`compileChunkSpecialized` 家族）。单次定向重编译代价 O(全部 chunk)，且为防内层编译重置外层状态，维护着 **14 个成员向量的 save/restore 补丁**（三份近乎相同的恢复代码块）——R157 修复注记自证此类悬垂指针曾是崩溃根因。
2. **跨运行编译缓存被阻断**。生成机器码中内嵌了大量仅在当前进程/当前 `CompileResult` 实例有效的绝对地址（清单见下），无法序列化复用。
3. **不支持指令的代价放大**。任一 chunk 含不支持 OpCode 即整体 `CompileError`，已编译部分全部作废。

### 前提勘误（立项前实测）

项目积压记录曾把该改造的难点归结为"单体 CodeHolder、codegen 内嵌 chunk 指针、**跨后端帧协议**"。立项前代码实测勘误：**JIT 与 StackVM/RegisterVM 之间不存在任何帧互操作**——StackVM 零 JIT 引用，JIT 从不调用 VM 代码（无 JIT→VM 调用路径，亦无 VM→JIT 入口；OSR/Deopt 两端均为 JIT 块）。真实跨块协议是 **JIT 各代码块之间共享 `JitContext` 的软帧协议**（帧链、操作数栈、try 栈经 r12 间接访问），而该协议在 lazy 模式下**已经跨 CodeHolder 工作多年**——`funcEntries_`/`methodEntries_` 本就存跨块绝对地址。因此"跨后端帧协议"不构成约束，改造难度大幅低于积压记录的评估。

### 内嵌地址清单（阶段 2 的输入）

生成机器码中的绝对地址五类：

| 类别 | 内容 | 可缓存性 |
|------|------|---------|
| A | extern "C" helper 函数指针（~40 个发射点，`&jitXxx` movabs） | 进程内稳定；持久化需重定位 |
| B | 常量池/chunk 派生指针（OP_STRING 的 `StringData*`、`c_str()`、OP_CLOSURE 的 `chunkPtr` 与指向 `chunk.code` 中部的 `upvalueDescs`） | 绑定当前 CompileResult 实例 |
| C | `JITBackend* this`（backendPtr 实参） | 绑定后端实例 |
| D | 块内标签地址（returnAddr/catchAddr/跳转标签，`lea label`） | 块内自洽，无需重定位 |
| E | NaN-boxing tag 常量、立即数 | 缓存安全 |

入口地址本就按 `entry + label_offset_from_base(label)` 记录——偏移表示现成，缺的只是重定位层。

## Decision

### 阶段 1a（已落地）：per-chunk 独立代码块

- **每个 chunk 一个独立 CodeHolder/代码块**：`compileAllChunks` 改为两遍流程——先逐块发射（`emitChunkBody`，自原 1950 行循环体提取），全部成功后逐块 `runtime_.add` + 入口解析 + 导入槽回填。发射期不分配可执行内存，任一 chunk 发射失败整体放弃（保持"不支持 OpCode → 整程序 CompileError，调用方回退 VM"语义不变）。
- **跨块引用两条通道**：
  1. **跨块 OP_CALL fast path → 导入槽**：同块调用（直接递归）保留编译期直连 `jmp`；跨块调用发射 `mov rax,[ctx+callImportSlots]; mov rax,[rax+slot*8]; jmp rax`——零 C++ 调用，保留 fast path 消除 `jitCallByName` 开销的设计初衷。槽位下标发射期分配、全部块 add 后按被调 chunk 名回填。**槽位表跨编译代合并追加**（各代块在表中拥有独立区段，永不重编号），`execute()` 入口随全部代码块一并清空；commit 仅回填本次编译代追加的 patch 区段。
  2. **非 main 块错误退出 → 退出桩**：全部发射辅助以 `Label epilogue` 参数表达错误出口（~40 处，签名不变）；main 块绑真实 epilogue（恢复 callee-saved + ret），非 main 块把本地 epilogue 标签绑定为退出桩 `mov rax,[ctx+errorExit]; jmp rax`。`rbp` 自 main prologue 后全程不变（函数块只用 r13/r15），跨块跳转与原单体布局的同块 `jmp epilogue` 等价。
- **协议三不变**：`JitContext` 邮箱布局（仅追加 `callImportSlots`/`errorExit` 两字段，遵循 ADR-006 append-only 约束）、`JitFrame` 72 字节软帧布局、extern "C" helper ABI。
- **所有权**：`codeBlocks_`（`JitCodeBlock` 向量）为代码块所有权权威，`execute()` 入口与析构按块释放；`currentEntry_` 仅镜像 main 块入口。tiering 内层编译的全部块移交既有机制（`ownedLazyEntries_` / `ownedSpecializedEntries_`），**目标块经 `registerSpecializedOwnership` 登记退休链、批量移交跳过目标块——同一块的所有权必须恰好登记一次**（双重登记导致析构双重 `release` → asmjit 位图越界 → 堆损坏，1a 验证期由 MSVC ASan 捕获并修复，见 Consequences）。
- 汇编教学捕获逐块采集后按 chunk 分节拼接；`StringLogger` 以堆对象持有（须活到 `runtime_.add`）。

### 阶段 1b：tiering 真单 chunk 编译

- 新增单 chunk 编译路径：仅发射目标 chunk 的块（函数块形态：入口 stub + 块体 + 退出桩，无 prologue/epilogue/闭包跳板），完成后**只更新目标 chunk 的映射条目**，不触碰其余 chunk 的映射/统计/基线备份。
- 删除 `compileSingleChunkLazy` / `compileChunkSpecialized` / `compileChunkSpecializedFloat` / `WithOsr` 两变体的全部 save/restore 补丁。
- 约束：① 单 chunk 编译**不得**清空/重初始化 per-chunk 统计数组与 `chunkTiers_`；② **不得**覆写 `ctx->errorExit`/`closureTrampoline_`（无 main 块，继承外层值）；③ IC callSiteId 采用追加编号（`nextCallSiteId_` 从现有容量起计，老块槽位永不失效）；④ fast path 仅在被调 chunk 已有非空 entryPtr 时可走（未编译被调走 mailbox → 运行期 lazy 触发），eager 全量路径不受影响。

### 阶段 2：进程内块缓存（共享存活设计，已落地）

实现中对重绑定方案做了一次**设计细化**：块的可执行内存由各实例的 `asmjit::JitRuntime` 持有，实例销毁即释放——进程内跨实例复用**不能**只靠重定位重绑（代码宿主也会消失）。落地形态：

- **缓存键** = `fnv1a64(字节码内容) ^ kJitCodegenVersion`。字节码内容指纹遍历全部 chunk（名称/代码字节/常量/元数据）+ globalSlotNames/enumInfos；任何影响 codegen 的字节码差异或 codegen 版本递增都会改变键值。**字符串常量按内容指纹**（raw bits 是每次编译新分配的堆指针，按位混入会导致同源码键不稳定——BlockCache 回归测试捕获）；标量按 NaN-boxing 原位编码（保守：内容不同必不同键，永不假命中）。
- **共享存活**：未命中时深拷贝 `CompileResult` 作为编译宿主（块内嵌指针——常量池 `StringData*`、`c_str()`、`chunkPtr`、upvalueDescs——天然指向缓存持有的副本），代码块从**进程级共享 `JitRuntime`** 分配（块生命周期归缓存，实例侧 `JitCodeBlock.owned=false` 不释放）；副本+块+映射元数据+导入槽清单+IC 尺寸一并入缓存（LRU 容量 8）。
- **命中路径**：新实例直接引用共享块与元数据重建映射/统计（`initPerChunkState`，与全量编译共用）/导入槽（同索引重填）/`errorExit`/闭包跳板，跳过重编译——**零重绑定、零字节拷贝**。
- **门控**：仅纯 eager 路径（`!lazyMode_ && !tieredMode_ && customThresholds_/customOsrThresholds_` 均空）——lazy/tiering/自定义阈值的生成代码依赖运行模式与阈值，按需重编译绕过缓存。
- **重定位记录层归入阶段 3**（磁盘持久化必须把代码从宿主实例解耦，A/B/C 类内嵌地址按 `{块内偏移, 类别, 符号}` 记录并在加载期重写）；进程内复用不需要它。

**明确出界（阶段 2 复核）**：per-chunk 优雅降级（无 JIT→VM 调用路径，保持整体 CompileError 回退）；ARM64 PoC 不迁移。

### 阶段 3（仅设计，未实施）：跨进程磁盘持久化

- 序列化每块：机器码字节 + 重定位表 + 入口偏移 + 元数据（chunk 名、arity、localCount）。文件头含 magic `"MLJC"`、格式版本、`kJitCodegenVersion`、source fnv1a64、asmjit ABI 版本、fnv1a32 校验——任一不匹配即拒绝加载回退重编译（与 BytecodeCache 五重校验同模式）。
- 加载：分配可执行内存 → 拷贝 → 应用重定位 → 注册入口。信任模型与 `.mbc` 字节码缓存一致（用户本机缓存目录、源哈希键控）。
- **明确不做 per-chunk 优雅降级**：JIT→VM 调用路径不存在（见前提勘误），构建它需要跨后端帧互操作与双引擎全局状态融合，收益/风险不成立；保持整体 `CompileError` 回退（ADR-006 调用方切换后端模式）。
- ARM64 PoC（`MINILANG_USE_JIT_A64`，默认关）不迁移。

## Consequences

- 正向：tiering 定向重编译从 O(全部 chunk) 降为 O(1)；删除 14 向量 save/restore 补丁；同源码重复运行免重编译（阶段 2）；为阶段 3 铺平道路。
- 风险与已验证教训：
  - **跨块间接跳转的性能**：fast path 从同块 rel32 直连变为跨块时一槽内存间接跳，须以 JIT 基准同会话 A/B 量化（禁止跨会话锚点）。
  - **所有权必须恰好一次**：1a 验证期双重登记目标块 → 析构双重 `release` → asmjit `bit_vector_index_of` 越界读 → 堆损坏，表象随布局漂移（int3 崩溃 / 断言对话框 / 静默挂起）——**MSVC ASan 树单测复现并定位**（JitAllocator::release → JIT.cpp dtor），修复为登记链唯一。该案例印证 CI ASan 门禁对 JIT 所有权改动的必要性。
  - **ctx 数组指针重同步义务**：恢复任何被 `compileAllChunks` 重置的向量后，必须重同步 `jitContext_` 全部 `data()` 指针（R157 教训的完整版，1a 已补全 INT/Float 特化路径缺失的同步）。
- 验证面：TestJIT 全家族（R138–R166）+ CoverageGaps + 三后端一致性差分全绿；新增跨块调用/重定位 rebind/跨实例复用回归。

## References

- [ADR-006 JIT 后端](ADR-006-jit-backend.md)（JitContext append-only 约束、tiering 机制表）
- [docs/specs/backend-consistency.md](../specs/backend-consistency.md)（四后端一致性不变量）
- CHANGELOG Unreleased 对应条目（实施明细与验证数据）
