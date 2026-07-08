# MiniLang 贡献指南 / Contributing Guide

感谢您对 MiniLang IDE 项目的关注！本文档提供开发环境搭建、编码规范与协作流程的完整指引。

Thank you for your interest in contributing to MiniLang IDE! This document covers environment setup, coding conventions, and collaboration workflows.

---

## 目录 / Table of Contents

- [开发环境要求 / Development Requirements](#开发环境要求--development-requirements)
- [快速搭建 / Quick Setup](#快速搭建--quick-setup)
- [编码规范 / Coding Conventions](#编码规范--coding-conventions)
- [提交信息格式 / Commit Message Format](#提交信息格式--commit-message-format)
- [PR 工作流 / Pull Request Workflow](#pr-工作流--pull-request-workflow)
- [三后端一致性验证 / Three-Backend Consistency](#三后端一致性验证--three-backend-consistency)
- [测试指南 / Testing Guide](#测试指南--testing-guide)
- [国际化指南 / i18n Guidelines](#国际化指南--i18n-guidelines)

---

## 开发环境要求 / Development Requirements

| 依赖项 | 最低版本 | 说明 |
|--------|---------|------|
| C++ 编译器 | C++20 | MSVC 2022+（19.51+）/ GCC 13+, Clang 16+ |
| Qt | 6.0+ | Qt 6.0 或更高版本（经 CI 验证 6.10.3） |
| CMake | 3.25+ | 构建系统生成器 |
| Ninja | 1.10+ | 推荐的构建后端（替代 Make） |

可选依赖：
- **ccache / sccache**：编译缓存加速（通过 `-DMINILANG_USE_CCACHE=ON` 启用）
- **Graphviz (dot)**：Doxygen API 文档类图生成（通过 `-DMINILANG_BUILD_DOCS=ON` 启用）
- **Qt Linguist**：翻译文件编辑（`lupdate` / `lrelease`）

---

## 快速搭建 / Quick Setup

### Windows

```bat
REM 1. 确保已安装 Visual Studio 2022（含 C++ 桌面开发工作负载）
REM 2. 确保 Qt 6.x 已安装（如 C:\Qt\6.10.3\msvc2022_64）
REM 3. 运行构建脚本：
scripts\build_ide.bat

REM Release 构建：
scripts\build_ide.bat release

REM 运行测试：
scripts\run_tests.bat
```

构建脚本 `scripts/build_ide.bat` 会自动检测 Visual Studio 和 Qt 安装路径，配置 CMake 并调用 Ninja 构建。

### Linux

```bash
# 安装依赖（Ubuntu/Debian 示例）
sudo apt install cmake ninja-build qt6-base-dev g++

# 配置与构建
cmake -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu
cmake --build out/build/debug --target minilang_ide

# 运行测试
cmake --build out/build/debug --target minilang_tests
./out/build/debug/minilang_tests
```

### macOS

```bash
# 安装依赖（Homebrew 示例）
brew install cmake ninja qt@6

# 配置与构建
cmake -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build out/build/debug --target minilang_ide
```

---

## 编码规范 / Coding Conventions

### 通用规则

| 规则 | 示例 |
|------|------|
| **缩进** | 4 个空格，不使用 Tab |
| **行宽上限** | 120 字符 |
| **类名** | `CamelCase`（如 `BytecodeChunk`、`IRBuilder`） |
| **方法/函数名** | `camelBack`（如 `visitVarDecl`、`getCurrentFrame`） |
| **成员变量后缀** | `_` 后缀（如 `tryDepth_`、`chunk_`、`opProfileCounts_`） |
| **常量** | `kCamelCase` 前缀（如 `kMaxTraceEntries`）或全大写 `MAX_RECURSION_DEPTH` |
| **枚举值** | 全大写下划线分隔（如 `OP_INT`、`NODE_VAR_DECL`） |

### C++ 特定规则

- **头文件守卫**：使用 `#pragma once`
- **智能指针优先**：`std::unique_ptr` 管理独占所有权，`std::shared_ptr` 管理共享所有权
- **const 正确性**：所有只读访问器标记 `const`，`mutable` 仅用于惰性缓存
- **switch 完整性**：封闭枚举的 switch 必须包含 `default` 分支
- **NaN-boxing 访问**：访问 Value 前必须用 `isXxx()` 校验类型

### 示例

```cpp
class BytecodeChunk {
public:
    int getColumn(int ip) const;    // const 只读访问器
    void addConstant(Value val);

private:
    std::vector<Value> constants_;  // 成员变量 _ 后缀
    static constexpr int kMaxConstants = 1024;  // kCamelCase 常量
};
```

---

## 提交信息格式 / Commit Message Format

采用 Conventional Commits 格式：

```
type(scope): description

[可选正文]

[可选脚注]
```

### 允许的 type

| type | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `refactor` | 重构（不改变外部行为） |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `docs` | 文档更新 |
| `build` | 构建系统/外部依赖 |
| `style` | 格式调整（不影响逻辑） |
| `chore` | 杂项维护 |

### 常用 scope

`lexer`, `parser`, `compiler`, `vm`, `ir`, `interpreter`, `gui`, `debug`, `i18n`, `cmake`, `samples`, `docs`

### 示例

```
feat(vm): add register-based VM backend with 32 virtual registers

Implement RegisterBytecode (50+ RegOp) and RegisterVM interpretation
loop. Enabled via setUseRegisterVM(true), disabled by default.

Refs: PERF-14
```

```
fix(compiler): strict type matching in addConstant dedup

Prevent int 0 and float 0.0 from being merged into the same constant
pool index, which caused type mismatch at runtime.
```

---

## PR 工作流 / Pull Request Workflow

1. **Fork & Branch**：从 `main` 创建特性分支（`feat/xxx` 或 `fix/xxx`）
2. **开发**：遵循编码规范，确保本地构建通过
3. **测试**：运行全量测试，确保无回归（见下方测试指南）
4. **提交**：使用规范的提交信息格式
5. **PR 描述**：说明变更的目的、方案和测试计划
6. **Review**：至少一位维护者审核通过后方可合并
7. **合并**：使用 squash merge 保持主线整洁

### PR 检查清单

- [ ] 全量测试通过（`scripts/run_tests.bat`）
- [ ] 三后端一致性验证通过（见下方）
- [ ] 新增功能有对应测试用例
- [ ] 用户可见字符串已用 `mlTr()` 包裹（见 i18n 指南）
- [ ] 文档已更新（如需要）

---

## 三后端一致性验证 / Three-Backend Consistency

MiniLang 维护三个执行后端，语义变更必须确保三后端行为一致：

| 后端 | 说明 | 启用方式 |
|------|------|---------|
| **Interpreter** | 树遍历解释器（基准） | 默认 |
| **StackVM** | 栈式虚拟机 | `setVM(true)` |
| **RegisterVM** | 寄存器式虚拟机 | `setVM(true)` + `setUseRegisterVM(true)` |

### 验证方法

1. 编写测试用例覆盖新语义特性
2. 在 `tests/TestVME2E.cpp` 中添加三后端对比测试
3. 确保 Interpreter / StackVM / RegisterVM（含 IR 优化）四种组合输出一致
4. IR 路径额外验证：`setIR(true)` + `setIROptimize(true)` 不改变语义

---

## 测试指南 / Testing Guide

### 构建测试

```bat
REM Windows
scripts\run_tests.bat

REM 或直接使用 CMake
cmake --build out/build/debug --target minilang_tests
out\build\debug\minilang_tests.exe
```

### 测试分类

| 测试文件 | 覆盖范围 |
|---------|---------|
| `TestLexer.cpp` | 词法分析器 |
| `TestParser.cpp` | 语法分析器 |
| `TestCompiler.cpp` | 编译器（字节码生成） |
| `TestIR.cpp` | IR 中间层 |
| `TestInterpreterE2E.cpp` | 解释器端到端 |
| `TestVME2E.cpp` | VM 端到端（含三后端一致性） |
| `TestNaNBox.cpp` | NaN-boxing 编码 |
| `TestBuiltinMethods.cpp` | 内置方法 |
| `TestFormatterAudit.cpp` | 代码格式化器 |
| `TestTeachingPanelsAudit*.cpp` | 教学面板数据完整性 |

### 测试命名约定

```
套件名: TestXxx 或 XxxSuite
用例名: 描述性名称（如 IntAndFloatZeroNotDeduped）
```

---

## 国际化指南 / i18n Guidelines

### 规则

所有用户可见的字符串 **必须** 使用 `mlTr()` 或 `tr()` 包裹：

```cpp
// 非 QObject 类（gui/ 下的面板、工具类等）
label->setText(mlTr("打开文件夹"));

// QObject 派生类中
action->setText(tr("运行"));

// 带翻译上下文
statusBar->showMessage(mlTrCtx("CodeEditor", "行号"));
```

### 不需要包裹的字符串

- 内部日志/调试信息
- 错误码标识符
- MiniLang 语言关键字

### 翻译文件维护

```bash
# 启用 i18n 构建
cmake -B out/build/debug -G Ninja -DMINILANG_ENABLE_I18N=ON

# lupdate 自动扫描 mlTr() 调用，更新 .ts 文件
# lrelease 编译 .ts -> .qm
```

翻译文件位于 `app/translations/minilang_zh_CN.ts`。

---

## 项目结构概览

```
ide/
├── app/              # 应用层（IdeController, DebugCoordinator, main.cpp）
├── ast/              # AST 节点定义
├── compiler/         # 编译器（Compiler, Bytecode, IR, VM, RegisterVM）
├── gui/              # GUI 面板与组件
├── interpreter/      # 树遍历解释器 + NaN-boxing Value
├── lexer/            # 词法分析器
├── parser/           # 语法分析器
├── samples/mini/     # MiniLang 示例程序
├── tests/            # 单元测试与审计测试
├── cmake/            # CMake 模块
├── scripts/          # 构建脚本
├── docs/             # 开发文档
└── CMakeLists.txt    # 顶层构建配置
```

---

## 行为准则

- 尊重每位贡献者，保持建设性讨论
- 代码审查聚焦技术本身，不对人
- 遇到不确定的设计决策，先开 Issue 讨论再动手

---

*最后更新：2026-07-05*
