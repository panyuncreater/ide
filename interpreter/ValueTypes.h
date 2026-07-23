#pragma once

// ============================================================
// ValueTypes.h — 类型定义模块
// ------------------------------------------------------------
// 从 Value.h 拆分出的类型定义与前置声明，供需要 ValueType 枚举
// 或 VMClosureData 但不需要完整 Value 定义的文件包含，
// 降低编译耦合度。
// ============================================================

#include <memory>
#include <string>
#include <vector>

// 前向声明（避免循环依赖）
class Environment;
class FunDecl;
struct BytecodeChunk;        // M3 fix: 闭包值持有函数 chunk 指针（D-1: 与 Bytecode.h 定义一致）
struct RegBytecodeChunk;     // R164 D.6: 协程值持有 RegisterVM chunk 指针（与 RegisterBytecode.h 一致）

// ============================================================
// Value 运行时值类型 — NaN-boxing + 侵入式引用计数（PERF-12）
// ============================================================
// PERF-12: 从 std::variant<shared_ptr<XData>>（24 字节）迁移到 NaNBox（8 字节）。
//   sizeof(Value) = 8 字节
//   标量（int/float/bool/null）内联存储于 NaNBox，零原子操作
//   堆类型（string/array/dict/instance/closure）通过 RefCounted* 指针存储，
//   侵入式引用计数（addRef/release），COW 通过 ensureUnique<T>() detach

/// 值类型枚举（PERF-12: 不再依赖 variant 索引，由 NaNBox::tag() + RefCounted::type 分发）
enum class ValueType {
    VAL_NULL,     // NaNBox NULL_BITS
    VAL_INT,      // NaNBox int48 内联 或 BoxedIntData* 装箱
    VAL_FLOAT,    // NaNBox double 原始位
    VAL_BOOL,     // NaNBox bool
    VAL_STRING,   // StringData* (RefCounted)
    VAL_ARRAY,    // ArrayData* (RefCounted)
    VAL_DICT,     // DictData* (RefCounted)
    VAL_INSTANCE, // InstanceData* (RefCounted)
    VAL_CLOSURE,  // ClosureData* (RefCounted)
    VAL_TUPLE,    // R98 元组与解构：TupleData* (RefCounted, immutable)
    VAL_ENUM_VARIANT, // R99 枚举与 ADT：EnumVariantData* (RefCounted, immutable)
    // R136 线程与并发原语：四种同步对象（均使用 shared_ptr<Inner> 共享底层资源）
    VAL_CHANNEL,  // ChannelData* (RefCounted) — 阻塞式消息通道（mutex+cv+queue）
    VAL_MUTEX,    // MutexData* (RefCounted) — 互斥锁（shared_ptr<std::mutex>）
    VAL_RWLOCK,   // RwLockData* (RefCounted) — 读写锁（shared_ptr<std::shared_mutex>）
    VAL_THREAD,   // ThreadData* (RefCounted) — 线程句柄（shared_ptr<std::thread>）
    // R164 协程/生成器：CoroutineData* (RefCounted)
    // 生成器函数调用返回的协程值，通过 .next() 恢复执行、.done() 判断是否耗尽。
    // Interpreter 使用重放模式（每次 .next() 从头执行，用 yieldId 跳过已返回的 yield）。
    VAL_COROUTINE
};

// ============================================================
// P2-8 fix: 类型名字符串常量（统一管理，避免多处硬编码）
// ============================================================
// 用于 Value::typeName() 和 Interpreter::checkType() 的类型注解匹配。
namespace TypeName {
constexpr const char* INT = "int";
constexpr const char* FLOAT = "float";
constexpr const char* BOOL = "bool";
constexpr const char* STRING = "string";
constexpr const char* NULL_T = "null";
constexpr const char* ARRAY = "array";
constexpr const char* DICT = "dict";
constexpr const char* INSTANCE = "instance";
constexpr const char* CLOSURE = "closure";
constexpr const char* TUPLE = "tuple"; // R98 元组与解构
constexpr const char* ENUM = "enum";  // R99 枚举与 ADT（泛型注解，匹配任意 enum variant）
// R136 线程与并发原语
constexpr const char* CHANNEL = "channel";
constexpr const char* MUTEX = "mutex";
constexpr const char* RWLOCK = "rwlock";
constexpr const char* THREAD = "thread";
// R164 协程/生成器
constexpr const char* COROUTINE = "coroutine";
} // namespace TypeName

// ============================================================
// VM 闭包数据结构（定义在 Value 之前，因 ClosureData 持有其 shared_ptr）
// ============================================================

/// VM 闭包 upvalue 数据（VM-05/06）
/// 定义在此处以便 ClosureData 通过 shared_ptr<VMClosureData> 引用，
/// 完整的 VMUpvalue 定义在 ValueData.h（需要 Value 完整定义）
struct VMClosureData {
    std::string functionName;
    std::vector<std::shared_ptr<struct VMUpvalue>> upvalues;
    const BytecodeChunk* chunkPtr = nullptr; // M3 fix: 直接持有函数 chunk 指针
};
