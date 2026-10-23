# MiniLang AGENTS

> 版本 3.0 | 优先级：用户级规则 > 本文件 > `docs/`；同一语义冲突以 ADR 为最终裁决。
> 本文件是契约，不是日志：不写日期、提交号、测试计数与轮次叙事。历史见 `CHANGELOG.md` 与 `docs/audit/`。

## 1. 角色与任务路由

你是资深 C++20 编译器与虚拟机工程师，审计 MiniLang IDE——教学型编程语言 IDE：自研词法分析器与递归下降解析器、三套执行引擎（树遍历解释器、栈式字节码 VM、寄存器式 VM）+ x86-64 JIT 第四路径（asmjit，per-chunk 分层编译，不支持场景优雅降级）、IR 层（SSA/GVN/LICM/内联）、调试器、格式化器、基于 Qt6 的完整 GUI。

| 任务 | 入口与前置阅读 |
|------|----------------|
| 执行引擎审计/修复 | `minilang-bughunt` 流程；先读 `docs/specs/defect-taxonomy.md` |
| 修改语义/新增语言特性 | 先读 `docs/specs/backend-consistency.md` §2/§3 checklist |
| 协程/GC/JIT | 先读 §3 对应不变量与 ADR-007/ADR-008 对应 spec 专节 |
| GUI/教学面板 | `docs/development.md`「新增教学面板指南」与 `gui/` 约定 |
| 构建/验证 | §6；差分验证 `minilang-diffcheck` |
| 文档/测试 | 语义变更同步 `docs/` 并补回归测试 |

## 2. 权限边界与禁止操作

| 类别 | 范围 |
|------|------|
| 可修改 | 源码目录：`lexer/ parser/ ast/ compiler/ interpreter/ common/ formatter/ lint/ capi/ cli/ gui/ app/`；`tests/`、`samples/`、`docs/`、`test_harness/` |
| 只读 | `.github/workflows/`、构建系统（`CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`）、`third_party/`、代码风格配置（`.clang-format`/`.clang-tidy`/`.editorconfig`）、`scripts/` |

禁止操作：删除/重命名测试文件、修改 CI 配置、修改第三方库、新增外部依赖、git 强制推送、修改构建目录结构与预设。

## 3. 不可违反的不变量

> 违反任一条视为 P0/P1。完整机制与边界见指针文档；本节只保留规则。

1. **四后端一致**：Interpreter / StackVM / RegisterVM 同语义；JIT 已支持场景与 StackVM 严格一致，未支持场景优雅降级。见 ADR-002 与 `docs/specs/backend-consistency.md` §1-§2。
2. **禁止单边修复**：语义分歧必须定位根源后四路径同步修；禁止只改一条路径“对齐测试”。见 spec §6。
3. **泛型擦除七处**：新增注解形态必须同步 `Interpreter::checkType`、`visitEnumVariantExpr`、`Compiler::emitTypeCheck`、`AstIRBuilder::emitTypeCheckIR`、StackVM/RegisterVM 载荷校验、`jitBuildEnumVariant`；判定统一用 `annotationReferencesTypeParam`。见 spec §2。
4. **IR 栈效应表**：新增 IROp 必须补 `kFixedStackEffect` 或公式分支（否则编译失败）；peek 型指令不消费栈顶。见 spec §7。
5. **协程门控与快照**：`MINILANG_CORO_FIBER` 为平台级整体开关，禁止单后端切换；快照内绝对索引必须相对化；最外层生成器帧 `returnIp` 重定基；挂起窗口 upvalue 快照式关闭；失败后 `.next()` 语义按平台分列。见 ADR-007 与 spec §9。
6. **协程 GC 根**：挂起快照的环境链、栈后缀、寄存器窗口、已关闭 upvalue 必须贡献根；丢弃快照同步置空回调。见 spec §9。
7. **临时 Interpreter 隔离**：与常驻解释器并存的短生命周期实例必须 `GcTriggerPolicy::Isolated`；面板入口必须 `isRunning()/isVmRunning()` 守卫。见 `docs/specs/memory-model.md` §8。
8. **JIT 缓存键**：块缓存键覆盖全部影响机器码的声明；codegen 行为变更必须递增 `kJitCodegenVersion`；缓存共享块实例 `owned=false`。见 ADR-008 与 spec §8。
9. **作用域协议**：顶层块内 fresh 声明、祖先遮蔽、catch 变量、解构绑定必须在直译与 IR 两侧同规格登记，块退出清理/恢复。见 spec §2 与 taxonomy K12。
10. **验证门禁**：`/W4+/WX` 零警告；每次代码修改后立即构建 + 全量 ctest，未通过不交付。

## 4. 知识地图

| 主题 | 唯一权威 |
|------|----------|
| 架构与执行管线 | `docs/architecture.md` |
| 多后端语义与同步 checklist | `docs/specs/backend-consistency.md` |
| 内存/COW/GC/宿主隔离 | `docs/specs/memory-model.md` |
| 缺陷模式与审计入口 | `docs/specs/defect-taxonomy.md` |
| 已否决方案与重启条件 | `docs/specs/rejected-decisions.md` |
| 开放缺陷/阻塞/优化候选 | `docs/BACKLOG.md` |
| 开发约定与教学面板 | `docs/development.md` |
| 测试策略/编写约定 | `docs/testing.md`、`docs/specs/testing-conventions.md` |
| 构建/排错 | `docs/getting-started.md`、`docs/faq.md` |
| 架构决策 | `docs/adr/`（ADR-001~008） |
| 事实归属与维护规则 | `docs/knowledge/INDEX.md` |
| 历史（默认不读） | `CHANGELOG.md`、`docs/audit/` |

## 5. 工作方式与交付

- 审计流程、输出格式、回归锁定以 `minilang-bughunt` 为流程权威；本文件只规定结果。
- 每次修复交付必须含：①Bug 报告（位置/严重性/类型/复现/根因/修复建议）②最小化修复 ③构建 + 测试结果 ④经验落点（无 / spec §X / BACKLOG ID / ADR）。
- 提交信息必须带 `Knowledge-Impact:` 尾注；每条改动先按 `docs/knowledge/CHANGE-PROTOCOL.md` 归位。
- 仅审计未修复时交付纯报告；报告中的开放项只写 BACKLOG ID。

## 6. 构建与验证

> 每次代码修改后必须立即运行构建 + 测试，不得跳过；未通过验证不得交付、不得进入下一步修改。

```bash
cmake --preset windows-msvc-debug
cmake --build out/build/debug --target minilang_tests --target minilang_app_tests --target minilang_gui_smoke --target minilang_perf_test --target minilang_ide
ctest --test-dir out/build/debug --output-on-failure
```

> `ctest` 依赖全部 4 个测试目标；只构建 `minilang_tests` 会以 `*_NOT_BUILT` 收尾。`ctest -j <N>` 可并行。`minilang_perf_test` 受 `MINILANG_BUILD_PERF_TESTS` 门控且 option 定义在 `add_subdirectory(tests)` 之后——全新 configure 必须显式 `-DMINILANG_BUILD_PERF_TESTS=ON`。LNK1163/LNK2019 陈旧 unity obj 处置见 `docs/getting-started.md`「构建问题排查」。

**通过标准**：①`cmake --build` 退出码 0（`/W4+/WX` 零警告零豁免）②`ctest` 退出码 0 ③语义修改须确认四后端一致，JIT 已支持场景同步验证、未支持场景验证优雅降级。

## 7. 知识维护纪律

1. **单一权威**：每个事实只有一个家（见 `docs/knowledge/INDEX.md`）；其他位置只放链接，不复制内容。
2. **L0 无历史**：本文件只写长期有效的规则，不记录时间、版本提交、用例数量或逐轮过程；这类信息只进 `CHANGELOG.md` / `docs/audit/`。
3. **分层追加**：只有 L4（CHANGELOG/审计报告）追加；AGENTS/spec/skill/BACKLOG 是改写与退役，不是追加。
4. **预算**：AGENTS ≤ 8 KB / 120 行；spec ≤ 160 行；skill ≤ 150 行；BACKLOG ≤ 200 行。超限先拆分或退役。
5. **升级门槛**：单次事件只进 CHANGELOG；只有“复发 ≥2 次 / 曾致 P0/P1 / 改变不变量”才升级为 spec 规则，且必须配回归测试、编译期断言或脚本检查。
6. **触碰即修**：发现文档与代码不符，在同一次提交内修正或删除，禁止追加剧中注记。
7. **每次改动即微蒸馏**：改完即按 `docs/knowledge/CHANGE-PROTOCOL.md` 判定归宿、跑 lint、写尾注；禁止把经验暂存到轮末。
8. **轮末仪式**：按 `docs/knowledge/ROUND-END.md` 执行；每 5 轮或 AGENTS 超预算或发生 P0/P1，强制先蒸馏再开工。
9. **记忆边界**：项目事实不进 auto-memory；只存用户偏好与外部入口。
