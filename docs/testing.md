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
| `TeachingPanels*` | `TestTeachingPanelsAudit*.cpp` | 教学面板数据完整性 |

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
| Lexer | `TestLexer.cpp` | Token 识别、插值字符串拆分、错误恢复 |
| Parser | `TestParser.cpp` | AST 构建、错误消息、嵌套深度限制 |
| Compiler | `TestCompiler.cpp` | 字节码生成、常量池去重、作用域分析 |
| IR | `TestIR.cpp` | IR 生成、优化 pass、lowering 正确性 |
| NaNBox | `TestNaNBox.cpp` | 编码/解码、边界值、NaN 规范化 |
| Formatter | `TestFormatterAudit.cpp` | 代码格式化、插值字符串重建 |

### 集成层

| 组件 | 测试文件 | 关注点 |
|------|---------|--------|
| Interpreter E2E | `TestInterpreterE2E.cpp` | 解释器端到端语义 |
| VM E2E | `TestVME2E.cpp` | 三后端一致性 |
| 内置方法 | `TestBuiltinMethods.cpp` | 数组/字典/字符串方法 |

### 审计层

| 测试文件 | 关注点 |
|---------|--------|
| `TestCompilerAudit.cpp` | 编译器边界条件审计 |
| `TestCompilerIRIsolationAudit.cpp` | IR 模块隔离审计 |
| `TestDebugAudit.cpp` | 调试器一致性审计 |
| `TestTeachingPanelsAudit*.cpp` | 教学面板数据完整性 |

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
