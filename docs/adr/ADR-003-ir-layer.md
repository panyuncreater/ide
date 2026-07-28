# ADR-003: IR 中间层

## Status

Accepted. 作为可选编译阶段实现，默认关闭。

## Background

栈式 VM 后端直接从 AST 生成字节码，跳过了中间表示层。这导致：
1. 优化 pass 只能在字节码层面进行，表达力有限
2. 无法展示编译管线中"中间表示 → 优化 → lowering"的标准流程
3. 寄存器式后端缺少统一的输入格式

## Decision

引入可选的 IR（中间表示）层，形成三段式编译管线：

```
AST → IRBuilder → IRModule → 优化 pass → BytecodeIRBackend → 栈式字节码
                                          → RegisterBytecodeBackend → 寄存器字节码
```

### IR 设计

- **三地址码**：每条指令最多两个源操作数 + 一个目标虚拟寄存器
- **基本块**：`IRBasicBlock` 包含线性指令序列，`IRFunction` 包含基本块列表
- **虚拟寄存器**：vreg 编号，由后端 lowering 时映射到栈槽或物理寄存器

### 优化 Pass

| Pass | 说明 | 栈式 VM | 寄存器 VM |
|------|------|---------|----------|
| 常量折叠 | 编译期计算常量表达式 | 默认开启 | 默认开启 |
| 死代码消除 | 移除未使用的 vreg 定义 | 默认开启 | 默认开启 |
| 复制传播 | vreg→常量等价替换 | 禁用 | 可选开启 |
| CSE | 基本块内公共子表达式消除 | 默认开启 | 默认开启 |
| 循环展开 | for 循环体 ≤20 指令且 N≤4 时展开 | 可选 | 可选 |

### 关键约束

- **栈式 VM 复制传播默认禁用**：复制传播会删除 LOAD_CONST，导致栈下溢
- **栈式 VM CSE 已默认开启**：Phase-5 修复了 CSE 与 DCE 的交互问题（`Compiler.cpp` `enableCSE=true`），栈式 VM 与寄存器 VM 均默认启用
- **寄存器 VM 复制传播默认禁用**：操作数 kind 检查不完整，常量索引被误当 vreg 编号
- 优化最多 3 轮迭代，避免编译时间爆炸

### 抽象接口

`IRBuilder` / `IRBackend` 抽象接口支持扩展多后端：
- `BytecodeIRBackend`：IR → 栈式字节码
- `RegisterBytecodeBackend`：IR → 寄存器字节码

## Impact

- **教学价值**：展示标准编译管线的 IR → 优化 → lowering 流程
- **优化能力**：为寄存器式后端提供有效的优化 pass
- **复杂度**：增加了编译管线的复杂度，需要维护 IR 与 AST 的语义一致性
- **性能**：启用 IR 优化后寄存器式后端可获得 10-30% 性能提升

## Alternatives

- **无 IR，直接 AST → 字节码**：简单但无法进行跨指令优化
- **LLVM IR**：过于重量级，不适合教学场景
- **SSA 形式 IR**：更利于优化但实现复杂度高，当前基本块级 IR 已满足需求
