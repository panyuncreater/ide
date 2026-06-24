# MiniLang IDE

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境，包含自研词法分析器、递归下降解析器、栈式字节码虚拟机、树遍历解释器、调试器、代码格式化器与完整 GUI。

## 功能特性

### 语言能力
- **基础类型**：`int` / `float` / `bool` / `string` / `null`，支持类型注解（`int a = 5;`）
- **变量与赋值**：`var x = 10;` 动态声明，也支持类型注解
- **控制流**：`if/else`、`while`、`for (init; cond; upd)`，支持嵌套块作用域
- **函数**：`fun f(x) { ... }`，支持闭包与 upvalue 捕获（3+ 层）
- **类与继承**：`class A extends B { ... }`，支持 `super.method()` 调用与字段同步回实例
- **数据结构**：数组 `[1, 2, 3]`、字典 `{"key": val}`，支持索引读写
- **运算符**：算术 `+ - * / %`、比较 `== != < > <= >=`、逻辑 `and or not`
- **内置方法**：`push` / `pop` / `length` / `substr` / `replace` / `join` 等

### IDE 能力
- **代码编辑器**：语法高亮、行号、断点标记、错误下划线、当前执行行高亮
- **词法/语法分析可视化**：Token 表格、AST 树形图（可缩放/平移）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令
- **双执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入）
  - 栈式字节码 VM（支持单步、栈/全局变量监视）
- **调试器**：断点（含条件断点）、单步进入/跳过/跳出、变量监视、调用栈
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` 或字符串自动续行）

## 架构

```
┌─────────────────────────────────────────────────┐
│  GUI 层 (app/ide.cpp, gui/*)                    │
│  QMainWindow + 各面板，仅处理事件与 UI 更新      │
├─────────────────────────────────────────────────┤
│  业务层 (app/IdeController)                     │
│  编译管线编排、Worker 线程管理、调试状态、VM 单步 │
├─────────────────────────────────────────────────┤
│  Worker 层 (app/InterpreterWorker)              │
│  独立 QThread 执行解释器，信号回传结果           │
├─────────────────────────────────────────────────┤
│  引擎层 (minilang_core 静态库)                  │
│  Lexer → Parser → AST → Compiler → Bytecode → VM │
│                 ↓                                │
│            Interpreter (树遍历)                  │
│            Formatter (Visitor 模式)              │
│            DebugController (断点/单步/条件求值)   │
└─────────────────────────────────────────────────┘
```

### 目录结构

| 目录 | 说明 |
|------|------|
| `lexer/` | 词法分析器，Token 类型定义 |
| `parser/` | 递归下降解析器，生成 AST |
| `ast/` | AST 节点定义与 Visitor 接口 |
| `interpreter/` | 树遍历解释器、Environment、Value、内置方法 |
| `compiler/` | 字节码编译器、VM、BytecodeChunk |
| `debug/` | DebugController（断点/单步/条件求值/变量快照） |
| `formatter/` | 代码格式化器（Visitor 模式） |
| `gui/` | Qt6 GUI 组件（编辑器/AST 视图/调试面板等） |
| `app/` | IdeController、InterpreterWorker、main、Ide 主窗口 |
| `common/` | Diagnostic、Logger |
| `tests/` | GoogleTest 单元测试（558 个） |
| `test_harness/` | 独立测试工具（AST/格式化器/调试一致性审计） |

## 构建与运行

### 环境要求
- **CMake** ≥ 3.20
- **C++20** 编译器（MSVC 19.51+ / GCC 13+ / Clang 16+）
- **Qt6** ≥ 6.0（Core / Gui / Widgets）
- **GoogleTest**（随 `third_party/` 提供，CMake 自动拉取）

### Windows 构建（NMake Makefiles + MSVC）

```powershell
# 1. 配置（首次）
cmake -S . -B build -G "NMake Makefiles" `
  -DCMAKE_PREFIX_PATH="D:/qt/6.10.3/msvc2022_64" `
  -DCMAKE_CXX_COMPILER="D:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"

# 2. 初始化 VS 开发环境
Import-Module "D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\vs" -SkipAutomaticLocation -Arch amd64
$env:PATH = "D:\qt\6.10.3\msvc2022_64\bin;" + $env:PATH

# 3. 编译
cd build
nmake
```

### 运行

```powershell
# 启动 IDE
.\minilang_ide.exe

# 运行单元测试
.\tests\minilang_tests.exe
```

### 快捷脚本

```powershell
.\scripts\build_ide.bat   # 构建 IDE
.\scripts\run_tests.bat   # 构建并运行测试
```

## 测试

项目包含 **558 个 GoogleTest 单元测试**，覆盖所有核心模块：

| 测试套件 | 覆盖模块 |
|----------|----------|
| `LexerTest` | lexer/ |
| `ParserTest` | parser/ |
| `TestInterpreterE2E` | interpreter/ (端到端) |
| `TestBuiltinMethods` | interpreter/BuiltinMethods |
| `TestCompiler` | compiler/Compiler |
| `TestVME2E` | compiler/VM (端到端) |
| `TestValue` | interpreter/Value |
| `CompilerConstantPoolTest` | compiler/ 常量池 |
| `CompilerGlobalSlotTest` | compiler/ 全局槽位 |

运行测试：

```powershell
cd build
.\tests\minilang_tests.exe
# 或通过 CTest
ctest
```

## 语言示例

```
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
print(arr.length());  // 4

var dict = {"key": "value", "count": 42};
print(dict["key"]);
```

## 工程约定

- **COW 优化**：Value 类型使用 Copy-On-Write，读操作必须使用 const 访问器避免深拷贝
- **DoS 防护**：源码 ≤10MB、Token ≤100 万、循环 ≤1000 万次、字符串 reserve ≤1MB
- **递归保护**：Parser/Compiler/Formatter/equals() 均有深度限制（512/256）
- **线程安全**：DebugController 使用 mutex + atomic，Worker 线程通过 Qt 信号回传
- **异常安全**：UI 槽函数包裹 try/catch，确保异常时回滚 UI 状态
- **资源管理**：unique_ptr 管理 QThread（自定义删除器先 quit+wait 再 delete）

## 许可证

本项目为教学用途。
