# MiniLang 核心架构 / Core Architecture

本文档描述 MiniLang IDE 的核心架构设计，包括编译管线、值表示、内存模型与安全约束。

---

## 编译管线概览

MiniLang 采用可选的三段式编译管线（外加 JIT 热路径分层编译）：

```
源码 → Lexer → Parser → AST → [IR → IR优化] → Bytecode → VM/RegisterVM
                                    ↓                        ↓（热路径）
                              Interpreter（树遍历，基准后端）   JIT（x86-64 本机码，不支持场景降级回 VM）
```

| 阶段 | 组件 | 说明 |
|------|------|------|
| 词法分析 | `lexer/Lexer` | 源码 → Token 流，支持插值字符串词法拆分 |
| 语法分析 | `parser/Parser` | Token 流 → AST，递归下降 |
| 中间表示 | `compiler/IR` | AST → IR 三地址码（可选），支持常量折叠/死代码消除/复制传播 |
| 字节码编译 | `compiler/Compiler` | AST → 栈式 VM 字节码 |
| 寄存器编译 | `compiler/RegisterBytecodeBackend` | IR → 寄存器式字节码 |
| 栈式 VM | `compiler/VM` | 执行栈式字节码 |
| 寄存器 VM | `compiler/RegisterVM` | 执行寄存器式字节码 |
| 解释器 | `interpreter/` | 直接遍历 AST 执行（基准后端） |
| JIT | `compiler/JIT*` | 栈式字节码 → x86-64 本机码分层编译（asmjit） |

---

## 值表示：NaN-boxing

Value 类型使用 8 字节 NaN-boxing 编码，将 `sizeof(Value)` 从 24 字节降至 8 字节。

### 编码方案

| 类型 | Tag（高 16 位） | Payload（低 48 位） |
|------|----------------|-------------------|
| int | `0x7FFC` | 48 位有符号整数（范围 ±2^47） |
| float | `0x7FFC` | IEEE 754 double 的 NaN payload（规范化为固定 marker） |
| bool | `0x7FFD` | 0 = false, 1 = true |
| null | `0x7FFE` | 0 |
| ptr | `0x7FF8`-`0x7FFB` | 48 位堆指针 |

### 设计约束

- 标量（int/float/bool/null）内联存储，零原子操作
- 堆类型通过侵入式引用计数（`RefCounted` 基类）管理
- NaN 完全规范化：所有 NaN 统一为 `NAN_BOXED_FLOAT_MARKER`，不保留 payload
- 类型访问前必须用 `isXxx()` 校验

详见 [ADR-001: NaN-boxing](adr/ADR-001-nan-boxing.md)

---

## 四后端架构

MiniLang 维护四个执行后端并存策略：

| 后端 | 类 | 特点 |
|------|------|------|
| Interpreter | `Interpreter` | 树遍历，基准实现，最易调试 |
| StackVM | `VM` | 栈式字节码，89 条 OpCode，1024×8B 定长操作数栈 + 栈顶指针 |
| RegisterVM | `RegisterVM` | 寄存器式字节码，68 条 RegOp，32 虚拟寄存器 R0-R31 |
| JIT | `JITBackend` | 基于 asmjit 的本地机器码后端（x86-64），可选启用（`MINILANG_USE_JIT`，x86-64 默认 ON）；另有实验性 ARM64 PoC（`MINILANG_USE_JIT_A64`，仅基本算术+控制流） |

三解释后端（Interpreter / StackVM / RegisterVM）必须保持语义一致性。IR 层作为可选中间表示，启用后在 lowering 前执行优化 pass。JIT 后端与 StackVM 共享 `BytecodeChunk` 输入，定位为"StackVM 的硬件加速器"，语义必须与三后端对齐。

### JIT 后端

JIT 后端将 `BytecodeChunk` 编译为 x86-64 本地机器码直接在 CPU 上执行，消除 dispatch loop 开销。实现 V8 风格的分层编译（Tier 0 Interpreter → Tier 1 Baseline → Tier 2 Specialized），含热点检测、类型反馈、INT/FLOAT 特化重编译、lazy compilation、OSR 栈帧迁移、反优化等现代 JIT 核心机制。JIT 代码通过 `JitContext` 邮箱模式与 C++ 运行时交互（`r12` 寄存器持有上下文指针，字段偏移经 `jit_offset` 命名常量 + `static_assert(offsetof)` 编译期校验）。

详见 [ADR-002: 三后端策略](adr/ADR-002-triple-backend.md)
详见 [ADR-003: IR 中间层](adr/ADR-003-ir-layer.md)
详见 [ADR-006: JIT 后端](adr/ADR-006-jit-backend.md)

---

## 内存模型

### COW（Copy-On-Write）

Value 类型使用 Copy-On-Write 策略：堆类型修改前检查独占所有权，避免不必要的深拷贝。

### 引用计数

- `RefCounted` 基类提供侵入式引用计数
- 独占所有权时修改无需复制
- 共享所有权时触发 COW 深拷贝

### GC

`GcManager` 管理循环引用检测，支持 mark/sweep 阶段。

详见 [ADR-004: COW 内存模型](adr/ADR-004-cow-memory.md)

---

## 安全约束

### DoS 防护

| 限制 | 值 |
|------|------|
| 源码大小 | ≤ 10MB |
| Token 数量 | ≤ 100 万 |
| 循环迭代 | ≤ 1000 万次 |
| 字符串 reserve | ≤ 1MB |

### 递归保护

Parser/Compiler/Formatter/equals() 均有深度限制（512/256）。

### 线程安全

- DebugController 使用 mutex + atomic
- Worker 线程通过 Qt 信号回传
- `std::localtime()` 替换为 `localtime_s` (Windows) / `localtime_r` (POSIX)

### 异常安全

- UI 槽函数包裹 try/catch，确保异常时回滚 UI 状态
- 深拷贝操作使用 `std::unique_ptr` 包裹防止 bad_alloc 泄漏

### 内存安全

- 所有 assert 检查的越界/类型保护在 Release 构建中使用运行时检查
- Value 数组/容器强制 value-initialize 防止 NaN-box 未初始化内存被误判

### 生命周期安全

- 不持有外部对象的裸指针，改用 `std::optional` 按值拷贝或 `std::shared_ptr` 共享所有权
- unique_ptr 管理 QThread（自定义删除器先 quit+wait 再 delete）

---

## 调试一致性

条件断点求值沙箱隔离程序状态（Environment 快照/恢复）；首行断点 pre-execution 检查；双后端暂停语义文档化；VmStepper 双后端分发使用 helper 统一状态机逻辑。

详见 [ADR-005: 调试一致性](adr/ADR-005-debug-consistency.md)

---

## 模块系统

### 导入/导出

```mini
// 全量导入
import "module.mini";

// 选择性导入
import { name1, name2 } from "module.mini";

// 导出
export fun myFunc() { ... }
export class MyClass { ... }
export var MY_CONST = 42;
```

### 模块隔离

非导出的顶层声明（VarDecl/FunDecl/ClassDecl）通过 `ModuleTopLevelRenamer` 重命名为 `__mod_<hash>__<name>` 格式，确保跨模块不可见。VM/IR/Interpreter 三路径行为一致。

---

## 教学面板架构

教学面板采用**注册表 + 惰性加载**架构（R132-A）：

- **元数据单一事实源**：`gui/PanelCatalog` 统一目录维护面板 id/显示名/分类/别名映射（`categories()`/`findById()`/`canonicalPanelId()`），导航树与路由共用同一份数据。
- **惰性工厂注册表**：`Ide::registerLazyTeachingPanels` 启动时仅向 `teachingPanelFactories_[panelId]` 注册工厂 lambda（按 4 个波次 helper 分组），首次访问时才构造面板并加入 `centerStack_`，避免启动时全量构造 42 个面板的子 widget 树/信号连接/高亮器。
- **面板分两类**：动态面板消费 `IdeController` 实时数据（调用栈、变量检查器、字节码轨迹）；静态面板的 Library 数据嵌入 .cpp，不依赖 IdeController（异常流、闭包检查器、IR 变换）。

新增面板的标准流程：PanelCatalog 登记元数据 → 对应波次 helper 中 `registrar(id, 标题, 工厂)` 一行注册，无需改动导航/路由/惰加载机制。

---

*参见 [development.md](development.md) 获取完整开发日志与工程变更记录。*
