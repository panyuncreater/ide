# MiniLang 示例代码（按主题分类）

本目录下的 `.mini` 示例按 **语言主题** 分入 9 个子文件夹，方便「语法示例」菜单与教学面板逐类检索。

> 文件夹使用数字前缀仅用于排序；每个文件夹名即对应一个主题。

## 目录结构

| 文件夹 | 主题 | 包含示例 | 覆盖特性 |
|--------|------|----------|----------|
| `01-basics/` | 基础入门 | `hello.mini` | print 输出、var 变量、字符串插值 `{expr}`、行注释 `//`、块注释 `/* */`（含嵌套）、嵌套字符串插值 |
| `02-types-and-operators/` | 类型、运算符与内置函数 | `types_and_operators.mini` | 类型注解（int/float/string/bool）、int 边界值、float 精度、null、动态类型、可选类型 `T?`（仅 null）、多维数组 `int[][]`、算术/比较/逻辑/一元运算符、and/or 短路返回原值、字符串拼接、内置函数（len/type/str/int/range/sum/abs/min/max） |
| `03-control-flow/` | 控制流 | `control_flow.mini` | if/else、while、for、break/continue、and/or 短路保护 |
| `04-functions-and-closures/` | 函数与闭包 | `functions_and_closures.mini` | 函数声明、递归、默认参数、链式调用 `f(x)(y)`、闭包捕获、闭包工厂、计数器闭包、函数作为一等公民（返回值/变量/数组） |
| `05-data-structures/` | 数组、字典与字符串 | `data_structures.mini` | 数组/字典字面量、push/pop/len/has/get/set/remove/contains/keys/values/join、索引读写、嵌套结构、字符串插值、转义序列、字符串方法（upper/lower/substr/indexOf/contains/trim/replace/split/startsWith/endsWith）、字符索引 |
| `06-object-oriented/` | 面向对象 | `oop_shapes.mini` | class、extends、super、this、方法重写（多态）、构造函数 init、成员赋值 |
| `07-error-handling/` | 异常处理 | `error_handling.mini` | try/catch/throw/finally、异常传播、嵌套 try/catch、try/finally（无 catch） |
| `08-modules-and-backends/` | 模块系统与执行后端 | `main.mini`、`math_utils.mini`、`string_utils.mini`、`shapes.mini`、`math_helpers.mini`、`modules_demo.mini`、`backends_showcase.mini` | import（全量 `import "path"` + 选择性 `import { names } from "path"`）、export（函数/变量/类）、模块隔离、三后端一致性、IR 优化变换、栈式 VM 字节码 |
| `09-debugging/` | 调试功能演示 | `debug_demo.mini` | 普通断点、条件断点、单步进入/跳过/跨出、变量监视（全局/局部/闭包作用域）、调用栈、递归深度、变量遮蔽、VM 栈状态 |

## 主题合并说明

原 14 个特性分类文件夹已按主题归并压缩为 8 个（后续新增 `09-debugging/` 调试主题，现共 9 个）：

- `01-basics/` ← 原 `01-basics/` + `13-comments/`
- `02-types-and-operators/` ← 原 `02-types/` + `03-operators/` + `12-builtins/`
- `03-control-flow/` ← 原 `04-control-flow/`
- `04-functions-and-closures/` ← 原 `05-functions/` + `06-higher-order-functions/`
- `05-data-structures/` ← 原 `08-collections/` + `09-strings/`
- `06-object-oriented/` ← 原 `07-object-oriented/`
- `07-error-handling/` ← 原 `10-error-handling/`
- `08-modules-and-backends/` ← 原 `11-modules/` + `14-vm-and-backends/`

## 特性覆盖审计

合并时同步审计了 MiniLang 全部语言特性的覆盖情况，新增以下原本缺失的演示：

- **嵌套字符串插值**（`01-basics/`）：`"outer: {"inner: {x}"}"` —— 插值内嵌套带插值的字符串，深度限制 64 层。
- **startsWith / endsWith 字符串方法**（`05-data-structures/`）：补充前缀/后缀判断演示（注意是 camelCase 而非 snake_case）。

## 覆盖情况

- 共 **15** 个示例文件，分布在 **9** 个主题文件夹中。
- 每个文件夹内的示例均已用 MiniLang **解释器（IDE 普通运行实际走的执行路径）实际运行验证通过（零报错）**，并在文件末尾附有与运行时一致的「预期输出」注释。`08-modules-and-backends/backends_showcase.mini` 的字节码注释另经编译器反汇编逐字核对。

## 实际运行验证中发现「文档声称但运行时并不支持」的特性

合并/运行时对每个示例做了真实编译 + 运行，发现以下此前被当作「已支持」的特性**实际会报错或行为异常**，已从示例中移除或规避对应演示：

- 泛型字典类型注解 `dict[K:V]`、`dict[string:int?]` —— 运行时报「类型注解违反」。
- 函数类型注解 `fun(...):ret` 与返回类型注解 `: ret` —— 运行时报「未定义的变量」。
- 可选类型 `T?` 当前**只能持有 `null`**，赋非 null 值会被类型检查拒绝。
- 两级成员赋值 `obj.inner.field = v` 不被 VM 支持（读取 `obj.inner.field` 可以）。
- 字符串方法 `starts_with` / `ends_with`（snake_case）未实现（报「字符串没有方法」）；camelCase 版本 `startsWith` / `endsWith` 已实现。
- 变量名不能用 `dict`（被当作字典类型关键字）。
- `"字符串" + 数组[i].方法()` 这种「字符串拼接 + 索引成员调用」写法解析异常，需用临时变量规避。
- **「将函数作为参数传入后再调用」（如 `applyTwice(f, x)`）在解释器中存在函数身份串扰的已知 bug**：嵌套调用时参数 `f` 会解析到错误的全局函数，输出结果错误且无规律。因此 `04-functions-and-closures/` 只演示「函数作为返回值/存入变量或数组后调用」这些**正常**的模式，不演示该坏模式。

这些限制已写入对应示例的注释，便于教学时如实说明语言的真实边界。

## 已知不支持的语法（仅作说明，无对应示例）

`&&`/`||`（请用 `and`/`or`）、复合赋值 `+= -=` 等、`++`/`--`、三元 `?:`、按位 `~ & | ^ << >>`、匿名函数/lambda、for-in/do-while/switch/match、let/const、char/元组/枚举/接口、尾调用优化、运算符重载、运行时泛型、解构、rest/spread、装饰器/宏、异步/线程、bare `throw;`（throw 必须带表达式）、`import * from "path"`（全量导入用 `import "path"` 不带花括号）。

## 运行方式

在 MiniLang IDE 中通过菜单 **帮助 → 语法示例** 浏览，或直接用 IDE 打开任意 `.mini` 文件运行。
跨文件示例（`08-modules-and-backends/` 内）因 import 按「当前文件所在目录」解析，请保持该目录完整。
