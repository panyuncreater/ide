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

> 表中每项的新增/修改都必须在三后端同步实现（含 IR 双出口），并以一致性测试锁定（§5）。

## 3. 新增语言特性：三后端同步 checklist

1. **语义定义先行**：在本文档 §2 或对应 ADR 中明确目标行为（含边界），三后端以此为唯一依据，禁止各自解释。
2. **三后端实现核对**：Interpreter（`interpreter/`）→ StackVM（`compiler/Compiler.cpp` + `compiler/VM.cpp`）→ RegisterVM（`compiler/RegisterBytecodeBackend.cpp` + `compiler/RegisterVM.cpp`），逐个确认实现存在且行为一致。
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
