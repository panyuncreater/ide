#pragma once

#include "common/Result.h"                 // S6 fix: 统一错误处理 Result<Value>
#include "interpreter/RuntimeExceptions.h" // S6 fix: RuntimeError 完整定义
#include "interpreter/Value.h"
#include <cstdint>
#include <functional> // E3 fix: executeSharedInput 接收 std::function 回调
#include <string>
#include <vector>

// ============================================================
// BuiltinMethods - 内置方法分发辅助类
// ============================================================
// 处理数组、字典、字符串的内置方法调用。
// 从 Interpreter::visitMethodCall 中提取，降低 Interpreter.cpp 复杂度。
// 错误通过直接抛出 RuntimeError 传达（与 Interpreter::runtimeError 语义一致）。

/// 内置方法调用结果（Interpreter 侧专用，含 objectModified 标志）
struct BuiltinMethodResult {
    Value result;        // 方法返回值
    bool objectModified; // 对象是否被修改（需要 writeBack）

    BuiltinMethodResult() : result(Value::nullValue()), objectModified(false) {}
    BuiltinMethodResult(Value r, bool modified = false) : result(std::move(r)), objectModified(modified) {}
};

// ============================================================
// #20 fix: 内建方法名枚举分发（供 VM 和 RegisterVM 共用）
// ------------------------------------------------------------
// 原本 VM::classifyBuiltinMethod 是 VM 的私有静态方法，RegisterVM 无法复用，
// 其 callBuiltinMethod 用长串 if-else 字符串比较分发。提取到共享头后两处均可
// 用单次 hash + switch 分发，消除重复字符串比较。
// ============================================================

/// 内建方法名枚举（消除运行时字符串比较）
enum class BuiltinMethod {
    // 数组方法
    ARR_PUSH,
    ARR_POP,
    ARR_LEN,
    ARR_REMOVE,
    ARR_CONTAINS,
    ARR_JOIN,
    // 字典方法
    DICT_LEN,
    DICT_KEYS,
    DICT_VALUES,
    DICT_HAS,
    DICT_REMOVE,
    DICT_GET,
    DICT_SET,
    // 字符串方法
    STR_LEN,
    STR_UPPER,
    STR_LOWER,
    STR_CONTAINS,
    STR_STARTS_WITH,
    STR_ENDS_WITH,
    STR_REPLACE,
    STR_SUBSTR,
    STR_INDEX_OF,
    STR_SPLIT,
    STR_TRIM,
    // 未知
    UNKNOWN
};

/// 将方法名分类为枚举（按长度快速筛选 + 单次字符串比较，后续 switch 分发）
BuiltinMethod classifyBuiltinMethod(const std::string& name);

// ============================================================
// 共享纯函数层（供 Interpreter 和 VM 共用，不抛异常）
// ============================================================
// S6 fix: 统一使用 Result<Value> 替代 Result<Value>。
// 错误通过 Result<Value>::err() 返回，成功通过 Result<Value>::ok() 返回。
// 调用方（Interpreter handle*Method）使用 to_runtime_error() 转为异常抛出。
// 调用方（VM dispatch）直接检查 is_err() 并返回错误码。
//
// 设计要点：
//   1. 不抛异常 —— 错误通过 Result<Value> 传达，便于 VM 使用返回码语义
//   2. 参数使用 const Value* + size_t 而非 const std::vector<Value>& —— 避免 VM
//      从 SmallArgs 构造 vector 的额外分配（零拷贝传递栈上参数）
//   3. 非变异方法 —— 接收者均为 const Value&，不触发 COW detach

/// 共享纯函数：执行 len 方法（适用于数组/字典/字符串）
/// - 数组：返回元素个数
/// - 字典：返回键值对个数
/// - 字符串：返回 UTF-8 码位个数（M6 fix: 按码位而非字节计数）
/// @param obj       目标对象（必须是数组/字典/字符串）
/// @param args       参数列表首指针（argCount==0 时可为 nullptr）
/// @param argCount   参数数量（len 期望 0）
/// @param line       调用行号（用于错误报告）
/// @param column     调用列号（用于错误报告）
Result<Value> executeSharedLen(const Value& obj, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行数组 contains 方法
/// 线性扫描数组，使用 Value::equals 判断相等（与 Interpreter/VM 原实现一致）
/// @param arr       数组对象
/// @param args       参数列表首指针
/// @param argCount   参数数量（contains 期望 1）
/// @param line       调用行号
/// @param column     调用列号
Result<Value> executeSharedArrayContains(const Value& arr, const Value* args, size_t argCount, int line = 0,
                                         int column = 0);

/// 共享纯函数：执行字典 has / contains 方法
/// 检查字典中是否存在指定键（键通过 Value::toString 转换为字符串）
/// @param dict      字典对象
/// @param method     方法名（"has" 或 "contains"，仅用于错误消息）
/// @param args       参数列表首指针
/// @param argCount   参数数量（has/contains 期望 1）
/// @param line       调用行号
/// @param column     调用列号
Result<Value> executeSharedDictHas(const Value& dict, const std::string& method, const Value* args, size_t argCount,
                                   int line = 0, int column = 0);

// ============================================================
// 字符串方法共享层（供 Interpreter 和 VM 共用，不抛异常）
// ============================================================
// 补齐 Interpreter 与 VM 的字符串方法一致性：
// startsWith / endsWith / substr / indexOf

/// 共享纯函数：执行字符串 startsWith 方法
Result<Value> executeSharedStrStartsWith(const Value& str, const Value* args, size_t argCount, int line = 0,
                                         int column = 0);

/// 共享纯函数：执行字符串 endsWith 方法
Result<Value> executeSharedStrEndsWith(const Value& str, const Value* args, size_t argCount, int line = 0,
                                       int column = 0);

/// 共享纯函数：执行字符串 substr 方法
/// substr(start) 或 substr(start, length)
Result<Value> executeSharedStrSubstr(const Value& str, const Value* args, size_t argCount, int line = 0,
                                     int column = 0);

/// 共享纯函数：执行字符串 indexOf 方法
/// 返回 UTF-8 字符位置（非字节位置），未找到返回 -1
Result<Value> executeSharedStrIndexOf(const Value& str, const Value* args, size_t argCount, int line = 0,
                                      int column = 0);

/// 共享纯函数：执行字符串 replace 方法
/// 将所有 from 子串替换为 to（单遍构建，O(N) 复杂度）
/// @param str       字符串对象
/// @param args       参数列表首指针（replace 期望 2：from, to）
/// @param argCount   参数数量
/// @param line       调用行号
/// @param column     调用列号
Result<Value> executeSharedStrReplace(const Value& str, const Value* args, size_t argCount, int line = 0,
                                      int column = 0);

/// 共享纯函数：执行字符串 upper 方法（转大写）
Result<Value> executeSharedStrUpper(const Value& str, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行字符串 lower 方法（转小写）
Result<Value> executeSharedStrLower(const Value& str, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行字符串 split 方法（按分隔符分割）
/// split(sep) 或 split()（默认分隔符为空格）
Result<Value> executeSharedStrSplit(const Value& str, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行字符串 trim 方法（去除首尾空白）
Result<Value> executeSharedStrTrim(const Value& str, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行字符串 contains 方法
/// ARCH-15 fix: 补齐 contains 家族（arr/dict/str）的共享层覆盖，
/// 消除 Interpreter（BuiltinMethods.cpp handleStringMethod 内联）与
/// VM（VM.cpp dispatchStringBuiltin 内联）的重复实现。
/// 检查字符串是否包含子串（子串通过 Value::toString 转换）
/// @param str       字符串对象
/// @param args       参数列表首指针（contains 期望 1）
/// @param argCount   参数数量
/// @param line       调用行号
/// @param column     调用列号
Result<Value> executeSharedStrContains(const Value& str, const Value* args, size_t argCount, int line = 0,
                                       int column = 0);

/// 共享纯函数：执行字典 keys 方法（返回所有键组成的数组）
Result<Value> executeSharedDictKeys(const Value& dict, const Value* args, size_t argCount, int line = 0,
                                    int column = 0);

/// 共享纯函数：执行字典 values 方法（返回所有值组成的数组）
Result<Value> executeSharedDictValues(const Value& dict, const Value* args, size_t argCount, int line = 0,
                                      int column = 0);

/// 共享纯函数：执行字典 get 方法（按键查找，支持默认值）
/// get(key) 或 get(key, default)
Result<Value> executeSharedDictGet(const Value& dict, const Value* args, size_t argCount, int line = 0, int column = 0);

/// 共享纯函数：执行数组 join 方法（用分隔符连接所有元素）
/// join() 或 join(sep)
Result<Value> executeSharedArrayJoin(const Value& arr, const Value* args, size_t argCount, int line = 0,
                                     int column = 0);

// ============================================================
// 顶层内置函数共享层（供 Interpreter 和 VM 共用，不抛异常）
// ============================================================
// 支持: len / type / str / int / abs / min / max / range / sum / input
// 用户自定义函数和类会覆盖这些内置函数（由调用方在分发时保证优先级）

/// 判断函数名是否为顶层内置函数
bool isBuiltinFunction(const std::string& name);

/// 执行顶层内置函数
/// @param funcName  函数名（len/type/str/int/abs/min/max/range/sum/input）
/// @param args       参数列表首指针（argCount==0 时可为 nullptr）
/// @param argCount   参数数量
/// @param line       调用行号（用于错误报告）
/// @param column     调用列号（用于错误报告）
Result<Value> executeSharedBuiltinFunction(const std::string& funcName, const Value* args, size_t argCount,
                                           int line = 0, int column = 0);

// ============================================================
// R98 W2: 高阶函数共享层（map / filter / reduce / forEach / find）
// ============================================================
// 这 5 个高阶函数需要调用用户传入的闭包值。由于 executeSharedBuiltinFunction
// 是无状态纯函数层（无法调用闭包），此处提供独立的共享算法层：
// 各后端（Interpreter / StackVM / RegisterVM）注入 ClosureInvoker 回调
// 实现闭包调用，算法本身（数组遍历、结果构建）三后端共享。
//
// 设计要点：
//   1. 闭包调用是后端特定的（Interpreter 用 AST eval，VM 用帧压栈+run loop，
//      RegisterVM 用寄存器帧+run loop），无法在共享层实现
//   2. 算法逻辑（遍历、过滤、归约）与后端无关，抽取到共享层避免 3x 重复
//   3. 与 input() 类似，各后端在调用 executeSharedBuiltinFunction 之前拦截
//      这 5 个名字，分派到本共享层

/// 闭包调用回调：各后端注入具体实现
/// @param closure  闭包值（用户传入的函数）
/// @param args     参数列表首指针
/// @param argCount 参数数量
/// @param line     调用行号（用于错误报告）
/// @param column   调用列号（用于错误报告）
/// @return 闭包调用的结果（成功返回值，失败返回错误）
using ClosureInvoker =
    std::function<Result<Value>(const Value& closure, const Value* args, size_t argCount, int line, int column)>;

/// 判断函数名是否为高阶内置函数（map/filter/reduce/forEach/find）
bool isHigherOrderBuiltin(const std::string& name);

/// map(arr, fn) — 对数组每个元素应用 fn，返回新数组
Result<Value> executeSharedMap(const Value& arr, const Value& closure, const ClosureInvoker& invoke, int line = 0,
                               int column = 0);

/// filter(arr, fn) — 过滤数组元素，返回满足 fn(elem)==true 的元素组成的新数组
Result<Value> executeSharedFilter(const Value& arr, const Value& closure, const ClosureInvoker& invoke, int line = 0,
                                  int column = 0);

/// reduce(arr, fn, initial) — 归约数组为单个值
/// fn 接受两个参数：(accumulator, currentElement)
Result<Value> executeSharedReduce(const Value& arr, const Value& closure, const Value& initial,
                                  const ClosureInvoker& invoke, int line = 0, int column = 0);

/// forEach(arr, fn) — 遍历数组对每个元素执行 fn，返回 null
Result<Value> executeSharedForEach(const Value& arr, const Value& closure, const ClosureInvoker& invoke, int line = 0,
                                   int column = 0);

/// find(arr, fn) — 查找第一个满足 fn(elem)==true 的元素，未找到返回 null
Result<Value> executeSharedFind(const Value& arr, const Value& closure, const ClosureInvoker& invoke, int line = 0,
                                int column = 0);

/// E3 fix: 执行 input() 函数（共享层，供 Interpreter 和 VM 共用）
/// input() 不走 executeSharedBuiltinFunction 注册表（依赖 inputCallback 跨线程交互），
/// 由调用方直接调用本函数并传入 callback。
/// - input() 返回空串
/// - input(prompt) 返回用户输入字符串
/// callback 抛出的异常（如超时 RuntimeError）会被捕获并附上调用点行号/列号。
/// @param inputCallback 输入回调（由 IDE 主线程注入；为空时返回空串，允许非交互式运行）
/// @param args       参数列表首指针（argCount==0 时可为 nullptr）
/// @param argCount   参数数量（input 期望 0 或 1）
/// @param line       调用行号
/// @param column     调用列号
Result<Value> executeSharedInput(const std::function<std::string(const std::string&)>& inputCallback, const Value* args,
                                 size_t argCount, int line = 0, int column = 0);

// ============================================================
// R136 线程与并发原语：channel/mutex/rwlock 内置函数 + 方法分发
// ============================================================
// channel()/mutex()/rwlock() 是无参数构造函数，走 executeSharedBuiltinFunction 注册表。
// spawn(fn, args...) 类似 input()——需要后端注入 ClosureInvoker，走独立路径。
// channel/mutex/rwlock/thread 的方法（send/recv/lock/unlock/join 等）通过
// handleSyncObjectMethod 统一分发，供 Interpreter/VM 共用。

/// 判断函数名是否为并发原语构造函数（channel/mutex/rwlock）
bool isConcurrencyBuiltin(const std::string& name);

/// 同步对象方法分发（channel.send/recv/close, mutex.lock/unlock/tryLock,
/// rwlock.readLock/readUnlock/writeLock/writeUnlock/tryReadLock/tryWriteLock,
/// thread.join/detach/isJoinable）
/// @param method  方法名
/// @param obj     同步对象值（可变引用，部分方法可能修改内部状态如 channel.close）
/// @param args    已求值的参数列表
/// @param line    调用行号
/// @param col     调用列号
/// @return        方法结果 + 是否修改了对象（用于 writeBack，目前始终 false——
///                同步对象内部状态通过 shared_ptr<Inner> 共享，无需 writeBack）
BuiltinMethodResult handleSyncObjectMethod(const std::string& method, Value& obj, const std::vector<Value>& args,
                                           int line, int col);

/// spawn(fn, args...) 共享层入口（类似 input()，由调用方注入闭包调用器）
/// @param closure    闭包值（用户传入的函数）
/// @param args       闭包参数列表首指针
/// @param argCount   参数数量
/// @param invoker    闭包调用回调（各后端注入：Interpreter 用 AST eval，
///                   VM 用帧压栈+run loop，RegisterVM 用寄存器帧+run loop）
/// @param line       调用行号
/// @param column     调用列号
/// @return Thread 值（成功）或 err（参数错误）
/// 注：invoker 必须线程安全或通过外部 mutex 序列化。当前实现 Interpreter
/// 在 callSpawnBuiltin 内通过 spawnMutex_ 序列化所有 spawn 出的闭包调用。
Result<Value> executeSharedSpawn(const Value& closure, const Value* args, size_t argCount,
                                 const ClosureInvoker& invoker, int line = 0, int column = 0);

/// 内置方法辅助类（全静态方法，无状态）
class BuiltinMethods {
public:
    /// 处理数组内置方法（push, pop, len, remove, contains, join）
    /// @param method  方法名
    /// @param obj     数组值（可变引用，push/pop/remove 会修改）
    /// @param args    已求值的参数列表
    /// @param line    调用行号（用于错误报告）
    /// @param col     调用列号（用于错误报告）
    /// @return        方法结果 + 是否修改了对象
    /// @throws RuntimeError 参数错误或方法不存在时
    static BuiltinMethodResult handleArrayMethod(const std::string& method, Value& obj, const std::vector<Value>& args,
                                                 int line, int col);

    /// 处理字典内置方法（len, keys, values, has/contains, get, remove）
    static BuiltinMethodResult handleDictMethod(const std::string& method, Value& obj, const std::vector<Value>& args,
                                                int line, int col);

    /// 处理字符串内置方法（len, upper, lower, split, replace, trim）
    /// 字符串是不可变的，所以 obj 为 const 引用
    static BuiltinMethodResult handleStringMethod(const std::string& method, const Value& obj,
                                                  const std::vector<Value>& args, int line, int col);
};
