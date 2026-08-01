// ============================================================
// ClosureInspectorPanel.cpp — 闭包检查器面板实现（第三档 P2-3b）
// ============================================================

#include "gui/ClosureInspectorPanel.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>
#include <sstream>

// ============================================================
// ClosureInspectorLibrary — 静态教学场景库
// ============================================================
// 12 个典型闭包场景，覆盖：
//   - 简单闭包（捕获单个外层变量）
//   - 计数器闭包（通过闭包修改外层变量）
//   - 多变量捕获
//   - 嵌套闭包（多层捕获）
//   - 闭包作为返回值
//   - 闭包数组
//   - 立即调用函数表达式（IIFE）
//   - 闭包逃逸（闭包超出定义作用域）
//   - 闭包作为参数（回调模式）[ROUND-60 新增]
//   - 递归闭包（自引用）[ROUND-60 新增]
//   - 块作用域 upvalue 关闭 [ROUND-60 新增]
//   - 互递归闭包 [ROUND-60 新增]
//
// 帮助学习者理解闭包机制：
//   - 闭包捕获外层作用域的变量引用（upvalue）
//   - upvalue 在外层帧弹出后堆化（从栈迁移到堆）
//   - 闭包调用时绑定 upvalue 到当前环境链
//   - 闭包逃逸后仍可访问捕获的变量

const std::vector<ClosureScenario>& ClosureInspectorLibrary::scenarios() {
    static const std::vector<ClosureScenario> kScenarios = {
        {"simple-capture",
         "📦 简单闭包（捕获单个变量）",
         "📦 最基础的闭包形式。内部函数捕获外层变量 x，"
         "即使外层函数返回后，闭包仍可访问 x。"
         "捕获的变量称为 upvalue（上层作用域的值）。",
         "fun makeCounter() {\n    var x = 0;\n    fun increment() {\n        x = x + 1;\n        return x;\n    }\n"
         "    return increment;\n}\n\nvar counter = makeCounter();\nprint(counter());  // 1\nprint(counter());  // 2\n"
         "print(counter());  // 3",
         {"x"},
         "🔗 by-reference (upvalue)",
         "💡 increment 闭包捕获外层变量 x。makeCounter 返回后，"
         "x 的栈帧被弹出，但 x 作为 upvalue 被堆化（迁移到堆）。"
         "每次调用 counter() 时，闭包通过 upvalue 访问并修改堆上的 x。"
         "这展示了闭包的\"状态保持\"能力——变量在闭包之间共享。"},
        {"counter-pattern",
         "🔢 计数器模式（通过闭包修改外层变量）",
         "🔢 经典计数器模式。闭包通过 upvalue 修改外层变量 count，"
         "实现私有状态（外部无法直接访问 count，只能通过闭包操作）。"
         "这是闭包实现封装的核心模式。",
         "fun makeCounter() {\n    var count = 0;\n    fun next() {\n        count = count + 1;\n        return "
         "count;\n    }\n    return next;\n}\n\nvar c1 = makeCounter();\nvar c2 = makeCounter();\nprint(c1());  // "
         "1\nprint(c1());  // 2\nprint(c2());  // 1（独立计数器）",
         {"count"},
         "🔗 by-reference (upvalue)",
         "💡 c1 和 c2 是两个独立的计数器，各自捕获自己的 count。"
         "makeCounter 每次调用创建新的栈帧，新的 count 变量。"
         "两个闭包的 upvalue 指向不同的堆地址，互不干扰。"
         "这展示了闭包实现私有状态的机制——每个闭包实例独立维护状态。"},
        {"multi-capture",
         "📦 多变量捕获",
         "📦 闭包可以捕获多个外层变量。每个捕获的变量成为独立的 upvalue，"
         "在闭包对象中按索引存储。MiniLang 的 OP_CLOSURE 指令"
         "编码了 upvalue 数量与每个 upvalue 的位置（栈槽或上层 upvalue）。",
         "fun makeAdder(base, delta) {\n    fun add() {\n        base = base + delta;\n        return base;\n    }\n   "
         " return add;\n}\n\nvar adder = makeAdder(10, 5);\nprint(adder());  // 15\nprint(adder());  // 20",
         {"base", "delta"},
         "🔗 by-reference (upvalue)",
         "💡 add 闭包捕获两个 upvalue：base 和 delta。"
         "OP_CLOSURE 指令编码两个 upvalue 条目，每个指定变量位置。"
         "闭包对象在堆上包含两个 upvalue 指针，指向堆化的 base 和 delta。"
         "这展示了闭包的多 upvalue 机制。"},
        {"nested-closure",
         "🔄 嵌套闭包（多层捕获）",
         "🔄 多层嵌套的闭包。最内层闭包捕获中间层变量，"
         "中间层又捕获外层变量。upvalue 可以指向另一层闭包的 upvalue"
         "（而非直接指向栈槽），形成 upvalue 链。",
         "fun outer() {\n    var a = 1;\n    fun middle() {\n        var b = 2;\n        fun inner() {\n            "
         "return a + b;\n        }\n        return inner;\n    }\n    return middle;\n}\n\nvar mid = outer();\nvar inn "
         "= mid();\nprint(inn());  // 3",
         {"a", "b"},
         "🔗 by-reference (upvalue chain)",
         "💡 inner 闭包捕获两个 upvalue：a 来自 outer，b 来自 middle。"
         "middle 闭包本身也捕获了 a。inner 的 upvalue a 指向 middle 的 upvalue a，"
         "而非直接指向 outer 的栈槽。这形成 upvalue 链。"
         "当 outer 返回时，a 堆化；当 middle 返回时，b 堆化。"
         "inner 调用时通过 upvalue 链访问堆上的 a 和 b。"},
        {"closure-as-return",
         "↩️ 闭包作为返回值",
         "↩️ 闭包作为函数返回值是函数式编程的核心模式。"
         "返回的闭包携带捕获的 upvalue，即使定义作用域已销毁。"
         "这是闭包\"逃逸\"的最常见形式——闭包超出定义作用域存活。",
         "fun makeMultiplier(factor) {\n    fun closure(x) {\n        return x * factor;\n    }\n    return "
         "closure;\n}\n\nvar double = makeMultiplier(2);\nvar triple = makeMultiplier(3);\nprint(double(5));   // "
         "10\nprint(triple(5));   // 15",
         {"factor"},
         "🔗 by-reference (upvalue, escaped)",
         "💡 makeMultiplier 返回一个匿名闭包，捕获 factor。"
         "makeMultiplier 返回后，factor 的栈帧被弹出，factor 堆化。"
         "double 和 triple 各自捕获不同的 factor（2 和 3）。"
         "这展示了闭包逃逸——闭包超出定义作用域后仍可访问捕获的变量。"},
        {"closure-array",
         "📚 闭包数组",
         "📚 多个闭包可以共享同一个 upvalue，也可以各自独立。"
         "在循环中创建闭包时，所有闭包共享同一个循环变量"
         "（MiniLang 中 var 声明的变量在同一作用域内共享），"
         "因此所有闭包引用的 upvalue 指向同一地址。",
         "var fns = [];\nvar i = 0;\nwhile (i < 3) {\n    fun getter() { return i; }\n    fns.push(getter);\n    i = i "
         "+ 1;\n}\nprint(fns[0]());  // 3（不是 0！）\nprint(fns[1]());  // 3\nprint(fns[2]());  // 3",
         {"i"},
         "🔗 by-reference (shared upvalue)",
         "⚠️ 所有闭包共享同一个 i 变量。循环结束后 i = 3，"
         "因此所有闭包调用都返回 3。这是闭包捕获循环变量的经典陷阱。"
         "若需要每个闭包捕获不同的值，需在每次迭代中创建新作用域"
         "（如使用 IIFE 或块作用域）。"
         "这展示了闭包共享 upvalue 的语义。"},
        {"iife",
         "⚡ 立即调用函数表达式（IIFE）",
         "⚡ IIFE 用于创建新作用域，避免变量泄漏或闭包共享问题。"
         "在循环中使用 IIFE 可为每次迭代创建独立作用域，"
         "使闭包捕获不同的变量值。",
         "var fns = [];\nvar i = 0;\nwhile (i < 3) {\n    fun makeCaptured(captured) {\n        fun getter() { return "
         "captured; }\n        return getter;\n    }\n    fns.push(makeCaptured(i));\n    i = i + 1;\n}\nprint("
         "fns[0]());  // 0\nprint(fns[1]());  // 1\nprint(fns[2]());  // 2",
         {"captured"},
         "🔗 by-reference (upvalue, per-iteration)",
         "💡 IIFE 每次调用创建新栈帧，参数 captured 绑定当前 i 的值。"
         "内部闭包捕获 captured（而非 i），因此每个闭包引用不同的 captured。"
         "这解决了闭包数组中共享循环变量的问题。"
         "这展示了通过 IIFE 创建独立 upvalue 的模式。"},
        {"closure-escape",
         "🏃 闭包逃逸（超出定义作用域）",
         "🏃 闭包逃逸指闭包超出定义作用域后仍被使用。"
         "逃逸的闭包通过 upvalue 访问已堆化的变量。"
         "MiniLang 的 GC 会在闭包不可达时释放 upvalue。",
         "fun makeAccumulator() {\n    var total = 0;\n    fun add(x) {\n        total = total + x;\n        return "
         "total;\n    }\n    return add;\n}\n\nvar acc = makeAccumulator();\n// makeAccumulator 已返回，total "
         "仍存活\nprint(acc(10));  // 10\nprint(acc(20));  // 30\nprint(acc(5));   // 35",
         {"total"},
         "🔗 by-reference (upvalue, heap-escaped)",
         "💡 makeAccumulator 返回后，total 的栈帧弹出，total 堆化。"
         "acc 闭包持有 total 的 upvalue 指针，total 在堆上存活。"
         "每次调用 acc(x) 时，闭包通过 upvalue 修改堆上的 total。"
         "total 在 acc 不可达时才会被 GC 回收。"
         "这展示了闭包逃逸后 upvalue 的堆化与 GC 机制。"},
        {"closure-as-param",
         "📞 闭包作为参数（回调模式）",
         "📞 高阶函数接受闭包作为参数。"
         "map/filter/reduce 等函数式模式都基于此。"
         "闭包作为一等公民（first-class value）可在函数间传递，"
         "调用时通过 OP_CALL 指令执行参数位置的闭包。"
         "本例中 add5 闭包捕获了外层变量 n 作为 upvalue，再作为参数传递给 apply。",
         "fun apply(fn, x) {\n    return fn(x);\n}\n\nfun makeAdder(n) {\n    fun add(x) {\n        "
         "return x + n;\n    }\n    return add;\n}\n\nvar add5 = makeAdder(5);\nprint(apply(add5, "
         "3));  // 8",
         {"n"},
         "🔗 by-reference (upvalue，闭包作为参数传递)",
         "💡 闭包是一等值，可作为参数传递。"
         "add5 闭包捕获 makeAdder 的局部变量 n 作为 upvalue，"
         "然后作为参数传递给 apply。apply 内部调用 fn(x) 时 VM 执行 OP_CALL。"
         "fn 在 apply 栈帧中作为参数存在，调用时被推入新栈帧作为 callee，"
         "同时携带 add5 的 upvalue 链。"
         "这展示了闭包作为一等公民的核心特征——既捕获状态又能被传递。"},
        {"closure-recursive",
         "🔄 递归闭包（自引用）",
         "🔄 闭包通过名字在自身函数体内引用自身。"
         "MiniLang 中函数名在 fun 声明完成后即绑定到当前作用域，"
         "因此闭包体内的自引用会在调用时通过作用域链查找。",
         "fun makeFactorial() {\n    fun fact(n) {\n        if (n <= 1) return 1;\n        return n * "
         "fact(n - 1);\n    }\n    return fact;\n}\n\nvar f = makeFactorial();\nprint(f(5));  // 120",
         {"fact"},
         "🔗 by-reference (upvalue self-ref)",
         "💡 递归闭包通过环境链自引用。"
         "fact 声明完成后，名字 fact 绑定到 makeFactorial 的环境。"
         "fact 体内调用 fact(n-1) 时，VM 通过作用域链查找 fact（找到自身）。"
         "fact 作为 upvalue 被 fact 闭包自身捕获，形成自引用循环。"
         "makeFactorial 返回后，fact 仍可通过 upvalue 访问自身。"},
        {"closure-close-upvalue",
         "🔒 块作用域 upvalue 关闭",
         "🔒 循环体内每次迭代创建新作用域，"
         "块结束时 OP_CLOSE_UPVALUE 显式关闭 upvalue，"
         "将栈变量迁移到堆。这使每个闭包捕获独立的变量副本。",
         "fun makeCallbacks() {\n    var callbacks = [];\n    for (var i = 0; i < 3; i = i + 1) {\n        "
         "var x = i * 10;\n        callbacks.push(fun() { return x; });\n    }\n    return callbacks;\n}\n\nvar "
         "cbs = makeCallbacks();\nprint(cbs[0]());  // 0\nprint(cbs[1]());  // 10\nprint(cbs[2]());  // 20",
         {"x"},
         "🔗 by-reference (upvalue, per-iteration)",
         "💡 每次循环迭代创建新的块作用域，var x 是该作用域的局部变量。"
         "迭代结束时 OP_CLOSE_UPVALUE 触发，x 从栈迁移到堆。"
         "每个闭包捕获的 x 指向不同的堆地址，因此返回 0/10/20。"
         "这与 closure-array 形成对比：closure-array 中所有闭包共享同一个 i。"
         "这展示了块作用域 upvalue 关闭机制的关键作用。"},
        {"closure-mutual-recursion",
         "🔀 互递归闭包",
         "🔀 两个闭包互相引用对方。"
         "isEven 调用 isOdd，isOdd 调用 isEven。"
         "前向引用（isOdd 在 isEven 之后定义）通过作用域链在调用时解析。",
         "fun makeMutual() {\n    fun isEven(n) {\n        if (n == 0) return true;\n        return isOdd(n "
         "- 1);\n    }\n    fun isOdd(n) {\n        if (n == 0) return false;\n        return isEven(n - "
         "1);\n    }\n    return isEven;\n}\n\nvar check = makeMutual();\nprint(check(4));  // true",
         {"isEven", "isOdd"},
         "🔗 by-reference (upvalue mutual)",
         "💡 互递归闭包通过共享环境互相引用。"
         "isEven 声明时 isOdd 尚未定义，但 isEven 体内的 isOdd 引用在调用时解析。"
         "VM 在 isEven 调用 isOdd 时通过作用域链查找（此时 isOdd 已绑定）。"
         "两个闭包都捕获对方作为 upvalue，形成双向引用。"
         "这展示了闭包前向引用与作用域链延迟解析的机制。"},
        // ---- 需求 8 扩充：共享状态对/记忆化/函数组合/一次性门闩 ----
        {"closure-shared-pair",
         "👯 闭包对（getter/setter 共享同一 upvalue）",
         "👯 同一作用域内定义的多个闭包捕获同一个变量时，"
         "它们共享同一个 upvalue（指向同一堆地址）。"
         "一个闭包的写入对另一个闭包立即可见——这是实现"
         "getter/setter 封装模式的基础。",
         "fun makeBox() {\n    var value = 0;\n    fun get() { return value; }\n    fun set(v) { value = v; "
         "}\n    return (get, set);\n}\n\nvar (get, set) = makeBox();\nset(42);\nprint(get());  // 42（set 的写入对 "
         "get 可见）",
         {"value"},
         "🔗 by-reference (shared upvalue, getter/setter)",
         "💡 get 与 set 两个闭包捕获同一个 value，OP_CLOSURE 为两者生成的 upvalue "
         "条目指向同一栈槽；makeBox 返回时 value 堆化一次，两个 upvalue 指向同一堆地址。"
         "set(42) 通过 upvalue 写堆，get() 读同一地址立即看到新值。"
         "这展示了共享 upvalue 实现封装私有状态的双接口模式。"},
        {"closure-memoize",
         "🧠 记忆化闭包（捕获字典缓存）",
         "🧠 闭包捕获一个字典作为缓存，重复调用相同参数时直接返回缓存结果。"
         "缓存状态对外不可见（私有），但在多次调用间持久——"
         "这是函数式编程中经典的记忆化（memoization）模式。",
         "fun makeMemoSquare() {\n    var cache = {};\n    fun square(n) {\n        var key = \"\" + n;\n        "
         "if (cache.has(key)) {\n            print(\"cache hit!\");\n            return cache[key];\n        }\n        "
         "cache[key] = n * n;\n        return cache[key];\n    }\n    return square;\n}\n\nvar sq = "
         "makeMemoSquare();\nprint(sq(9));  // 81（计算）\nprint(sq(9));  // cache hit! 81（缓存）",
         {"cache"},
         "🔗 by-reference (upvalue, 持久缓存)",
         "💡 cache 字典作为 upvalue 被 square 捕获，makeMemoSquare 返回后堆化。"
         "每次调用 sq() 读写同一个堆上字典；注意 COW 语义下 cache[key] = v 的写回"
         "经 OP_WRITEBACK_*_UPVALUE 指令写回 upvalue，否则变异只发生在副本上。"
         "这展示了闭包 + 容器 upvalue 的写回机制与私有缓存模式。"},
        {"closure-compose",
         "🔗 函数组合（闭包捕获闭包）",
         "🔗 compose(f, g) 返回新闭包，捕获的 upvalue 本身就是两个闭包。"
         "闭包值与普通值一样可被捕获——upvalue 指向的堆槽里存的是 "
         "ClosureData 指针。这是函数式管道（pipeline）的基础。",
         "fun compose(f, g) {\n    fun composed(x) {\n        return f(g(x));\n    }\n    return "
         "composed;\n}\n\nfun double(x) { return x * 2; }\nfun inc(x) { return x + 1; }\n\nvar doubleThenInc = "
         "compose(inc, double);\nprint(doubleThenInc(5));  // 11（先 ×2 再 +1）",
         {"f", "g"},
         "🔗 by-reference (upvalue 持有闭包值)",
         "💡 composed 捕获 f、g 两个 upvalue，它们的值是 ClosureData*（闭包也是一等值）。"
         "调用 composed(5) 时，VM 先通过 upvalue 取 g 执行 OP_CALL_EXPR，再取 f 调用。"
         "upvalue 堆槽里存闭包指针使引用计数链延长：composed 活着就拖着 f、g 不被 GC。"
         "这展示了闭包捕获闭包的组合模式与对象存活链。"},
        {"closure-once",
         "🚪 一次性门闩（once 模式）",
         "🚪 闭包捕获布尔标志实现「只执行一次」语义：首次调用执行并置位，"
         "后续调用直接返回缓存结果。初始化防重入、惰性单例都是这个模式。",
         "fun makeOnce() {\n    var called = false;\n    var result = null;\n    fun once() {\n        if "
         "(!called) {\n            called = true;\n            result = \"initialized\";\n            print("
         "\"init!\");\n        }\n        return result;\n    }\n    return once;\n}\n\nvar init = "
         "makeOnce();\nprint(init());  // init! initialized\nprint(init());  // initialized（不再 init）",
         {"called", "result"},
         "🔗 by-reference (upvalue 状态机)",
         "💡 called 与 result 两个 upvalue 构成闭包内部状态机。"
         "首次调用通过 OP_SET_UPVALUE 置 called=true 并写 result；"
         "后续调用走 OP_GET_UPVALUE 读到 true 直接返回。"
         "外部无法重置 called（私有性），保证初始化逻辑恰好执行一次。"
         "这展示了多 upvalue 协作实现状态机的闭包封装能力。"},
    };
    return kScenarios;
}

// ============================================================
// UpvaluePhaseLibrary — 静态生命周期图解库
// ============================================================
// 6 个 upvalue 生命周期阶段，覆盖：
//   - 创建（闭包定义时编码 upvalue）
//   - 捕获（绑定外层变量到 upvalue）
//   - 堆化（栈帧弹出时迁移到堆）
//   - 访问（闭包调用时通过 upvalue 读写）
//   - 关闭（OP_CLOSE_UPVALUE 显式关闭）
//   - 销毁（GC 回收不可达 upvalue）

const std::vector<UpvaluePhaseDoc>& UpvaluePhaseLibrary::phases() {
    static const std::vector<UpvaluePhaseDoc> kPhases = {
        {"create", "create",
         "📦 闭包定义时（OP_CLOSURE 指令执行），VM 读取 upvalue 数量与每个 upvalue 的位置。"
         "upvalue 位置可以是\"栈槽\"（直接外层变量）或\"上层 upvalue\"（嵌套闭包）。"
         "VM 创建 Closure 对象，分配 upvalue 数组。",
         "栈：弹出 OP_CLOSURE 的操作数，推入 Closure 对象"},
        {"capture", "capture",
         "🔗 捕获阶段：VM 遍历 upvalue 描述符，为每个 upvalue 查找或创建 Upvalue 对象。"
         "若 upvalue 指向栈槽，VM 在 openUpvalues_ 中查找或创建条目。"
         "Upvalue 对象持有指向栈槽的指针（is_open=true）。",
         "栈：Upvalue 指针指向栈槽，openUpvalues_ 插入条目"},
        {"heap", "heap",
         "🏗️ 堆化阶段：当外层函数返回时（OP_RETURN），栈帧被弹出。"
         "VM 调用 closeUpvaluesFrom(slot) 关闭所有指向该栈槽及以上的 upvalue。"
         "关闭的 upvalue 将值从栈复制到堆，Upvalue 对象改为指向堆地址（is_open=false）。",
         "栈：栈槽弹出，Upvalue.value 从栈复制到堆，openUpvalues_ 移除条目"},
        {"access", "access",
         "🔄 访问阶段：闭包调用时（OP_CALL），VM 通过 OP_GET_UPVALUE / OP_SET_UPVALUE "
         "指令读写 upvalue。若 upvalue 仍 open，直接访问栈槽；若已 closed，访问堆地址。"
         "这实现了闭包对外层变量的透明访问。",
         "栈：OP_GET_UPVALUE 推入 upvalue 值，OP_SET_UPVALUE 弹出值写入 upvalue"},
        {"close", "close",
         "✨ 显式关闭：OP_CLOSE_UPVALUE 指令显式关闭 upvalue（用于变量离开作用域时）。"
         "VM 调用 closeUpvaluesFrom(slot)，将所有指向 slot 及以上的 open upvalue 堆化。"
         "常用于块作用域结束时确保变量被堆化（如 for 循环变量）。",
         "栈：OP_CLOSE_UPVALUE 执行后，upvalue 从栈指向堆"},
        {"destroy", "destroy",
         "🧹 销毁阶段：当闭包不可达时（无引用指向 Closure 对象），GC 回收闭包。"
         "闭包的 upvalue 数组被释放，若 upvalue 引用的堆值无其他引用，"
         "堆值也被回收。MiniLang 使用引用计数 + 周期回收。",
         "栈/堆：Closure 引用计数归零，upvalue 数组释放，堆值（若无引用）被回收"},
    };
    return kPhases;
}

// ============================================================
// ClosureInspectorPanel 实现
// ============================================================

ClosureInspectorPanel::ClosureInspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 顶部页面切换按钮
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(4, 4, 4, 4);
    pageScenarioBtn_ = new QPushButton("教学场景库", this);
    pagePhaseBtn_ = new QPushButton("upvalue 生命周期", this);
    pageScenarioBtn_->setCheckable(true);
    pagePhaseBtn_->setCheckable(true);
    pageScenarioBtn_->setChecked(true);
    topBar->addWidget(pageScenarioBtn_);
    topBar->addWidget(pagePhaseBtn_);
    topBar->addStretch();
    mainLayout->addLayout(topBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_);

    // 子页 1：教学场景库
    auto* scenarioPage = new QWidget(stack_);
    buildScenarioPage(scenarioPage);
    stack_->addWidget(scenarioPage);

    // 子页 2：upvalue 生命周期
    auto* phasePage = new QWidget(stack_);
    buildPhasePage(phasePage);
    stack_->addWidget(phasePage);

    // 顶部按钮切换
    connect(pageScenarioBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(stack_->widget(0));
        pageScenarioBtn_->setChecked(true);
        pagePhaseBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pagePhaseBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(stack_->widget(1));
        pageScenarioBtn_->setChecked(false);
        pagePhaseBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // 默认选中第一个
    if (!ClosureInspectorLibrary::scenarios().empty()) {
        scenarioList_->setCurrentRow(0);
    }
    if (!UpvaluePhaseLibrary::phases().empty()) {
        phaseList_->setCurrentRow(0);
    }
}

/// 构建「闭包场景」子页 UI。
void ClosureInspectorPanel::buildScenarioPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(splitter);
    scenarioDetail_ = new QTextBrowser(splitter);
    scenarioDetail_->setOpenExternalLinks(false);

    for (const auto& s : ClosureInspectorLibrary::scenarios()) {
        scenarioList_->addItem(QString::fromStdString(s.title));
    }

    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});

    layout->addWidget(splitter);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, [this](int row) { populateScenarioDetail(row); });
}

/// 构建「捕获阶段」子页 UI。
void ClosureInspectorPanel::buildPhasePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    phaseList_ = new QListWidget(splitter);
    phaseDetail_ = new QTextBrowser(splitter);
    phaseDetail_->setOpenExternalLinks(false);

    for (const auto& p : UpvaluePhaseLibrary::phases()) {
        phaseList_->addItem(QString::fromStdString(p.phase));
    }

    splitter->addWidget(phaseList_);
    splitter->addWidget(phaseDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});

    layout->addWidget(splitter);

    connect(phaseList_, &QListWidget::currentRowChanged, this, [this](int row) { populatePhaseDetail(row); });
}

/// 填充指定闭包场景的详细说明与示例代码。
void ClosureInspectorPanel::populateScenarioDetail(int index) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    if (index < 0 || index >= static_cast<int>(scenarios.size())) {
        scenarioDetail_->clear();
        return;
    }
    const auto& s = scenarios[index];

    std::ostringstream oss;
    oss << "<h2>" << s.title << "</h2>";
    oss << "<p><b>ID:</b> " << s.id << "</p>";
    oss << "<p><b>说明:</b></p>";
    oss << MarkdownRenderer::markdownToHtmlFragment(s.description).toStdString();

    oss << "<h3>示例代码</h3><pre>" << s.sampleCode << "</pre>";

    oss << "<h3>捕获变量</h3><ul>";
    for (const auto& v : s.capturedVars) {
        oss << "<li>" << v << "</li>";
    }
    oss << "</ul>";

    oss << "<p><b>捕获类型:</b> " << s.captureType << "</p>";

    oss << "<h3>教学注释</h3>";
    oss << MarkdownRenderer::markdownToHtmlFragment(s.teachingNote).toStdString();

    oss << "<hr><p><a href=\"#load\">载入到编辑器</a></p>";

    scenarioDetail_->setHtml(QString::fromStdString(oss.str()));

    // 连接载入信号
    disconnect(scenarioDetail_, nullptr, this, nullptr);
    connect(scenarioDetail_, &QTextBrowser::anchorClicked, this,
            [this, s](const QUrl&) { emit loadSampleRequested(QString::fromStdString(s.sampleCode)); });
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 填充指定捕获阶段的说明与图示。
void ClosureInspectorPanel::populatePhaseDetail(int index) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    if (index < 0 || index >= static_cast<int>(phases.size())) {
        phaseDetail_->clear();
        return;
    }
    const auto& p = phases[index];

    std::ostringstream oss;
    oss << "<h2>" << p.phase << "</h2>";
    oss << "<p><b>分类:</b> " << p.category << "</p>";
    oss << "<p><b>说明:</b></p>";
    oss << MarkdownRenderer::markdownToHtmlFragment(p.description).toStdString();
    oss << "<p><b>栈/堆效应:</b> " << p.stackEffect << "</p>";

    phaseDetail_->setHtml(QString::fromStdString(oss.str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}
