# clang-tidy 静态分析报告（阶段六 · 只报告不修改）

- 生成时间：2026-08-01 08:21:36
- 工具：(复用 testing/static-analysis/reports/clang-tidy-findings.jsonl，未重新分析)  版本：-
- 配置：C:\Users\v\Desktop\ide\ide\testing\static-analysis\.clang-tidy
- 分析范围：bugprone-* / clang-analyzer-* / cppcoreguidelines-*（重渲染）
- 翻译单元：- 个（成功 - / 超时 - / 解析失败 -）
- 总耗时：0.0s

## 分级统计

| 严重度 | 数量 |
|--------|-----:|
| 高（high） | 7 |
| 中（medium） | 556 |
| 低（low） | 2342 |

合计 **2905** 条独立告警（去重后），涉及 38 类检查项、210 个文件。

## Top 20 问题清单（文件:行号 + 检查项 + 一句话解释）

| # | 严重度 | 文件:行号 | 检查项 | 一句话解释 |
|--:|--------|-----------|--------|-----------|
| 1 | 高 | `gui/FindReplacePanel.cpp:195` | `clang-analyzer-core.CallAndMessage` | 以未初始化/空值作为调用或参数。 |
| 2 | 高 | `gui/LabManualPanel.cpp:215` | `clang-analyzer-core.CallAndMessage` | 以未初始化/空值作为调用或参数。 |
| 3 | 高 | `gui/LabManualPanel.cpp:270` | `clang-analyzer-core.CallAndMessage` | 以未初始化/空值作为调用或参数。 |
| 4 | 高 | `compiler/JITCodeGen.cpp:435` | `bugprone-undefined-memory-manipulation` | undefined behavior, source object type 'const Value' is not TriviallyC… |
| 5 | 高 | `gui/WelcomeWizard.cpp:263` | `clang-analyzer-cplusplus.NewDeleteLeaks` | new 分配的内存在某路径上泄漏。 |
| 6 | 高 | `interpreter/BuiltinMethods.cpp:167` | `clang-analyzer-core.NonNullParamChecker` | Forming reference to null pointer |
| 7 | 高 | `interpreter/Value.h:952` | `clang-analyzer-core.FixedAddressDereference` | Access to field 'value' results in a dereference of a fixed address |
| 8 | 中 | `app/VmStepper.cpp:18` | `cppcoreguidelines-prefer-member-initializer` | 宜用成员初始化列表而非构造体内赋值。 |
| 9 | 中 | `gui/ActivityBar.cpp:22` | `cppcoreguidelines-prefer-member-initializer` | 宜用成员初始化列表而非构造体内赋值。 |
| 10 | 中 | `gui/AstBuilderToyPanel.cpp:83` | `cppcoreguidelines-prefer-member-initializer` | 宜用成员初始化列表而非构造体内赋值。 |
| 11 | 中 | `gui/AstBuilderToyPanel.cpp:140` | `cppcoreguidelines-prefer-member-initializer` | 宜用成员初始化列表而非构造体内赋值。 |
| 12 | 中 | `app/DebugCoordinator.cpp:83` | `cppcoreguidelines-avoid-do-while` | avoid do-while loops |
| 13 | 中 | `app/DebugCoordinator.cpp:86` | `cppcoreguidelines-avoid-do-while` | avoid do-while loops |
| 14 | 中 | `app/IdeController.cpp:137` | `cppcoreguidelines-avoid-do-while` | avoid do-while loops |
| 15 | 中 | `app/IdeController.cpp:540` | `cppcoreguidelines-avoid-do-while` | avoid do-while loops |
| 16 | 中 | `app/VmStepper.h:325` | `bugprone-narrowing-conversions` | 隐式收窄转换，可能丢位/溢出。 |
| 17 | 中 | `cli/fuzz_core.cpp:192` | `bugprone-narrowing-conversions` | 隐式收窄转换，可能丢位/溢出。 |
| 18 | 中 | `cli/fuzz_core.cpp:523` | `bugprone-narrowing-conversions` | 隐式收窄转换，可能丢位/溢出。 |
| 19 | 中 | `compiler/BytecodeCache.cpp:1519` | `bugprone-narrowing-conversions` | 隐式收窄转换，可能丢位/溢出。 |
| 20 | 中 | `cli/lsp_core.cpp:824` | `cppcoreguidelines-init-variables` | 局部变量声明时未初始化。 |
> 说明：同一检查项在 Top 20 中最多列 4 条（多样化展示），完整分布见下「按检查项汇总」。


## 按检查项汇总（命中数降序）

| 检查项 | 严重度 | 命中 | 说明 |
|--------|--------|-----:|------|
| `cppcoreguidelines-owning-memory` | 低 | 1508 | 裸指针持有所有权，宜用智能指针。 |
| `cppcoreguidelines-pro-type-static-cast-downcast` | 低 | 298 | 静态下转型，缺乏运行期类型检查。 |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic` | 低 | 158 | 指针算术，越界风险。 |
| `cppcoreguidelines-prefer-member-initializer` | 中 | 139 | 宜用成员初始化列表而非构造体内赋值。 |
| `cppcoreguidelines-pro-type-reinterpret-cast` | 低 | 131 | 使用 reinterpret_cast（类型不安全）。 |
| `cppcoreguidelines-pro-type-vararg` | 低 | 116 | 使用 C 变参（类型不安全）。 |
| `cppcoreguidelines-avoid-do-while` | 中 | 77 | avoid do-while loops |
| `cppcoreguidelines-special-member-functions` | 低 | 76 | 定义了部分特殊成员函数但未补全（三/五法则）。 |
| `bugprone-narrowing-conversions` | 中 | 71 | 隐式收窄转换，可能丢位/溢出。 |
| `cppcoreguidelines-init-variables` | 中 | 59 | 局部变量声明时未初始化。 |
| `cppcoreguidelines-avoid-const-or-ref-data-members` | 中 | 35 | 类含 const/引用成员，影响可赋值性。 |
| `bugprone-throwing-static-initialization` | 中 | 30 | initialization of 'kStdMath' with static storage duration may throw an… |
| `bugprone-exception-escape` | 中 | 29 | an exception may be thrown in function '~Logger' which should not thro… |
| `bugprone-branch-clone` | 中 | 27 | if/else 或 switch 分支体完全相同（疑似复制粘贴错误）。 |
| `cppcoreguidelines-pro-type-member-init` | 低 | 23 | 构造后存在未初始化的成员。 |
| `bugprone-empty-catch` | 中 | 20 | empty catch statements hide issues; to handle exceptions appropriately… |
| `bugprone-implicit-widening-of-multiplication-result` | 中 | 16 | 乘法在收窄类型上进行后再加宽，可能已溢出。 |
| `cppcoreguidelines-avoid-non-const-global-variables` | 低 | 15 | variable 'g_gcManagerAlive' is non-const and globally accessible, cons… |
| `cppcoreguidelines-pro-type-const-cast` | 低 | 14 | 使用 const_cast 去除常量性。 |
| `cppcoreguidelines-explicit-virtual-functions` | 中 | 13 | annotate this function with 'override' or (rarely) 'final' |
| `bugprone-argument-comment` | 中 | 9 | argument name 'objectModified' in comment does not match parameter nam… |
| `cppcoreguidelines-use-default-member-init` | 中 | 5 | use default member initializer for 'bits_' |
| `bugprone-misplaced-widening-cast` | 中 | 5 | either cast from 'int' to 'size_t' (aka 'unsigned long long') is ineff… |
| `cppcoreguidelines-rvalue-reference-param-not-moved` | 中 | 4 | 右值引用形参未被 move。 |
| `bugprone-switch-missing-default-case` | 中 | 4 | switching on non-enum value without default case may not cover all cas… |
| `cppcoreguidelines-use-enum-class` | 中 | 4 | enum '(unnamed enum at C:\Users\v\Desktop\ide\ide\compiler\VM.cpp:830:… |
| `clang-analyzer-deadcode.DeadStores` | 低 | 3 | 赋值后从未被读取（死存储）。 |
| `clang-analyzer-core.CallAndMessage` | 高 | 3 | 以未初始化/空值作为调用或参数。 |
| `cppcoreguidelines-avoid-goto` | 中 | 2 | avoid using 'goto' for flow control |
| `bugprone-unused-local-non-trivial-variable` | 中 | 2 | unused local variable 'line' of type 'std::string' (aka 'basic_string<… |
| `bugprone-unchecked-string-to-number-conversion` | 中 | 2 | 'atoi' used to convert a string to an integer value, but function will… |
| `cppcoreguidelines-missing-std-forward` | 中 | 1 | forwarding reference parameter 'contextBuilder' is never forwarded ins… |
| `clang-analyzer-optin.core.EnumCastOutOfRange` | 中 | 1 | The value '33' provided to the cast expression is not in the valid ran… |
| `bugprone-undefined-memory-manipulation` | 高 | 1 | undefined behavior, source object type 'const Value' is not TriviallyC… |
| `clang-analyzer-core.NonNullParamChecker` | 高 | 1 | Forming reference to null pointer |
| `clang-analyzer-cplusplus.NewDeleteLeaks` | 高 | 1 | new 分配的内存在某路径上泄漏。 |
| `bugprone-casting-through-void` | 中 | 1 | do not cast 'FARPROC' (aka 'long long (*)()') to 'minilang_plugin_init… |
| `clang-analyzer-core.FixedAddressDereference` | 高 | 1 | Access to field 'value' results in a dereference of a fixed address |

## 命中最多的文件（Top 20）

| 文件 | 命中 |
|------|-----:|
| `gui/JitVisualizerPanel.cpp` | 103 |
| `gui/MemoryModelPanel.cpp` | 84 |
| `compiler/AstIRBuilder.cpp` | 83 |
| `compiler/JITCodeGenHelpers.cpp` | 62 |
| `gui/IRTransformPanel.cpp` | 59 |
| `gui/ProfileDashboardPanel.cpp` | 58 |
| `interpreter/Value.h` | 57 |
| `gui/VmStackSandboxPanel.cpp` | 55 |
| `gui/RegisterAllocatorPanel.cpp` | 51 |
| `parser/Parser.cpp` | 48 |
| `gui/LoopUnrollingPanel.cpp` | 48 |
| `gui/GcVisualizerPanel.cpp` | 48 |
| `gui/BugHuntPanel.cpp` | 48 |
| `interpreter/InterpreterCalls.cpp` | 47 |
| `interpreter/BuiltinMethods.cpp` | 47 |
| `gui/WelcomeWizard.cpp` | 47 |
| `gui/MemoryLayoutPanel.cpp` | 46 |
| `compiler/JITCodeGen.cpp` | 44 |
| `gui/InlineCachePanel.cpp` | 44 |
| `cli/main.cpp` | 42 |

