---
name: "minilang-build"
description: "MiniLang IDE 构建、测试与诊断。当用户要求构建/编译 MiniLang、运行测试、配置 CMake，或遇到构建错误（链接失败、头文件缺失、ABI 错误、Qt 路径问题）时自动调用。"
---

# MiniLang IDE 构建与测试指南

本 Skill 提供 MiniLang IDE 项目的标准构建、测试与错误诊断流程。当用户要求构建、运行测试、配置 CMake，或遇到构建错误时，按本指南操作。

## 1. 环境要求

| 组件 | 要求 | 说明 |
|------|------|------|
| 编译器 | MSVC 19.51+（Windows）/ GCC 13+（Linux）/ Clang 16+（macOS） | 需完整 C++20 支持 |
| Qt | 6.x msvc2022_64（Windows）/ gcc_64（Linux）/ clang_64（macOS） | CI 使用 6.8.3 |
| 生成器 | Ninja | CMakePresets.json 统一使用 Ninja |
| CMake | ≥ 3.25 | 构建系统 |

**关键约束**：Windows 上 Qt 必须是 `msvc2022_64` 构建，MinGW GCC 与 Qt MSVC ABI 不兼容。

## 2. 标准构建命令

### 构建全部测试目标（ctest 前置）

```bash
cmake --preset windows-msvc-debug
cmake --build out/build/debug --target minilang_tests --target minilang_app_tests --target minilang_gui_smoke --target minilang_perf_test
```

> 只构建 `minilang_tests` 时 ctest 会以 `*_NOT_BUILT` 收尾并返回非 0。`minilang_perf_test` 受 `MINILANG_BUILD_PERF_TESTS` 门控，全新 configure 需显式 `-DMINILANG_BUILD_PERF_TESTS=ON`。

### 运行测试

```bash
ctest --test-dir out/build/debug --output-on-failure
```

### 构建 IDE 主程序

```bash
cmake --build out/build/debug --target minilang_ide
```

### 便捷脚本

| 脚本 | 用途 |
|------|------|
| `scripts/build.bat` | 构建 IDE（自动检测环境） |
| `scripts/run_tests.bat` | 构建并运行测试 |
| `configure.bat` | 首次配置（自动检测 VS + Qt） |

## 3. CMake Preset 输出目录

| Preset | 输出目录 | 平台 |
|--------|---------|------|
| `windows-msvc-debug` | `out/build/debug/` | Windows |
| `windows-msvc-release` | `out/build/release/` | Windows |
| `windows-msvc-asan` | `out/build/asan/` | Windows ASan |
| `linux-gcc-debug` | `out/build/linux-debug/` | Linux |
| `linux-gcc-release` | `out/build/linux-release/` | Linux |
| `macos-clang-debug` | `out/build/macos-debug/` | macOS |
| `macos-clang-release` | `out/build/macos-release/` | macOS |
| `linux-gcc-coverage` | `out/build/linux-coverage/` | Linux Coverage |

## 4. 验证通过标准

验证通过标准以根目录 `AGENTS.md`「验证通过标准」章节为唯一权威源（编译零错误、测试全绿、三后端一致性），每次代码修改后**必须**按该标准验证。

## 5. 已知构建问题与解决方案

### 问题 1：MinGW GCC 与 Qt MSVC ABI 不兼容

**症状**：`undefined reference to __imp__ZN7QObject...`

**解决**：使用 `windows-msvc-debug` Preset，确保 MSVC 环境已初始化。

### 问题 2：MSVC 环境未初始化

**症状**：`fatal error C1083: 无法打开包括文件: "cstddef"`

**解决**：通过 VS DevShell 初始化：
```powershell
Import-Module "D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\vs" -Arch amd64 -SkipAutomaticLocation
```

### 问题 3：环境变量跨会话丢失

**症状**：分步执行时第二步失败。

**解决**：环境初始化与构建命令用 `;` 合并为单条 PowerShell 命令。

### 问题 4：Qt 路径未设置

**症状**：`Could NOT find Qt6`

**解决**：设置 `$env:QTDIR = "<Qt路径>/msvc2022_64"`，确保 `$env:PATH` 包含 `$env:QTDIR\bin`。

### 问题 5：SEH 崩溃（修改头文件 struct 布局后）

**症状**：运行时 0xc0000005 访问违例。

**解决**：删除 `out/build/debug` 目录后全量重配置 + 重编译。

### 问题 6：LNK2019 未解析外部符号

**症状**：链接阶段 `unresolved external symbol`。

**解决**：确认 Qt 是 msvc2022_64 构建；检查新增 .cpp 文件是否已加入 CMakeLists.txt。

## 6. 筛选测试

```bash
# 运行指定测试套件
ctest --test-dir out/build/debug -R "CompilerAudit" --output-on-failure

# 运行指定测试
ctest --test-dir out/build/debug -R "VME2E\.DirectClassNoImport" --verbose
```

## 7. 重要提醒

1. **Windows 永远不要使用 MinGW GCC**，Qt 是 msvc2022_64 构建的
2. **环境初始化与构建命令合并**为单条 PowerShell 命令（用 `;` 分隔）
3. **构建前必须设置 QTDIR** 环境变量
4. **每次修改后必须运行测试**验证无回归
5. **使用 --target** 避免构建全部目标：`--target minilang_tests` 或 `--target minilang_ide`
