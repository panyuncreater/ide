# MiniLang 测试指南 / Testing Guide

本文档描述 MiniLang 的多后端一致性验证方法论与测试策略。

---

## 四后端一致性验证

MiniLang 维护三个解释执行后端（Interpreter / StackVM / RegisterVM）外加 JIT 第四执行路径，任何语义变更必须确保三解释后端行为一致；JIT 对已支持场景与 StackVM 严格一致，未支持场景优雅降级（不崩溃不错值）。

### 后端矩阵

| 组合 | 配置 | 说明 |
|------|------|------|
| Interpreter | 默认 | 树遍历解释器，语义基准 |
| StackVM | `setVM(true)` | 栈式虚拟机 |
| StackVM + IR | `setVM(true)` + `setIR(true)` | 栈式 VM + IR 中间层 |
| StackVM + IR + Opt | `setVM(true)` + `setIR(true)` + `setIROptimize(true)` | 栈式 VM + IR 优化 |
| RegisterVM | `setVM(true)` + `setUseRegisterVM(true)` | 寄存器式 VM |
| RegisterVM + IR + Opt | 上述 + `setIROptimize(true)` | 寄存器 VM + 全部优化 |
| JIT | `MINILANG_USE_JIT=ON`（x86-64 默认） | 栈式 VM 热路径分层编译，与 StackVM 一致性断言 + 未支持场景优雅降级断言（TestJIT / TestJITCoverageGaps） |

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

项目包含 9 个独立于 GoogleTest 的可执行程序（`test_harness/` 目录），用于特定场景的端到端验证：

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

项目当前包含 **110 个测试 .cpp 文件**，`minilang_tests` 目标总计 **3996 个测试用例 / 501 个测试套件**，全量 ctest（含 minilang_app_tests / minilang_gui_smoke）共 **4076 个测试全部通过**（截至 2026-07-31）。测试覆盖多后端语义一致性、新语言特性（运算符重载、宏模板、trait/mixin、async/await、`?` 错误传播、插件/沙箱/C API）、教学面板数据完整性、前端组件、IR 优化（含 SSA 基础设施）、调试器（含 DAP pause 同步暂停 + Interpreter 状态回滚）、JIT、模块系统、并发原语、TCO 尾调用优化、try/catch 异常捕获等全部核心模块。

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
3. **Ninja 依赖追踪**：修改 `Bytecode.h` 等核心头文件的符号签名（含追加默认参数）后，unity batch 可能残留 stale obj 导致 LNK2019/LNK1120。处置：运行 `scripts/fix_stale_objs.bat`（定向删除四个核心子库 obj + minilang_core.lib，无需全量重建）；`scripts/build.bat` 与 `scripts/run_tests.bat` 已内置链接失败自动清理+重试一次
4. **测试污染**（已澄清，2026-07-25 P3-A6）：原以为是模块缓存污染，CHANGELOG 2026-07-24 条目确认实际是 `build_debug` 目录 stale binary 假失败，重新构建后通过。但审查发现 `Compiler::compileViaIR` 的状态重置不完整（缺 `moduleLoadingStack_` / `moduleExports_` 清理），与 `compileViaRegisterIR` 的 P2-B fix 不对称——P3-A6 已补齐该缺陷，三路径状态重置一致
5. **VM 闭包 upvalue 解析缺口**（已修复，2026-07-24 W3-2-Bug）：原 StackVM/IR/RegisterVM 路径的两个 upvalue 缺口已修复——(a) 闭包内对捕获变量执行方法调用链（`b.data.push(4)`）的 upvalue 接收者写回；(b) 函数内定义的类方法捕获外层函数变量。修复方案：Compiler `emitMethodCallWriteback`/`visitMemberAssign`/`visitIndexAssign` 对 upvalue 基变量发射 `OP_WRITEBACK_MEMBER_UPVALUE`/`OP_WRITEBACK_INDEX_UPVALUE`；`emitMethodBody` 存储 upvalue 描述符，VM `OP_DEFINE_CLASS` 与 RegisterVM `REG_DEFINE_CLASS` 执行 `captureMethodUpvalues` 为有 upvalue 的方法创建 `VMClosureData`。详见 CHANGELOG.md W3-2-Bug 条目
6. **闭包关闭后的快照语义**（AUDIT-R5 R2，已核实为三后端一致）：闭包返回/逃逸定义作用域后（只能经数组索引 `cs[0]()` 调用，因全局闭包变量调用 `var c=f(); c()` 不被 VM/IR 支持），**三后端均为快照语义**：多个闭包共享同一被捕获变量时，关闭后经一个闭包的写入不会对另一个闭包可见（Interpreter/StackVM/RegisterVM 均输出初值快照 `0`）。与 Lua/JS/Python 的共享闭包语义不同，但**三后端一致**——项目核心不变量是三后端一致而非匹配特定主流语言。（注：AUDIT-R5 初版曾误认为“Interpreter 快照 vs VM 共享”；实测推翻——该可运行模式下三后端本就一致（均 `0`），若将 Interpreter 单边改为共享（得 `2`）反而破坏一致性。由 `ConsistencyDiff.AuditUpvalue_R2_MultiClosureSharedCounterAfterClose` 回归锁。若今后需对齐 Lua/JS 共享语义，需**三后端同步**引入真共享 upvalue cell，属架构级改动。）
7. **可变导出量的导入可见性差异**（AUDIT-R5 R5，已修复）：历史上 Interpreter 将导出值按值拷贝进导入方环境（快照语义），而 VM/IR 将模块内联编译进主程序、导出名直接共享同一全局槽（共享/实时语义）——实测确认三后端不一致：模块导出整型变量 `counter` 和修改它的函数 `inc`，主程序导入两者后调 `inc` 再打印 `counter`，Interpreter 输出导入时旧值 `0`，而 StackVM/StackVM-IR/RegisterVM-IR 输出修改后新值 `2`。**已统一到 Interpreter 基准（快照语义）**：VM/IR 在模块内联后记录导出「可变变量」的模块槽位并 `detachName` 释放其名称（`common` 之外的 `GlobalSlotAllocator::detachName`），导入方另分配新槽位并在导入点 emit `OP_GET_GLOBAL(模块槽)+OP_DEFINE_GLOBAL(新槽)` 拷贝快照（IR 路径同时更新 `varMap_`）；模块内部引用按槽位索引固化不受影响，模块内变异不再经导入方名称可见。**快照仅适用于变量导出**——函数/类导出经名称解析（`functionChunks_`/类注册），不拷贝不 detach（否则类实例化 `OP_CLASS_NEW`/函数调用会因 varMap_ 重定向而损坏），且函数/类不被重新赋值，共享与快照观测等价。命名空间导入（`import * as`）本就是快照字典，三后端一致。由 `VME2EImportSnapshot.*`（4 后端配置回归锁）守护。残余：循环导入的可变变量（未 detach，保持共享）与预编译 `.minic` 模块（无 AST 类别信息，变量集为空→快照无操作，保持共享）——均为极边缘/IDE 专用场景，且现有测试未覆盖该可观差异。
8. **模块路径规范化与大小写去重**（AUDIT-R5 R6/BUG-M4/M5，已修复）：连续斜杠/`./`/`/./` 规范化已统一到 `common/ModulePath.h::normalizeModulePathKey` 单一事实源（Interpreter/Compiler/AstIRBuilder/pathHash/clearModuleCache 五处共用），修复 `a//b` 与 `a/b` 双重加载（BUG-M5）。大小写去重（BUG-M4）通过分离“缓存去重键”与“loader 请求路径”修复：`moduleCacheKey`（Windows 上 ASCII 小写折叠）作所有缓存/去重 map 键与 `pathHash` 输入，`normalizeModulePathKey`（保留大小写）供 `moduleLoader_`/`mtimeChecker`/预编译解析器。三后端（Interpreter/StackVM/RegisterVM）均已分离两个字段：Windows 上同一文件的不同大小写拼写去重为同一模块（避免双重加载），而大小写敏感的 loader（测试用 `std::unordered_map`、Linux/macOS 文件系统）仍收到原大小写路径。由 `VME2EImport.CaseInsensitiveDedupOnWindows`（Windows guarded）回归锁。
9. **finally 内 break/continue 与待处理异常的交互**（AUDIT-R6 实证确认，**已修复** AUDIT-R6 B1）：原三后端行为均不正确且互不一致——Interpreter：异常继续传播且 break 标志残留，外层 catch 被截断；StackVM 非-IR：每轮泄漏异常值至“栈溢出”；IR 路径：finally 自链死循环。现已统一为 Java 式“finally 的控制流转移丢弃待处理异常/return”：Interpreter 在异常路径 runFinallyBlock 后若 loopFlow_ 被置位则吞掉异常；两个编译器的 emitFinallyBlock 在发射 finally 体期间暂时弹出自身 tryFinallyStack_ 条目（消除自链），异常副本用唯一临时全局暂存异常值（break 跳出时跳过 reload+rethrow 即丢弃，且无栈残留）。由 `AuditR6FinallyAbrupt` 套件 5 个四路径回归锁。**AUDIT-R7 F1 补充修复**：原残余限制①（“abrupt 替换 abrupt”时 pendingJumpStack_ 残留陈旧续跳目标）已实证为 P0（`try{break}finally{return}` 致 StackVM 常量池索引越界、RegisterVM 寄存器越界 C++ 异常逃逸宿主）并修复：三 VM + JIT 的 pendingJumpStack_ 条目附带 frameIndex（仿 tryStack_），FINALLY_END 只消费本帧条目、惰性丢弃已返回深帧残留、不碰更浅帧在途目标（同时修复 finally 体内调用含 try-finally 函数时被调函数误弹调用方目标的场景）。由 `AuditR7Regression.AbruptReplaceAbruptNoStaleJumpTarget` 与 `CalleeFinallyDoesNotStealCallerJumpTarget` 四路径回归锁。**残余窄化限制（P2）**：同帧内“abrupt 替换 abrupt”（如 `while{try{break}finally{continue}}`）仍可能残留同帧条目；递归中同一 try 站点 finally 体重入时异常暂存临时全局可能被内层覆盖。均为反模式叠加边角，优先级低。
10. **Interpreter 无 TCO（尾调用优化）**（AUDIT-R7 确认的已知设计差异，**已修复 B1 2026-07-28**）：原深尾递归（`return f(n-1)` 形态）在三条 VM 路径经 TCO 帧复用恒定栈深完成，而 Interpreter 无帧复用机制，超过 256 层报“递归深度超过限制”。**B1 修复**：Interpreter 在 `visitReturnStmt` 引入蹦床（trampoline）——识别自尾调用（与 VM 共享 `TCO::identifyTailCall` 单一事实源）后抛 `TailCallSignal`，由 `callNamedFunction`/`invokeMethod` 的蹦床循环捕获并帧复用执行，SelfFunction/SelfMethod 均支持，四后端深尾递归行为一致。安全不变量：① 仅蹦床调用点启用上下文，init/生成器/闭包变量/高阶回调执行点显式禁用（信号不跨边界逃逸）；② try/catch/finally 内不 TCO（finally 语义保留，与 VM tryDepth 判据一致）；③ 运行时重绑定校验（名称遮蔽回退普通调用；子类 override 保持虚分派）。**连带修复 B1-Shadow（P1）**：三条 VM 路径编译期 TCO 判据缺少“函数名被局部变量遮蔽”检查（`fun f(n){ var f = other; return f(x); }` 被误判自递归 → 死循环至指令预算耗尽），Compiler 用 `currentLocals_`、AstIRBuilder 用 `varMap_` Kind::LOCAL 检查后回退普通调用。由 `InterpreterTCO.*`（13 个四后端用例）与更新后的 `TCOBasic/TCOApplied` 回归锁。

---

## 覆盖率基线与提升路线图

### 当前阈值

| 平台 | 工具 | 行覆盖率阈值 | CI 配置 |
|------|------|-------------|---------|
| Windows | OpenCppCoverage | 75% | `.github/workflows/ci.yml` coverage-windows job |
| Linux | gcovr | 75% | `.github/workflows/ci.yml` coverage-linux job |

阈值检查工具：`scripts/coverage_threshold.py`（解析 Cobertura XML，低于阈值退出码 1）

### 提升路径

| 阶段 | 目标 | 状态 | 备注 |
|------|------|------|------|
| 基线 | 60% | ✅ 已完成 | 初始门槛 |
| 第 1 步 | 65% | ✅ 已完成（2026-07-24） | 本轮 W3-3 |
| 第 2 步 | 70% | ✅ 已完成（2026-07-25） | 本轮 P3-19，新增 TestCoverageGaps2.cpp（10 个测试） |
| 最终 | 75% | ✅ 已完成（2026-07-25，P3-A2） | CI 两平台门槛已同步提升至 75.0（ci.yml L409 Windows + L506 Linux） |

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
python scripts/coverage_threshold.py --report out\build\debug\coverage.xml --line-threshold 75
```

#### Linux (gcovr)

```bash
cmake -B out/build/debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage"
cmake --build out/build/debug --target minilang_tests
gcovr --xml -o coverage.xml --fail-under-line 75 build/
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
| 闭包 upvalue 修改 | `ClosureUpvalueGaps` (3) | 捕获字段读取/修改/多闭包共享 | ✅（W3-2-Bug1c 修复） |
| 类方法自由变量 | `ClassFreeVarGaps` (3) | 方法读/写全局变量 / 嵌套类捕获函数变量 | ✅（W3-2-Bug2 修复） |
| enum variant 错误路径 | `EnumVariantErrorGaps` (3) | 未定义 variant / arity 不匹配 / 正常 variant | ✅ |
| try-catch 闭包传播 | `TryCatchClosureGaps` (2) | catch 块引用外层变量 / finally+return | ✅ |
| 循环寄存器分配 | `LoopRegisterGaps` (3) | 循环不变量保持 / 嵌套循环 / break+continue | ✅ |

#### P3-19 轮次（TestCoverageGaps2.cpp，10 个测试）

P3-19 轮次针对「后续覆盖率提升方向」5 个点名的低覆盖区域添加定向测试到 `TestCoverageGaps2.cpp`（10 个测试）：

| 缺口区域 | 测试套件 | 覆盖路径 | 状态 |
|---------|---------|---------|------|
| 模块系统循环导入深层嵌套 | `CircularImportGaps2` (2) | 3 层 a→b→c→a + 跨模块函数调用 / 4 层 a→b→c→d→a + 部分导出 | ✅ |
| 并发原语 channel 边界 | `ConcurrencyGaps2` (2) | 空通道 tryRecv 非阻塞 / 关闭通道 recv 立即返回 null | ✅ |
| 并发原语 spawn 异常传播 | `ConcurrencyGaps2` (1) | spawn 闭包 throw → join 异常传播 | ✅（P3-A1 修复） |
| 并发原语 mutex 边界 | `ConcurrencyGaps2` (1) | 二次 lock 防护（tryLock 验证） | ✅ |
| IR 优化 LICM | `LICMGaps2` (1) | 含 LOAD_CONST 循环不变量的 while 循环 LICM 外提 | ✅ |
| IR 优化 GVN | `GVNGaps2` (1) | if/else 两分支等价表达式（42+1）跨块消除 | ✅ |
| IR 优化 inline 阈值边界 | `InlineGaps2` (2) | 14 指令应内联（≤15）/ 16 指令不应内联（>15） | ✅ |

#### 第三批（TestCoverageGaps3.cpp，10 个测试）

针对 P3-19 之后「后续覆盖率提升方向」仍点名的方向 4/5 剩余缺口添加定向测试到 `TestCoverageGaps3.cpp`（10 个测试）：

| 缺口区域 | 测试套件 | 覆盖路径 | 状态 |
|---------|---------|---------|------|
| 并发原语 rwlock 读写锁排斥 | `RwLockGaps3` (3) | 读锁持有时写锁被排斥（写者饥饿边界）/ 写锁持有时读锁被排斥 / 读锁释放后写锁可获取，四后端一致 | ✅ |
| 并发原语 mutex 自死锁检测 | `MutexDeadlockGaps3` (3) | 同线程重复 lock → fail-fast 报死锁 runtime 错误 / lock-unlock-relock 不误报 / 自持时 tryLock 返回 false，四后端一致 | ✅ |
| IR 优化 LICM 嵌套循环 | `LICMNestedGaps3` (1) | 两层嵌套 while 循环 + 内层循环不变量，LICM 分析不崩溃且控制流完整 | ✅ |
| IR 优化 GVN 副作用不消除 | `GVNSideEffectGaps3` (1) | 两次相同 CALL f()（含副作用）不被 GVN 值编号消除 | ✅ |
| IR 优化级联内联 | `InlineCascadeGaps3` (2) | 单趟 main→a→b 残留 1 CALL / 迭代到不动点全部塌缩为 0 CALL | ✅ |

### 后续覆盖率提升方向

1. **JIT 后端**：浮点边界值（NaN/Infinity/-0.0/subnormal）、递归深度边界（256/257）、异常边角场景（re-throw / finally return）已在 P3-A3 R165 系列覆盖；R161 闭包-字段交互已随 V-P1-6 JIT 同步修复启用（R161Closure* 全部通过）；JIT 模块系统/并发原语/字符串方法/enum+match 已由 `TestJITCoverageGaps.cpp`（14 用例）锁定——字符串拼接/比较/索引与 StackVM 严格一致；字符串方法（jit-runtime 降级）、enum variant（OP_BUILD_ENUM_VARIANT 未实现）、channel/mutex/spawn（内建函数未注册）锁定为“StackVM 正确 + JIT 优雅降级不崩溃”，待 JIT 补全实现后断言自动收紧为一致性
2. **模块系统**：预编译模块 `.minic` 序列化/反序列化边界（循环导入深层嵌套已在 P3-19 覆盖；反序列化健壮性已由 `BytecodeCacheRobustness`（5 用例：空文件/截断头部/版本不匹配/checksum 失配/payload 截断）覆盖，验证畸形输入安全返回 nullopt 不崩溃）
3. **GUI 组件**：App 编排层（DebugCoordinator / WorkerManager / PipelineRunner / InterpreterWorker）已由 `minilang_app_tests` 目标的 `TestAppOrchestration.cpp`（47 用例）覆盖（断点/watchpoint 生命周期、Worker prepare/start/stop/forceStop 幂等、跨线程信号、线程安全并发访问）；教学面板交互逻辑已由 `TestTeachingPanelsE2E.cpp` 覆盖；仅 `IdeController` 完整生命周期因拉入完整 GUI 依赖链而未纳入独立测试目标（架构性取舍）
4. **并发原语**：channel 超时 API（`recvTimeout(ms)`）已实现并四后端一致（`ConcurrencyGaps2` 覆盖）；rwlock 读写锁排斥边界已由 `RwLockGaps3`（3 用例）覆盖；mutex 自死锁检测已实现（MutexData 记录持有线程 id，同线程重复 lock → fail-fast 报 runtime 错误而非挂起，保留跨线程 std::mutex 语义）并由 `MutexDeadlockGaps3`（3 用例，四后端）锁定；spawn 异常传播已在 P3-A1 修复，三后端一致
5. **IR 优化**：基础 LICM/GVN/inline 边界已在 P3-19 覆盖；LICM 嵌套循环、GVN 含副作用表达式不消除、级联内联已由 `TestCoverageGaps3.cpp`（`LICMNestedGaps3`/`GVNSideEffectGaps3`/`InlineCascadeGaps3`，4 用例）覆盖

---

*参见 [architecture.md](architecture.md) 了解编译管线架构，[development.md](development.md) 了解完整开发日志。*
