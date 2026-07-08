# MiniLang IDE

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt6](https://img.shields.io/badge/Qt-6-green)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-1762-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境。通过从零实现一门完整编程语言（词法分析 → 解析 → 解释/编译 → 虚拟机）来教授编译原理与运行时设计的核心概念。

与同类教学项目不同，MiniLang 同时实现了**三套执行引擎**（树遍历解释器、栈式字节码 VM、寄存器式 VM）并共享一套 IR 中间表示层，配合完整的 Qt GUI 可视化面板，让学习者直观对比不同执行模型的差异与取舍。

## 功能特性

**语言能力**：int/float/bool/string/null 类型、闭包、类继承、数组/字典 (COW)、字符串插值、try/catch 异常、模块系统 (import/export)

**三执行引擎**：

| 特性 | 树遍历解释器 | 栈式 VM | 寄存器式 VM |
|------|------------|---------|------------|
| 执行方式 | 递归遍历 AST | 取指-解码-执行 | 取指-解码-执行 |
| 数据传递 | 函数返回值 | 操作数栈 push/pop | 32 虚拟寄存器 R0-R31 |
| 适用场景 | 教学调试、REPL | 通用执行、字节码可视化 | 性能对比、IR 研究 |

**IDE 能力**：语法高亮编辑器、AST 树形图、字节码反汇编、IR 可视化、调试器（断点/单步/变量监视）、代码格式化器、REPL（含 %magic 命令）、20+ 教学面板

## 架构

```
┌──────────────────────────────────────────────────────────┐
│  GUI 层 (app/ide.cpp, gui/*)                             │
│  QMainWindow + 各面板，仅处理事件与 UI 更新               │
├──────────────────────────────────────────────────────────┤
│  业务层 (app/IdeController)                              │
│  编译管线编排、Worker 线程管理、调试状态、VM 单步          │
├──────────────────────────────────────────────────────────┤
│  Worker 层 (app/InterpreterWorker)                       │
│  独立 QThread 执行解释器，信号回传结果                    │
├──────────────────────────────────────────────────────────┤
│  引擎层 (minilang_core 静态库)                           │
│                                                          │
│  Lexer -> Parser -> AST                                  │
│       |            |                                     │
│       |            +-> Formatter (代码格式化)             │
│       |            |                                     │
│       |            +-> Interpreter (引擎1: 树遍历)        │
│       |            |                                     │
│       |            +-> Compiler -> BytecodeChunk          │
│       |            |       |                             │
│       |            |       +-> VM (引擎2: 栈式)           │
│       |            |                                     │
│       |            +-> AstIRBuilder -> IRModule           │
│       |                    |         |                   │
│       |                    |         +-> BytecodeIRBackend│
│       |                    |         |    -> VM (引擎2)   │
│       |                    |                             │
│       |                    +-> RegisterBytecodeBackend    │
│       |                             |                   │
│       |                             +-> RegisterVM       │
│       |                                (引擎3: 寄存器式)  │
│       |                                                  │
│       +-> DebugController (断点/单步/条件求值)            │
└──────────────────────────────────────────────────────────┘
```

## 快速构建

### 环境要求

- CMake >= 3.25、C++20 编译器（MSVC 19.51+ / GCC 13+ / Clang 16+）
- Qt6 >= 6.0（CI 验证版本 6.10.3；需 Core/Gui/Widgets/Svg/Xml）

### Windows

```powershell
configure.bat              # 自动检测 VS + Qt6，配置 CMake
cmake --build out/build/debug
./out/build/debug/minilang_ide.exe
```

### Linux / macOS

```bash
./scripts/build.sh            # 自动检测 Qt6，配置并构建（Debug）
./scripts/build.sh release    # Release 构建
./out/build/linux-release/minilang_ide
```

### Docker

```bash
docker compose run --rm build   # 容器内构建
docker compose up dev           # 运行（需 X11 转发）
```

> 详细构建说明、CMake 选项、Docker 使用方法参见 [docs/getting-started.md](docs/getting-started.md)

## 语言示例

```text
// 变量与函数
var name = "MiniLang";
fun fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

// 类与继承
class Animal {
    fun speak() { print(this.name + " makes a sound"); }
}
class Dog extends Animal {
    fun speak() { super.speak(); print(this.name + " barks"); }
}

// 字符串插值 + 异常处理
try {
    print("Hello, {name}! fib(10) = {fib(10)}");
} catch (e) {
    print("Error: " + e);
}
```

更多示例见 `samples/mini/` 目录。

## 目录结构

| 目录 | 说明 |
|------|------|
| `lexer/` | 词法分析器，Token 类型定义 |
| `parser/` | 递归下降解析器，生成 AST |
| `ast/` | AST 节点定义与 Visitor 接口 |
| `interpreter/` | 树遍历解释器、Environment、Value、内置方法 |
| `compiler/` | 字节码编译器、IR 中间表示层、VM、BytecodeChunk |
| `debug/` | DebugController（断点/单步/条件求值/变量快照） |
| `formatter/` | 代码格式化器（Visitor 模式） |
| `gui/` | Qt6 GUI 组件（编辑器/AST 视图/教学面板等） |
| `app/` | IdeController、InterpreterWorker、Ide 主窗口 |
| `common/` | Diagnostic、Logger、IBackend、TypeChecker |
| `tests/` | GoogleTest 单元测试（1762 个） |
| `docs/` | 架构文档、开发指南、设计决策记录 |

## 测试

```bash
# Windows
./out/build/debug/tests/minilang_tests.exe

# Linux/macOS
./scripts/run_tests.sh

# CTest 筛选
cd out/build/debug && ctest -R LexerTest.* --verbose
```

项目包含 **1762 个 GoogleTest 单元测试**（195 个测试套件），覆盖前端（Lexer/Parser）、解释器、编译器与虚拟机、IR 中间层、三后端一致性、格式化器、教学面板数据完整性等。

## 文档

| 文档 | 说明 |
|------|------|
| [快速开始](docs/getting-started.md) | 构建、运行、开发环境配置 |
| [核心架构](docs/architecture.md) | 编译管线、NaN-boxing、三后端、内存模型 |
| [开发指南](docs/development.md) | 文档导航、设计决策记录索引 |
| [测试指南](docs/testing.md) | 三后端一致性验证方法论 |
| [设计决策 (ADR)](docs/adr/) | 架构决策记录 |
| [FAQ](docs/faq.md) | 常见问题 |
| [变更日志](CHANGELOG.md) | 开发演进历史 |

## 贡献

本项目为教学项目，欢迎提交 Issue 报告问题或建议。详细的工程约定参见 [docs/development.md](docs/development.md)。

## 许可证

本项目为教学用途，采用 MIT 许可证。详见 [LICENSE](./LICENSE)。
