---
name: "minilang-bughunt"
description: "MiniLang 执行引擎/语义 Bug 排查与修复全流程。当用户要求排查三后端语义不一致、修复崩溃/错值/栈不平衡，或按 AGENTS.md 审计交付修复时自动调用。"
---

# MiniLang Bug 排查与修复全流程

本 Skill 是 Bug 审计/修复流程与交付格式的权威：**复现 → 定位 → 修复 → 验证 → 回归锁定 → 交付**。构建命令与通过标准以 `AGENTS.md` §6 为准；缺陷分类见 `docs/specs/defect-taxonomy.md`。

## 1. 复现（Reproduce）

**目标**：用最小用例稳定触发 Bug，并确定差异面（哪个后端对、哪个后端错）。

1. **最小化**：删除无关代码，保留触发 Bug 的最小语句集；优先内嵌到 gtest 字符串（项目测试惯例，如 `TestVME2E.cpp` 内嵌 MiniLang 源码），避免依赖外部 .mini 文件。
2. **三路径复现**（确定差异面）：
   - 差分工具：`minilang-fuzz --mode file <path>` 文件模式单跑（`cli/fuzz_core.h`：`processFile` 文件输入模式）
   - 测试矩阵：`ctest --test-dir out/build/debug -R "ThreeEngines|VME2E" --output-on-failure`
   - C API 后端切换：`capi/minilang_capi.h` `set_backend`（三后端可编程切换）
3. **记录**：期望输出 vs 各后端实际输出，明确「哪条路径正确、哪条路径错误、错在哪一步」。

> 复现不成立时（无法稳定触发），先检查是否为测试污染/stale binary（见 `minilang-diffcheck` §4 已知陷阱），再回到最小化。

## 2. 定位（Root Cause）

1. **按清单对号入座**：对照 `docs/specs/defect-taxonomy.md` 的 15 类缺陷，命中类别后直接进入对应审计入口：
   - IR 栈不平衡 → `compiler/ir/BytecodeIRBackend.cpp` 各 `visit*` emit 收尾
   - 四后端不一致 → 同语义四实现 diff（`interpreter/` vs `compiler/backend-stack/VM.cpp` vs `compiler/backend-reg/RegisterVM.cpp`）
2. **差分定位**：同一语义在 Interpreter / StackVM / RegisterVM 三处实现的逐分支对比，先找「哪条路径缺了分支」。
3. **资源生命周期**：RAII guard / shared_ptr / raw pointer 获取与释放配对（AGENTS.md §5 策略 4），重点 `interpreter/RefCounted.h` 侵入式计数与 `GcManager` 交互。
4. **栈/寄存器平衡**：每个 emit 路径 push/pop 配对审计（策略 5）。
5. **错误传播**：错误标志在每个检查点是否被吞（策略 7）。

## 3. 修复（Fix）

1. **最小化改动**：仅修目标 Bug，不顺手重构（AGENTS.md §5 交付格式要求）。
2. **三后端同步原则**：语义修复必须三后端（+IR 双路径）同步落地；**单边修复破坏一致性**——历史案例：upvalue 快照语义曾因单边修改方案被实测推翻（`docs/testing.md` AUDIT-R5 R2，三后端本就一致，单边改反破坏）。
3. **防御顺序**：先修根因，再补边界（0/负数/空/NAN）。

## 4. 验证（Verify）

按 `AGENTS.md` §6 验证标准执行：

```bash
cmake --preset windows-msvc-debug
cmake --build out/build/debug --target minilang_tests
ctest --test-dir out/build/debug --output-on-failure
```

- **定向验证**：`ctest --test-dir out/build/debug -R "受影响套件" --output-on-failure`，先绿定向再跑全量
- **三后端矩阵 + JIT 降级**：见 `minilang-diffcheck`（语义修改必做）
- 未通过验证的代码不得交付、不得进入下一步修改（`AGENTS.md` §6）

## 5. 回归锁定（Lock）

修复必须携带回归测试，**无锁定不交付**：

| 语义类别 | 测试命名约定 | 落点参考 |
|---------|-------------|---------|
| 三后端一致性 | `ConsistencyDiff.*` / `TestThreeEngines*` | `tests/TestThreeEnginesConsistency.cpp`、`TestThreeEnginesFuzz.cpp` |
| VM 端到端语义 | `VME2E*` | `tests/TestVME2E.cpp` |
| 审计类核查 | `Test<模块>Audit*` | `tests/TestCompilerAudit.cpp` 等 |
| 批量综合审计 | `TestAuditBatch*` | `tests/TestAuditBatch*.cpp` |
| JIT 一致性/降级 | `TestJIT*` | `tests/TestJIT.cpp`、`TestJITCoverageGaps.cpp`（14 用例降级锁定） |

锁定测试必须**先验证能捕获旧缺陷**（改回 Bug 代码应红），再随修复一起交付。

## 6. 交付（Deliver）

按本 Skill 的交付格式输出：
1. Bug 报告：位置 / 严重性（P0/P1/P2）/ 类型 / 复现场景 / 根因分析 / 修复建议
2. 代码修复：最小化改动
3. 验证结果：构建 + 定向 + 全量 + 四后端矩阵结果，逐项给出通过/失败
4. 经验落点：无 / spec §X / BACKLOG ID / ADR（语义变更必填）

纯审计（未实施修复）时交付纯报告即可。

## 7. 快速检查清单

- [ ] 复现用例最小且三后端差异面明确
- [ ] 根因落在已知 Bug 模式清单的哪个类别
- [ ] 修复为最小改动且三后端同步
- [ ] 定向测试 → 全量测试 → 三后端矩阵 → JIT 降级全部通过
- [ ] 回归测试已锁定且能捕获旧缺陷
- [ ] 交付格式完整（报告/修复/验证）
