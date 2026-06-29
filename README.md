# MiniLang IDE

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境，包含自研词法分析器、递归下降解析器、栈式字节码虚拟机、树遍历解释器、调试器、代码格式化器、IR 中间表示层与完整 GUI。

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
- **词法/语法分析可视化**：Token 表格、AST 树形图（Reingold-Tilford 算法自动布局，可缩放/平移，父节点居中于子节点群）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令
- **IR 中间表示层**：可选 AST → IR → Bytecode 三段式编译，52 个 IROp 指令，支持 SSA-like 虚拟寄存器、多函数 lowering、闭包 upvalue 捕获、写回指令、全局槽位分配、优化 pass（常量折叠 / 死代码消除）、IR 可视化面板与 IR 调试器集成
- **三执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入）
  - 栈式字节码 VM（支持单步、栈/全局变量监视）
  - 寄存器式 VM（32 虚拟寄存器 R0-R31，50+ RegOp，IR 三地址码直接 lowering）
- **调试器**：断点（含条件断点）、单步进入/跳过/跳出、变量监视、调用栈
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` 或未闭合字符串自动续行，正确处理字符串插值 `"...{expr}..."` 的嵌套上下文），续行中按空行可中止；表达式语句自动求值并打印结果；异步执行不阻塞 UI

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
│  Lexer → Parser → AST → Formatter               │
│              ↓                                   │
│            Interpreter (树遍历)                  │
│              ↓                                   │
│       Compiler → IR → Bytecode → VM             │
│            DebugController (断点/单步)           │
└─────────────────────────────────────────────────┘
```

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
| `tests/` | GoogleTest 单元测试（705 个） |
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

项目包含 **747 个 GoogleTest 单元测试**，覆盖所有核心模块：

| 测试套件 | 覆盖模块 |
|----------|----------|
| `LexerTest` | lexer/ |
| `ParserTest` | parser/ |
| `TestInterpreterE2E` | interpreter/ (端到端) |
| `TestBuiltinMethods` | interpreter/BuiltinMethods |
| `TestCompiler` | compiler/Compiler |
| `TestVME2E` | compiler/VM (端到端) |
| `TestValue` | interpreter/Value |
| `TestIR` | compiler/IR（中间表示层） |
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
print(arr.len());  // 4

var cfg = {"key": "value", "count": 42};
print(cfg["key"]);  // value
```

### REPL 面板行为

REPL 面板位于 IDE 底部，支持交互式求值与多行输入：

- **续行判定**：输入未闭合的 `{ ( [`、未闭合的字符串字面量、未闭合的块注释、`try` 缺少 `catch` 时自动进入续行模式（提示符 `... `）。续行判定正确识别字符串插值 `"...{expr}..."` 的嵌套上下文——`{` 在字符串内开启表达式上下文，`}` 闭合后回到字符串模式
- **中止续行**：续行模式下按回车（空行）可中止续行，丢弃已累积的输入并退出续行模式
- **表达式求值**：表达式语句（如 `1 + 2;`）自动求值并打印结果。注意：MiniLang 语句必须以 `;` 结尾（`1 + 2` 无分号会报"期望 ';'"）
- **特殊命令**：
  - `help` — 显示语法与 REPL 行为帮助
  - `clear` — 清空输出区与续行缓冲（**不**重置已定义的变量/函数/类，重置全部状态需重启 IDE）
- **异步执行**：每行输入通过 `std::async` 在独立线程执行，UI 不阻塞。执行期间输入框禁用，完成后自动恢复并打印结果
- **跨行定义**：REPL 支持跨行定义函数与类。先 `class A {}` 再 `class B extends A {}` 可正常解析父类（类继承父类名存字符串运行时查找，与文件模式一致）。函数无提升——`f(); fun f() {}` 会报"未定义的函数: f"

## 工程约定

- **PERF-12 NaN-boxing**：Value 类型使用 8 字节 NaN-boxing 编码（sizeof 从 24 字节降至 8 字节），标量（int/float/bool/null）内联存储零原子操作，堆类型通过侵入式引用计数（RefCounted 基类）管理
- **PERF-13 VMStack**：VM 操作数栈使用定长数组（1024 × 8B = 8KB）+ 栈顶指针替代 `std::vector`，消除 `push_back` 容量检查和堆分配开销
- **PERF-14 寄存器式 VM**：双后端并存策略，新增独立 RegisterBytecode（50+ RegOp，32 虚拟寄存器 R0-R31）+ RegisterBytecodeBackend（IR 三地址码直接 lowering）+ RegisterVM 解释循环，通过 `setUseRegisterVM(true)` 启用；默认关闭，栈式 VM 完整保留
- **PERF-15 IR 复制传播**：`copyPropagationPass` 完整实现，记录 vreg→常量等价关系并替换后续引用；仅在寄存器式后端启用（`optimizeIR` 新增 `enableCopyPropagation` 参数），栈式后端禁用避免删除 LOAD_CONST 导致栈下溢
- **COW 优化**：Value 类型使用 Copy-On-Write，堆类型修改前通过 `ensureUnique<T>()` 检查 `isUnique()` 确保独占所有权
- **DoS 防护**：源码 ≤10MB、Token ≤100 万、循环 ≤1000 万次、字符串 reserve ≤1MB
- **递归保护**：Parser/Compiler/Formatter/equals() 均有深度限制（512/256）
- **线程安全**：DebugController 使用 mutex + atomic，Worker 线程通过 Qt 信号回传；`std::localtime()` 等非线程安全函数替换为 `localtime_s` (Windows) / `localtime_r` (POSIX)
- **异常安全**：UI 槽函数包裹 try/catch，确保异常时回滚 UI 状态；深拷贝操作使用 `std::unique_ptr` 包裹防止 bad_alloc 泄漏
- **内存安全**：所有 assert 检查的越界/类型保护在 Release 构建中使用运行时检查（`std::abort` / `runtimeError`）；Value 数组/容器强制 value-initialize 防止 NaN-box 未初始化内存被误判为合法 float
- **防御性编程**：封闭枚举 switch 必须加 default 分支；边界检查覆盖全深度、全路径、全状态；测试 helper 检查中间错误状态而非仅断言输出
- **生命周期安全**：不持有外部对象的裸指针（编译器结果、实例绑定等），改用 `std::optional` 按值拷贝或 `std::shared_ptr` 共享所有权；QThread::terminate 后立即退出进程（损坏状态无法安全析构）
- **资源管理**：unique_ptr 管理 QThread（自定义删除器先 quit+wait 再 delete）
- **IR 中间层**：可选 AST → IR → Bytecode 三段式编译，IRBuilder/IRBackend 抽象接口可扩展多后端；启用 `setIR(true)` + `setIROptimize(true)` 后在 lowering 前执行常量折叠 / 死代码消除 pass（最多 3 轮迭代）；寄存器式后端额外启用复制传播（`setUseRegisterVM(true)` + `setIROptimize(true)`）

### 性能优化阶段成果

近期完成了多个性能与正确性优化批次，全部通过 705/705 单元测试：

- **VMUpvalue O(1) 帧定位**（#18）：`VMUpvalue` 新增 `owningFrameIdx` 字段，`OP_SET_UPVALUE` 用 O(1) 索引替代原 O(frames) 线性扫描定位目标帧；passthrough upvalue 复用 shared_ptr 自动透传该字段
- **executeReturn 字段同步合并遍历**（#19）：Path B1 中"写 caller `this.fields()`"与"写 caller field slots"两次 O(fieldCount) 遍历合并为单次遍历，slot 索引用 `BytecodeChunk::fieldSlotIndex` O(1) 查找
- **内建方法枚举分发**（#20）：提取共享自由函数 `classifyBuiltinMethod(name) -> BuiltinMethod` 枚举，VM / RegisterVM / Interpreter 共用；按 `name.size()` 快速筛选 + 单次字符串比较 + `switch` 分发，消除长串 if-else 字符串比较链；共享方法名（`len` / `contains`）用 fallthrough case 标签跨类型复用
- **Environment boundInstance_ 缓存**（#21）：`get` / `set` / `hasVariable` 缓存 `lastCheckedInstance` 指针，`boundInstance_` 沿作用域链继承且同链多数层指针相同，命中缓存时跳过重复 `fields()` 哈希查找；仅当链上出现不同 `boundInstance_`（`bindInstance` 显式覆盖）时才重新检查
- **类方法哈希索引**（#12）：`VM` 新增两级 `unordered_map<className, unordered_map<methodName, BytecodeChunk*>>` 索引，`initExecution` 一次性扫描 `functionChunks_` 构建，`findMethodChunk` 用哈希查找替代每层继承链拼 "Class.method" 字符串
- **寄存器帧零堆分配**（#8）：`RegCallFrame::registers` 从 `std::vector<Value>` 改为 `std::array<Value, 32>` + `uint8_t registerCount`，利用 32 寄存器硬上限消除每帧堆分配
- **VMStack 定长数组**（PERF-13）：VM 操作数栈使用 `Value[1024]` + `size_t top_` 替代 `std::vector`，消除 `push_back` 容量检查与堆分配开销
- **NaN-boxing Value**（PERF-12）：`sizeof(Value)` 从 24 字节降至 8 字节，标量内联存储零原子操作，堆类型通过侵入式 `RefCounted` 基类管理一次原子递增

## 许可证

本项目为教学用途。
