# MiniLang 与主流语言对比表

本表面向有 C / Python / Java 背景的新手，帮助快速建立 MiniLang 的差异心智模型。
**加粗** 标记的单元格是 MiniLang 与三种语言都不同的「特异语义」，最容易踩坑，请重点留意。

---

## 对比总表

| 维度 | MiniLang | C | Python | Java |
|------|----------|---|--------|------|
| 类型系统 | 强类型 + 类型注解强制 | 静态弱类型 | 动态强类型 | 静态强类型 |
| 整数除法 | 截断向零（C 风格） | 截断向零 | 浮点除法（`/`）+ 整除（`//`） | 浮点除法（`/`）+ 整除（需转 `int`） |
| and/or 短路返回 | **返回操作数原值（非布尔）** | 返回布尔 | 返回操作数原值 | 返回布尔 |
| 字符串 | 不可变 + 拼接新建 | `char` 数组 | 不可变 | 不可变 |
| 数组 | **COW（写时复制）** | 原生数组 | 引用语义 | 引用语义 |
| 字典 | COW + key 必须 string | 无原生 | 引用语义 | `HashMap` |
| 闭包 | 嵌套命名函数（不支持匿名函数表达式） | 不支持 | lambda + 嵌套函数 | lambda |
| 异常 | `try/catch/finally` + 字符串标签 | 无 | `try/except/finally` | `try/catch/finally` |
| 类 | 单继承 + `init` 构造 | 无 | 多继承 | 单继承 + `interface` |
| 模块系统 | `import "path"` + 循环依赖检测 | `#include` | `import` + `sys.path` | `import` + classpath |
| 函数提升 | **无提升**（必须先声明后使用） | 无提升 | 有提升 | 有提升 |
| 类型注解 | 强制（`var x: int = 1`） | 类型声明 | 可选 | 类型声明 |
| 内存管理 | 引用计数 + mark-sweep GC | 手动 `malloc/free` | 引用计数 + GC | GC |
| REPL | 内置 + magic 命令 | 无 | 内置 | `jshell` |
| 编译目标 | 三后端（解释器 / 栈 VM / 寄存器 VM） | 原生机器码 | 字节码 | 字节码 |

---

## 维度详解

### 类型系统
MiniLang 是强类型语言，**类型注解一旦写出就强制**：`int x = 1; x = "str";` 会触发类型违反警告。注解可省略（`var x = 1`），但写出后不可违背。与 Python 的「注解仅装饰」、C 的「弱类型隐式转换」都不同。`null` 兼容所有类型注解，`float` 注解接受 `int` 值（宽化），但 `int` 注解拒绝 `float` 值。

### 整数除法
`/` 对两个 `int` 操作数执行**截断向零**的整数除法（C 风格），`print(1 / 2)` 输出 `0` 而非 `0.5`。这与 Python 的 `/`（浮点）截然不同，与 C 一致。若要浮点结果，至少一个操作数须为 `float`：`1.0 / 2` 得 `0.5`。三后端语义统一。

### and/or 短路返回
`and` / `or` 短路求值后**返回操作数原值**，而非布尔。例如 `0 or "fallback"` 得 `"fallback"`，`1 and 2` 得 `2`。与 Python 一致，但与 C/Java（返回 `bool`）不同。`if (a and b)` 的判断基于「真假性」而非布尔值，新手若把结果当作 `true/false` 用于算术或显式比较会踩坑。`not` 返回布尔。

### 字符串
字符串不可变，`+` 拼接会新建字符串。支持 UTF-8 码位语义（`len` / `substr` / `indexOf` 按码位计数，而非字节）。字符串插值 `"hello {name}"` 是一等语法（非 `+` 拼接），嵌套深度限制 64 层。

### 数组
数组采用 **COW（Copy-On-Write）**：`var b = a;` 时 `b` 与 `a` 共享底层数据；只有当 `b` 被**修改**（如 `b.push(4)`）且不独占所有权时才触发深拷贝。这看似 Python/Java 的引用语义，但**赋值给新变量不立即复制**——只要任何一方不写，就共享。新手常误以为 `var b = a; b.push(4);` 后 `a` 不变，实际可能仍共享（取决于是否独占）。

### 字典
字典同样是 COW，且**键强制为 `string`**。键不存在时返回 `null`（不报错）。与 Python（任意可哈希 key）、Java（`HashMap` 任意 key）不同，MiniLang 的 `{"key": val}` 写法决定了 key 字面量必须是字符串。

### 闭包
MiniLang **只支持嵌套命名函数**作为闭包载体：`fun outer() { fun inner() {...} return inner; }`。**不支持**匿名函数表达式 `var f = fun() {};`（解析器会报「意外的 Token: 'fun'」），也不支持 Python/Java 的 `lambda`。需要把回调用具名函数声明再引用传递。

### 异常
`try { ... } catch (e) { ... }` + 可选 `finally`。`throw` 可抛任意值（字符串最常见）。`catch` 参数独占 slot，防止覆盖外层变量。语义接近 Java，但异常标签是值而非类型。

### 类
单继承（`class B extends A`），构造函数固定名 `init`。不支持多继承（Python 风格）与 interface（Java 风格）。`super.method()` 调用父类方法，三后端 super 语义一致。字段按继承链展平存储。

### 模块系统
`import "path"` 路径**相对当前文件**（不是工作区根），有路径遍历防护（拒绝 `..` 与绝对路径），并检测循环依赖。`import {a, b} from "mod";` 选择性导入，`export` 显式导出。非导出的顶层声明通过 `ModuleTopLevelRenamer` 重命名为 `__mod_<hash>__<name>` 确保跨模块不可见。

### 函数提升
**无提升**——必须先声明后使用。`f(); fun f() {}` 会报「未定义的函数」。这与 Python（`def` 在执行到时才绑定，但有编译期名称检查）、Java（类成员可前向引用）都不同，更接近 C 的严格顺序声明。新手从 Python 迁移尤其容易踩坑。

### 类型注解
类型注解**强制**：写出 `var x: int = 1` 后，`x = "str"` 报类型违反。三后端（解释器 / 栈 VM / 寄存器 VM）运行时统一强制，编译期另有 `TypeChecker` 检查。

### 内存管理
堆类型通过侵入式 `RefCounted` 引用计数管理，`GcManager` mark-sweep 处理循环引用。开发者无需手动 `free`，但需注意循环引用对 GC 的依赖（与 Python 模型接近）。

### REPL
内置 REPL 支持 `%magic` 命令（`%help` / `%ast` / `%ir` / `%tokens` / `%disassemble` / `%compare` / `%profile` / `%memory` / `%version` / `%reset`），可快速调用各教学面板的数据获取逻辑。支持多行续行（未闭合 `{ ( [` / 字符串 / `try` 缺 `catch`）。

### 编译目标
MiniLang 同时实现**三套执行引擎**（树遍历解释器 / 栈式字节码 VM / 寄存器式 VM）并共享一套 IR 中间表示层。同一源码在三后端必须语义等价，这是项目的核心约束，也是教学价值所在。

---

## 常见陷阱

以下 8 条是新手最常踩的 MiniLang 特殊语义，按踩坑频率排序：

### 1. `var b = a; b.push(4);` 后 `a` 也变了（COW 触发条件）
```mini
var a = [1, 2, 3];
var b = a;        // b 与 a 共享底层数组（refCount = 2）
b.push(4);        // 写前检查：b 不独占 → 触发 COW 深拷贝
print(a);         // [1, 2, 3] —— a 未受影响
print(b);         // [1, 2, 3, 4]
```
**但**若 `a` 之后不再被使用并被回收，`b` 独占所有权时 `b.push(4)` 不会复制。新手误以为「赋值即复制」或「永远是引用」都是错的——**写时才决定**。需要独立副本请显式构造新数组。

### 2. `f(); fun f() {}` 报错（无函数提升）
```mini
f();              // ❌ 报「未定义的函数」
fun f() { print("hi"); }
```
MiniLang **无提升**，必须先声明后使用。Python 的 `def` 也是执行到才绑定，但 MiniLang 更严格——调用时函数名必须已在作用域。从 Python 迁移的新手尤其要注意。

### 3. `var x = 1; x = "str";` 报错（类型注解强制）
```mini
var x = 1;        // 推断为 int（无注解时仍动态）
x = "str";        // OK —— 无注解则允许重新赋值为其他类型

int y = 1;        // 有类型注解
y = "str";        // ❌ 类型违反警告 / 运行时错误
```
`var` 声明无注解时可重新赋值为其他类型；一旦写出注解（`int y` / `var y: int`），三后端统一强制不可违背。

### 4. `print(1 / 2);` 输出 `0` 不是 `0.5`（整数除法截断）
```mini
print(1 / 2);     // 0（截断向零，C 风格）
print(1.0 / 2);   // 0.5（至少一个 float 操作数 → 浮点除法）
print(-1 / 2);    // 0（截断向零，不是 -1）
```
与 Python 的 `/`（始终浮点）不同，与 C 一致。要浮点结果，把任一操作数写成 `float`。

### 5. `if (a and b)` 返回的不是 `true/false`（短路返回原值）
```mini
var x = 0 or "fallback";   // x = "fallback"（不是 true）
var y = 1 and 2;            // y = 2（不是 true）
if (a and b) { ... }        // 基于「真假性」判断，OK
var flag = (a and b);       // flag 可能是任意值，不是 bool！
```
`and` / `or` **返回操作数原值**（非布尔），与 Python 一致，与 C/Java（返回 `bool`）不同。需要布尔结果时用 `not not (a and b)` 或显式比较。

### 6. `var f = fun() {};` 报错（不支持匿名函数表达式，必须用嵌套命名函数）
```mini
var f = fun() { print("hi"); };   // ❌ 报「意外的 Token: 'fun'」

// ✅ 正确写法：命名函数声明 + 引用
fun _f() { print("hi"); }
var f = _f;
f();
```
MiniLang **只支持命名函数**作为一等值。回调模式需用具名函数声明再传递引用，不能像 Python `lambda` / Java `() -> {}` 那样内联写匿名函数。

### 7. `fun outer() { var x = 1; return fun() { return x; }; }` 同上
```mini
fun outer() {
    var x = 1;
    return fun() { return x; };   // ❌ 同样报错
}

// ✅ 正确写法
fun outer() {
    var x = 1;
    fun inner() { return x; }     // 命名嵌套函数捕获 x
    return inner;
}
```
闭包必须用具名嵌套函数，支持 3+ 层 upvalue 捕获。

### 8. `import "mod";` 路径相对当前文件（不是工作区根）
```mini
// 假设目录结构：
//   project/
//     main.mini
//     utils/
//       string_utils.mini

// 在 main.mini 中：
import "utils/string_utils.mini";   // ✅ 相对当前文件
import "/project/utils/...";         // ❌ 拒绝绝对路径
import "../sibling/...";             // ❌ 拒绝 .. 父目录引用（路径遍历防护）
```
import 路径**相对当前文件**，与 Python 的「相对 sys.path / 包路径」不同。模块路径非空校验，路径遍历防护拒绝 `..` 与绝对路径，并检测循环依赖。

---

*参见 [faq.md](./faq.md) 获取按主题分组的常见问题解答，[architecture.md](./architecture.md) 获取核心架构设计。*
