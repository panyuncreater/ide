---
name: "minilang-diffcheck"
description: "MiniLang 三后端差分一致性验证与 JIT 降级验证操作。当修改涉及执行语义、或用户要求确认 Interpreter/StackVM/RegisterVM 行为一致时自动调用。"
---

# MiniLang 差分一致性验证操作手册

本 Skill 提供三后端（Interpreter / StackVM / RegisterVM）差分验证与 JIT 降级验证的**可执行操作步骤**。方法论与套件矩阵见 `docs/testing.md`，同步约定见 `docs/specs/backend-consistency.md`；本 Skill 只写命令与陷阱，不重复理论。

## 1. 差分矩阵（语义修改必跑）

```bash
cmake --preset windows-msvc-debug
cmake --build out/build/debug --target minilang_tests
ctest --test-dir out/build/debug -R "ThreeEngines|VME2E|TestJIT|TestJITCoverageGaps|TestAuditBatch" --output-on-failure
```

| 套件 | 覆盖范围 | 文件 |
|------|---------|------|
| `TestThreeEngines*` | 三后端差分（6 组合矩阵 + 随机程序回归） | `tests/TestThreeEnginesConsistency.cpp`、`TestThreeEnginesFuzz.cpp` |
| `VME2E*` | 三后端端到端语义一致性（含条件断点、模块隔离） | `tests/TestVME2E.cpp` |
| `TestJIT*` | JIT 与 StackVM 一致性 + 降级断言 | `tests/TestJIT.cpp`、`TestJITCoverageGaps.cpp` |
| `TestAuditBatch*` | 批量综合审计（编译器/IR/调试/四后端/内建方法） | `tests/TestAuditBatch*.cpp` |

## 2. 快速定位不一致

1. **从套件收敛到用例**：
   ```bash
   ctest --test-dir out/build/debug -R "TestThreeEnginesConsistency" --verbose
   # 或用用例级过滤：
   ctest --test-dir out/build/debug -R "套件名\.用例名" --output-on-failure
   ```
2. **独立复现**：`minilang-fuzz --mode file <path>` 单跑 .mini 文件（`cli/fuzz_core.h` `processFile`），比对期望输出。
3. **C API 后端切换**：`capi/minilang_capi.h` `set_backend` 编程式切换三后端，同一输入对比输出。
4. **组合开关确认**：RegisterVM 相关失败先确认测试是否启用 `setVM(true)` + `setUseRegisterVM(true)` + `setIROptimize(true)` 组合（`docs/testing.md` L19-20），避免漏测 IR 优化路径。

## 3. JIT 降级验证

- **构建**：JIT 为 x86-64 默认启用（`MINILANG_USE_JIT=ON`）；验证降级需确保该开关生效。
- **断言语义**：
  - 已支持场景：断言 JIT 与 StackVM **严格一致**；
  - 未支持场景（闭包、模块系统、并发原语、字符串方法、enum variant 等，见 `TestJITCoverageGaps.cpp` 14 用例）：断言 **StackVM 正确 + JIT 优雅降级不崩溃不错值**。
- **收紧方向**：JIT 补全实现后，`TestJITCoverageGaps` 对应断言自动收紧为一致性断言（缺口清单即降级断言清单）。

## 4. 已知陷阱（假失败排查）

1. **stale binary 假失败**：`build_debug/` 旧产物未重新构建时测试结果不可信——**先重新构建再测**（历史：模块缓存污染曾被误判为 Bug，实为 stale binary，见 `docs/testing.md` L253）。
2. **测试污染**：模块缓存跨用例污染导致偶发失败——关注 `Compiler::compileViaIR` / `compileViaRegisterIR` 状态重置完整性（`moduleLoadingStack_` / `moduleExports_` 清理，三路径必须对称）。
3. **用例顺序依赖**：gtest 默认用例顺序执行，依赖共享状态的用例可能受前序影响——失败用例单独跑验证（`--gtest_filter=套件.用例`）。
4. **仅单后端红**：先确认是「四后端都错（语义定义问题）」还是「单后端错（实现不对称）」，修复策略完全不同（见 `minilang-bughunt` §3）。
5. **diff_test 不在 ctest 口径**：该独立可执行未注册为 ctest 用例（BACKLOG B-007），V10/V11 曾因此逃过全量回归——语义修改后须手动运行 `out/build/debug/test_harness/diff_test.exe`（或对应构建产物）。

## 5. 输出

验证结束输出一份简短结论：
- 矩阵结果：每个套件 通过/失败 + 失败用例列表
- 差异面结论：哪个后端 × 哪个语义错
- 是否涉及 JIT 降级断言变化
- 结论：可交付 / 需修复（转 `minilang-bughunt`）
