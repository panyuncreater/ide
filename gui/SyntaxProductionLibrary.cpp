#include "gui/SyntaxExplorerPanel.h"

// ============================================================
// SyntaxProductionLibrary — 语法产生式参考数据
// ------------------------------------------------------------
// 覆盖 MiniLang 的核心产生式，每条配可运行样例代码。
// 数据源自 parser/Parser.cpp 的实际产生式实现。
// F8 改进：每条产生式新增 naturalLanguage 字段，提供人话翻译、
// 示例对比（正确/错误写法）和注意事项，降低 EBNF 门槛。
// ============================================================

const std::vector<SyntaxProduction>& SyntaxProductionLibrary::items() {
    static const std::vector<SyntaxProduction> kItems = {
        SyntaxProduction{
            "var-decl", "var 声明",
            "varDecl := \"var\" IDENTIFIER (\":\" typeAnnotation)? \"=\" expression \";\"\n"
            "         | typeAnnotation IDENTIFIER \"=\" expression \";\"",
            "动态声明变量，或使用类型注解强制类型。三后端统一强制类型注解，"
            "null 兼容所有类型，float 注解接受 int 值（宽化），int 注解拒绝 float 值。",
            "var x = 10;\nint count = 0;\nfloat pi = 3.14;\nstring name = \"MiniLang\";\nprint(x);",
            // F8 自然语言描述
            "var 声明的写法：\n\n"
            "  必须以 var 关键字开头（或直接用类型注解开头）\n"
            "  接着是变量名（标识符）\n"
            "  可选择性地加冒号和类型注解：var x: int\n"
            "  然后是等号 = 和初始值表达式\n"
            "  最后以分号 ; 结尾\n\n"
            "  示例:\n"
            "    ✓ var x = 42;            // 动态类型\n"
            "    ✓ int count = 0;         // 类型注解开头\n"
            "    ✓ float pi = 3.14;\n"
            "    ✗ var x = 42             ← 缺少分号!\n"
            "    ✗ var 42 = x;            ← 变量名不能是数字!\n\n"
            "  注意事项:\n"
            "    • 类型注解一旦指定就强制检查（int 注解拒绝 float 值）\n"
            "    • null 兼容所有类型注解\n"
            "    • float 注解接受 int 值（宽化）"
        },
        SyntaxProduction{
            "if-stmt", "if 语句",
            "ifStmt := \"if\" \"(\" expression \")\" block (\"else\" block)?\n"
            "        | \"if\" \"(\" expression \")\" statement (\"else\" statement)?",
            "条件分支。then/else 分支可以是 block（{}包裹）或单语句（无花括号）。"
            "Parser 层限制 import/export 不能在无花括号单语句体中使用。",
            "var x = 10;\nif (x > 5) {\n    print(\"big\");\n} else {\n    print(\"small\");\n}",
            "if 语句的写法：\n\n"
            "  必须以 if 关键字开头\n"
            "  接着是括号包裹的条件表达式: if (条件)\n"
            "  然后是代码块（用 { } 包围的一或多条语句）\n"
            "  可以选择性地跟一个 else 和另一个代码块\n\n"
            "  示例:\n"
            "    ✓ if (x > 0) { print(x); }\n"
            "    ✓ if (x > 0) { print(x); } else { print(\"non-positive\"); }\n"
            "    ✗ if x > 0 { print(x); }        ← 缺少括号!\n"
            "    ✗ if (x > 0) print(x);          ← 单语句体受限（import/export 不可用）\n\n"
            "  注意事项:\n"
            "    • 条件必须用圆括号 ( ) 包裹\n"
            "    • 代码块建议用花括号 { } 包围\n"
            "    • else 部分是可选的\n"
            "    • 支持 else if 链：if (...) {...} else if (...) {...} else {...}"
        },
        SyntaxProduction{
            "while-stmt", "while 循环",
            "whileStmt := \"while\" \"(\" expression \")\" (block | statement)",
            "条件循环。支持 break/continue。break 退出循环，continue 跳过当前迭代。"
            "条件为假时跳过循环体。",
            "var i = 0;\nwhile (i < 5) {\n    print(i);\n    i = i + 1;\n}",
            "while 循环的写法：\n\n"
            "  必须以 while 关键字开头\n"
            "  接着是括号包裹的条件表达式: while (条件)\n"
            "  然后是循环体（代码块或单语句）\n\n"
            "  示例:\n"
            "    ✓ while (i < 10) { print(i); i = i + 1; }\n"
            "    ✓ while (true) { if (x == 0) break; }\n"
            "    ✗ while i < 10 { ... }     ← 缺少括号!\n"
            "    ✗ while (i < 10) print(i); ← 忘记更新 i，死循环!\n\n"
            "  注意事项:\n"
            "    • 条件必须用圆括号包裹\n"
            "    • 循环体内必须更新条件变量，否则死循环\n"
            "    • break 立即退出循环\n"
            "    • continue 跳过本次迭代剩余代码，进入下一轮条件判断"
        },
        SyntaxProduction{
            "for-stmt", "for 循环",
            "forStmt := \"for\" \"(\" (varDecl | assignment | \";\") expression \";\" assignment \")\" \n"
            "          (block | statement)",
            "C 风格 for 循环。init/condition/update 三段式，每段可空。"
            "支持 break/continue。循环变量在顶层时退出后清理（DELETE_VAR）。",
            "var sum = 0;\nfor (var i = 0; i < 10; i = i + 1) {\n    sum = sum + i;\n}\nprint(sum);  // 45",
            "for 循环的写法：\n\n"
            "  C 风格三段式，每段用分号分隔：\n"
            "    for (初始化; 条件; 更新) { 循环体 }\n\n"
            "  示例:\n"
            "    ✓ for (var i = 0; i < 10; i = i + 1) { print(i); }\n"
            "    ✓ for (var i = 0; i < 10; i = i + 1) print(i);   // 单语句体\n"
            "    ✓ for (;;) { ... }                                  // 三段全空（死循环）\n"
            "    ✗ for (var i = 0 i < 10; i = i + 1) { ... }        ← 缺少分号!\n"
            "    ✗ for (i = 0; i < 10; i++) { ... }                 ← MiniLang 不支持 i++\n\n"
            "  注意事项:\n"
            "    • MiniLang 不支持 i++ / i--，必须写成 i = i + 1\n"
            "    • 三段式每段可以为空，但分号不可省略\n"
            "    • 循环变量在顶层声明时退出后清理（DELETE_VAR）\n"
            "    • 支持 break / continue"
        },
        SyntaxProduction{
            "fun-decl", "函数声明",
            "funDecl := \"fun\" IDENTIFIER \"(\" params \")\" (\":\" typeAnnotation)? block\n"
            "params := (param (\",\" param)*)?\n"
            "param  := IDENTIFIER (\":\" typeAnnotation)? (\"=\" literal)?",
            "函数声明。支持默认参数（仅字面量 + 负数字面量，复杂表达式被 Parser 拒绝）。"
            "函数无提升——f(); fun f() {} 会报\"未定义的函数\"。"
            "支持闭包与 upvalue 捕获（3+ 层）。",
            "fun add(a, b = 1) {\n    return a + b;\n}\nprint(add(5));     // 6\nprint(add(5, 2));  // 7",
            "函数声明的写法：\n\n"
            "  必须以 fun 关键字开头\n"
            "  接着是函数名（标识符）\n"
            "  然后是括号包裹的参数列表: fun name(参数)\n"
            "  可选返回类型注解: fun name(...) : int\n"
            "  最后是代码块 { }\n\n"
            "  参数形式：\n"
            "    ✓ name           // 必需参数\n"
            "    ✓ name: int      // 带类型注解\n"
            "    ✓ name = 10      // 默认值（仅字面量）\n\n"
            "  示例:\n"
            "    ✓ fun add(a, b) { return a + b; }\n"
            "    ✓ fun greet(name: string = \"World\") { print(\"Hi, \" + name); }\n"
            "    ✗ add(5);  fun add(a, b) { ... }   ← 函数无提升，必须先声明后使用!\n"
            "    ✗ fun f(x = a + 1) { ... }         ← 默认值必须是字面量\n\n"
            "  注意事项:\n"
            "    • 函数无提升（hoisting），必须先声明后使用\n"
            "    • 默认参数仅接受字面量 + 负数字面量\n"
            "    • 支持闭包与 upvalue 捕获（3+ 层嵌套）\n"
            "    • return 不在函数末尾时需要明确返回值"
        },
        SyntaxProduction{
            "class-decl", "类与继承",
            "classDecl := \"class\" IDENTIFIER (\"extends\" IDENTIFIER)? classBody\n"
            "classBody := \"{\" (varDecl | funDecl)* \"}\"",
            "类声明。extends 后父类名为字符串运行时查找。"
            "类方法自动预留 slot 0 给 this，字段按继承链展平存储。"
            "支持 super.method() 调用，三后端 super 调用语义一致。",
            "class Animal {\n    fun init(name) { this.name = name; }\n    fun speak() { print(this.name); }\n}\nclass Dog extends Animal {\n    fun speak() {\n        super.speak();\n        print(\"(woof)\");\n    }\n}\nDog(\"Rex\").speak();",
            "类声明的写法：\n\n"
            "  必须以 class 关键字开头\n"
            "  接着是类名（标识符）\n"
            "  可选 extends 父类名: class Child extends Parent\n"
            "  然后是类体 { }，包含字段声明和方法\n\n"
            "  示例:\n"
            "    ✓ class Point {\n"
            "        var x = 0;\n"
            "        var y = 0;\n"
            "        fun init(x, y) { this.x = x; this.y = y; }\n"
            "      }\n"
            "    ✓ class Dog extends Animal { fun speak() { super.speak(); } }\n"
            "    ✗ class { ... }              ← 缺少类名!\n\n"
            "  注意事项:\n"
            "    • 类方法自动预留 slot 0 给 this\n"
            "    • 字段按继承链展平存储\n"
            "    • super.method() 调用父类方法（三后端语义一致）\n"
            "    • super 只能在类方法中使用（非方法上下文为运行时错误）\n"
            "    • init 是构造方法，实例化时自动调用: Dog(\"Rex\") 等价于 new Dog(\"Rex\")"
        },
        SyntaxProduction{
            "try-stmt", "try/catch 异常处理",
            "tryStmt := \"try\" block \"catch\" \"(\" IDENTIFIER \")\" block\n"
            "throwStmt := \"throw\" expression? \";\"",
            "异常处理。throw 可抛任意值。catch 参数独占 slot（防止覆盖外层变量）。"
            "catch 块内 throw 会跳过清理代码——为正确处理，catch 块用 OP_TRY_BEGIN 包装，"
            "异常路径复制 cleanup 字节码后 OP_THROW rethrow。",
            "fun divide(a, b) {\n    if (b == 0) throw \"Division by zero\";\n    return a / b;\n}\ntry {\n    print(divide(10, 0));\n} catch (e) {\n    print(\"Error: \" + e);\n}",
            "try/catch 异常处理的写法：\n\n"
            "  try 后跟一个代码块（可能出错的代码）\n"
            "  catch 后跟括号包裹的异常变量名和代码块\n"
            "  throw 可抛出任意值（字符串、数字、对象等）\n"
            "  支持 try-finally（无 catch）和 try-catch-finally\n\n"
            "  示例:\n"
            "    ✓ try { throw \"error\"; } catch (e) { print(e); }\n"
            "    ✓ try { ... } finally { print(\"cleanup\"); }   // 无 catch\n"
            "    ✓ try { ... } catch (e) { ... } finally { ... }\n"
            "    ✗ try { ... }                          ← 必须有 catch 或 finally!\n"
            "    ✗ try { ... } catch { ... }            ← catch 必须有 (变量名)\n\n"
            "  注意事项:\n"
            "    • throw 可抛任意值（不限于 Error 对象）\n"
            "    • catch 变量独占 slot，防止覆盖外层变量\n"
            "    • finally 块无论是否异常都会执行（含 return 中断）\n"
            "    • catch 块内 throw 会 rethrow，跳过清理代码（已正确处理）"
        },
        SyntaxProduction{
            "import-stmt", "模块导入",
            "importStmt := \"import\" (\"{\" IDENTIFIER (\",\" IDENTIFIER)* \"}\" | \"*\") \"from\" STRING \";\"\n"
            "exportStmt := \"export\" (varDecl | funDecl | classDecl)",
            "模块系统。import/export 限制在顶层作用域（Parser 在 statement() 入口显式拒绝）。"
            "模块路径非空校验，路径遍历防护（拒绝 .. 父目录引用和绝对路径）。"
            "VM 路径通过编译期模块内联支持 import。",
            "// 假设 utils.mini 中有 export fun greet(name) { ... }\nimport { greet } from \"utils.mini\";\nprint(greet(\"MiniLang\"));",
            "import / export 模块系统的写法：\n\n"
            "  导入语法（两种形式）：\n"
            "    import { 名称1, 名称2 } from \"模块路径\";   // 命名导入\n"
            "    import * from \"模块路径\";                  // 全部导入\n\n"
            "  导出语法：\n"
            "    export var x = 10;            // 导出变量\n"
            "    export fun f() { ... }        // 导出函数\n"
            "    export class C { ... }        // 导出类\n\n"
            "  示例:\n"
            "    ✓ import { greet } from \"utils.mini\";\n"
            "    ✓ import * from \"utils.mini\";\n"
            "    ✓ export fun add(a, b) { return a + b; }\n"
            "    ✗ import { greet } \"utils.mini\";       ← 缺少 from 关键字!\n"
            "    ✗ import { greet } from utils.mini;    ← 模块路径必须是字符串!\n"
            "    ✗ import { greet } from \"../hack.mini\"; ← 路径遍历被拒绝!\n\n"
            "  注意事项:\n"
            "    • import / export 只能在顶层作用域使用\n"
            "    • 模块路径必须是字符串字面量\n"
            "    • 路径遍历防护：拒绝 .. 和绝对路径\n"
            "    • 非导出顶层名称在导入方不可见（模块隔离）"
        },
        SyntaxProduction{
            "string-interp", "字符串插值",
            "interpolatedString := '\"' (stringPart | \"{\" expression \"}\")* '\"'",
            "字符串插值。支持表达式嵌入。空插值 {} 报语法错误。"
            "插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。"
            "Lexer 通过 INTERP_START/INTERP_END/STRING_PART 三种 token 类型识别。",
            "var name = \"World\";\nvar items = [1, 2, 3];\nprint(\"Hello, {name}! You have {items.len()} items.\");",
            "字符串插值的写法：\n\n"
            "  用双引号 \" 包裹字符串\n"
            "  在字符串中用 { } 嵌入任意表达式\n"
            "  表达式会被求值并转换为字符串\n\n"
            "  示例:\n"
            "    ✓ \"Hello, {name}!\"                       // 变量插值\n"
            "    ✓ \"Result: {1 + 2 * 3}\"                  // 表达式插值\n"
            "    ✓ \"Items: {arr.len()}\"                   // 方法调用\n"
            "    ✓ \"Nested: {\"inner {x}\"}\"               // 嵌套插值\n"
            "    ✗ \"Hello, {}\"                            ← 空插值报语法错误!\n"
            "    ✗ 'Hello, {name}'                         ← 单引号不支持插值!\n\n"
            "  注意事项:\n"
            "    • 用双引号 \" 而非单引号 '\n"
            "    • 空插值 {} 报语法错误\n"
            "    • 嵌套深度限制 64 层防栈溢出\n"
            "    • 表达式会被自动 toString()"
        },
        SyntaxProduction{
            "data-structures", "数组与字典",
            "arrayLiteral := \"[\" (expression (\",\" expression)*)? \"]\"\n"
            "dictLiteral   := \"{\" (STRING \":\" expression (\",\" STRING \":\" expression)*)? \"}\"",
            "数组与字典字面量。字典键强制为 string，键不存在时返回 null。"
            "数组/字典使用 Copy-On-Write（COW），写前检查独占所有权。"
            "支持索引读写（a[0] / d[\"key\"]）和嵌套索引赋值（a[0][1]=x）。",
            "var arr = [1, 2, 3];\narr.push(4);\nprint(arr.len());  // 4\n\nvar cfg = {\"key\": \"value\", \"count\": 42};\nprint(cfg[\"key\"]);  // value",
            "数组与字典字面量的写法：\n\n"
            "  数组：用方括号 [ ] 包裹，元素用逗号分隔\n"
            "    ✓ [1, 2, 3]\n"
            "    ✓ [\"a\", \"b\", \"c\"]\n"
            "    ✓ []                  // 空数组\n"
            "    ✓ [[1, 2], [3, 4]]    // 嵌套数组\n\n"
            "  字典：用花括号 { } 包裹，键必须是字符串，键值用冒号分隔\n"
            "    ✓ {\"name\": \"Alice\", \"age\": 30}\n"
            "    ✓ {}                  // 空字典\n"
            "    ✗ {name: \"Alice\"}     ← 键必须是字符串字面量!\n"
            "    ✗ {\"key\" = \"value\"}  ← 必须用冒号 : 而非等号 =\n\n"
            "  注意事项:\n"
            "    • 字典键强制为 string 类型\n"
            "    • 不存在的键返回 null（不报错）\n"
            "    • 数组/字典使用 Copy-On-Write：a=b 共享数据，写时才复制\n"
            "    • 支持索引读写：a[0] / d[\"key\"]\n"
            "    • 支持嵌套索引赋值：a[0][1] = x"
        },
        SyntaxProduction{
            "operators", "运算符与短路求值",
            "expression := assignment\n"
            "assignment := or_ (\"=\" assignment)?\n"
            "or_  := and_ (\"or\" and_)*\n"
            "and_ := equality (\"and\" equality)*\n"
            "equality   := comparison ((\"==\" | \"!=\") comparison)*\n"
            "comparison := term ((\"<\" | \">\" | \"<=\" | \">=\") term)*\n"
            "term := factor ((\"+\" | \"-\") factor)*\n"
            "factor := unary ((\"*\" | \"/\" | \"%\") unary)*\n"
            "unary := (\"not\" | \"-\") unary | call",
            "运算符优先级链。整数除法截断向零（三后端统一）。"
            "and/or 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（非布尔）。"
            "+ 支持字符串拼接（任一操作数为字符串即触发）。",
            "var x = 5;\nprint(x > 0 and \"positive\" or \"non-positive\");\nprint(7 / 2);    // 3\nprint(7.0 / 2);  // 3.5\nprint(0 or \"default\");  // 0（保留原值）",
            "运算符的写法（按优先级从低到高）：\n\n"
            "  or          （最低优先级）\n"
            "  and\n"
            "  ==  !=\n"
            "  <   >   <=  >=\n"
            "  +   -\n"
            "  *   /   %\n"
            "  not  -（一元）     （最高优先级）\n\n"
            "  示例:\n"
            "    ✓ 1 + 2 * 3              // = 7（乘法优先）\n"
            "    ✓ (1 + 2) * 3            // = 9（括号改变优先级）\n"
            "    ✓ 7 / 2                  // = 3（整数除法截断向零）\n"
            "    ✓ 7.0 / 2                // = 3.5（浮点除法）\n"
            "    ✓ x > 0 and x < 10       // 等价于 (x > 0) and (x < 10)\n"
            "    ✓ \"a\" + \"b\"             // = \"ab\"（字符串拼接）\n"
            "    ✗ 1 + 2 * 3 == 7 + 1     // 注意：等价于 ((1+(2*3))==7)+1，不是 1+((2*3)==(7+1))\n\n"
            "  注意事项:\n"
            "    • 整数除法截断向零：-7 / 2 = -3（不是 -4）\n"
            "    • and/or 短路返回操作数原值（非布尔）：\n"
            "        - 0 or \"default\" → 0（保留原值，不返回 true/false）\n"
            "        - \"hi\" and 0 → 0\n"
            "    • not 返回布尔值\n"
            "    • + 任一操作数为字符串即触发拼接：\"a\" + 1 → \"a1\""
        }
    };
    return kItems;
}
