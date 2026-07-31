# ADR-002: 三后端并存策略

## Status

Accepted. 自项目初始采用 Interpreter 基准后端，后续迭代新增 StackVM 和 RegisterVM。

> **部分修订**：本 ADR 将 JIT 列为被拒绝方案，但自 R138 起 JIT 后端已作为第四套执行引擎落地，详见 [ADR-006](ADR-006-jit-backend.md)。当前项目实际为"四后端并存"。

## Background

教学型语言 IDE 需要同时满足多个目标：
1. **易调试**：教师和学生需要逐步理解执行过程
2. **高性能**：展示编译优化效果需要高效执行
3. **教学对比**：不同执行策略的性能差异是重要教学内容

单一后端无法同时满足这些目标。

## Decision

维护三个执行后端并存：

### Interpreter（树遍历解释器）

- 直接遍历 AST 节点执行
- 最简单的实现，最易调试
- 作为语义基准（reference implementation）
- 支持完整的 debug 路径（变量快照、调用栈）

### StackVM（栈式虚拟机）

- AST → 字节码 → 栈式 VM 执行
- 操作数栈使用定长数组（1024 × 8B = 8KB）+ 栈顶指针
- 消除 `std::vector` 的 push_back 容量检查和堆分配开销
- 通过 `setVM(true)` 启用

### RegisterVM（寄存器式虚拟机）

- IR → 寄存器式字节码 → 寄存器 VM 执行
- 32 虚拟寄存器 R0-R31，68 条 RegOp 指令集
- IR 三地址码直接 lowering，减少栈操作开销
- 通过 `setVM(true)` + `setUseRegisterVM(true)` 启用

### 一致性要求

所有语义变更必须确保三后端行为一致。测试策略：
- `TestVME2E.cpp` 中每个测试用例覆盖 Interpreter / StackVM / RegisterVM 三种路径
- IR 路径额外验证：`setIR(true)` + `setIROptimize(true)` 不改变语义

## Impact

- **维护成本**：每个语义变更需要三处实现 + 测试
- **教学价值**：学生可以直观对比不同执行策略的性能差异
- **调试能力**：Interpreter 提供完整 debug 路径，VM 提供字节码级可视化
- **测试覆盖**：全量测试套件包含三后端一致性验证

## Alternatives

- **单后端（仅 Interpreter）**：简单但无法展示编译优化，性能差
- **双后端（Interpreter + StackVM）**：缺少寄存器式 VM 的教学对比
- **JIT 编译**：复杂度过高，不适合教学场景（*注：后于 R138 采纳，见 ADR-006*）
