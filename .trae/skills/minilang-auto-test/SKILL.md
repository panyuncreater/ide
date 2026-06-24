---
name: "minilang-auto-test"
description: "Builds and runs module-matched GoogleTest + test_harness tests after editing MiniLang source files. Invoke automatically after any Edit/Write to .cpp/.h files under lexer/, parser/, ast/, interpreter/, compiler/, debug/, formatter/."
---

# MiniLang 自动测试 Skill

修改 MiniLang 项目源码后，自动构建并运行与被修改模块对应的测试。

## 触发条件

**必须自动触发**：当使用 Edit/Write 工具修改以下目录中的 `.cpp` 或 `.h` 文件后立即执行：
- `lexer/`、`parser/`、`ast/`、`interpreter/`、`compiler/`、`debug/`、`formatter/`

**不触发**：修改 `tests/`、`test_harness/`、`gui/`、`app/`、`CMakeLists.txt`、`.md` 文件时（除非用户明确要求）。

## 模块 → 测试映射表

根据被修改的源文件路径，确定需要运行的 GoogleTest 过滤器和 test_harness 范围：

| 被修改的源文件 | GoogleTest gtest_filter | test_harness |
|---|---|---|
| `lexer/Lexer.*`、`lexer/Token.*` | `LexerTest.*` | ✅ 运行 |
| `parser/Parser.*` | `ParserTest.*` | ✅ 运行 |
| `ast/ASTNode.*` | `ParserTest.*` | ✅ 运行 |
| `interpreter/Interpreter.*` | `InterpreterE2E.*` | ✅ 运行 |
| `interpreter/Environment.*` | `InterpreterE2E.*` | ✅ 运行 |
| `interpreter/BuiltinMethods.*` | `*Builtin*.*` | ❌ 跳过 |
| `interpreter/Value.*` | `Value*.*` | ❌ 跳过 |
| `compiler/Compiler.*` | `Compiler*.*` | ❌ 跳过 |
| `compiler/VM.*`、`compiler/Bytecode.*` | `VME2E.*:VMConsistency.*` | ❌ 跳过 |
| `debug/DebugController.*` | 运行全部（无直接测试） | ❌ 跳过 |
| `formatter/Formatter.*` | 运行全部（无直接测试） | ❌ 跳过 |

**多个文件被修改时**：合并所有对应的 gtest_filter，用 `:` 分隔。例如同时修改 Lexer.cpp 和 Parser.cpp，则 filter 为 `LexerTest.*:ParserTest.*`。

## 执行步骤

### 步骤 1：确定测试范围

读取被修改的文件路径，查上表得到 `GTEST_FILTER` 和是否运行 test_harness。

### 步骤 2：构建并运行 GoogleTest

**关键约束**：
- 必须使用 NMake Makefiles 生成器（Ninja 在本机 PowerShell 下有 GetOverlappedResult IO bug）
- VS 开发环境不会跨 PowerShell 调用持久化，必须在同一命令中初始化
- 构建目录为 `build_fix/`（已配置好 NMake + MSVC + Qt6）

执行以下 PowerShell 命令（将 `<GTEST_FILTER>` 替换为实际过滤器）：

```powershell
Import-Module "D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\vs" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation 2>&1 | Out-Null
$env:PATH = "D:\qt\6.10.3\msvc2022_64\bin;" + $env:PATH
Set-Location "c:\Users\v\Desktop\ide\ide\build_fix"
$proc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c nmake minilang_tests > build_stdout.txt 2> build_stderr.txt" -WorkingDirectory "c:\Users\v\Desktop\ide\ide\build_fix" -PassThru -WindowStyle Hidden
$proc | Wait-Process -Timeout 600
Write-Output "Build ExitCode: $($proc.ExitCode)"
```

**构建失败处理**：
- 读取 `build_fix\build_stderr.txt` 和 `build_fix\build_stdout.txt` 的最后 60 行
- 分析编译错误，报告给用户，**不继续运行测试**

**构建成功后运行测试**：

```powershell
Import-Module "D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\vs" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation 2>&1 | Out-Null
$env:PATH = "D:\qt\6.10.3\msvc2022_64\bin;" + $env:PATH
Set-Location "c:\Users\v\Desktop\ide\ide\build_fix\tests"
& ".\minilang_tests.exe" --gtest_filter="<GTEST_FILTER>" 2>&1 | Select-Object -Last 30
```

**特殊情况**：如果映射表指示"运行全部"（debug/formatter 模块），则不加 `--gtest_filter` 参数，运行全部 558 个测试。

### 步骤 3：构建并运行 test_harness（如适用）

仅当映射表中 test_harness 列为 ✅ 时执行。

test_harness 是独立的 cl.exe 编译测试，不依赖 Qt/CMake，覆盖 Lexer/Parser/AST/Interpreter/Environment 核心模块。

```powershell
Import-Module "D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\vs" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation 2>&1 | Out-Null
Set-Location "c:\Users\v\Desktop\ide\ide\test_harness"
$proc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c build.bat" -WorkingDirectory "c:\Users\v\Desktop\ide\ide\test_harness" -PassThru -WindowStyle Hidden
$proc | Wait-Process -Timeout 300
Write-Output "test_harness Build ExitCode: $($proc.ExitCode)"
```

构建成功后运行：

```powershell
Set-Location "c:\Users\v\Desktop\ide\ide\test_harness"
if (Test-Path "test_runner.exe") {
    & ".\test_runner.exe" 2>&1 | Select-Object -Last 30
}
```

### 步骤 4：报告结果

向用户报告：
1. **GoogleTest 结果**：通过/失败数量、耗时。例如 `✅ GoogleTest: 45/45 passed (12ms)`
2. **test_harness 结果**：通过/失败数量。例如 `✅ test_harness: 28/28 passed`
3. **失败详情**：如有失败，列出失败的测试名和失败原因（截取关键行）

## 重要注意事项

1. **不要跳过构建步骤**：即使只改了头文件，也必须重新编译，因为头文件变更会影响所有包含它的源文件
2. **VS 环境必须每次初始化**：`Enter-VsDevShell` 的环境变量不会跨 PowerShell 调用持久化，必须在同一命令内完成初始化+构建+运行
3. **Qt6 DLL 路径**：运行 minilang_tests.exe 前必须将 `D:\qt\6.10.3\msvc2022_64\bin` 加入 PATH，否则会找不到 Qt6Core.dll 等
4. **NMake 而非 Ninja**：本机 Ninja 在 PowerShell 下有 GetOverlappedResult IO bug，必须使用 NMake Makefiles 生成器
5. **构建目录**：使用 `build_fix/` 目录，该目录已用 NMake Makefiles + MSVC cl.exe + Qt6(msvc2022_64) 正确配置
6. **超时处理**：构建超时 600 秒、test_harness 超时 300 秒，超时后报告给用户
7. **编译错误优先**：如果构建失败，不要尝试运行测试，直接报告编译错误

## 示例场景

### 场景 1：修改了 `lexer/Lexer.cpp`

1. 查表：gtest_filter = `LexerTest.*`，test_harness = ✅
2. 构建 GoogleTest：`nmake minilang_tests`
3. 运行：`minilang_tests.exe --gtest_filter="LexerTest.*"`
4. 构建 test_harness：`build.bat`
5. 运行：`test_runner.exe`
6. 报告结果

### 场景 2：修改了 `compiler/VM.cpp`

1. 查表：gtest_filter = `VME2E.*:VMConsistency.*`，test_harness = ❌
2. 构建 GoogleTest：`nmake minilang_tests`
3. 运行：`minilang_tests.exe --gtest_filter="VME2E.*:VMConsistency.*"`
4. 跳过 test_harness
5. 报告结果

### 场景 3：修改了 `debug/DebugController.cpp`

1. 查表：gtest_filter = 无直接测试，运行全部，test_harness = ❌
2. 构建 GoogleTest：`nmake minilang_tests`
3. 运行：`minilang_tests.exe`（无 filter，运行全部 558 个测试）
4. 跳过 test_harness
5. 报告结果
