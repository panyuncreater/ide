# MiniLang 开发指南

本文档面向参与 MiniLang 开发与维护的开发者，记录项目的工程约定、设计决策与性能优化策略。

## 核心设计原则

### PERF-12 NaN-boxing

Value 类型使用 8 字节 NaN-boxing 编码（`sizeof` 从 24 字节降至 8 字节），标量（int/float/bool/null）内联存储零原子操作，堆类型通过侵入式引用计数（`RefCounted` 基类）管理。

### PERF-13 VMStack

VM 操作数栈使用定长数组（1024 x 8B = 8KB）+ 栈顶指针替代 `std::vector`，消除 `push_back` 容量检查和堆分配开销。

### PERF-14 寄存器式 VM

双后端并存策略，新增独立 RegisterBytecode（50+ RegOp，32 虚拟寄存器 R0-R31）+ RegisterBytecodeBackend（IR 三地址码直接 lowering）+ RegisterVM 解释循环，通过 `setUseRegisterVM(true)` 启用；默认关闭，栈式 VM 完整保留。

### PERF-15 IR 复制传播

`copyPropagationPass` 完整实现，记录 vreg->常量等价关系并替换后续引用；仅在寄存器式后端启用（`optimizeIR` 新增 `enableCopyPropagation` 参数），栈式后端禁用避免删除 LOAD_CONST 导致栈下溢。

### COW 优化

Value 类型使用 Copy-On-Write，堆类型修改前检查独占所有权，避免不必要的深拷贝。

## 安全约束

### DoS 防护

源码 <=10MB、Token <=100 万、循环 <=1000 万次、字符串 reserve <=1MB。

### 递归保护

Parser/Compiler/Formatter/equals() 均有深度限制（512/256）。

### 线程安全

DebugController 使用 mutex + atomic，Worker 线程通过 Qt 信号回传；`std::localtime()` 等非线程安全函数替换为 `localtime_s` (Windows) / `localtime_r` (POSIX)。

### 异常安全

UI 槽函数包裹 try/catch，确保异常时回滚 UI 状态；深拷贝操作使用 `std::unique_ptr` 包裹防止 bad_alloc 泄漏。

### 内存安全

所有 assert 检查的越界/类型保护在 Release 构建中使用运行时检查（`std::abort` / `runtimeError`）；Value 数组/容器强制 value-initialize 防止 NaN-box 未初始化内存被误判为合法 float。

### 防御性编程

封闭枚举 switch 必须加 default 分支；边界检查覆盖全深度、全路径、全状态；测试 helper 检查中间错误状态而非仅断言输出。

### 生命周期安全

不持有外部对象的裸指针（编译器结果、实例绑定等），改用 `std::optional` 按值拷贝或 `std::shared_ptr` 共享所有权；QThread::terminate 后立即退出进程（损坏状态无法安全析构）。

### 资源管理

unique_ptr 管理 QThread（自定义删除器先 quit+wait 再 delete）。

### 安全性硬约束

所有 assert 检查的越界/类型保护改为运行时 `std::abort`；NaNBox 类型访问前必须用 `isXxx()` 校验；容器越界错误路径强制走 const 访问器避免 COW 深拷贝；`std::localtime()` 替换为线程安全的 `localtime_s`/`localtime_r`。

## IR 中间层

可选 AST -> IR -> Bytecode 三段式编译，IRBuilder/IRBackend 抽象接口可扩展多后端；启用 `setIR(true)` + `setIROptimize(true)` 后在 lowering 前执行常量折叠 / 死代码消除 pass（最多 3 轮迭代）；寄存器式后端额外启用复制传播（`setUseRegisterVM(true)` + `setIROptimize(true)`）。

## 调试一致性

条件断点求值沙箱隔离程序状态（Environment 快照/恢复）；首行断点 pre-execution 检查；双后端暂停语义文档化；VmStepper 双后端分发使用 helper 统一状态机逻辑。
