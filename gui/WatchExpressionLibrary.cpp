// ============================================================
// WatchExpressionLibrary.cpp — Watch 表达式教学场景库（独立编译单元）
// ------------------------------------------------------------
// 从 WatchPanel.cpp 抽取 WatchExpressionLibrary::scenarios() 静态数据，
// 使其可作为独立编译单元加入测试目标（不依赖 IdeController.h）。
// 与 BugHuntLibrary.cpp / BugHuntVariantLibrary.cpp / SandboxLevels.cpp
// 同模式：仅依赖标准库 + Qt6::Core（QString），不依赖 Qt6::Widgets / IdeController。
// ============================================================

#include "gui/WatchPanel.h"

// ============================================================
// WatchExpressionLibrary — 静态教学场景库
// ============================================================
// 6 个典型 watch 表达式场景，覆盖：
//   - 简单变量（标量类型）
//   - 算术表达式
//   - 数组/字典索引访问
//   - 实例字段访问（this.field）
//   - 方法调用
//   - 闭包捕获变量
//
// 帮助学习者理解 watch 表达式的求值机制：
//   - 表达式在沙箱中求值（独立 Interpreter 实例）
//   - 自动注入当前作用域的 globals + locals
//   - 支持 this 绑定（实例字段访问）
//   - 求值结果完整展示类型 + 值（而非仅 truthy）

const std::vector<WatchScenario>& WatchExpressionLibrary::scenarios() {
    static const std::vector<WatchScenario> kScenarios = {
        {"simple-var", "🔢 简单变量（i）", "i",
         "🔢 最基础的 watch 表达式。直接观察循环变量 i 的当前值。\n\n"
         "**求值机制**：表达式在沙箱 Interpreter 中求值，自动注入 VM/Interpreter 当前作用域的 globals + "
         "locals。每次步进/断点命中时自动刷新，让学习者观察变量值随程序执行的变化。",
         "var i = 0;\nvar sum = 0;\nwhile (i < 10) {\n    sum = sum + i;\n    i = i + 1;  // 在此行设置断点\n}\n"
         "print(sum);",
         "int"},
        {"arithmetic-expr", "⚙️ 算术表达式（sum * 2 + 1）", "sum * 2 + 1",
         "⚙️ watch 支持任意算术表达式。可在不修改源码的情况下观察中间计算结果。\n\n"
         "**求值机制**：表达式解析为 AST 后在沙箱中求值。运算符优先级、结合性、类型转换"
         "（int/float）与正常运行时语义一致。",
         "var sum = 0;\nvar i = 0;\nwhile (i < 100) {\n    sum = sum + i;\n    i = i + 1;  // 在此行设置断点\n}\n"
         "print(sum);",
         "int"},
        {"array-index", "📚 数组索引访问（arr[i]）", "arr[i]",
         "📚 观察数组在当前索引处的元素。watch 表达式支持索引访问操作符 `[]`。\n\n"
         "**求值机制**：arr 和 i 从当前作用域注入，索引访问在沙箱中执行。若索引越界，"
         "状态列显示 \"求值异常\"，不会影响程序正常运行。",
         "var arr = [10, 20, 30, 40, 50];\nvar i = 0;\nwhile (i < 5) {\n    print(arr[i]);\n    i = i + 1;  // "
         "在此行设置断点\n}",
         "int"},
        {"instance-field", "🎯 实例字段访问（this.value）", "this.value",
         "🎯 在方法内部观察实例字段的当前值。watch 表达式支持 this 绑定。\n\n"
         "**求值机制**：若当前作用域存在 this 变量且为实例，沙箱 Interpreter 自动绑定 "
         "boundInstance_，使裸字段名（如 `value`）和方法体内部一样可访问。this.value 和 "
         "value 在方法内等价。",
         "class Counter {\n    var value;\n    fun init() { value = 0; }\n    fun inc() {\n        value = value + "
         "1;  // 在此行设置断点\n    }\n}\nvar c = Counter();\nvar i = 0;\nwhile (i < 5) {\n    c.inc();\n    i = i "
         "+ 1;\n}\nprint(c.value);",
         "int"},
        {"method-call", "🔧 方法调用（c.getValue()）", "c.getValue()",
         "🔧 watch 支持方法调用表达式。可在不修改源码的情况下观察对象状态查询结果。\n\n"
         "**求值机制**：方法调用在沙箱 Interpreter 中执行，c 从当前作用域注入。方法内部"
         "的 this 绑定、字段访问、return 语句与正常运行时语义一致。注意：方法内的副作用"
         "（如修改字段）在沙箱中执行后被丢弃（沙箱化保护）。",
         "class Counter {\n    var value;\n    fun init() { value = 0; }\n    fun inc() { value = value + 1; }\n    "
         "fun getValue() { return value; }\n}\nvar c = Counter();\nvar i = 0;\nwhile (i < 5) {\n    c.inc();\n    "
         "i = i + 1;  // 在此行设置断点\n}\nprint(c.getValue());",
         "int"},
        {"closure-capture", "🔒 闭包捕获变量（counter()）", "counter()",
         "🔒 watch 支持闭包调用表达式。可观察闭包的捕获状态和返回值。\n\n"
         "**求值机制**：闭包对象从当前作用域注入，调用时恢复捕获的环境。每次调用都会"
         "更新闭包内部状态（沙箱化保护：副作用在求值后被丢弃）。",
         "fun makeCounter() {\n    var count = 0;\n    fun inc() {\n        count = count + 1;\n        return "
         "count;\n    }\n    return inc;\n}\nvar counter = makeCounter();\nvar i = 0;\nwhile (i < 5) {\n    "
         "print(counter());  // 在此行设置断点\n    i = i + 1;\n}",
         "int"},
    };
    return kScenarios;
}
