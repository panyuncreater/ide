# MiniLang 开发指南

本文档是 MiniLang IDE 开发文档的导航入口。详细的架构设计、决策记录与测试方法请参见各子文档。

## 文档导航

| 文档 | 说明 |
|------|------|
| [快速开始](getting-started.md) | 构建、运行、开发环境配置（Windows/Linux/macOS/Docker） |
| [核心架构](architecture.md) | 编译管线、NaN-boxing、三后端、内存模型、安全约束 |
| [测试指南](testing.md) | 三后端一致性验证方法论、测试分类与编写指南 |
| [设计决策 (ADR)](adr/) | 架构决策记录，详见下方索引 |
| [贡献指南](../CONTRIBUTING.md) | 环境搭建、编码规范、PR 流程 |

## 设计决策记录 (ADR)

| ADR | 主题 | 状态 |
|-----|------|------|
| [ADR-001](adr/ADR-001-nan-boxing.md) | NaN-boxing 值表示 | Accepted |
| [ADR-002](adr/ADR-002-triple-backend.md) | 三后端并存策略 | Accepted |
| [ADR-003](adr/ADR-003-ir-layer.md) | IR 中间层 | Accepted |
| [ADR-004](adr/ADR-004-cow-memory.md) | COW 内存模型 | Accepted |
| [ADR-005](adr/ADR-005-debug-consistency.md) | 调试一致性 | Accepted |

## 核心设计原则

- **PERF-12 NaN-boxing**：8 字节值编码，标量零分配，堆类型侵入式引用计数
- **PERF-13 VMStack**：定长数组 1024×8B + 栈顶指针，消除 push_back 开销
- **PERF-14 寄存器式 VM**：32 虚拟寄存器 R0-R31，50+ RegOp，IR 直接 lowering
- **PERF-15 IR 复制传播**：vreg→常量等价替换，仅寄存器后端启用
- **COW 优化**：修改前检查独占所有权，避免不必要的深拷贝

## 安全约束摘要

| 类别 | 措施 |
|------|------|
| DoS 防护 | 源码 ≤10MB、Token ≤100 万、循环 ≤1000 万次 |
| 递归保护 | Parser/Compiler/Formatter 深度限制 512/256 |
| 线程安全 | mutex + atomic、localtime_s/localtime_r |
| 异常安全 | UI 槽 try/catch、unique_ptr 防 bad_alloc 泄漏 |
| 内存安全 | 运行时越界检查、NaN-box 类型前置校验、value-initialize |
| 生命周期 | 无裸指针、shared_ptr 共享所有权、QThread unique_ptr 管理 |


完整开发日志请参阅 [CHANGELOG.md](../CHANGELOG.md) 及 [归档目录](changelog/archive/)。
