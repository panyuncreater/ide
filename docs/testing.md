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

项目当前包含 **47 个测试 .cpp 文件**（约 27,000 行），总计 **1773 个测试用例**（截至最近一轮）。测试覆盖三后端语义一致性、教学面板数据完整性、前端组件、IR 优化、调试器等全部核心模块。

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

---

*参见 [architecture.md](architecture.md) 了解编译管线架构，[development.md](development.md) 了解完整开发日志。*
