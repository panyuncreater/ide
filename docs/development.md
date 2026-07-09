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


## 最近变更摘要

- **第七十轮（2026-07-09）**：UI 修复方案修正——DisableStylesheet 副作用（P1 × 2）。第六十八轮的 DisableStylesheet 虽阻止了 loadStylesheet 覆盖，但导致 ADS 按钮图标丢失（qproperty-icon 规则未执行）和 QToolTip 黑框（回退 Windows 11 原生黑色样式）。改用 `setColorSchemeMode(Light)`：保留构造时 default.css 初始加载（提供图标+基础样式），仅阻止后续 palette-change 重载。adsQss 补全 qproperty-icon 规则，QPalette 设置 ToolTipBase/ToolTipText，qApp 级别追加 QToolTip QSS。全量 1763/1763 测试通过。
- **第六十九轮（2026-07-09）**：REPL %magic 命令系统 5 项 Bug 修复（P1 × 3 + P2 × 2）。P1：(1) magic 命令在 isInputComplete 之前拦截，解决 `%ast fun f() {` 因未闭合括号被误判续行的问题；(2) 分析类命令（%ast/%tokens/%disassemble/%ir）支持参数代码，使用 ScopedAnalysis 临时 Lexer/Parser/Compiler 分析参数，不依赖 IdeController；(3) %reset 真正重置 REPL 环境（新增 Interpreter::resetReplEnvironment() 重建 globalEnv_、清空所有缓存）。P2：handleTokens idx 列对齐 off-by-one 修复；help 提示过时文本更新。全量 1763/1763 测试通过。
- **第六十八轮（2026-07-09）**：UI 三大顽疾真正根因修复——ADS loadStylesheet 覆盖（P1 × 3）。**注：本轮的 DisableStylesheet 方案在第七十轮被 setColorSchemeMode(Light) 替代**，因 DisableStylesheet 过于激进导致按钮图标丢失和 QToolTip 黑框回归。根因分析（setPalette→ApplicationPaletteChange→loadStylesheet 覆盖）正确，但修复方案 DisableStylesheet 有副作用，已修正。全量 1758/1758 测试通过。
- **第六十七轮（2026-07-09）**：UI 三大顽疾修复尝试（P1 × 2 + P2 × 1）——**本轮修复实际无效，根因分析错误**。归咎于 QFluentKit registerWidget 覆盖 itemViewQss，但真正根因是 ADS loadStylesheet（见第六十八轮）。移除了 5 个控件的 registerWidget 调用 + 添加 ADS tab QLabel 子选择器规则 + QTimer::singleShot(0) restoreState。全量 1758/1758 测试通过。

完整开发日志请参阅 [CHANGELOG.md](../CHANGELOG.md) 及 [归档目录](changelog/archive/)。
