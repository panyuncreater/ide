# ADR-005: 调试一致性

## Status

Accepted. 随三后端架构逐步建立。

## Background

MiniLang 的三个执行后端（Interpreter / StackVM / RegisterVM）在调试场景下需要保持一致的用户体验：
- 断点命中行为一致
- 变量快照内容一致
- 调用栈信息一致
- 条件断点求值结果一致

但三个后端的内部状态表示完全不同（AST 节点 vs 栈槽 vs 寄存器），如何统一调试接口是关键挑战。

## Decision

通过统一的抽象层和状态转换实现调试一致性：

### VmStepper 统一分发

`VmStepper` 作为调试器的统一入口，根据当前后端类型分发到对应实现：

```
VmStepper
├── getCurrentFrameLocals() → VM 或 RegisterVM 的局部变量快照
├── step() → 统一的状态机逻辑（helper 函数）
└── pause/resume → 双后端暂停语义
```

### 条件断点求值沙箱

条件断点表达式在独立的 Interpreter 环境中求值，与程序执行状态隔离：

1. **快照**：保存当前 Environment（全局变量 + 局部变量）
2. **注入**：将快照注入临时 Interpreter 环境
3. **求值**：在临时环境中执行条件表达式
4. **恢复**：求值完成后恢复原始 Environment

关键实现：
- VM 模式：通过 `VM::getCurrentFrameLocals()` 从栈槽反查局部变量
- RegisterVM 模式：通过 `RegisterVM::getCurrentFrameLocals()` 从寄存器窗口反查
- 局部变量遮蔽同名全局变量（与运行时语义一致）

### Slot/Reg → Name 映射

编译时记录局部变量的 slot（VM）或 reg（RegisterVM）到名称的映射：

| 组件 | 数据结构 | 说明 |
|------|---------|------|
| `BytecodeChunk` | `localSlotNames` | 索引即栈槽号 |
| `RegBytecodeChunk` | `localRegNames` | 索引即寄存器号 |
| `IRFunction` | `localSlotNames` | IR 层中间存储 |

记录点：`visitVarDecl`、`visitFunDecl`（参数 + this）、`visitClassDecl` 方法编译、TryStmt catch 变量。

### 首行断点

程序启动时在首行设置 pre-execution 断点，确保调试器能在程序执行第一行前暂停。

### 暂停语义

- StackVM：`VM::pause()` 设置原子标志，dispatch 循环检查后退出
- RegisterVM：同 StackVM 策略
- Interpreter：通过递归深度检查实现暂停

## Impact

- **用户体验**：三个后端的调试行为一致，用户无感知切换
- **维护成本**：需要维护 slot→name 映射和状态转换逻辑
- **限制**：槽位复用（兄弟作用域）时后声明的变量名覆盖先前的，属于已知限制
- **性能**：条件断点求值有额外开销（Environment 快照/恢复），但仅在断点命中时发生

## Alternatives

- **各后端独立调试接口**：实现简单但用户体验不一致
- **外部调试协议（如 DAP）**：过于重量级，不适合单进程教学 IDE
- **纯 Interpreter 调试**：放弃 VM 调试能力，丧失字节码级教学价值
