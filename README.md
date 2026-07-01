# MiniLang IDE

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境，包含自研词法分析器、递归下降解析器、栈式字节码虚拟机、树遍历解释器、调试器、代码格式化器、IR 中间表示层与完整 GUI。

## 功能特性

### 语言能力
- **基础类型**：`int` / `float` / `bool` / `string` / `null`，支持类型注解（`int a = 5;`）。`int` 支持 int48 范围内联存储，超范围自动装箱；`string` 按 UTF-8 码位语义处理（length/substr/indexOf 均按码位计数）
- **变量与赋值**：`var x = 10;` 动态声明，也支持类型注解。类型注解三后端统一强制（OP_TYPE_CHECK / REG_TYPE_CHECK / TYPE_CHECK IR 指令），null 兼容所有类型注解，float 注解接受 int 值（宽化），int 注解拒绝 float 值
- **控制流**：`if/else`、`while`、`for (init; cond; upd)`，支持嵌套块作用域。循环支持 `break`/`continue`，`continue` 跳过当前迭代，`break` 退出循环
- **函数**：`fun f(x) { ... }`，支持闭包与 upvalue 捕获（3+ 层）。函数无提升——`f(); fun f() {}` 会报"未定义的函数"。支持默认参数（`fun f(a=1, b=2)`），参数从左到右求值，不允许前向引用
- **类与继承**：`class A extends B { ... }`，支持 `super.method()` 调用与字段同步回实例。类继承父类名存字符串运行时查找，支持跨行定义（REPL 中先 `class A {}` 再 `class B extends A {}`）。类方法自动预留 slot 0 给 `this`，字段按继承链展平存储。三后端 super 调用语义一致（BUG-INH-4 修复：IR 路径 LOAD_MUTATED + STORE_LOCAL 后补发 OP_POP 消费残留值，确保 super.method() 返回值正确）
- **数据结构**：数组 `[1, 2, 3]`、字典 `{"key": val}`，支持索引读写。字典键强制为 string（B6 fix），键不存在时返回 null。数组/字典使用 Copy-On-Write（COW），写前检查 `isUnique()` 确保独占所有权
- **运算符**：算术 `+ - * / %`、比较 `== != < > <= >=`、逻辑 `and or not`。整数除法截断向零（三后端统一，AUDIT-DIV-UNIFY 修复）。`and/or` 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（三后端统一，非布尔）。`+` 支持字符串拼接（任一操作数为字符串即触发）
- **内置方法**：`push` / `pop` / `length` / `substr` / `replace` / `join` / `has` / `contains` / `get` / `set` 等。`len`/`contains` 跨类型复用（数组/字典/字符串共用）。`substr` 负索引/超长索引返回空串。`replace` 全局字面量替换（非正则）
- **字符串插值**：`"hello {name}, count={x+1}"` 支持表达式嵌入。插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。空插值 `{}` 报语法错误
- **异常处理**：`try { ... } catch (e) { ... } throw expr`。`throw` 可抛任意值。`catch` 参数独占 slot（防止覆盖外层变量）。`try` 缺失 `catch` 会导致 REPL 续行提示
- **模块系统**：`import {a, b} from "module";`、`import * as m from "module";`、`export var x = 1;`、`export fun f() {}`。import/export 限制在顶层作用域。模块路径非空校验，路径遍历防护（拒绝 ".." 父目录引用和绝对路径，SEC-1 修复）。IR/VM 路径不支持 import，报编译错误而非崩溃（BUG-MOD-1 修复）

### IDE 能力
- **代码编辑器**：语法高亮、行号、断点标记、错误下划线、当前执行行高亮、代码折叠（基于块结构）、查找替换（带迭代限制防 UI 阻塞）
- **词法/语法分析可视化**：Token 表格（10000 行显示上限）、AST 树形图（Reingold-Tilford 算法自动布局，四 pass 后序遍历 + 子树轮廓合并 + shift 传播保证父节点居中，可缩放/平移）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令，同时兼容栈式 VM（OpCode）和寄存器式 VM（RegOp）
- **IR 中间表示层**：可选 AST → IR → Bytecode 三段式编译，52 个 IROp 指令，支持 SSA-like 虚拟寄存器、多函数 lowering、闭包 upvalue 捕获、写回指令、全局槽位分配、优化 pass（常量折叠 / 死代码消除 / 复制传播）、IR 可视化面板与 IR 调试器集成。IR 路径退出后自动恢复 `setUseIR(false)` 不污染后续编译路径
- **三执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入、条件断点沙箱求值）
  - 栈式字节码 VM（支持单步、栈/全局变量监视、条件断点）
  - 寄存器式 VM（32 虚拟寄存器 R0-R31，50+ RegOp，IR 三地址码直接 lowering）
- **调试器**：断点（含条件断点，条件求值沙箱隔离程序状态）、单步进入/跳过/跳出、变量监视、调用栈、首行断点 pre-execution 检查。双后端调试路径暂停语义文档化：Interpreter pre-execution 暂停，VM post-execution 检查（IP 指向下条指令，变量快照反映断点行 pre-execution 状态）
- **类型检查器**：编译期类型注解检查（MiniLangTypeChecker + LiteralTypeWalker），三后端运行时统一强制（OP_TYPE_CHECK / REG_TYPE_CHECK / TYPE_CHECK），共享 `typeMatchValue` 函数统一类型匹配逻辑。类型违反报告 DiagSource::TypeChecker 警告
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释。幂等性 + 往返不变量验证（AST 结构比较），括号保留遵循运算符优先级（右嵌套同优先级加括号，左嵌套冗余括号丢弃）
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` / 未闭合字符串 / 未闭合块注释 / `try` 缺失 `catch` 自动续行，正确处理字符串插值嵌套上下文），续行中按空行可中止；表达式语句自动求值并打印结果；异步执行不阻塞 UI；`help`/`clear` 特殊命令

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
| `tests/` | GoogleTest 单元测试（1047 个） |
| `test_harness/` | 独立测试工具（AST/格式化器/调试一致性审计，formatter_audit 含 100+ 审计用例） |

## 构建与运行

### 环境要求
- **CMake** ≥ 3.20
- **C++20** 编译器（MSVC 19.51+ / GCC 13+ / Clang 16+）
- **Qt6** ≥ 6.0（Core / Gui / Widgets）
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
.\out\build\Qt-Debug\minilang_ide.exe

# 4. 运行单元测试
.\out\build\Qt-Debug\tests\minilang_tests.exe
# 或：cd out/build/Qt-Debug && ctest -R LexerTest.* --verbose  # 筛选单个用例
```

> **Qt 路径**：`configure.bat` 默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`。如安装在别处，先设置环境变量：
> `set QTDIR=<你的Qt路径>\msvc2022_64` 再运行 `configure.bat`。

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

### Qt 运行时依赖部署

构建完成后，CMake 会自动调用 `windeployqt` 将 Qt6 DLL（`Qt6Core[d].dll` / `Qt6Gui[d].dll` / `Qt6Widgets[d].dll`）和 `platforms\qwindows[d].dll` 插件复制到可执行文件同目录。因此：

- **构建产物目录下的 `.exe` 可直接双击运行**，无需手动配置 PATH。
- 若手动拷贝 `.exe` 到其他目录，需重新运行 `windeployqt <exe路径>` 部署依赖，或确保目标机器已安装 Qt6 且其 `bin` 目录在 PATH 中。

### 快捷脚本

```powershell
.\scripts\build_ide.bat   # 构建 IDE
.\scripts\run_tests.bat   # 构建并运行测试
```

> 注意：`scripts/*.bat` 中包含机器特定的硬编码路径（如 `D:\qt\6.10.3\...`），在其他机器上需按本机环境修改。**首次构建建议使用 `configure.bat`**，它会自动探测路径。

## 测试

项目包含 **1047 个 GoogleTest 单元测试**（66 个测试套件），覆盖所有核心模块：

| 测试套件 | 覆盖模块 |
|----------|----------|
| `LexerTest` | lexer/ |
| `ParserTest` | parser/ |
| `InterpreterE2E` | interpreter/ (端到端) |
| `ArrayBuiltin*Test` / `DictBuiltin*Test` / `StringBuiltin*Test` | interpreter/BuiltinMethods（按类型分组） |
| `BuiltinErrorHandlingTest` / `BuiltinObjectModifiedTest` | 内建方法错误路径 |
| `CompilerBasicTest` / `CompilerVariableTest` / `CompilerControlFlowTest` / `CompilerFunctionTest` / `CompilerClassTest` | compiler/Compiler |
| `VME2E` / `RegVME2E` / `VMConsistency` | compiler/VM / RegisterVM (端到端 + 一致性) |
| `Value*Test`（13 个套件） | interpreter/Value（构造/COW/深拷贝/相等/数组/字典/实例/闭包等） |
| `IRFunctionTest` / `IRBuilderTest` / `BytecodeIRBackendTest` / `IRE2E` / `IROptimizeTest` / `IRToStringTest` | compiler/IR（中间表示层） |
| `CompilerConstantPoolTest` / `CompilerGlobalSlotTest` | compiler/ 常量池 / 全局槽位 |
| `ConsistencyDiff` | 三后端一致性差分测试（190 用例） |
| `CompilerTypeCheckTest` | 类型检查器（含 float→int 拒绝断言强化） |
| `BackendConsistency` | 三后端类型注解一致性 |
| `LexerAudit` / `ParserAudit` / `ASTCompleteness` / `IntegrationAudit` | 审计套件 |
| `NaNBoxTest` / `IRShadowFix` / `NestedLvalueProbe` / `MethodCallProbe` | 回归与探针测试 |

运行测试：

```powershell
# 方式 1：直接运行测试二进制
.\out\build\Qt-Debug\tests\minilang_tests.exe

# 方式 2：通过 CTest（已用 gtest_discover_tests 注册为独立用例，可筛选）
cd out\build\Qt-Debug
ctest                                  # 运行全部
ctest -R LexerTest.* --verbose         # 筛选指定套件
ctest -R "VME2E\..*" --output-on-failure
```

测试覆盖要点：
- **正常路径**：所有语言特性的 happy path 覆盖
- **错误路径**：除零、类型违反、数组越界、空指针访问、未定义变量/函数等
- **三后端一致性**：同一代码在 Interpreter / StackVM / RegisterVM 行为一致（已知差异：非方法上下文 super 错误消息文本不同——Interpreter 报 "super 只能在类方法中使用"，IR 路径报 "未定义的变量: this"；错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，行为一致）。整数除法截断向零、`and/or` 返回操作数原值均已三后端统一（AUDIT-DIV-UNIFY / AUDIT-ANDOR 修复）。整数溢出检测三后端统一（BUG-OVF-1/2 修复）
- **边界条件**：接近 MAX_RECURSION_DEPTH、MAX_FRAMES 的极限场景
- **类型检查**：类型注解强制、null 兼容性、int/float 宽化规则、float→int 拒绝（断言强化：同时验证警告消息内容含期望类型名）
- **格式化器质量**：幂等性 + 表达式级 AST 往返等价性（19 用例）+ 语句级 AST 往返等价性（16 用例，覆盖 if/for/while/class/try 等结构）
- **test_harness**：独立 cl.exe 编译，含 formatter_audit（100+ 审计用例）、stress_test（100 个压力测试）、debug_test（调试不变量审计）

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
  - `reload "mod"` — 清除指定模块缓存，下次 import 重新加载源码（模块缓存刷新）
  - `reload all` — 清除所有模块缓存
- **异步执行**：每行输入通过 `std::async` 在独立线程执行，UI 不阻塞。执行期间输入框禁用，完成后自动恢复并打印结果。协作式中止：closeEvent 发送 `stopRequested_` 标志，checkBreak 在每个语句节点检查并抛异常，最多等待 5 秒后强制退出
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
- **调试一致性**：条件断点求值沙箱隔离程序状态（Environment 快照/恢复）；首行断点 pre-execution 检查；双后端暂停语义文档化；VmStepper 双后端分发使用 helper 统一状态机逻辑
- **安全性硬约束**：所有 assert 检查的越界/类型保护改为运行时 `std::abort`；NaNBox 类型访问前必须用 `isXxx()` 校验；容器越界错误路径强制走 const 访问器避免 COW 深拷贝；`std::localtime()` 替换为线程安全的 `localtime_s`/`localtime_r`
- **异常安全**：UI 槽函数包裹 try/catch；深拷贝操作使用 `std::unique_ptr` 包裹防止 bad_alloc 泄漏；ensureUnique<T>() 深拷贝用 unique_ptr 包裹

### 性能优化与正确性阶段成果

近期完成了多个性能与正确性优化批次，全部通过 1047/1047 单元测试 + 100+ formatter_audit 审计用例：

> **测试质量审计 2026-06-30**：审计发现并修复 2 个测试覆盖缺口：(1) TypeChecker `int` 注解拒绝 `float` 值零覆盖——新增 5 个 CompilerTypeCheckTest 测试（A11-A15），覆盖声明/赋值路径的 float→int 拒绝，并强化断言：不仅验证警告计数，还验证警告消息内容含期望类型名，防止误报通过。(2) Formatter 语句级 AST 往返等价性仅覆盖表达式级（19 用例）——新增 `test_statements()` 函数 + 16 个 S1-S16 测试用例覆盖 if/for/while/class/try 等语句结构，对每条语句做 `Parse(src)` vs `Parse(format(src))` 的 AST 结构等价比较，检测 Formatter 在格式化时是否丢失或重组子节点。

- **VMUpvalue O(1) 帧定位**（#18）：`VMUpvalue` 新增 `owningFrameIdx` 字段，`OP_SET_UPVALUE` 用 O(1) 索引替代原 O(frames) 线性扫描定位目标帧；passthrough upvalue 复用 shared_ptr 自动透传该字段
- **executeReturn 字段同步合并遍历**（#19）：Path B1 中"写 caller `this.fields()`"与"写 caller field slots"两次 O(fieldCount) 遍历合并为单次遍历，slot 索引用 `BytecodeChunk::fieldSlotIndex` O(1) 查找
- **内建方法枚举分发**（#20）：提取共享自由函数 `classifyBuiltinMethod(name) -> BuiltinMethod` 枚举，VM / RegisterVM / Interpreter 共用；按 `name.size()` 快速筛选 + 单次字符串比较 + `switch` 分发，消除长串 if-else 字符串比较链；共享方法名（`len` / `contains`）用 fallthrough case 标签跨类型复用
- **Environment boundInstance_ 缓存**（#21）：`get` / `set` / `hasVariable` 缓存 `lastCheckedInstance` 指针，`boundInstance_` 沿作用域链继承且同链多数层指针相同，命中缓存时跳过重复 `fields()` 哈希查找；仅当链上出现不同 `boundInstance_`（`bindInstance` 显式覆盖）时才重新检查
- **类方法哈希索引**（#12）：`VM` 新增两级 `unordered_map<className, unordered_map<methodName, BytecodeChunk*>>` 索引，`initExecution` 一次性扫描 `functionChunks_` 构建，`findMethodChunk` 用哈希查找替代每层继承链拼 "Class.method" 字符串
- **寄存器帧零堆分配**（#8）：`RegCallFrame::registers` 从 `std::vector<Value>` 改为 `std::array<Value, 32>` + `uint8_t registerCount`，利用 32 寄存器硬上限消除每帧堆分配
- **VMStack 定长数组**（PERF-13）：VM 操作数栈使用 `Value[1024]` + `size_t top_` 替代 `std::vector`，消除 `push_back` 容量检查与堆分配开销
- **NaN-boxing Value**（PERF-12）：`sizeof(Value)` 从 24 字节降至 8 字节，标量内联存储零原子操作，堆类型通过侵入式 `RefCounted` 基类管理一次原子递增
- **字符串码位缓存**（#21）：StringData 缓存 `codepointCount`，消除 len()/substr() 在循环中的 O(n²) 重复扫描。缓存失效时机：非 const stringVal() 返回可变引用时 reset()
- **IR 常量/全局去重哈希表**：IRFunction::addConstant/addGlobal 使用 hash 侧表维护去重索引，消除 O(n²) 线性扫描。constants/globalNames 不允许外部直接 push_back/clear
- **RegClassInfo 懒预计算**：继承链展平字段和 init 方法名在 REG_DEFINE_CLASS 时预计算缓存，避免实例化时重复遍历继承链
- **条件断点沙箱优化**：evaluateCondition 快照作用域链变量 + 绑定实例字段，求值后恢复。残余限制：容器变异仍影响共享 ref-counted 对象（Value 引用语义），已文档化为可接受现状

### 继承链正确性修复（BUG-INH 系列）

针对三后端继承语义一致性审计发现的问题：

- **BUG-INH-1**（P0）：IR 路径丢失类字段默认值表达式——`AstIRBuilder::visitClassDecl` 未收集字段初始化器字面量，所有字段被硬编码为 null。修复：DEFINE_CLASS IR 操作数新增 fieldDefaultConstIdx，lowering 时按 Value 类型 emit 对应常量加载指令
- **BUG-INH-4**（P0）：StackVM IR 路径 `super.method()` 返回错误值——`LOAD_MUTATED + STORE_LOCAL` 后值残留导致 `RETURN` 弹出错误值。修复：visitMethodCall 的 isVarRef/isSuperCall 分支在 emitStoreVar 后，若存储目标是 LOCAL/UPVALUE 则显式 emit IROp::POP 消费残留值
- **BUG-INH-3**（P2）：super 方法未找到时三后端错误消息不一致——RegVM 报 "父类链中无方法"/"无方法:" 与 Interpreter/StackVM 的 "没有方法" 不一致。修复：统一为 "类 X 没有方法 Y"
- **BUG-INH-2**（P2，文档化）：非方法上下文中使用 super 时错误消息不一致——Interpreter 报 "super 只能在类方法中使用"，IR 路径报 "未定义的变量: this"。文档化为已知差异（不崩溃；错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，仅消息文本不同）

### 整数溢出与模块系统修复（BUG-OVF / BUG-MOD / SEC 系列）

- **BUG-OVF-1**（P1）：RegisterVM REG_NEGATE 整数溢出错误消息缺少后缀——统一为 "整数溢出：无法对最小值取负"
- **BUG-OVF-2**（P1）：RegisterVM REG_DIV 整数溢出错误消息不一致——统一为 "整数运算溢出"（对齐 Interpreter/StackVM 的 computeArith DIV 分支）
- **BUG-MOD-1**（P0）：IR 路径 import 语句崩溃（Debug）/静默生成坏 IR（Release）——AstIRBuilder 新增 hasError_/errorMessage_ 错误报告接口，NODE_IMPORT_STMT 独立 case 设置错误标志，compileViaIR/compileViaRegisterIR 检查并转化为用户可见 diagnostic
- **SEC-1**（P0）：模块路径遍历攻击漏洞——路径校验移到 loader 检查之前，拒绝 ".." 父目录引用和绝对路径，防止 import 读取项目目录外文件

### REPL 正确性修复

- **REPL 模块缓存刷新**：原重新 import 模块时 `moduleCache_` 命中即复用旧 moduleEnv，不重新调用 loader 读源码——用户修改模块源文件后重新 import 仍得旧值。新增 `Interpreter::clearModuleCache(path)` / `clearAllModuleCache()` 方法；ReplPanel 新增 `reload "mod"` / `reload all` 命令清除缓存后下次 import 重新加载
- **REPL 异步执行超时**：原 `waitReplFuture()` 调用 `replFuture_.wait()` 无超时，死循环场景下 closeEvent 永久阻塞。新增 `Interpreter::stopRequested_` 原子标志；`checkBreak` 在每个语句节点检查标志并抛 `std::runtime_error`（被异步 lambda 的 catch 捕获）；`waitReplFuture` 改为 `requestReplStop()` + `wait_for(5s)` + 超时回退阻塞。正常代码（含 checkBreak 调用）能在毫秒级响应中止

### 第六轮 Bug 修复（AUDIT-BUG-F1~F13 系列）

通过三个并行 agent 覆盖 Lexer/BuiltinFunctions/DebugController/GUI/Compiler/Interpreter 边界，共修复 13 个 bug（2 HIGH / 7 MED / 4 LOW），新增 5 个回归测试，全部 1047/1047 测试通过：

- **F1**（HIGH）：`constructClassInstance` 中 `callStack_.emplace_back` 在 `CallFrameGuard` 构造之前执行，guard 保存错误的 `savedStackDepth` 导致 init 帧不弹出，循环构造实例时 callStack_ 泄漏。修复：交换两行顺序，guard 先于 emplace_back
- **F2**（HIGH）：REPL `reload` 命令用 `startsWith("reload")` 误匹配 `reloadable`/`reloadX` 等标识符，命中后清空用户输入。修复：改为精确匹配 + 空格前缀
- **F3**（MED）：`VmStepper::vmCrossedDeeper_` 仅在 `stop()` 中重置，`stepByMode` 入口未重置，导致 STEP_OVER 退化为 STEP_IN。修复：在 stepByMode 入口添加重置
- **F4**（MED）：`DebugController::crossedLine_` 在条件断点求值之前重置，条件不满足时已清 false，单行循环中条件断点永不再触发。修复：将重置推迟到断点真正命中之前
- **F5**（MED）：`IrViewer::setUpdatesEnabled(false)` 后填充逻辑无异常保护，异常时 `setUpdatesEnabled(true)` 永不执行，列表永久冻结。修复：用 try/catch 包裹填充逻辑
- **F6**（MED）：`onRun`/`onDebug` 在 `isReplRunning()` 检查之前执行 `clearAll()`，REPL 运行时清空用户历史输出后才报错。修复：将互斥检查移到 `clearAll()` 之前
- **F7**（MED）：IR 路径 `visitTryStmt` 的 catch 变量绑定到外层 `varMap_` 且不恢复，catch 块后仍可引用，与 Interpreter/StackVM 语义不一致。修复：为三条路径（函数内、顶层遮蔽、顶层无遮蔽）添加 varMap_ 保存/恢复
- **F8**（MED）：模块导入的 try/catch 块在异常时未调用 `moduleEnv->closeCapturedVariables()`，逃逸闭包的 capturedVars 保持初始值。修复：在 catch 块中添加 closeCapturedVariables 调用
- **F9**（MED）：`RegisterVM::executeSharedBuiltinFunction` 调用仅传 3 参数，line/column 取默认值 0,0，错误消息显示"行 0:0"。修复：从当前帧的 chunk 获取行号并传入
- **F10**（LOW）：Lexer 插值表达式中嵌套字符串的 `start_` 未在 `advance()` 前更新，导致 token 列号/lexeme 错误。修复：添加 `start_ = current_`
- **F11**（LOW）：`substr` 对负 `start` 静默返回空串，但对负 `len` 报错，错误处理不对称。修复：负 `start` 改为报错
- **F12**（LOW）：`SyntaxHighlighter` 不识别字符串插值 `{expr}`，整个字符串统一着色。修复：识别 `{` 作为插值起始，用 braceDepth 跟踪嵌套，插值表达式内字符不标记 mask
- **F13**（LOW）：`CodeEditor` 空白区域点击 `cursorForPosition` 返回文档末尾光标，导致在最后一行设置断点。修复：检查 Y 坐标是否超出最后一个块的下边界

### 第七轮 Bug 修复（AUDIT-BUG-G1~G4 + H5 系列）

针对三后端 IR 路径栈语义、Parser 类型注解识别、Formatter 缩进、默认参数 RAII 等问题，共修复 12 个 bug（2 P0 / 4 P1 / 6 P2），新增 16 个回归测试（G1~G4 + H5 系列），全部 1047/1047 测试通过：

- **G1**（P0）：IR 路径 `AND/OR` 短路在顶层代码使用 `STORE_LOCAL/LOAD_LOCAL` 临时槽（`nextLocalSlot_` 分配），但 StackVM 主帧 `basePointer=0` 且栈初始为空，`OP_SET_LOCAL/OP_GET_LOCAL` 的 `bp+slot` 越界检查失败（"局部变量槽越界"）。修复：`VM::initExecution` 为主帧预留 `mainChunk_.localCount` 个 null 槽。正常 Compiler 路径顶层代码用 `OP_DEFINE_VAR`（globals_ 哈希），`localCount=0`，此处为 no-op
- **G2**（P1）：Parser `parseParamList` / `forStmt` 初始化用 `checkNext(TK_IDENTIFIER)` 识别 `ClassName paramName` 模式，遇到 `ClassName[] paramName` 时下一 token 是 `[` 而非标识符，导致误判为非类型声明。修复：新增 `isClassTypeDeclStart()` 同时识别 `ClassName paramName` 与 `ClassName[] paramName` 两种序列
- **G3**（P1）：Formatter 裸复合语句（if/while/for 作为 `thenBranch`/`body`）出现双重缩进——L17 "修复" 在 4 处 `isSelfTerminating` 分支额外添加了 `currentIndent_++/--`，但 `formatNode` 内部已用 `currentIndent_++` 处理 body 缩进。修复：移除 4 处额外缩进对（revert L17）
- **G4**（P0，IR 栈泄漏）：IR 路径 `build()/visitBlock()` 仅对 `FUN_CALL/METHOD_CALL` emit POP，其余 14 种表达式语句（`ASSIGNMENT/BINARY_OP/VAR_REF/UNARY_OP/NUMBER_LITERAL/STRING_LITERAL/BOOL_LITERAL/NULL_LITERAL/ARRAY_LITERAL/DICT_LITERAL/MEMBER_GET/INDEX_GET/TERNARY/INTERPOLATION`）的返回值残留在栈上，循环内泄漏必然触发栈溢出。修复：`needsPopForExprStmt(NodeType)` 覆盖全部 16 种节点类型；`visitForStmt` 对 update 表达式与表达式 initializer emit POP；`visitAssignment` 对 GLOBAL 存储后 emit `LOAD_GLOBAL` 重载值确保 POP 安全
- **H5**（P1）：StackVM `OP_CALL_EXPR`（闭包作为表达式调用）参数压栈顺序与 `OP_CALL` 不一致，导致闭包调用取到错误参数。修复：与 `OP_CALL` 对齐参数压栈顺序，新增 4 个 H5 回归测试覆盖 0/1/多参数与循环内调用
- **BUGFIX-P2 / H2**（P2，条件断点 UAF）：`evaluateCondition` 恢复阶段先调用 `restoreLocalVariables`（整表替换 `variables`）会使 `inst` 指针悬垂，再执行 `inst->fields() = fields` 触发 UAF。修复：恢复顺序改为先恢复实例字段（`inst` 仍有效）再恢复局部变量；`Environment::restoreLocalVariables` 内部在新 map 中重新锚定 `boundInstance_` 防止悬垂
- **L1**（P2，IR 栈残留）：IR 路径 `visitForStmt` 的 `JUMP_IF_FALSE` peek 不 pop，条件值在循环体/退出路径均残留栈上；`visitTryStmt` catchVarName 为空时异常值残留栈上。修复：for 循环引入 `exitLabel`（条件假路径，需 POP）与 `endLabel`（break 路径，栈已空）双标签模式；try 块 `STORE_LOCAL` 后 emit POP 对齐直接 Compiler 路径；catchVarName 为空时显式 emit POP
- **BUGFIX-P2**（P2，RegisterVM 字段未找到无方法回退）：`REG_MEMBER_GET` / `REG_SUPER_MEMBER_GET` 字段未找到时直接报错，无方法回退（StackVM `OP_MEMBER_GET` 有 `findMethodChunk` 回退）。修复：沿继承链查找方法（与 StackVM 一致），未找到才报 "类 X 没有字段或方法 'Y'"
- **BUGFIX-P2**（P2，RegisterVM 默认参数非字面量）：`fillDefaultArgs` 未处理 `0xFFFF` 哨兵值（表示非字面量默认表达式），错误地将 0xFFFF 当作常量索引导致越界。修复：与 StackVM 对齐，`0xFFFF` 返回 false 回退到 Interpreter 路径求值
- **BUGFIX-P2**（P2，VM ASCII 缓存误命中）：`lastAsciiStrPtr_` 仅比对指针，堆地址复用（原 string 释放后新 string 复用同地址）会导致非 ASCII 字符串误判为 ASCII。修复：新增 `lastAsciiStrSize_` 同时比对指针 + size
- **BUGFIX-P2**（P2，currentEnv_ 异常泄漏）：`callClosureValue` / `constructClassInstance` / `callNamedFunction` 中默认参数求值使用 `auto savedEnv = currentEnv_; ... currentEnv_ = savedEnv;` 手动恢复，`evaluate()` 抛异常时 `currentEnv_` 不恢复导致泄漏错误 scope。修复：改为 RAII guard（结构体析构函数恢复）
- **AUDIT**（GUI/REPL 一致性）：(1) `irAction_` 在运行期间未禁用，查看正在编译/执行的 IR 导致状态不一致；(2) `onNew/onOpen` 在运行/调试期间未禁止，清空/替换正在执行的代码导致状态混乱；(3) `loadFile` 文件大小检查使用裸 `MAX_SOURCE_SIZE` 而非 `RuntimeLimits::MAX_SOURCE_SIZE`；(4) `ReplPanel` 异步执行 RuntimeError 经信号显示后，`pollReplFuture` 仍打印 "null" 结果造成重复输出——新增 `hadReplError_` 原子标志，错误时跳过结果输出；(5) `WorkerManager::stopForClose` 成功路径未与 `forceStop` 对齐做完整清理（debugMode / REPL 状态 / debugger / 主回调），导致下次运行读到脏状态；(6) `IdeController` VM 条件断点求值器每次命中都重新 Lexer+Parser 严重影响循环内条件断点性能——改用 `shared_ptr<unordered_map>` 缓存条件 AST

### 工程基础设施改进

- **Windows 一键配置脚本**：新增 `configure.bat`，自动检测本机 Visual Studio 与 Qt6 安装位置（默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`，可通过 `QTDIR` 环境变量覆盖），自动初始化 MSVC 开发环境后调用对应 CMake Preset 完成配置。新增 `windows-msvc-debug` / `windows-msvc-release` 两个共享 Preset（Ninja 生成器）
- **CMake Preset 体系**：CMakePresets.json + CMakeUserPresets.json 双层分离——共享 Preset 在 `CMakePresets.json`（仓库内），机器特定 Qt 路径在 `CMakeUserPresets.json`（gitignore），避免硬编码路径污染仓库

## 许可证

本项目为教学用途。
