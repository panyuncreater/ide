# MiniLang 测试指南 / Testing Guide

本文档描述 MiniLang 的三后端一致性验证方法论与测试策略。

---

## 三后端一致性验证

MiniLang 维护三个执行后端，任何语义变更必须确保三后端行为一致。

### 后端矩阵

| 组合 | 配置 | 说明 |
|------|------|------|
| Interpreter | 默认 | 树遍历解释器，语义基准 |
| StackVM | `setVM(true)` | 栈式虚拟机 |
| StackVM + IR | `setVM(true)` + `setIR(true)` | 栈式 VM + IR 中间层 |
| StackVM + IR + Opt | `setVM(true)` + `setIR(true)` + `setIROptimize(true)` | 栈式 VM + IR 优化 |
| RegisterVM | `setVM(true)` + `setUseRegisterVM(true)` | 寄存器式 VM |
| RegisterVM + IR + Opt | 上述 + `setIROptimize(true)` | 寄存器 VM + 全部优化 |

### 验证方法

#### 1. 端到端测试（TestVME2E.cpp）

每个测试用例在三后端上运行相同源码，比较输出：

```cpp
TEST(VME2EBasic, Arithmetic) {
    const char* src = "print(1 + 2);";

    // Interpreter
    Interpreter interp(src);
    auto interpResult = interp.run();

    // StackVM
    VM vm;
    Compiler compiler;
    auto chunk = compiler.compile(src);
    vm.loadChunk(chunk);
    auto vmResult = vm.run();

    // RegisterVM
    RegisterVM regVm;
    // ... 类似流程
    auto regResult = regVm.run();

    EXPECT_EQ(interpResult, vmResult);
    EXPECT_EQ(interpResult, regResult);
}
```

#### 2. 测试套件组织

| 套件 | 文件 | 覆盖范围 |
|------|------|---------|
| `VME2E*` | `TestVME2E.cpp` | 三后端端到端语义一致性 |
| `VMConditionalBreakpoint` | `TestVME2E.cpp` | 条件断点 slot→name 映射 |
| `VME2EImportIsolation` | `TestVME2E.cpp` | 模块隔离三路径一致性 |
| `ThreeEngines*` | `TestThreeEnginesConsistency.cpp` | 三后端差分一致性（6 组合矩阵） |
| `ThreeEnginesFuzz*` | `TestThreeEnginesFuzz.cpp` | 三后端随机程序差分回归 |
| `TeachingPanels*` | `TestTeachingPanelsAudit*.cpp` | 教学面板数据完整性（5 个审计文件） |
| `TeachingPanelsE2E*` | `TestTeachingPanelsE2E.cpp` | 教学面板端到端交互测试 |
| `TeachingTreePanel*` | `TestTeachingTreePanel.cpp` | PanelCatalog + 导航审计 |
| `AuditBatch*` | `TestAuditBatch1-5.cpp` | 综合审计（编译器/IR/调试/三后端/内置方法） |
| `BugHunt*` | `TestBugHuntDifficultyAudit.cpp` / `TestBugHuntVariantAudit.cpp` | BugHunt 难度分级 + 变体挑战 |
| `LearningPath*` | `TestLearningPathAudit.cpp` | 学习路径数据完整性 |
| `MagicCommands*` | `TestMagicCommandsAudit.cpp` | REPL %magic 命令 |
| `Sandbox*` / `TokenPuzzle*` / `AstToy*` | `TestSandboxAudit.cpp` / `TestTokenPuzzleAudit.cpp` / `TestAstToyAudit.cpp` | 交互式教学组件 |
| `IROptReplay*` | `TestIROptReplayAudit.cpp` | IR 优化回放 |
| `MemoryAnim*` | `TestMemoryAnimAudit.cpp` | 内存模型动画数据 |
| `MarkdownRenderer*` | `TestMarkdownRendererAudit.cpp` | Markdown 渲染器 |
| `ErrorHintEngine*` | `TestErrorHintEngine.cpp` | 错误提示引擎 |
| `CodeSnippet*` | `TestCodeSnippetAudit.cpp` | 代码模板系统 |
| `*Gaps*` | `TestCoverageGaps.cpp` / `TestCoverageGaps2.cpp` | 覆盖率缺口定向补充（类型注解/UTF-8/闭包/类方法/enum/try-catch/循环寄存器 + 循环导入深层嵌套/并发原语边界/IR 优化 pass 验证） |

#### 3. IR 优化安全性

IR 优化 pass 不能改变程序语义。验证策略：

- 对每个测试用例，比较 `IR无优化` 和 `IR有优化` 的输出
- 常量折叠：验证折叠结果与运行时计算一致
- 死代码消除：验证消除的代码确实不影响输出
- 复制传播：仅在寄存器后端启用，验证传播后值一致

### 运行测试

```bat
REM Windows：运行全量测试
scripts\run_tests.bat

REM 仅运行特定测试
out\build\debug\minilang_tests.exe --gtest_filter="VME2E*"

REM Linux/macOS
cmake --build out/build/debug --target minilang_tests
./out/build/debug/minilang_tests
```

---

## 测试分类

### 单元层

| 组件 | 测试文件 | 关注点 |
|------|---------|--------|
| Lexer | `TestLexer.cpp` / `TestLexerParserAudit.cpp` | Token 识别、插值字符串拆分、错误恢复 |
| Parser | `TestParser.cpp` | AST 构建、错误消息、嵌套深度限制 |
| Compiler | `TestCompiler.cpp` | 字节码生成、常量池去重、作用域分析 |
| IR | `TestIR.cpp` / `TestIRAudit.cpp` / `TestIROptReplayAudit.cpp` | IR 生成、优化 pass、lowering 正确性 |
| Value | `TestValue.cpp` | NaN-boxing 编解码、值类型转换、边界值 |
| NaNBox | `TestNaNBox.cpp` | 编码/解码、边界值、NaN 规范化 |
| Formatter | `TestFormatterAudit.cpp` | 代码格式化、插值字符串重建 |
| 三后端一致性 | `TestThreeEnginesConsistency.cpp` / `TestThreeEnginesFuzz.cpp` | 6 组合矩阵差分 + 随机程序回归 |
| 解释器探针 | `TestIRShadowFix.cpp` / `TestNestedLvalueProbe.cpp` / `TestMethodCallProbe.cpp` | 特定语义边界条件 |

### 集成层

| 组件 | 测试文件 | 关注点 |
|------|---------|--------|
| Interpreter E2E | `TestInterpreterE2E.cpp` | 解释器端到端语义 |
| VM E2E | `TestVME2E.cpp` | 三后端一致性 |
| 内置方法 | `TestBuiltinMethods.cpp` | 数组/字典/字符串方法 |
| 错误提示 | `TestErrorHintEngine.cpp` | Levenshtein 拼写建议 |
| Magic 命令 | `TestMagicCommandsAudit.cpp` | REPL %magic 命令系统 |
| Markdown 渲染 | `TestMarkdownRendererAudit.cpp` | 教学面板 Markdown 渲染 |

### 审计层

| 测试文件 | 关注点 |
|---------|--------|
| `TestCompilerAudit.cpp` | 编译器边界条件审计 |
| `TestCompilerIRIsolationAudit.cpp` | IR 模块隔离审计 |
| `TestThreeEnginesAudit.cpp` | 三后端综合审计 |
| `TestDebugAudit.cpp` | 调试器一致性审计 |
| `TestAuditBatch1-5.cpp` | 批量综合审计（编译器/IR/调试/三后端/内置方法） |
| `TestTeachingPanelsAudit*.cpp` | 教学面板数据完整性（5 个审计文件） |
| `TestTeachingPanelsE2E.cpp` | 教学面板端到端交互测试 |
| `TestTeachingTreePanel.cpp` | PanelCatalog + 教学树导航审计 |
| `TestBugHuntDifficultyAudit.cpp` / `TestBugHuntVariantAudit.cpp` | BugHunt 难度分级 + 变体挑战 |
| `TestLearningPathAudit.cpp` | 学习路径数据完整性 |
| `TestIROptReplayAudit.cpp` | IR 优化回放验证 |
| `TestMemoryAnimAudit.cpp` | 内存模型动画数据审计 |
| `TestSandboxAudit.cpp` / `TestTokenPuzzleAudit.cpp` / `TestAstToyAudit.cpp` | 交互式教学组件数据 |
| `TestCodeSnippetAudit.cpp` | 代码模板系统审计 |

---

## 独立测试工具

### test_harness/（独立可执行测试）

项目包含 9 个独立于 GoogleTest 的可执行程序（`test_harness/` 目录，约 4100 行），用于特定场景的端到端验证：

| 工具 | 用途 |
|------|------|
| `formatter_audit` | Formatter round-trip 审计 |
| `stress_test` | 压力测试（大文件/深嵌套） |
| `diff_test` | 三后端差分测试 |
| `ast_test` | AST 结构验证 |
| `debug_test` | 调试器端到端测试 |
| `audit_verify` | 审计结果验证 |
| `super_init_test` | super.init() 调用语义 |
| `super_notfound_test` | super 方法未找到错误处理 |
| `test_main` | 通用测试入口 |

test_harness 使用独立的 DebugController stub，不依赖 Qt Widgets，通过 `MINILANG_BUILD_TEST_HARNESS=ON` 启用构建。

### 性能基准测试

`minilang_perf_test` 是独立的性能基准可执行目标（`TestPerformanceRegression.cpp`），与 `minilang_tests` 分离以避免拖慢常规测试。输出 JSON 到 stdout，不依赖 pass/fail 断言。

---

## 测试统计

项目当前包含 **87 个测试 .cpp 文件**，总计 **3452 个测试用例**（截至 2026-07-25）。测试覆盖三后端语义一致性、教学面板数据完整性、前端组件、IR 优化（含 SSA 基础设施）、调试器（含 DAP pause 同步暂停 + Interpreter 状态回滚）、JIT、模块系统、并发原语、TCO 尾调用优化、try/catch 异常捕获等全部核心模块。

---

## 测试编写指南

### 命名约定

```
// 套件名：TestXxx 或 XxxSuite
// 用例名：描述性名称
TEST(VME2EBasic, IntArithmetic) { ... }
TEST(VME2EImportIsolation, NonExportNamesHidden) { ... }
```

### 三后端测试模板

```cpp
TEST(VME2EFeature, Description) {
    const char* src = R"(
        // MiniLang source code here
        print("expected output");
    )";

    // 1. Interpreter
    Interpreter interp(src);
    auto out1 = interp.runCaptureOutput();

    // 2. StackVM
    Compiler c1;
    auto chunk1 = c1.compile(src);
    VM vm;
    vm.loadChunk(chunk1);
    auto out2 = vm.runCaptureOutput();

    // 3. RegisterVM
    Compiler c2;
    c2.setUseRegisterVM(true);
    auto chunk2 = c2.compile(src);
    RegisterVM rvm;
    rvm.loadChunk(chunk2);
    auto out3 = rvm.runCaptureOutput();

    EXPECT_EQ(out1, out2);
    EXPECT_EQ(out1, out3);
}
```

### 教学面板测试

教学面板的 Library 静态数据需要验证完整性：

```cpp
TEST(TeachingPanelsXxx, ScenarioCount) {
    auto scenarios = XxxLibrary::scenarios();
    EXPECT_GE(scenarios.size(), 6);
}

TEST(TeachingPanelsXxx, IdUnique) {
    auto scenarios = XxxLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        EXPECT_TRUE(ids.insert(s.id).second) << "Duplicate ID: " << s.id;
    }
}
```

---

## 已知限制

1. **槽位复用**：兄弟作用域的局部变量复用同一栈槽时，slot→name 映射中后声明的变量名覆盖先前的
2. **IR 优化与栈式 VM**：复制传播和 CSE 在栈式 VM 后端默认禁用，因为与栈操作语义冲突
3. **Ninja 依赖追踪**：修改 `Bytecode.h` 等核心头文件后可能需要全量重编译（删除 build 目录）
4. **测试污染**（已澄清，2026-07-25 P3-A6）：原以为是模块缓存污染，CHANGELOG 2026-07-24 条目确认实际是 `build_debug` 目录 stale binary 假失败，重新构建后通过。但审查发现 `Compiler::compileViaIR` 的状态重置不完整（缺 `moduleLoadingStack_` / `moduleExports_` 清理），与 `compileViaRegisterIR` 的 P2-B fix 不对称——P3-A6 已补齐该缺陷，三路径状态重置一致
5. **VM 闭包 upvalue 解析缺口**（已修复，2026-07-24 W3-2-Bug）：原 StackVM/IR/RegisterVM 路径的两个 upvalue 缺口已修复——(a) 闭包内对捕获变量执行方法调用链（`b.data.push(4)`）的 upvalue 接收者写回；(b) 函数内定义的类方法捕获外层函数变量。修复方案：Compiler `emitMethodCallWriteback`/`visitMemberAssign`/`visitIndexAssign` 对 upvalue 基变量发射 `OP_WRITEBACK_MEMBER_UPVALUE`/`OP_WRITEBACK_INDEX_UPVALUE`；`emitMethodBody` 存储 upvalue 描述符，VM `OP_DEFINE_CLASS` 与 RegisterVM `REG_DEFINE_CLASS` 执行 `captureMethodUpvalues` 为有 upvalue 的方法创建 `VMClosureData`。详见 CHANGELOG.md W3-2-Bug 条目

---

## 覆盖率基线与提升路线图

### 当前阈值

| 平台 | 工具 | 行覆盖率阈值 | CI 配置 |
|------|------|-------------|---------|
| Windows | OpenCppCoverage | 70% | `.github/workflows/ci.yml` coverage-windows job |
| Linux | gcovr | 70% | `.github/workflows/ci.yml` coverage-linux job |

阈值检查工具：`scripts/coverage_threshold.py`（解析 Cobertura XML，低于阈值退出码 1）

### 提升路径

| 阶段 | 目标 | 状态 | 备注 |
|------|------|------|------|
| 基线 | 60% | ✅ 已完成 | 初始门槛 |
| 第 1 步 | 65% | ✅ 已完成（2026-07-24） | 本轮 W3-3 |
| 第 2 步 | 70% | ✅ 已完成（2026-07-25） | 本轮 P3-19，新增 TestCoverageGaps2.cpp（10 个测试） |
| 最终 | 75% | 待推进 | 分步提升避免 CI 长期红色 |

### 本地生成覆盖率报告

#### Windows (OpenCppCoverage)

```bat
REM 安装 OpenCppCoverage
choco install opencppcoverage

REM 生成覆盖率报告
OpenCppCoverage --export_type=cobertura:out\build\debug\coverage.xml ^
    --modules minilang_tests ^
    --sources c:\Users\v\Desktop\ide\ide ^
    -- out\build\debug\tests\minilang_tests.exe

REM 检查阈值
python scripts/coverage_threshold.py --report out\build\debug\coverage.xml --line-threshold 70
```

#### Linux (gcovr)

```bash
cmake -B out/build/debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage"
cmake --build out/build/debug --target minilang_tests
gcovr --xml -o coverage.xml --fail-under-line 70 build/
```

### 已识别覆盖率缺口与定向测试

#### W3-2 轮次（TestCoverageGaps.cpp，33 个测试）

W3-2 轮次通过代码审查识别以下低覆盖区域，并添加定向测试到 `TestCoverageGaps.cpp`（33 个测试）：

| 缺口区域 | 测试套件 | 覆盖路径 | 状态 |
|---------|---------|---------|------|
| 复合类型注解 | `TypeAnnotationGaps` (11) | `int?` / `dict[K:V]` / `int[][]` / `string` / `bool` / `float` 四后端 | ✅ |
| 类型注解违例 | `TypeViolationGaps` (2) | int←string / dict 值类型不匹配错误路径 | ✅ |
| UTF-8 字符串索引 | `Utf8StringIndexGaps` (6) | 中文字符索引 / 混合 ASCII+中文 / emoji / 越界 | ✅ |
| 类类型注解自动构造 | `ClassTypeAnnotationGaps` (2) | `var p: Point;` 等价 `Point()` 构造 | ✅ |
| 闭包 upvalue 修改 | `ClosureUpvalueGaps` (3) | 捕获字段读取/修改/多闭包共享 | ⚠️ 1 个 VM bug 待修复 |
| 类方法自由变量 | `ClassFreeVarGaps` (3) | 方法读/写全局变量 / 嵌套类捕获函数变量 | ⚠️ 1 个 VM bug 待修复 |
| enum variant 错误路径 | `EnumVariantErrorGaps` (3) | 未定义 variant / arity 不匹配 / 正常 variant | ✅ |
| try-catch 闭包传播 | `TryCatchClosureGaps` (2) | catch 块引用外层变量 / finally+return | ✅ |
| 循环寄存器分配 | `LoopRegisterGaps` (3) | 循环不变量保持 / 嵌套循环 / break+continue | ✅ |

#### P3-19 轮次（TestCoverageGaps2.cpp，10 个测试）

P3-19 轮次针对「后续覆盖率提升方向」5 个点名的低覆盖区域添加定向测试到 `TestCoverageGaps2.cpp`（10 个测试）：

| 缺口区域 | 测试套件 | 覆盖路径 | 状态 |
|---------|---------|---------|------|
| 模块系统循环导入深层嵌套 | `CircularImportGaps2` (2) | 3 层 a→b→c→a + 跨模块函数调用 / 4 层 a→b→c→d→a + 部分导出 | ✅ |
| 并发原语 channel 边界 | `ConcurrencyGaps2` (2) | 空通道 tryRecv 非阻塞 / 关闭通道 recv 立即返回 null | ✅ |
| 并发原语 spawn 异常传播 | `ConcurrencyGaps2` (1) | spawn 闭包 throw → join 异常传播 | ⚠️ 1 个 VM bug 待修复 |
| 并发原语 mutex 边界 | `ConcurrencyGaps2` (1) | 二次 lock 防护（tryLock 验证） | ✅ |
| IR 优化 LICM | `LICMGaps2` (1) | 含 LOAD_CONST 循环不变量的 while 循环 LICM 外提 | ✅ |
| IR 优化 GVN | `GVNGaps2` (1) | if/else 两分支等价表达式（42+1）跨块消除 | ✅ |
| IR 优化 inline 阈值边界 | `InlineGaps2` (2) | 14 指令应内联（≤15）/ 16 指令不应内联（>15） | ✅ |

### 后续覆盖率提升方向

1. **JIT 后端**：浮点边界值（NaN/Infinity/-0.0/subnormal）、递归深度边界（256/257）、异常边角场景（re-throw / finally return）已在 P3-A3 R165 系列覆盖；仍待覆盖：R161 闭包-字段交互（被禁用待 V-P1-6 JIT 同步缺失 bug 修复后启用）、JIT 模块系统/并发原语/字符串方法/enum+match 表达式
2. **模块系统**：预编译模块 `.minic` 序列化/反序列化边界（循环导入深层嵌套已在 P3-19 覆盖）
3. **GUI 组件**：IdeController 生命周期、Worker 线程状态清理、教学面板交互逻辑
4. **并发原语**：channel 超时 API（需先扩展 ChannelInner）、mutex 死锁检测、rwlock 写锁饥饿（spawn 异常传播已在 P3-A1 修复，三后端一致）
5. **IR 优化**：LICM 嵌套循环、GVN 含副作用表达式不消除、级联内联（基础 LICM/GVN/inline 边界已在 P3-19 覆盖）

---

*参见 [architecture.md](architecture.md) 了解编译管线架构，[development.md](development.md) 了解完整开发日志。*
