#include "gui/SyntaxExplorerPanel.h"

// ============================================================
// SyntaxProductionLibrary — 语法产生式参考数据
// ------------------------------------------------------------
// 覆盖 MiniLang 的核心产生式，每条配可运行样例代码。
// 数据源自 parser/Parser.cpp 的实际产生式实现。
// ============================================================

const std::vector<SyntaxProduction>& SyntaxProductionLibrary::items() {
    static const std::vector<SyntaxProduction> kItems = {
        SyntaxProduction{
            "var-decl", "var 声明",
            "varDecl := \"var\" IDENTIFIER (\":\" typeAnnotation)? \"=\" expression \";\"\n"
            "         | typeAnnotation IDENTIFIER \"=\" expression \";\"",
            "动态声明变量，或使用类型注解强制类型。三后端统一强制类型注解，"
            "null 兼容所有类型，float 注解接受 int 值（宽化），int 注解拒绝 float 值。",
            "var x = 10;\nint count = 0;\nfloat pi = 3.14;\nstring name = \"MiniLang\";\nprint(x);"
        },
        SyntaxProduction{
            "if-stmt", "if 语句",
            "ifStmt := \"if\" \"(\" expression \")\" block (\"else\" block)?\n"
            "        | \"if\" \"(\" expression \")\" statement (\"else\" statement)?",
            "条件分支。then/else 分支可以是 block（{}包裹）或单语句（无花括号）。"
            "Parser 层限制 import/export 不能在无花括号单语句体中使用。",
            "var x = 10;\nif (x > 5) {\n    print(\"big\");\n} else {\n    print(\"small\");\n}"
        },
        SyntaxProduction{
            "while-stmt", "while 循环",
            "whileStmt := \"while\" \"(\" expression \")\" (block | statement)",
            "条件循环。支持 break/continue。break 退出循环，continue 跳过当前迭代。"
            "条件为假时跳过循环体。",
            "var i = 0;\nwhile (i < 5) {\n    print(i);\n    i = i + 1;\n}"
        },
        SyntaxProduction{
            "for-stmt", "for 循环",
            "forStmt := \"for\" \"(\" (varDecl | assignment | \";\") expression \";\" assignment \")\" \n"
            "          (block | statement)",
            "C 风格 for 循环。init/condition/update 三段式，每段可空。"
            "支持 break/continue。循环变量在顶层时退出后清理（DELETE_VAR）。",
            "var sum = 0;\nfor (var i = 0; i < 10; i = i + 1) {\n    sum = sum + i;\n}\nprint(sum);  // 45"
        },
        SyntaxProduction{
            "fun-decl", "函数声明",
            "funDecl := \"fun\" IDENTIFIER \"(\" params \")\" (\":\" typeAnnotation)? block\n"
            "params := (param (\",\" param)*)?\n"
            "param  := IDENTIFIER (\":\" typeAnnotation)? (\"=\" literal)?",
            "函数声明。支持默认参数（仅字面量 + 负数字面量，复杂表达式被 Parser 拒绝）。"
            "函数无提升——f(); fun f() {} 会报\"未定义的函数\"。"
            "支持闭包与 upvalue 捕获（3+ 层）。",
            "fun add(a, b = 1) {\n    return a + b;\n}\nprint(add(5));     // 6\nprint(add(5, 2));  // 7"
        },
        SyntaxProduction{
            "class-decl", "类与继承",
            "classDecl := \"class\" IDENTIFIER (\"extends\" IDENTIFIER)? classBody\n"
            "classBody := \"{\" (varDecl | funDecl)* \"}\"",
            "类声明。extends 后父类名为字符串运行时查找。"
            "类方法自动预留 slot 0 给 this，字段按继承链展平存储。"
            "支持 super.method() 调用，三后端 super 调用语义一致。",
            "class Animal {\n    fun init(name) { this.name = name; }\n    fun speak() { print(this.name); }\n}\nclass Dog extends Animal {\n    fun speak() {\n        super.speak();\n        print(\"(woof)\");\n    }\n}\nDog(\"Rex\").speak();"
        },
        SyntaxProduction{
            "try-stmt", "try/catch 异常处理",
            "tryStmt := \"try\" block \"catch\" \"(\" IDENTIFIER \")\" block\n"
            "throwStmt := \"throw\" expression? \";\"",
            "异常处理。throw 可抛任意值。catch 参数独占 slot（防止覆盖外层变量）。"
            "catch 块内 throw 会跳过清理代码——为正确处理，catch 块用 OP_TRY_BEGIN 包装，"
            "异常路径复制 cleanup 字节码后 OP_THROW rethrow。",
            "fun divide(a, b) {\n    if (b == 0) throw \"Division by zero\";\n    return a / b;\n}\ntry {\n    print(divide(10, 0));\n} catch (e) {\n    print(\"Error: \" + e);\n}"
        },
        SyntaxProduction{
            "import-stmt", "模块导入",
            "importStmt := \"import\" (\"{\" IDENTIFIER (\",\" IDENTIFIER)* \"}\" | \"*\") \"from\" STRING \";\"\n"
            "exportStmt := \"export\" (varDecl | funDecl | classDecl)",
            "模块系统。import/export 限制在顶层作用域（Parser 在 statement() 入口显式拒绝）。"
            "模块路径非空校验，路径遍历防护（拒绝 .. 父目录引用和绝对路径）。"
            "VM 路径通过编译期模块内联支持 import。",
            "// 假设 utils.mini 中有 export fun greet(name) { ... }\nimport { greet } from \"utils.mini\";\nprint(greet(\"MiniLang\"));"
        },
        SyntaxProduction{
            "string-interp", "字符串插值",
            "interpolatedString := '\"' (stringPart | \"{\" expression \"}\")* '\"'",
            "字符串插值。支持表达式嵌入。空插值 {} 报语法错误。"
            "插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。"
            "Lexer 通过 INTERP_START/INTERP_END/STRING_PART 三种 token 类型识别。",
            "var name = \"World\";\nvar items = [1, 2, 3];\nprint(\"Hello, {name}! You have {items.len()} items.\");"
        },
        SyntaxProduction{
            "data-structures", "数组与字典",
            "arrayLiteral := \"[\" (expression (\",\" expression)*)? \"]\"\n"
            "dictLiteral   := \"{\" (STRING \":\" expression (\",\" STRING \":\" expression)*)? \"}\"",
            "数组与字典字面量。字典键强制为 string，键不存在时返回 null。"
            "数组/字典使用 Copy-On-Write（COW），写前检查独占所有权。"
            "支持索引读写（a[0] / d[\"key\"]）和嵌套索引赋值（a[0][1]=x）。",
            "var arr = [1, 2, 3];\narr.push(4);\nprint(arr.len());  // 4\n\nvar cfg = {\"key\": \"value\", \"count\": 42};\nprint(cfg[\"key\"]);  // value"
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
            "var x = 5;\nprint(x > 0 and \"positive\" or \"non-positive\");\nprint(7 / 2);    // 3\nprint(7.0 / 2);  // 3.5\nprint(0 or \"default\");  // 0（保留原值）"
        }
    };
    return kItems;
}
