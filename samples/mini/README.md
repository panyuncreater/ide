# MiniLang 示例代码（按语言特性分类）

本目录下的所有 `.mini` 示例已按 **语言特性类别** 分入子文件夹，方便「语法示例」菜单与教学面板逐类检索。

> 文件夹使用数字前缀仅用于排序；每个文件夹名即对应一项特性类别。

> 样本基数：原 `samples/mini/` 下 **19** 个 `.mini` 示例 → 重组并补写后共 **28** 个。

## 目录结构

| 文件夹 | 对应特性 | 包含示例 |
|--------|----------|----------|
| `01-basics/` | 基础：print 输出、var 变量、字符串插值、行注释 | `hello.mini` |
| `02-types/` | 数据类型与字面量、类型注解（含可选/泛型/函数/多维注解） | `type_system.mini`、`type_annotations.mini`★ |
| `03-operators/` | 算术/比较/逻辑/一元运算符、字符串拼接 | `operators.mini`★ |
| `04-control-flow/` | if/else、while、for、break、continue | `control_flow.mini` |
| `05-functions/` | 函数声明、递归、默认参数、链式调用 | `fibonacci.mini`、`functions_advanced.mini`★ |
| `06-higher-order-functions/` | 高阶函数、闭包、函数作为一等公民 | `higher_order.mini`、`closure_patterns.mini` |
| `07-object-oriented/` | class、extends、super、this、多态、成员赋值 | `oop_shapes.mini`、`object_fields.mini`★ |
| `08-collections/` | 数组/字典字面量与内置方法 | `collections.mini`、`collection_methods.mini`★ |
| `09-strings/` | 字符串插值、转义、内置字符串方法 | `string_interp.mini`、`string_methods.mini`★ |
| `10-error-handling/` | try/catch/throw/finally、异常传播 | `error_handling.mini`、`try_finally.mini`★ |
| `11-modules/` | import/export、模块隔离（相互 import 的同目录文件） | `main.mini`、`math_utils.mini`、`string_utils.mini`、`shapes.mini`、`math_helpers.mini`、`modules_demo.mini` |
| `12-builtins/` | 内置函数与标准库（len/type/str/int/range/sum/abs/min/max） | `builtins_demo.mini`★ |
| `13-comments/` | 行注释、块注释（含嵌套） | `comments_demo.mini`★ |
| `14-vm-and-backends/` | IDE 执行后端 / 编译器内部演示（非语言特性） | `backend_compare_demo.mini`、`ir_transform_demo.mini`、`stack_vm_demo.mini` |

★ = 本次为补齐特性覆盖而**新增**的示例。

## 覆盖情况

- 共 **28** 个示例：原 19 个 + 新增 9 个。
- MiniLang 全部 **受支持的语言特性均已覆盖**（含此前缺失的可选类型、泛型字典注解、函数/多维类型注解、默认参数、一元 `!`/`not`、成员赋值、字典/数组全套方法、字符串全套方法、转义序列、字符索引、`finally`、内置函数 `type/str/int/range/sum`、嵌套块注释）。
- 详细说明见随附报告《MiniLang 特性清单与示例覆盖分析》。

## 已知不支持的语法（仅作说明，无对应示例）

`&&`/`||`（请用 `and`/`or`）、复合赋值 `+= -=` 等、`++`/`--`、三元 `?:`、按位 `~ & | ^ << >>`、匿名函数/lambda、for-in/do-while/switch/match、let/const、char/元组/枚举/接口、尾调用优化、运算符重载、运行时泛型、解构、rest/spread、装饰器/宏、异步/线程。

## 运行方式

在 MiniLang IDE 中通过菜单 **帮助 → 语法示例** 浏览，或直接用 IDE 打开任意 `.mini` 文件运行。
跨文件示例（`11-modules/` 内）因 import 按「当前文件所在目录」解析，请保持该目录完整。
