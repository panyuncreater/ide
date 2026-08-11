# MiniLang 测试编写约定（Spec）

> 本文档定义测试文件的**命名、划分、注册与回归锁定规则**，供新增/修改测试时遵循。
> 测试套件全景、矩阵与统计见 `docs/testing.md`；本文件只写「怎么写」，不重复「测什么」。

## 1. 文件命名约定

测试文件位于 `tests/`（根 CMake 目标 `minilang_tests`，注册见 `tests/CMakeLists.txt`），命名规则：

| 前缀 | 语义 | 示例 |
|------|------|------|
| `Test<模块>` | 模块功能测试 | `TestLexer.cpp`、`TestCompiler.cpp`、`TestFormatterAudit.cpp` |
| `Test<模块>Audit` | 审计类核查（静态/批量核查既有行为） | `TestCompilerAudit.cpp`、`TestDebugAudit.cpp`、`TestIRAudit.cpp` |
| `TestThreeEngines*` | 三后端差分一致性（矩阵/随机） | `TestThreeEnginesConsistency.cpp`、`TestThreeEnginesFuzz.cpp` |
| `TestAuditBatch*` | 批量综合审计（多模块打包） | `TestAuditBatch1-5.cpp` |
| `VME2E*` | VM 端到端语义（套件名以 `VME2E` 开头） | `tests/TestVME2E.cpp` |
| `TestJIT*` | JIT 一致性/降级 | `TestJIT.cpp`、`TestJITCoverageGaps.cpp` |
| `Test*Probe` | 特定语义探针（单点边界） | `TestNestedLvalueProbe.cpp`、`TestMethodCallProbe.cpp` |
| `Test*Audit*.cpp`（教学面板） | 教学面板数据完整性 | `TestTeachingPanelsAudit1-5.cpp` |

**规则**：
- 新增测试优先放入既有相关文件；仅当主题独立时才新建文件。
- 文件内用例命名：套件名用类名（`TEST(ClassName, CaseName)`），`ClassName` 语义化归类（如 `VME2EImport`、`ConsistencyDiff`、`CompilerAudit`）。

## 2. 测试类别划分

| 类别 | 定位 | 适用场景 | 数量控制 |
|------|------|---------|---------|
| 功能测试 | 单模块正确性 | 新特性基础行为 | 随特性增长 |
| 端到端（E2E） | 多路径语义一致 | 跨后端/跨模块集成 | 每特性必配 |
| 差分（Consistency/Fuzz） | 三后端一致 | 语义变更、回归 | 修复必配 |
| 审计（Audit） | 批量核查既有代码 | 系统性排查轮 | 按审计批次 |
| 探针（Probe） | 单点边界条件 | 疑难语义澄清 | 小而专 |
| 压力（stress/diff_test 等 harness） | 大文件/深嵌套/随机 | `test_harness/` 独立程序 | 独立目标 |

> 判别口诀：**新特性先功能后 E2E；修复必带差分/回归；审计走 Audit 批次；单点疑难用 Probe。**

## 3. 三后端矩阵选择规则

- 语义相关测试**默认**覆盖 Interpreter / StackVM / RegisterVM 三路径；RegisterVM 路径需启用组合开关（`setVM(true)` + `setUseRegisterVM(true)` + `setIROptimize(true)`，见 `docs/testing.md` L19-20）。
- JIT 相关断言：已支持场景断言一致，未支持场景断言优雅降级（见 `docs/specs/backend-consistency.md` §4）。
- 边界值覆盖：0、负数、空容器、NAN/Infinity、深嵌套、超长字符串（与 Spec 矩阵规则一致）。

## 4. 回归锁定规则

1. **修复必锁**：任何语义/崩溃修复必须携带回归测试，命名对应语义类别（`ConsistencyDiff.*` / `VME2E*` / `Test<模块>Audit*`）。
2. **先红后绿**：锁定测试先验证能捕获旧缺陷（改回 Bug 代码应失败），再随修复交付。
3. **历史教训引用**：复杂语义锁定测试应注释引用决策出处（如 upvalue 快照案例注释引用 `docs/testing.md` AUDIT-R5 R2），避免后人误改。

## 5. 注册与运行

- **注册**：新测试文件加入 `tests/CMakeLists.txt` 的 `minilang_tests` 源列表；新建独立 harness 程序见 `test_harness/CMakeLists.txt` 模式。
- **构建**：`cmake --build out/build/debug --target minilang_tests`
- **运行**：
  - 全量：`ctest --test-dir out/build/debug --output-on-failure`
  - 套件过滤：`ctest --test-dir out/build/debug -R "套件名" --output-on-failure`
  - 用例过滤：`ctest --test-dir out/build/debug -R "套件\.用例" --verbose`（或 `minilang_tests.exe --gtest_filter=套件.用例`）
- **禁用规则**：`MINILANG_BUILD_TESTS=OFF` 可关闭测试构建（`docs/getting-started.md` 选项表）；平台特定用例用 gtest guard（如 Windows 专用模块路径大小写测试）。
