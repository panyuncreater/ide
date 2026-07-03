# MiniLang IDE

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt6](https://img.shields.io/badge/Qt-6-green)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-1047-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境，包含自研词法分析器、递归下降解析器、栈式字节码虚拟机、树遍历解释器、调试器、代码格式化器、IR 中间表示层与完整 GUI。

本项目旨在通过从零实现一门完整编程语言（词法分析 -> 解析 -> 解释/编译 -> 虚拟机）来教授编译原理与运行时设计的核心概念。所有实现均可在 IDE 中可视化追踪，包括 Token 流、AST 树形图、字节码反汇编、IR 可视化与调试器单步执行。适合对编译原理、语言实现或运行时系统感兴趣的开发者与学习者。

与同类教学项目（如 Crafting Interpreters 的 Lox）不同，MiniLang 同时实现了**三套执行引擎**（树遍历解释器、栈式字节码 VM、寄存器式 VM）并共享一套 IR 中间表示层，配合完整的 Qt GUI 可视化面板，让学习者可以直观对比不同执行模型的差异与取舍。

## 界面预览

> 截图待补充。计划包含：
>
> - IDE 主界面（编辑器 + AST 视图 + 调试面板）
> - REPL 交互面板
> - IR 可视化面板
>
> 如需添加截图，请将图片放入 docs/screenshots/ 目录并在此处引用。

## 目录

- [界面预览](#界面预览)
- [功能特性](#功能特性)
- [架构](#架构)
- [构建与运行](#构建与运行)
- [快速开始](#快速开始)
- [开发环境](#开发环境)
- [测试](#测试)
- [语言示例](#语言示例)
- [示例程序](#示例程序)
- [贡献](#贡献)
- [许可证](#许可证)

## 功能特性

### 语言能力

- **基础类型**：`int` / `float` / `bool` / `string` / `null`，支持类型注解（`int a = 5;`）。`int` 支持 int48 范围内联存储，超范围自动装箱；`string` 按 UTF-8 码位语义处理（length/substr/indexOf 均按码位计数）
- **变量与赋值**：`var x = 10;` 动态声明，也支持类型注解。支持类型注解，三后端统一强制。null 兼容所有类型注解，float 注解接受 int 值（宽化），int 注解拒绝 float 值
- **控制流**：`if/else`、`while`、`for (init; cond; upd)`，支持嵌套块作用域。循环支持 `break`/`continue`，`continue` 跳过当前迭代，`break` 退出循环
- **函数**：`fun f(x) { ... }`，支持闭包与 upvalue 捕获（3+ 层）。函数无提升——`f(); fun f() {}` 会报"未定义的函数"。支持默认参数（`fun f(a=1, b=2)`），参数从左到右求值，不允许前向引用
- **类与继承**：`class A extends B { ... }`，支持 `super.method()` 调用与字段同步回实例。类继承父类名存字符串运行时查找，支持跨行定义（REPL 中先 `class A {}` 再 `class B extends A {}`）。类方法自动预留 slot 0 给 `this`，字段按继承链展平存储。三后端 super 调用语义一致
- **数据结构**：数组 `[1, 2, 3]`、字典 `{"key": val}`，支持索引读写。字典键强制为 string，键不存在时返回 null。数组/字典使用 Copy-On-Write（COW），写前检查独占所有权
- **运算符**：算术 `+ - * / %`、比较 `== != < > <= >=`、逻辑 `and or not`。整数除法截断向零（三后端统一）。`and/or` 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（三后端统一，非布尔）。`+` 支持字符串拼接（任一操作数为字符串即触发）
- **内置方法**：`push` / `pop` / `len` / `substr` / `replace` / `join` / `has` / `contains` / `get` / `set` 等。`len`/`contains` 跨类型复用（数组/字典/字符串共用）。`substr` 负索引/超长索引返回空串。`replace` 全局字面量替换（非正则）
- **字符串插值**：`"hello {name}, count={x+1}"` 支持表达式嵌入。插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。空插值 `{}` 报语法错误
- **异常处理**：`try { ... } catch (e) { ... } throw expr`。`throw` 可抛任意值。`catch` 参数独占 slot（防止覆盖外层变量）。`try` 缺失 `catch` 会导致 REPL 续行提示
- **模块系统**：`import {a, b} from "module";`、`import * as m from "module";`、`export var x = 1;`、`export fun f() {}`。import/export 限制在顶层作用域。模块路径非空校验，路径遍历防护（拒绝 ".." 父目录引用和绝对路径）。VM 路径通过编译期模块内联支持 import

### IDE 能力

- **代码编辑器**：语法高亮、行号、断点标记、错误下划线、当前执行行高亮、代码折叠（基于块结构）、查找替换（带迭代限制防 UI 阻塞）
- **词法/语法分析可视化**：Token 表格（10000 行显示上限）、AST 树形图（自动布局，可缩放/平移）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令，同时兼容栈式 VM（OpCode）和寄存器式 VM（RegOp）
- **IR 中间表示层**：可选 AST -> IR -> Bytecode 三段式编译，52 个 IROp 指令，支持 SSA-like 虚拟寄存器、多函数 lowering、闭包 upvalue 捕获、写回指令、全局槽位分配、优化 pass（常量折叠 / 死代码消除 / 复制传播）、IR 可视化面板与 IR 调试器集成。IR 路径退出后自动恢复配置，不污染后续编译路径
- **三执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入、条件断点沙箱求值）
  - 栈式字节码 VM（支持单步、栈/全局变量监视、条件断点）
  - 寄存器式 VM（32 虚拟寄存器 R0-R31，50+ RegOp，IR 三地址码直接 lowering）
- **调试器**：断点（含条件断点，条件求值沙箱隔离程序状态）、单步进入/跳过/跳出、变量监视、调用栈、首行断点 pre-execution 检查。双后端调试路径暂停语义文档化：Interpreter pre-execution 暂停，VM post-execution 检查（IP 指向下条指令，变量快照反映断点行 pre-execution 状态）
- **类型检查器**：编译期类型注解检查，三后端运行时统一强制，类型违反报告警告
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释。幂等性 + 往返不变量验证（AST 结构比较），括号保留遵循运算符优先级（右嵌套同优先级加括号，左嵌套冗余括号丢弃）
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` / 未闭合字符串 / 未闭合块注释 / `try` 缺失 `catch` 自动续行，正确处理字符串插值嵌套上下文），续行中按空行可中止；表达式语句自动求值并打印结果；异步执行不阻塞 UI；`help`/`clear` 特殊命令

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

### 三引擎对比

| 特性 | 树遍历解释器 | 栈式 VM | 寄存器式 VM |
|------|------------|---------|------------|
| 执行方式 | 递归遍历 AST | 取指-解码-执行循环 | 取指-解码-执行循环 |
| 中间表示 | 无（直接执行 AST） | BytecodeChunk（~60 OpCode） | RegBytecodeChunk（~50 RegOp） |
| 数据传递 | 函数返回值 | 操作数栈（push/pop） | 32 虚拟寄存器（R0-R31） |
| IR 优化 | 不适用 | 可选（常量折叠/死代码消除） | 可选（+ 复制传播） |
| 调试支持 | 完整（DebugController） | 有限（VmStepper 单步） | 有限（VmStepper 单步） |
| 适用场景 | 教学调试、REPL | 通用执行、字节码可视化 | 性能对比、IR 研究 |

### 目录结构

| 目录 | 说明 |
|------|------|
| `lexer/` | 词法分析器，Token 类型定义 |
| `parser/` | 递归下降解析器，生成 AST |
| `ast/` | AST 节点定义与 Visitor 接口 |
| `interpreter/` | 树遍历解释器、Environment、Value、内置方法 |
| `compiler/` | 字节码编译器、IR 中间表示层、VM、BytecodeChunk |
| `debug/` | DebugController（断点/单步/条件求值/变量快照） |
| `formatter/` | 代码格式化器（Visitor 模式） |
| `gui/` | Qt6 GUI 组件（编辑器/AST 视图/调试面板等） |
| `app/` | IdeController、InterpreterWorker、main、Ide 主窗口 |
| `common/` | Diagnostic、Logger、IBackend、TypeChecker |
| `tests/` | GoogleTest 单元测试（1047 个） |
| `test_harness/` | 独立测试工具（AST/格式化器/调试一致性审计） |
| `samples/mini/` | MiniLang 示例程序（模块系统演示） |
| `docs/` | 开发指南与文档 |

## 构建与运行

### 环境要求

- **CMake** >= 3.20
- **C++20** 编译器（MSVC 19.51+ / GCC 13+ / Clang 16+）
- **Qt6** >= 6.0（Core / Gui / Widgets）
- **GoogleTest**（随 `third_party/` 提供，CMake 自动拉取）

### Windows 一键构建（推荐）

项目提供 `configure.bat` 自动检测本机 Visual Studio 与 Qt6 安装位置，调用对应的 CMake Preset 完成配置：

```powershell
# 1. 配置（首次）—— 自动初始化 MSVC 环境 + 检测 Qt6
configure.bat              # Debug（默认）
configure.bat release      # Release

# 2. 编译
cmake --build out/build/Qt-Debug
# 或：cmake --build out/build/Qt-Release

# 3. 运行 IDE（Qt DLL 已由 windeployqt 自动部署到同目录，可直接双击运行）
./out/build/Qt-Debug/minilang_ide.exe

# 4. 运行单元测试
./out/build/Qt-Debug/tests/minilang_tests.exe
# 或：cd out/build/Qt-Debug && ctest -R LexerTest.* --verbose  # 筛选单个用例
```

> **Qt 路径**：`configure.bat` 默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`。如安装在别处，先设置环境变量：
> `set QTDIR=<你的Qt路径>/msvc2022_64` 再运行 `configure.bat`。

### 手动配置（替代方式）

若不使用 `configure.bat`，可直接调用 CMake（需自行初始化 MSVC 开发环境并设置 Qt 路径）：

```powershell
# 方式 A：使用 CMake Preset（需 CMakeUserPresets.json 指定 QTDIR）
cmake --preset Qt-Debug
cmake --build out/build/Qt-Debug

# 方式 B：手动指定生成器与 Qt 路径
cmake -S . -B build -G "NMake Makefiles" `
  -DCMAKE_PREFIX_PATH="<Qt路径>\msvc2022_64"
cmake --build build
```

### Linux / macOS

依赖：CMake >= 3.20、C++20 编译器（GCC 13+ / Clang 16+）、Qt6（Core/Gui/Widgets）

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="<Qt安装路径>"
cmake --build build
./build/minilang_ide
```

### Qt 运行时依赖部署

构建完成后，CMake 会自动调用 `windeployqt` 将 Qt6 DLL（`Qt6Core[d].dll` / `Qt6Gui[d].dll` / `Qt6Widgets[d].dll`）和 `platforms\qwindows[d].dll` 插件复制到可执行文件同目录。因此：

- **构建产物目录下的 `.exe` 可直接双击运行**，无需手动配置 PATH。
- 若手动拷贝 `.exe` 到其他目录，需重新运行 `windeployqt <exe路径>` 部署依赖，或确保目标机器已安装 Qt6 且其 `bin` 目录在 PATH 中。

### 快捷脚本

```powershell
./scripts/build_ide.bat   # 构建 IDE
./scripts/run_tests.bat   # 构建并运行测试
```

> 注意：`scripts/*.bat` 中包含机器特定的硬编码路径（如 `D:\qt\6.10.3\...`），在其他机器上需按本机环境修改。**首次构建建议使用 `configure.bat`**，它会自动探测路径。

## 快速开始

构建完成后，运行 IDE：

```powershell
# Windows
./out/build/Qt-Debug/minilang_ide.exe
```

```bash
# Linux / macOS
./build/minilang_ide
```

在编辑器中输入以下代码，点击运行：

```text
print("Hello, MiniLang!");

fun fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

for (var i = 0; i < 10; i = i + 1) {
    print(fib(i));
}
```

更多语法请参考"语言示例"章节。

## 开发环境

### 推荐 IDE

- **CLion**：直接打开 `CMakeLists.txt`，CMake Preset 自动识别，调试体验最佳
- **Qt Creator**：打开 `CMakeLists.txt`，Qt 集成度高，UI 设计方便
- **VS Code**：安装 CMake Tools + C/C++ 扩展，轻量灵活

### CMake Preset 说明

项目采用双层 Preset 体系：

- `CMakePresets.json`（仓库内）：共享配置，定义 `windows-msvc-debug` / `windows-msvc-release` 等通用 Preset
- `CMakeUserPresets.json`（gitignore）：机器特定配置，存放 Qt 安装路径等本地信息

首次克隆后运行 `configure.bat` 会自动生成 `CMakeUserPresets.json`。

### 调试技巧

- **解释器调试**：在 IDE 中点击"调试"按钮，支持断点、单步、变量监视、调用栈
- **VM 调试**：打开"VM 栈面板"查看字节码执行过程，支持单步执行
- **IR 调试**：启用 IR 路径后打开"IR 可视化面板"，查看中间表示与优化效果
- **单元测试调试**：在 CLion/Qt Creator 中直接运行 GoogleTest 用例，或使用 `ctest -R <pattern> --verbose`

### 代码规范

- 编码风格遵循项目现有约定（参见 `AGENTS.md`）
- 新增代码需通过全量测试（1047/1047）+ formatter_audit 审计
- 提交前运行 `./scripts/run_tests.bat` 确认无回归
- 详细工程约定参见 [docs/development.md](./docs/development.md)

## 测试

项目包含 **1047 个 GoogleTest 单元测试**（66 个测试套件），覆盖所有核心模块：

### 前端模块

| 测试套件 | 覆盖模块 |
|----------|----------|
| `LexerTest` | lexer/ |
| `ParserTest` | parser/ |
| `LexerAudit` / `ParserAudit` | 审计套件 |

### 解释器

| 测试套件 | 覆盖模块 |
|----------|----------|
| `InterpreterE2E` | interpreter/ (端到端) |
| `ArrayBuiltin*Test` / `DictBuiltin*Test` / `StringBuiltin*Test` | interpreter/BuiltinMethods（按类型分组） |
| `BuiltinErrorHandlingTest` / `BuiltinObjectModifiedTest` | 内建方法错误路径 |
| `Value*Test`（13 个套件） | interpreter/Value（构造/COW/深拷贝/相等/数组/字典/实例/闭包等） |

### 编译器与虚拟机

| 测试套件 | 覆盖模块 |
|----------|----------|
| `CompilerBasicTest` / `CompilerVariableTest` / `CompilerControlFlowTest` / `CompilerFunctionTest` / `CompilerClassTest` | compiler/Compiler |
| `VME2E` / `RegVME2E` / `VMConsistency` | compiler/VM / RegisterVM (端到端 + 一致性) |
| `CompilerConstantPoolTest` / `CompilerGlobalSlotTest` | compiler/ 常量池 / 全局槽位 |

### IR 中间层

| 测试套件 | 覆盖模块 |
|----------|----------|
| `IRFunctionTest` / `IRBuilderTest` / `BytecodeIRBackendTest` / `IRE2E` / `IROptimizeTest` / `IRToStringTest` | compiler/IR（中间表示层） |

### 一致性与类型

| 测试套件 | 覆盖模块 |
|----------|----------|
| `ConsistencyDiff` | 三后端一致性差分测试（190 用例） |
| `CompilerTypeCheckTest` | 类型检查器（含 float->int 拒绝断言强化） |
| `BackendConsistency` | 三后端类型注解一致性 |

### 审计与回归

| 测试套件 | 覆盖模块 |
|----------|----------|
| `ASTCompleteness` / `IntegrationAudit` | 审计套件 |
| `NaNBoxTest` / `IRShadowFix` / `NestedLvalueProbe` / `MethodCallProbe` | 回归与探针测试 |

运行测试：

```powershell
# 方式 1：直接运行测试二进制
./out/build/Qt-Debug/tests/minilang_tests.exe

# 方式 2：通过 CTest（已用 gtest_discover_tests 注册为独立用例，可筛选）
cd out/build/Qt-Debug
ctest                                  # 运行全部
ctest -R LexerTest.* --verbose         # 筛选指定套件
ctest -R "VME2E\..*" --output-on-failure
```

测试覆盖要点：
- **正常路径**：所有语言特性的 happy path 覆盖
- **错误路径**：除零、类型违反、数组越界、空指针访问、未定义变量/函数等
- **三后端一致性**：同一代码在 Interpreter / StackVM / RegisterVM 行为一致。整数除法截断向零、`and/or` 返回操作数原值、整数溢出检测均已三后端统一。已知差异：非方法上下文 super 错误消息文本不同（错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，行为一致）
- **边界条件**：接近 MAX_RECURSION_DEPTH、MAX_FRAMES 的极限场景
- **类型检查**：类型注解强制、null 兼容性、int/float 宽化规则、float->int 拒绝（断言强化：同时验证警告消息内容含期望类型名）
- **格式化器质量**：幂等性 + 表达式级 AST 往返等价性（19 用例）+ 语句级 AST 往返等价性（16 用例，覆盖 if/for/while/class/try 等结构）
- **test_harness**：独立 cl.exe 编译，含 formatter_audit（100+ 审计用例）、stress_test（100 个压力测试）、debug_test（调试不变量审计）

## 语言示例

### 基础语法

```text
// 变量与类型注解
var name = "MiniLang";
int count = 0;
float pi = 3.14159;

// 函数与闭包
fun makeCounter() {
    var n = 0;
    fun inc() {
        n = n + 1;
        return n;
    }
    return inc;
}

var counter = makeCounter();
print(counter());  // 1
print(counter());  // 2

// 类与继承
class Animal {
    fun init(name) {
        this.name = name;
    }
    fun speak() {
        print(this.name + " makes a sound");
    }
}

class Dog extends Animal {
    fun speak() {
        super.speak();
        print(this.name + " barks");
    }
}

var dog = Dog("Rex");
dog.speak();

// 数据结构
var arr = [1, 2, 3];
arr.push(4);
print(arr.len());  // 4

var cfg = {"key": "value", "count": 42};
print(cfg["key"]);  // value
```

### 进阶特性

```text
// 字符串插值
var name = "World";
var items = [1, 2, 3];
print("Hello, {name}! You have {items.len()} items.");

// 异常处理
fun divide(a, b) {
    if (b == 0) throw "Division by zero";
    return a / b;
}

try {
    var result = divide(10, 0);
    print(result);
} catch (e) {
    print("Error: " + e);  // Error: Division by zero
}

// 模块导入（需配合 samples/mini/ 中的模块文件）
// import { greet } from "string_utils.mini";
// print(greet("MiniLang"));  // Hello, MiniLang!

// 类型注解与检查
int add(int a, int b) {
    return a + b;
}

// print(add(1, 2));     // OK: 3
// print(add(1.5, 2));   // 警告: float 值不能赋给 int 参数
```

## 示例程序

`samples/mini/` 目录包含一组演示模块系统的示例程序：

| 文件 | 说明 |
|------|------|
| `main.mini` | 主程序，演示跨文件 import 与多模块协作 |
| `math_utils.mini` | 数学工具模块（add/multiply/factorial/fibonacci） |
| `string_utils.mini` | 字符串工具模块（greet/repeat/isEmpty/padLeft） |
| `shapes.mini` | 图形类模块（Shape/Circle/Rectangle 继承体系） |

运行方式：在 IDE 中打开 `samples/mini/main.mini` 并点击运行。

## 贡献

本项目为教学项目，欢迎提交 Issue 报告问题或建议。如需贡献代码，请先开 Issue 讨论方案。

详细的工程约定与开发规范参见 [docs/development.md](./docs/development.md)。

## 许可证

本项目为教学用途，采用 MIT 许可证。详见 [LICENSE](./LICENSE)。
