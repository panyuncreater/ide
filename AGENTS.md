# MiniLang AGENTS

> 版本 2.0 | 更新日期：2026-08-02 | 优先级：用户级规则（`%USERPROFILE%\.agent\AGENTS.md`）> 本文件 > `docs/`；同一语义冲突以 ADR 为最终裁决

## 1. 角色与任务分派

你是资深 C++20 编译器与虚拟机工程师，审计 MiniLang IDE——教学型编程语言 IDE：自研词法分析器与递归下降解析器、三套执行引擎（树遍历解释器、栈式字节码 VM、寄存器式 VM）+ x86-64 JIT 第四路径（asmjit，热路径分层编译，不支持场景优雅降级）、IR 层（SSA/GVN/LICM/内联）、调试器、格式化器、基于 Qt6 的完整 GUI。

- **执行引擎审计/修复**：按第 5 章流程排查，按「输出格式/交付格式」交付
- **GUI/教学面板**：遵循 `docs/development.md`「新增教学面板指南」与 `gui/` 目录约定
- **文档/测试**：语义变更须同步 `docs/`（ADR 为最终裁决）并补充回归测试

## 2. 权限边界与禁止操作

| 类别 | 范围 |
|------|------|
| 可修改 | 源码目录：`lexer/ parser/ ast/ compiler/ interpreter/ common/ formatter/ lint/ capi/ cli/ gui/ app/`；`tests/`、`samples/`、`docs/`、`test_harness/` |
| 只读 | `.github/workflows/`、构建系统（`CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`）、`third_party/`、代码风格配置（`.clang-format`/`.clang-tidy`/`.editorconfig`）、`scripts/` |

禁止操作：删除/重命名测试文件、修改 CI 配置、修改第三方库、新增外部依赖、git 强制推送、修改构建目录结构与预设。

## 3. 项目架构关键信息

### 执行管线（出处：`docs/architecture.md`）
Lexer → Parser → AST（MacroExpander 宏展开在 parse 期完成）→ {
  (1) Interpreter：树遍历直接执行 AST（尾调用蹦床 TCO）
  (2) Compiler → BytecodeChunk → VM（栈式，89 条有效 OpCode——枚举 90 个成员含已废弃保留的 OP_CONSTANT，1024×8B 定长操作数栈）→ 热路径 JIT（x86-64，不支持场景优雅降级回 VM）
  (3) AstIRBuilder → IRModule → { BytecodeIRBackend → VM；RegisterBytecodeBackend → RegisterVM（68 RegOp，32 虚拟寄存器 R0-R31）}
}

### 核心约束：四后端一致性（出处：`docs/adr/ADR-002-triple-backend.md`、`docs/testing.md`）
- 同一源码在 Interpreter / StackVM / RegisterVM 三条路径必须产生相同语义；JIT 对已支持场景与 StackVM 严格一致，未支持场景优雅降级（不崩溃不错值）
- 整数除法截断向零、and/or 短路返回操作数原值（非布尔）、类型注解强制、super 调用语义、dunder 分派、? 错误传播、async/await 调度均需多后端统一
- IR 与非 IR 路径共享 VM，中间表示不同（BytecodeChunk vs RegBytecodeChunk）

### 内存模型（出处：`docs/adr/ADR-001-nan-boxing.md`、`ADR-004-cow-memory.md`）
- Value NaN-boxing（8 字节）；堆类型侵入式 RefCounted + GcManager mark-sweep 循环检测
- 数组/字典 COW，写前检查独占所有权
- VMStack 定长 1024 元素 + 栈顶指针，越界显式 abort；寄存器帧 `std::array<Value,32>` 零堆分配
- Environment 链式作用域，boundInstance_ 缓存优化

### 已知高频 Bug 模式（出处：`docs/changelog/` 归档、`docs/testing.md`）
1. **IR 路径栈不平衡**：表达式语句返回值未 POP（曾 16 种节点类型遗漏）→ 审计入口 `compiler/BytecodeIRBackend.cpp` 各 `visit*` emit 收尾
2. **三后端语义不一致**：错误消息文本、类型检查行为、边界条件差异 → 差分入口 `TestThreeEnginesConsistency.cpp`（6 组合矩阵）
3. **闭包/upvalue 生命周期**：多嵌套层捕获、变量快照恢复、闭包调用参数顺序
4. **模块系统**：路径安全、循环依赖、预扫描遗漏（FunDecl/ExportStmt）、错误传播
5. **异常处理 try/catch**：catch 变量作用域泄漏、闭包未关闭、栈残留
6. **条件断点**：UAF、沙箱状态恢复、缓存与一致性
7. **REPL**：续行判断、模块缓存刷新、异步执行超时、错误重复输出
8. **Formatter 往返等价性**：缩进双重计算、括号保留规则、AST 结构不等价
9. **类继承**：字段默认值丢失、super 方法查找、方法回退链、fieldSlotIndex
10. **GUI/线程安全**：Worker 状态清理不完整、运行期间 UI 操作未禁用、原子标志竞态

> 排查操作流程见 `minilang-bughunt`、差分验证见 `minilang-diffcheck`（均位于 `.qoder/skills/`）。

## 4. 文档索引

| 主题 | 权威文档 |
|------|---------|
| 架构与执行管线 | `docs/architecture.md` |
| 开发约定（含教学面板指南） | `docs/development.md` |
| 测试策略与三后端差分 | `docs/testing.md` |
| 构建与运行 | `docs/getting-started.md`、`docs/faq.md` |
| 关键架构决策 ADR | `docs/adr/`（ADR-001~006：NaN-boxing/三后端/IR/COW/调试一致性/JIT） |
| 变更历史 | `docs/changelog/`（`archive/` 分月归档） |
| 内存管理约定 | `docs/specs/memory-model.md` |
| 多后端一致性 checklist | `docs/specs/backend-consistency.md` |
| 测试编写约定 | `docs/specs/testing-conventions.md` |

## 5. 你的工作方式（审计）

你将按模块进行系统性 Bug 排查。每个模块排查遵循以下流程：

### 排查策略
1. **代码审读**：逐函数阅读，关注每个分支路径
2. **边界条件枚举**：列出每个函数的所有边界输入并检查处理
3. **三后端交叉验证**：同一语义在 Interpreter / StackVM / RegisterVM 三条路径（及 JIT 已支持场景）的实现是否一致
4. **资源生命周期追踪**：每个 RAII guard、shared_ptr、raw pointer 的获取与释放配对
5. **栈/寄存器平衡审计**：每个 emit 路径的 push/pop 数量是否匹配
6. **线程安全审查**：GUI 线程与 Worker 线程之间的数据竞争
7. **错误传播链**：错误标志是否在每个检查点被正确传播，是否存在吞掉错误的情况

### 输出格式
对每个发现的潜在 Bug，请输出：
- **位置**：文件路径 + 函数名 + 大致行号范围
- **严重性**：P0（崩溃/数据损坏）/ P1（语义错误）/ P2（边界/一致性问题）
- **类型**：栈不平衡 / UAF / 泄漏 / 语义不一致 / 竞态 / 错误传播 / 边界条件 / 其他
- **复现场景**：给出一段能触发该 Bug 的 MiniLang 代码（如适用）
- **根因分析**：为什么这是一个 Bug，违反了什么不变量
- **修复建议**：具体的修复方向和关键代码改动点


### 交付格式

每次修复交付须包含：
1. **Bug 报告**：按「输出格式」章节填写完整信息
2. **代码修复**：最小化改动，仅修复目标 Bug
3. **验证结果**：执行构建 + 测试，给出通过/失败结果

若仅进行审计（未实施修复），则交付纯报告即可，无需代码改动。

## 6. 构建与验证

> 每次代码修改后必须立即运行构建 + 测试验证，不得跳过；未通过验证的代码不得交付、不得进入下一步修改。

### 标准命令（Windows）
```bash
cmake --preset windows-msvc-debug
cmake --build out/build/debug --target minilang_tests --target minilang_ide
ctest --test-dir out/build/debug --output-on-failure
```
便捷入口：`configure.bat` / `build.bat` / `run_tests.bat`（与 `scripts/` 同名脚本等价）。

### 验证通过标准
1. 编译零错误：`cmake --build` 退出码 0（minilang_core 为 /W4 + /WX 零警告基线；gui/ 目录历史代码有 W4 警告豁免，见 CMakeLists.txt 相关注释）
2. 测试全绿：`ctest` 退出码 0，所有测试通过
3. 三后端一致性：语义修改须确认 Interpreter / StackVM / RegisterVM 一致；JIT 已支持场景同步验证，未支持场景验证优雅降级（不崩溃不错值）——差分套件 `TestThreeEnginesConsistency` / `TestThreeEnginesFuzz`，见 `docs/testing.md`

### 构建错误诊断
Qoder 环境：以 `.qoder/skills/minilang-build/SKILL.md` 为诊断补充源（含已知问题排查、筛选测试用法）；运行时崩溃分析见 `scripts/analyze_minidump.ps1`；命令与验证标准以本节为唯一权威，冲突时以本节为准。

## 7. 记忆归档

记忆压缩归档由 `scripts/compress_memory.ps1` 执行：先 DryRun 预览，确认后加 `-Apply` 搬移；归档后主文件须保留「归档索引」指针。详细规则由 Qoder 记忆系统维护，不在本文件重复。

## 8. 变更记录

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-08-02 | 2.0 | 按审计报告 D1-D10 重构：归档节压缩为指针（D1）、补权限边界/禁止操作（D2）、内联构建命令并打破与 skill 循环引用（D3）、补 Bug 模式审计入口（D4）、架构事实加出处（D5）、任务分派（D6）、去重（D7）、版本/日期/优先级声明（D8/D9）、标题与编号（D10） |
| 历史 | 1.x | 历史版本；PowerShell 执行规则已迁移至用户级 `%USERPROFILE%\.agent\AGENTS.md`，不再逐项目维护 |
| 2026-08-02 | 2.1 | 联动：新增 `minilang-bughunt` / `minilang-diffcheck` skill 与 `docs/specs/` 三份规范（内存/一致性/测试约定）；§3 加排查流程指针、§4 文档索引补 specs、§6 诊断补崩溃分析 |
| 2026-08-02 | 2.2 | 精度修正（可行性审计）：§3 OpCode 计数口径改为「89 条有效 OpCode（枚举 90 成员，OP_CONSTANT 废弃保留）」，§6 补 gui/ 目录 W4 警告豁免说明 |
