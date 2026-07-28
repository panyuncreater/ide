# ADR-006: JIT 后端（第四执行引擎）

## Status

Accepted. 自 R138 起作为第四套执行引擎实现，基于 asmjit 的本地机器码编译后端。默认通过 CMake 选项 `MINILANG_USE_JIT=ON` 启用。

## Background

[ADR-002](ADR-002-triple-backend.md) 在 Alternatives 中明确写道："JIT 编译：复杂度过高，不适合教学场景"。该判断在项目初期成立——当时三后端（Interpreter / StackVM / RegisterVM）已能满足"易调试 + 高性能 + 教学对比"三个目标，引入 JIT 会显著增加维护成本与教学复杂度。

随着项目演进，三个动因促使重新评估该决策：

1. **教学闭环缺失**：栈式 VM 与寄存器式 VM 都是解释执行（dispatch loop），无法展示"字节码 → 本地机器码"这一编译原理的关键跃迁。学生只能对比"解释执行 vs 解释执行"，缺少"解释执行 vs 本地执行"的性能量级对比。
2. **优化技术教学空白**：类型反馈、热点检测、特化重编译、OSR、分层编译、反优化等现代 JIT 核心技术（V8/HotSpot 的核心机制）在三后端模型中无法体现。这些恰恰是编译原理课程的高级主题。
3. **性能上限探针**：三后端中 RegisterVM 最快但仍受 dispatch loop 开销限制，无法展示"消除解释器开销后"的真实硬件性能上限。

## Decision

新增第四套执行引擎 **JIT 后端**（`JITBackend`），基于 [asmjit](https://asmjit.com) 将 `BytecodeChunk` 编译为 x86-64 本地机器码直接在 CPU 上执行。与 StackVM 共享编译管线输入（`CompileResult` 中的 `BytecodeChunk`），定位为"StackVM 的硬件加速器"。

### 与 ADR-002 的关系

JIT 后端**不取代**三后端策略，而是**补充**第四后端。三后端一致性约束（Interpreter / StackVM / RegisterVM 语义等价）保持不变，JIT 作为可选加速层，其语义必须与三后端对齐（同一 MiniLang 源码在四后端产生相同结果）。ADR-002 的"JIT 复杂度过高"判断在教学层面仍然成立——JIT 代码（~5000 行）远比 StackVM dispatch loop 复杂，故 JIT 作为**可选**后端而非默认后端，教学路径仍以三后端为主。

### asmjit vs LLVM 的取舍

| 维度 | asmjit | LLVM ORCJIT |
|------|--------|-------------|
| 依赖体积 | 单头文件 + 单静态库（~2MB） | 完整 LLVM 工具链（~500MB） |
| 集成复杂度 | CMake `FetchContent` 一行接入 | 需匹配 LLVM 版本、链接 30+ 库 |
| 编译时开销 | 手写机器码发射，毫秒级 | LLVM IR → SelectionDAG → MachineInstr，百毫秒级 |
| 优化能力 | 无（需手写所有优化 pass） | 完整优化管线（内联、向量化、逃逸分析等） |
| 教学透明度 | 高（每条指令显式 emit，可逐步讲解） | 低（优化在黑盒 pass manager 内完成） |
| 平台支持 | x86/x64/ARM 手写 | 全平台（通过 TableGen） |

**选择 asmjit** 的核心理由：
1. **教学透明度优先**：MiniLang 是教学项目，JIT 的价值在于让学生看到"字节码如何一条条映射到机器码"。asmjit 的低级 emit API（`a.mov` / `a.add` / `a.jmp`）让每条指令的生成过程可读可讲；LLVM 的优化黑盒无法满足这一教学目标。
2. **依赖轻量**：课程设计交付物需"一键编译"，LLVM 的庞大依赖会显著抬高构建门槛。
3. **优化 pass 手写可控**：类型反馈、特化重编译、OSR 等优化技术需作为教学演示显式实现，asmjit 的"无内置优化"反而是优势——每个优化 pass 都是项目自有代码，可逐行讲解。

代价：无法享受 LLVM 的成熟优化（自动内联、向量化、逃逸分析），所有优化需手写。这与项目"教学优先"定位一致。

### 分层编译（Tiered Compilation）设计

JIT 后端实现 V8 风格的三层编译模型：

| Tier | 名称 | 实现 | 触发条件 | 用途 |
|------|------|------|----------|------|
| Tier 0 | Interpreter | 树遍历解释器 | 分层模式启用时非 main chunk 初始层 | 快速启动，零 JIT 开销 |
| Tier 1 | Baseline | 非特化 JIT（含类型反馈收集） | 首次调用（lazy compilation） | 快速编译，收集运行时类型信息 |
| Tier 2 | Specialized | INT/FLOAT 特化 JIT | 热点计数达阈值 + 类型反馈稳定 | 消除类型检查，走原生路径 |

**升级链**：Tier 0 → Tier 1（首次调用 lazy compile）→ Tier 2（热点 + 类型稳定）
**降级链**：Tier 2 → Tier 1（反优化，类型保护失败时即时触发）

分层编译依赖 lazy compilation 基础设施（`setTieredCompilation(true)` 自动启用 `lazyMode_`）。Tier 0→1 升级复用 `compileSingleChunkLazy`，Tier 1→2 升级复用 `compileChunkSpecialized`/`compileChunkSpecializedFloat`。

### 关键运行时机制

| 机制 | 实现 | 引入版本 |
|------|------|----------|
| 热点检测 | per-chunk 调用计数器 + 阈值检查（协作式安全点） | R150-R151 |
| 类型反馈 | per-chunk `TypeFeedback`（int/float/other 计数），算术指令 generic 路径收集 | R152 |
| INT 特化 | `otherCount==0 && floatCount==0` 时跳过类型检查走 INT 原生路径 | R152 |
| FLOAT 特化 | `floatCount>0 && otherCount==0` 时跳过 INT 原生路径走 generic 路径 | R153 |
| Lazy compilation | 函数/方法 chunk 首次调用时按需编译到独立 CodeHolder | R157 |
| OSR（On-Stack Replacement） | 循环回边达阈值时保存/恢复栈帧 + 跳转特化版本入口点 | R157（简化版）/ R158（真正栈帧迁移） |
| 反优化（Deoptimization） | 特化版本类型保护失败时即时保存栈帧 + 跳回 Tier 1 baseline | R158（标记）/ R159（即时） |

### JitContext 邮箱模式

JIT 生成的本地代码无法直接持有 C++ 对象引用（asmjit::Label 是编译期概念，运行时不存在）。所有 JIT 代码与 C++ 运行时的交互通过 `JitContext` 结构体的固定偏移字段（"邮箱"）间接传递：

- JIT 代码通过 `r12` 寄存器持有 `JitContext*`，以硬编码 offset 访问字段
- C++ 辅助函数（`extern "C"`）通过参数接收 `JitContext*`，读写邮箱字段返回结果给 JIT 代码
- 新增字段必须**末尾追加**（offset 递增），不可插入中间位置（会破坏已有 offset 引用）

当前 `JitContext` 含 29 个字段（offset 0-224），覆盖输出回调、错误状态、全局变量、帧栈、类信息、方法入口邮箱、热点/类型反馈/OSR/deopt 等运行时机制。

### 寄存器分配（Windows x64 ABI）

| 寄存器 | 用途 | 性质 |
|--------|------|------|
| r12 | `JitContext* ctx` | callee-saved，跨调用保留 |
| r13 | 当前帧 basePointer | callee-saved，指向帧第一个参数 |
| r15 | 操作数栈顶 | callee-saved，向低地址增长 |
| rax/rcx/rdx | 临时寄存器（算术/参数/比较） | caller-saved |
| rsp | 原生栈（仅函数调用 shadow space） | — |

## Impact

- **教学价值**：补全"解释执行 → 本地执行"的对比维度，展示类型反馈/热点检测/OSR/分层编译/反优化等现代 JIT 核心技术
- **性能**：JIT 相比 StackVM 在数值密集型基准上达 40-100x 加速（消除 dispatch loop 开销）
- **维护成本**：JIT.cpp ~5000 行，是项目最复杂的单一模块；每个 OpCode 需手写机器码 emit
- **一致性约束扩展**：四后端一致性（Interpreter / StackVM / RegisterVM / JIT），`EXPECT_FIVE_BACKENDS` 宏条件编译
- **平台限制**：当前生产级支持仅 x86-64（asmjit 的 ARM 后端未接入）；Windows x64 ABI 与 System V ABI 分支处理
- **ARM64 PoC**：`MINILANG_USE_JIT_A64` 选项提供实验性 ARM64 PoC（`JITA64CodeGen.cpp`，370 行），仅覆盖基本算术 + 控制流 + 局部/全局变量子集（对齐 x86-64 R138-R140），无函数调用/类/闭包/异常/GC 集成，无单元测试覆盖。默认 OFF，不建议生产使用，仅供后续 ARM64 教学平台移植参考

## Alternatives

- **维持 ADR-002 拒绝 JIT**：教学闭环缺失，无法展示本地执行与 JIT 优化技术
- **LLVM ORCJIT**：依赖过重，优化黑盒不可教，构建门槛过高
- **LuaJIT 风格 trace JIT**：trace 录制 + 录制路径特化，实现复杂度更高，且 MiniLang 字节码结构非 trace-friendly
- **Cranelift**：介于 asmjit 与 LLVM 之间，有内置优化但教学透明度低于 asmjit

## 演进里程碑

| 版本 | 里程碑 |
|------|--------|
| R138 | PoC：mainChunk 整数算术子集 |
| R139-R140 | 控制流 + 全局变量 + 比较指令 + 除法 |
| R141 | 函数调用（OP_CALL + 帧栈 + 局部变量） |
| R146-R147 | 数组/字典/元组 + INDEX_SET |
| R148-R149 | 类支持 + 方法调用 + super + writeBack |
| R150-R151 | 热点检测 + 阈值重编译触发 |
| R152-R153 | 类型反馈 + INT/FLOAT 特化重编译 |
| R154 | 嵌套左值赋值链 + 容器杂项 OpCode |
| R155-R156 | 闭包值创建/调用 + 完整 upvalue 捕获 |
| R157 | Lazy compilation + OSR（简化版） |
| R158 | 真正 OSR 栈帧迁移 + 分层编译 + 反优化 |
| R159 | 即时反优化 + Tier 0→1 自动升级 |

## 参考

- [ADR-002: 三后端并存策略](ADR-002-triple-backend.md)
- [核心架构](../architecture.md)
- [asmjit 官方文档](https://asmjit.com)
