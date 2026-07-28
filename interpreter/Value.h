/**
 * @file interpreter/Value.h
 * @brief 运行时值结构体 Value（NaN-boxing + 侵入式引用计数）。
 *
 * PERF-12: 将 Value 从 std::variant<..., shared_ptr<XData>, ...>（24 字节）
 * 迁移到 NaNBox（8 字节）+ 侵入式引用计数。
 *
 * 核心改进：
 *   - sizeof(Value) = 8 字节（原 24 字节），VM 栈缓存局部性提升 3 倍
 *   - 标量拷贝（int/float/bool/null）：零原子操作，仅拷贝 8 字节
 *   - 堆类型拷贝（string/array/dict/instance/closure）：一次原子递增
 *   - int48 范围内的整数内联存储，超范围自动装箱（BoxedIntData）
 *   - COW 语义保留：写入前检查 refCount==1，否则深拷贝
 *   - 公开 API 完全向后兼容（调用方无需修改）
 *
 * 存储布局：
 *   NaNBox box_  (8 字节)
 *     ├─ 标量类型：直接编码（int48/float64/bool/null）
 *     └─ 堆类型：  PTR_TAG_BASE | 48位 RefCounted* 指针
 *
 * 引用计数：
 *   - 堆数据类型继承 RefCounted（atomic refCount + ValueType type）
 *   - Value 的拷贝/移动/析构手动管理 addRef/release
 *   - COW detach 通过 isUnique() 判断
 *
 * 已知限制：环形容器泄漏（见 RefCounted.h 的 "#9 文档化" 章节）
 *   - 自引用容器（如 `a=[]; a.append(a)` 或 `d={}; d.self=d`）
 *     会形成引用计数永不归零的循环，导致内存泄漏直至进程退出。
 *   - 闭包场景的循环已通过 ClosureData.env = weak_ptr<Environment>
 *     静态打破；用户层容器循环依赖 OS 退出回收，视为可接受。
 *
 * @see NaNBox RefCounted GcManager Environment
 */
#pragma once

// ============================================================
// Value.h — 运行时值结构体（PERF-12: NaN-boxing + intrusive refcount）
// ------------------------------------------------------------
// 将 Value 从 std::variant<..., shared_ptr<XData>, ...>（24 字节）
// 迁移到 NaNBox（8 字节）+ 侵入式引用计数。
//
// 核心改进：
//   - sizeof(Value) = 8 字节（原 24 字节），VM 栈缓存局部性提升 3 倍
//   - 标量拷贝（int/float/bool/null）：零原子操作，仅拷贝 8 字节
//   - 堆类型拷贝（string/array/dict/instance/closure）：一次原子递增
//   - int48 范围内的整数内联存储，超范围自动装箱（BoxedIntData）
//   - COW 语义保留：写入前检查 refCount==1，否则深拷贝
//   - 公开 API 完全向后兼容（调用方无需修改）
//
// 存储布局：
//   NaNBox box_  (8 字节)
//     ├─ 标量类型：直接编码（int48/float64/bool/null）
//     └─ 堆类型：  PTR_TAG_BASE | 48位 RefCounted* 指针
//
// 引用计数：
//   - 堆数据类型继承 RefCounted（atomic refCount + ValueType type）
//   - Value 的拷贝/移动/析构手动管理 addRef/release
//   - COW detach 通过 isUnique() 判断
//
// 已知限制：环形容器泄漏（见 RefCounted.h 的 "#9 文档化" 章节）
//   - 自引用容器（如 `a=[]; a.append(a)` 或 `d={}; d.self=d`）
//     会形成引用计数永不归零的循环，导致内存泄漏直至进程退出。
//   - 闭包场景的循环已通过 ClosureData.env = weak_ptr<Environment>
//     静态打破；用户层容器循环依赖 OS 退出回收，视为可接受。
// ============================================================

#include "common/RuntimeLimits.h"
#include "interpreter/GcManager.h"    // Bug2 fix: 循环引用 GC
#include "interpreter/NaNBox.h"       // PERF-12: 8字节 NaN-boxing 编码
#include "interpreter/NumericUtils.h" // 共享溢出检查（B6 fix）
#include "interpreter/RefCounted.h"   // PERF-12: 侵入式引用计数基类
#include "interpreter/ValueTypes.h"

#include "common/Utf8Utils.h" // perf1 fix: Value::codepointCount() 委托 Utf8::codepointCount
#include <array>
#include <atomic> // L3 fix: ClosureData::functionId 全局生成器
#include <cassert>
#include <charconv> // Perf-Finding2: VAL_INT toString 用 to_chars 替代 std::to_string（避免 locale 查询）
#include <cmath>
#include <condition_variable> // R136 ChannelData 同步原语
#include <cstdint>
#include <cstdio>
#include <cstdlib> // AUDIT-NANBOX fix: std::abort — Release 构建中 assert 被剥离，类型不匹配时静默返回垃圾值（如指针间类型混淆 reinterpret_cast 错误类型），改为运行时 abort 与 NaNBox::asXxx 风格一致
#include <memory>
#include <mutex>        // R136 ChannelData/MutexData 同步原语
#include <optional>     // perf1 fix: StringData 缓存 codepointCount; L4 fix: dictKeyFromValue 返回 optional
#include <queue>        // R136 ChannelData 消息缓冲
#include <shared_mutex> // R136 RwLockData 读写锁
#include <string>       // R97 #5: 移除 <sstream>（dictKeyToString 改用 std::to_chars 后不再使用）
#include <thread>       // R136 ThreadData 线程句柄
#include <unordered_map>
#include <unordered_set>
#include <variant> // L4 fix: DictKey = variant<string, int64_t, bool, double>
#include <vector>

// ============================================================
// Value 运行时值结构体
// ============================================================

struct Value {
    // Bug2 fix: GcManager mark-sweep 需要访问 private 嵌套类型和 box_ 进行容器图遍历
    friend class GcManager;

private:
    // ---- 堆类型数据载体（继承 RefCounted， intrusive 引用计数）----
    struct StringData : RefCounted {
        std::string value;
        // perf1 fix: 缓存 UTF-8 码位数，避免 len()/substr() 在循环中 O(n²) 重复扫描。
        // mutable 允许 const stringVal() 路径的 codepointCount() 填充缓存。
        // 非 const stringVal() 返回可变引用时 reset() 使缓存失效（字符串可能被修改）。
        mutable std::optional<int64_t> cachedCodepointCount;
        // R97 #3 fix: 缓存 isAscii 标志，避免字符串索引 ASCII 快速路径每次 O(n) 扫描。
        // 替代两后端各自维护的 lastAsciiStr* 4 字段缓存（BUG-015 三重验证），
        // 消除堆地址复用误命中风险（isAscii 直接存在 StringData 内，与指针绑定）。
        // mutable 允许 const 路径懒填充；非 const stringVal() 返回可变引用时 reset() 失效。
        mutable std::optional<bool> cachedIsAscii;
        StringData() : RefCounted(ValueType::VAL_STRING) {}
        explicit StringData(std::string v) : RefCounted(ValueType::VAL_STRING), value(std::move(v)) {}
    };

    struct ArrayData : RefCounted {
        std::vector<Value> elements;
        ArrayData() : RefCounted(ValueType::VAL_ARRAY) {
            // Bug2 fix: 注册到 GcManager 跟踪循环引用
            GcManager::instance().registerTracked(this);
        }
        explicit ArrayData(std::vector<Value> v) : RefCounted(ValueType::VAL_ARRAY), elements(std::move(v)) {
            GcManager::instance().registerTracked(this);
        }
    };

    // L4 fix（2026-07-19）: 字典键支持 string/int/bool/float 四种类型。
    // 原 DictData::entries 为 unordered_map<string, Value>，导致 d[1]/d[true]/d[3.14] 必须
    // stringify 后存储，丢失类型信息（d[1] 与 d["1"] 误判为同一键）。
    // 引入 DictKey variant 后，d[1] 与 d["1"] 是不同的键，符合 Python/JS 语义。
    // float 键用 double 位模式 hash 避免精度漂移导致的 hash 不一致。
    // null/array/dict/instance/closure 不允许作为键（fromValue 返回 nullopt，调用方报错）。
    // 注意：DictKey/DictKeyHash/DictKeyEqual/DictMap/dictKeyFromValue/dictKeyToValue/
    // dictKeyToString 为 public，因为 dictVal()/tryGetMutableDict()/Value(const DictMap&)
    // 等公开 API 使用这些类型作为返回值或参数，外部调用方需能命名这些类型。
public:
    using DictKey = std::variant<std::string, int64_t, bool, double>;

    /// DictKey 的 hash 函数：按 variant index 区分类型，避免 bool(true) 与 int(1) 冲突。
    /// float 用位模式 hash，避免 std::hash<double> 在不同平台 NaN/-0.0 行为不一致。
    /// R97 #2 fix: 改为透明 hash（C++20 is_transparent），支持 unordered_map::find<string_view>
    /// 等异构查找，避免查找路径构造完整 DictKey（含 string 拷贝）的开销。
    /// 关键不变量：透明路径（如 string_view）与 DictKey 路径（DictKey{string}）必须产生
    /// 相同 hash，否则 find<string_view> 找不到桶。本实现用"类型前缀 XOR value hash"模式：
    /// DictKey{string} 的 hash = kStringTag ^ hash(string)，与 find(string_view) 一致。
    struct DictKeyHash {
        using is_transparent = void; // 启用 C++20 透明查找

        // 类型标签（避免不同类型相同值的 hash 碰撞，如 bool(true) vs int(1)）
        // 用斐波那契哈希常数的不同位移区分类型，确保 4 类键的 hash 空间基本不重叠
        static constexpr size_t kStringTag = 0x9E3779B9; // 黄金分割常数
        static constexpr size_t kIntTag = 0x6A09E667;    // sqrt(2) 的高位
        static constexpr size_t kBoolTag = 0xBB67AE85;   // sqrt(3) 的高位
        static constexpr size_t kDoubleTag = 0x3C6EF372; // sqrt(5) 的高位

        // 透明路径
        size_t operator()(const std::string& s) const { return kStringTag ^ std::hash<std::string>{}(s); }
        size_t operator()(const std::string_view& sv) const { return kStringTag ^ std::hash<std::string_view>{}(sv); }
        size_t operator()(int64_t v) const { return kIntTag ^ std::hash<int64_t>{}(v); }
        size_t operator()(bool v) const { return kBoolTag ^ std::hash<bool>{}(v); }
        size_t operator()(double v) const {
            if (v == 0.0) v = 0.0; // P0 fix: 规范化 -0.0 → +0.0，保证 hash 不变量
            uint64_t bits;
            std::memcpy(&bits, &v, sizeof(double));
            return kDoubleTag ^ std::hash<uint64_t>{}(bits);
        }
        // DictKey 路径（必须与透明路径产生相同 hash）
        size_t operator()(const DictKey& k) const {
            return std::visit(
                [&](const auto& v) -> size_t {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        return kStringTag ^ std::hash<std::string>{}(v);
                    else if constexpr (std::is_same_v<T, int64_t>)
                        return kIntTag ^ std::hash<int64_t>{}(v);
                    else if constexpr (std::is_same_v<T, bool>)
                        return kBoolTag ^ std::hash<bool>{}(v);
                    else {
                        double dv = v;
                        if (dv == 0.0) dv = 0.0; // P0 fix: 规范化 -0.0 → +0.0
                        uint64_t bits;
                        std::memcpy(&bits, &dv, sizeof(double));
                        return kDoubleTag ^ std::hash<uint64_t>{}(bits);
                    }
                },
                k);
        }
    };

    /// R97 #2 fix: DictKey 的透明相等比较器，支持 unordered_map::find<string_view> 等异构查找。
    /// 比较时先检查 variant index（类型必须匹配），避免 bool(true) 与 int(1) 误判相等。
    struct DictKeyEqual {
        using is_transparent = void; // 启用 C++20 透明查找

        bool operator()(const DictKey& a, const DictKey& b) const { return a == b; }
        bool operator()(const DictKey& a, const std::string_view& b) const {
            return std::holds_alternative<std::string>(a) && std::get<std::string>(a) == b;
        }
        bool operator()(const DictKey& a, const std::string& b) const {
            return std::holds_alternative<std::string>(a) && std::get<std::string>(a) == b;
        }
        bool operator()(const DictKey& a, int64_t b) const {
            return std::holds_alternative<int64_t>(a) && std::get<int64_t>(a) == b;
        }
        bool operator()(const DictKey& a, bool b) const {
            return std::holds_alternative<bool>(a) && std::get<bool>(a) == b;
        }
        bool operator()(const DictKey& a, double b) const {
            return std::holds_alternative<double>(a) && std::get<double>(a) == b;
        }
    };

    /// R97 #2 fix: DictMap typedef 简化 unordered_map 类型引用（19 处统一引用）。
    using DictMap = std::unordered_map<DictKey, Value, DictKeyHash, DictKeyEqual>;

    /// DictKey → Value 转换（用于 dict.keys() 返回键数组）。
    static Value dictKeyToValue(const DictKey& k) {
        return std::visit(
            [&](const auto& v) -> Value {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return Value(v);
                else if constexpr (std::is_same_v<T, int64_t>)
                    return Value(v);
                else if constexpr (std::is_same_v<T, bool>)
                    return Value(v);
                else
                    return Value(v);
            },
            k);
    }

    /// Value → DictKey 转换（用于字典访问/赋值时的键构造）。
    /// 非 string/int/bool/float 类型返回 nullopt，调用方应报错。
    static std::optional<DictKey> dictKeyFromValue(const Value& v) {
        if (v.isString())
            return DictKey{v.stringVal()};
        if (v.isInt())
            return DictKey{v.intVal()};
        if (v.isBool())
            return DictKey{v.boolVal()};
        if (v.isFloat())
            return DictKey{v.floatVal()};
        return std::nullopt;
    }

    /// DictKey → string 转换（用于 toString/打印/Formatter 兼容旧接口）。
    /// 注意：int/bool/float 键 stringify 后可能与 string 键碰撞（如 d[1] 与 d["1"]），
    /// 但仅用于显示，不用于查找。
    /// R97 #5 fix: 用 std::to_chars 替代 std::to_string/std::ostringstream（与
    /// Value::toStringImpl 的 VAL_INT/VAL_FLOAT 路径对齐，避免 locale 查询与堆分配）。
    static std::string dictKeyToString(const DictKey& k) {
        return std::visit(
            [&](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return v;
                else if constexpr (std::is_same_v<T, int64_t>) {
                    char buf[32];
                    auto res = std::to_chars(buf, buf + sizeof(buf), v);
                    return std::string(buf, res.ptr);
                } else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else {
                    // 对齐 Value::toStringImpl 的 VAL_FLOAT 格式化逻辑（std::to_chars general 17）
                    if (std::isnan(v))
                        return "nan";
                    if (std::isinf(v))
                        return v > 0 ? "inf" : "-inf";
                    char buf[64];
                    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 17);
                    if (res.ec != std::errc{})
                        return "nan";
                    return std::string(buf, res.ptr);
                }
            },
            k);
    }

private:
    struct DictData : RefCounted {
        DictMap entries;
        DictData() : RefCounted(ValueType::VAL_DICT) { GcManager::instance().registerTracked(this); }
    };

    struct InstanceData : RefCounted {
        std::string className;
        std::unordered_map<std::string, Value> fields;
        InstanceData() : RefCounted(ValueType::VAL_INSTANCE) { GcManager::instance().registerTracked(this); }
        explicit InstanceData(const std::string& cn) : RefCounted(ValueType::VAL_INSTANCE), className(cn) {
            GcManager::instance().registerTracked(this);
        }
    };

    struct ClosureData : RefCounted {
        std::string name;
        std::weak_ptr<Environment> env; // V4 fix: weak_ptr 打破闭包→环境→闭包的循环引用
        std::vector<std::string> params;
        std::unordered_map<std::string, Value> capturedVars;
        // A3 fix: shared_ptr 所有权，避免 AST 重建后 funRegistry_/methods 持有的裸指针悬垂。
        std::shared_ptr<FunDecl> body;
        std::shared_ptr<VMClosureData> vmClosure; // VM-05/06: VM 闭包数据
        // L3 fix（2026-07-19）: 全局唯一函数标识符，用于闭包相等性判断。
        // 每次 makeClosure 调用分配新 functionId；COW 拷贝（拷贝构造）保留同一 functionId，
        // 因此同一闭包值的拷贝判等，独立 makeClosure 创建的闭包不判等（即使 name/env 相同）。
        // 替代原"name + env"判等契约，解决"同名同环境但函数体不同的闭包误判相等"的限制。
        uint64_t functionId = 0;

        ClosureData() : RefCounted(ValueType::VAL_CLOSURE) {}
        ClosureData(const std::string& n, std::shared_ptr<Environment> e, const std::vector<std::string>& p,
                    std::shared_ptr<FunDecl> b, uint64_t fid)
            : RefCounted(ValueType::VAL_CLOSURE), name(n), env(e), params(p), body(std::move(b)), functionId(fid) {}
    };

    // PERF-12: 超出 int48 范围的 int64 装箱到堆上
    struct BoxedIntData : RefCounted {
        int64_t value;
        explicit BoxedIntData(int64_t v) : RefCounted(ValueType::VAL_INT), value(v) {}
    };

    // R98 元组与解构：immutable 元组数据载体
    // 元组与数组的区别：immutable（无 tryGetMutableTuple）、固定长度、
    // 支持解构绑定 var (a, b) = tupleExpr、支持位置索引访问 t.0/t.1。
    // 仍注册到 GcManager 以支持循环引用检测（虽然 immutable 无法形成自引用，
    // 但元组元素可能持有可变容器，间接形成环——注册确保 markPhase 能进入元素）。
    struct TupleData : RefCounted {
        std::vector<Value> elements;
        TupleData() : RefCounted(ValueType::VAL_TUPLE) { GcManager::instance().registerTracked(this); }
        explicit TupleData(std::vector<Value> v) : RefCounted(ValueType::VAL_TUPLE), elements(std::move(v)) {
            GcManager::instance().registerTracked(this);
        }
    };

    // R99 枚举与 ADT：immutable 枚举 variant 数据载体
    // 设计：enumName + variantName + fields。immutable（无 tryGetMutable）。
    // 泛型类型参数在运行时擦除（与 ClosureData functionId 同样的"类型擦除"哲学），
    // 类型注解校验由 TypeChecker/Interpreter::typeMatch 在构造时完成。
    // 注册到 GcManager 以支持 fields 中可能持有的容器形成间接环。
    struct EnumVariantData : RefCounted {
        std::string enumName;      // enum 类型名（如 "Option"/"Color"/"Shape"）
        std::string variantName;   // variant 名（如 "Some"/"Red"/"Circle"）
        std::vector<Value> fields; // ADT 参数（空 = 简单枚举 variant）
        EnumVariantData() : RefCounted(ValueType::VAL_ENUM_VARIANT) { GcManager::instance().registerTracked(this); }
        EnumVariantData(const std::string& en, const std::string& vn, std::vector<Value> f)
            : RefCounted(ValueType::VAL_ENUM_VARIANT), enumName(en), variantName(vn), fields(std::move(f)) {
            GcManager::instance().registerTracked(this);
        }
    };

    // ============================================================
    // R136 线程与并发原语：四种同步对象数据载体
    // ------------------------------------------------------------
    // 设计要点：
    //   - 所有同步对象使用 shared_ptr<Inner> 共享底层资源（mutex/cv/queue/thread）
    //   - Value 拷贝（addRef）时 shared_ptr 共享同一 Inner，多线程访问安全
    //   - 不注册 GcManager：同步对象不形成循环引用（不持有指向自身的 Value）
    //   - 不参与 COW：tryGetMutableXXX 不提供，所有变异通过 Inner->mu 保护
    //   - ensureUnique 不调用：sync 类型无 tryGetMutable，COW 路径不会触发
    //   - cloneImpl 共享同一 Inner（深拷贝语义：同一通道/锁/线程，而非独立副本）
    //
    // 注：ChannelInner/ThreadInner 必须前向声明为自由结构体并定义在 Value 之后，
    // 因 std::queue<Value> 与 std::thread 等成员需要 Value 完整类型，
    // 而 Value 在自身定义内是不完整类型。
    // ============================================================

    // 前向声明（完整定义在 Value 结构体之后）
    struct ChannelInner;
    struct ThreadInner;

    // 阻塞式消息通道：send 阻塞直到 recv，recv 阻塞直到 send或 close
    struct ChannelData : RefCounted {
        std::shared_ptr<ChannelInner> inner;
        ChannelData(); // 定义在 Value 之后
    };

    // 互斥锁：lock 阻塞直到获取，unlock 释放，tryLock 非阻塞尝试
    struct MutexData : RefCounted {
        std::shared_ptr<std::mutex> inner;
        MutexData() : RefCounted(ValueType::VAL_MUTEX), inner(std::make_shared<std::mutex>()) {}
    };

    // 读写锁：readLock 共享获取，writeLock 独占获取，对应 unlock 释放
    struct RwLockData : RefCounted {
        std::shared_ptr<std::shared_mutex> inner;
        RwLockData() : RefCounted(ValueType::VAL_RWLOCK), inner(std::make_shared<std::shared_mutex>()) {}
    };

    // 线程句柄：spawn 返回，join 等待结束，detach 分离
    struct ThreadData : RefCounted {
        std::shared_ptr<ThreadInner> inner;
        ThreadData(); // 定义在 Value 之后
    };

    // R164 协程/生成器：协程数据载体（重放模式）
    // 持有生成器函数声明 + 定义时环境 + 调用参数，通过 .next() 恢复执行。
    // Interpreter 重放模式：每次 .next() 从头执行函数体，用 yieldId 计数器
    // 跳过已返回的 yield，到达目标 yieldId 时求值 value 并中断返回。
    // currentYieldId 表示下一个要返回的 yield 编号（0-based）。
    // done 标志表示所有 yield 已耗尽（currentYieldId >= yieldCount）。
    // currentValueBox 缓存最近一次 .next() 返回的值（供 .current() 查询）。
    // R164 fix: 用 std::vector<Value> 单元素容器绕开 Value 不完整类型问题——
    // CoroutineData 嵌套在 Value 类内部，此时 Value 尚未定义完成，直接声明
    // `Value currentValue;` 值成员会触发 C2079（使用未定义的 struct Value）。
    // std::vector<Value> 支持不完整类型（与同结构 args 字段同模式），且 cloneImpl
    // 赋值时 vector::operator= 会调用 Value::operator= 正确处理堆类型引用计数。
    // 未来读取时用 currentValueBox.empty() ? Value::nullValue() : currentValueBox.front()。
    struct CoroutineData : RefCounted {
        std::shared_ptr<FunDecl> generatorDecl;  // 生成器函数 AST
        std::shared_ptr<Environment> closureEnv; // 定义时环境（闭包捕获）
        std::vector<Value> args;                 // 调用参数
        int currentYieldId = 0;                  // 下一个要返回的 yieldId
        int yieldCount = 0;                      // 总 yield 数（从 FunDecl.yieldCount 复制）
        bool done = false;                       // 是否已耗尽
        std::vector<Value> currentValueBox;      // 最近一次 yield 的值（单元素容器）
        // R164 D.5: VM 路径字段（非拥有，functionChunks_ 拥有 BytecodeChunk 生命周期）
        const BytecodeChunk* vmChunk = nullptr;  // 生成器字节码块指针
        // R164 fixup: vmClosure 改为单元素容器绕开 C2079（CoroutineData 嵌套在 Value 类内部时
        // Value 尚未定义完成，直接声明 Value vmClosure; 触发 C2079，与 currentValueBox 同模式）
        std::vector<Value> vmClosureBox;           // 闭包值（含 upvalue 绑定，单元素容器）
        // R164 D.6: RegisterVM 路径字段（非拥有，functionChunks_ 拥有 RegBytecodeChunk 生命周期）
        // 与 vmChunk 互斥使用：vmChunk 用于 StackVM/StackVM-IR 路径，regChunk 用于 RegisterVM 路径。
        const RegBytecodeChunk* regChunk = nullptr;
        CoroutineData() : RefCounted(ValueType::VAL_COROUTINE) {}
    };

    // ---- 唯一存储成员：8 字节 NaNBox ----
    NaNBox box_;

    // ---- COW detach — 写入前确保独占所有权 ----
    template <typename T> T* ensureUnique() {
        T* ptr = box_.asPtr<T>();
        if (!ptr->isUnique()) {
            // 引用计数 > 1：深拷贝一份新数据
            // P0 fix: 用 unique_ptr 包裹 cloned，防止 new T(*ptr) 抛 bad_alloc 时
            // 部分已克隆的子树泄漏（如 ArrayData/DictData/InstanceData 的 cloneImpl
            // 递归克隆嵌套 Value，中途抛异常会泄漏已分配的子对象）。
            auto cloned = std::make_unique<T>(*ptr); // 拷贝构造（RefCounted 拷贝 ctor 重置 refCount=1）
            // AUDIT-BUG-C1 fix: 拷贝构造不会调用 GcManager::registerTracked（仅显式构造函数调用）。
            // COW 克隆的容器必须注册到 GcManager，否则循环引用（如 b.push(b) 后 COW detach）
            // 不会被 collectCycle 回收，导致永久内存泄漏。
            // AUDIT-P2-CORRECT fix: 在 release 旧引用前注册到 GcManager，
            // 若 registerTracked 抛 bad_alloc，unique_ptr 自动 delete cloned，
            // 旧引用计数不变（ptr->release() 未执行），仍由调用方持有，无双重释放风险。
            // 原实现先 release/transfer 所有权再 registerTracked，若 registerTracked 抛异常，
            // raw 泄漏（已脱离 unique_ptr）且 box_ 仍指向旧 ptr（release 已执行→双重释放）。
            if constexpr (std::is_same_v<T, ArrayData> || std::is_same_v<T, DictData> ||
                          std::is_same_v<T, InstanceData>) {
                GcManager::instance().registerTracked(cloned.get());
            }
            ptr->release(); // 释放旧引用
            T* raw = cloned.release();
            box_ = NaNBox::fromPtr(static_cast<const void*>(raw));
            return raw;
        }
        return ptr;
    }

    // ---- 从堆指针构造（内部工厂，接管所有权）----
    static Value fromHeapPtr(RefCounted* ptr) {
        Value v;
        v.box_ = NaNBox::fromPtr(static_cast<const void*>(ptr));
        return v;
    }

public:
    // ---- 构造 / 拷贝 / 移动 ----

    Value() : box_(NaNBox::null()) {}

    // 整型构造：int48 范围内内联，超范围装箱
    explicit Value(int64_t v) {
        if (NaNBox::canEncodeInt(v)) {
            box_ = NaNBox::fromInt(v);
        } else {
            // PERF-12: 超出 int48 范围，堆分配 BoxedIntData
            box_ = NaNBox::fromPtr(static_cast<const void*>(new BoxedIntData(v)));
        }
    }
    explicit Value(int v) : Value(static_cast<int64_t>(v)) {}

    // 浮点构造
    explicit Value(double v) : box_(NaNBox::fromFloat(v)) {}

    // 布尔构造
    explicit Value(bool v) : box_(NaNBox::fromBool(v)) {}

    // 字符串构造
    explicit Value(const std::string& v) : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(v)))) {}
    explicit Value(std::string&& v) : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(std::move(v))))) {}
    explicit Value(const char* v)
        // AUDIT-BUG-I5 fix: nullptr 防御——std::string(nullptr) 是 UB
        : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(v ? std::string(v) : std::string())))) {}

    // 数组构造
    explicit Value(const std::vector<Value>& v) : box_(NaNBox::fromPtr(static_cast<const void*>(new ArrayData(v)))) {}
    explicit Value(std::vector<Value>&& v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new ArrayData(std::move(v))))) {}

    // 字典构造（旧接口：string 键，向后兼容）
    explicit Value(const std::unordered_map<std::string, Value>& v) {
        auto* d = new DictData();
        for (const auto& kv : v)
            d->entries.emplace(DictKey{kv.first}, kv.second);
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }
    explicit Value(std::unordered_map<std::string, Value>&& v) {
        auto* d = new DictData();
        for (auto& kv : v)
            d->entries.emplace(DictKey{std::move(kv.first)}, std::move(kv.second));
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }
    // L4 fix: 新接口——DictKey 键字典构造
    // R97 #2 fix: 类型从 unordered_map<DictKey,Value,DictKeyHash> 改为 DictMap（含 DictKeyEqual）
    explicit Value(const DictMap& v) {
        auto* d = new DictData();
        d->entries = v;
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }
    explicit Value(DictMap&& v) {
        auto* d = new DictData();
        d->entries = std::move(v);
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }

    // PERF-12: 手动引用计数管理
    Value(const Value& other) : box_(other.box_) {
        if (box_.isPointer()) {
            box_.asPtr<RefCounted>()->addRef();
        }
    }

    Value& operator=(const Value& other) {
        if (this != &other) {
            // 先固化 other 的位并 addRef，再 release 旧值——避免"自子对象赋值"
            // （如 a = a.arrayVal()[i]）时 release 旧容器导致 other 悬垂的 UAF。
            NaNBox saved = other.box_;
            if (saved.isPointer()) {
                saved.asPtr<RefCounted>()->addRef();
            }
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = saved;
        }
        return *this;
    }

    Value(Value&& other) noexcept : box_(other.box_) { other.box_ = NaNBox::null(); }

    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            // 先把 other 抽空，再 release 旧值——避免 other 别名到 *this 拥有的
            // 容器子元素时，release 触发容器析构使 other 悬垂。
            NaNBox saved = other.box_;
            other.box_ = NaNBox::null();
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = saved;
        }
        return *this;
    }

    ~Value() {
        if (box_.isPointer()) {
            box_.asPtr<RefCounted>()->release();
        }
    }

    /// 显式深拷贝（需要完全独立副本时使用）
    /// 注意：环形容器结构（如 a.append(a)）会被检测到并打断（back-edge
    /// 指向浅拷贝引用），避免无限递归栈溢出。线性深嵌套超过
    /// MAX_CLONE_DEPTH 也会终止并返回浅拷贝。
    Value clone() const {
        if (!box_.isPointer()) {
            // 标量：直接拷贝 NaNBox
            Value result;
            result.box_ = box_;
            return result;
        }
        thread_local std::unordered_map<const void*, Value> tlsCloned;
        tlsCloned.clear();
        // Bug #42 fix: 异常时清理 thread_local 缓存，防止泄漏。
        try {
            Value result = cloneImpl(0, tlsCloned);
            tlsCloned.clear(); // 释放临时引用
            return result;
        } catch (...) {
            tlsCloned.clear();
            throw;
        }
    }

    // clone 递归深度上限（对齐 equals/toString 的 MAX_*_DEPTH）
    static constexpr int MAX_CLONE_DEPTH = RuntimeLimits::MAX_CLONE_DEPTH;

    // ---- R143 JIT 辅助：raw bits 借用语义 ----
    // Value 不是 POD（有用户定义的拷贝/析构管理引用计数），不能直接 memcpy。
    // 这对方法用于 JIT 后端与 raw bits 之间安全转换：
    //   - fromBitsBorrowed：从 raw bits 构造 Value，对指针类型 addRef，
    //     使新 Value 在 RAII 下正确管理引用计数（析构会 release）。
    //     调用方需保证 bits 在调用期间对应对象存活（即 bits 来源的 Value 未被析构）。
    //   - bitsOf：提取 Value 的 raw bits，不修改引用计数。
    //     原 Value 仍正常管理自己的引用计数，调用方拿到的 bits 可后续用
    //     fromBitsBorrowed 恢复为 Value。
    // 标量类型（INT/FLOAT/BOOL/NULL）零开销：isPointer() 为 false 时跳过 addRef。
    static Value fromBitsBorrowed(uint64_t bits) {
        Value v;
        v.box_ = NaNBox::fromBits(bits);
        if (v.box_.isPointer()) {
            v.box_.asPtr<RefCounted>()->addRef();
        }
        return v;
    }

    static uint64_t bitsOf(const Value& v) { return v.box_.rawBits(); }

    /// 转移所有权（R143 JIT 辅助）：返回内部 raw bits 并将本 Value 置为 null，
    /// 避免析构时 release 释放堆对象。调用方拿到的 raw bits 成为唯一引用，
    /// 必须通过 fromBitsBorrowed 恢复为 Value（或最终释放）以避免泄漏。
    /// 用于 JIT 辅助函数返回字符串/数组等堆类型的 raw bits。
    static uint64_t detach(Value& v) {
        uint64_t bits = v.box_.rawBits();
        v.box_ = NaNBox::null();
        return bits;
    }

private:
    /// 递归深拷贝实现。
    /// cloned 映射：旧 RefCounted* → 已克隆的新 Value，用于打断环形 back-edge
    /// 和避免 DAG 重复克隆。遇到已克隆节点直接返回新引用（addRef）。
    Value cloneImpl(int depth, std::unordered_map<const void*, Value>& cloned) const {
        if (!box_.isPointer()) {
            Value result;
            result.box_ = box_;
            return result;
        }
        auto* rc = box_.asPtr<RefCounted>();
        // 深度保护：超限时返回浅拷贝（保留原引用），避免栈溢出
        if (depth >= MAX_CLONE_DEPTH) {
            return *this; // 拷贝构造 + addRef
        }
        // 环检测：遇到已克隆节点直接复用（addRef 后返回）
        auto it = cloned.find(rc);
        if (it != cloned.end()) {
            return it->second; // 拷贝构造 + addRef
        }

        switch (rc->type) {
        case ValueType::VAL_INT: {
            auto* p = static_cast<BoxedIntData*>(rc);
            return fromHeapPtr(new BoxedIntData(p->value));
        }
        case ValueType::VAL_STRING: {
            auto* p = static_cast<StringData*>(rc);
            return fromHeapPtr(new StringData(p->value));
        }
        case ValueType::VAL_ARRAY: {
            auto* p = static_cast<ArrayData*>(rc);
            auto* newPtr = new ArrayData();
            Value result = fromHeapPtr(newPtr);
            // 先登记到 cloned，再递归克隆元素——这样环 back-edge
            // 在递归遇到此节点时能从 cloned 取到新引用。
            cloned.emplace(rc, result);
            newPtr->elements.reserve(p->elements.size());
            for (const auto& elem : p->elements) {
                newPtr->elements.push_back(elem.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_DICT: {
            auto* p = static_cast<DictData*>(rc);
            auto* newPtr = new DictData();
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->entries.reserve(p->entries.size());
            for (const auto& kv : p->entries) {
                newPtr->entries.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_INSTANCE: {
            auto* p = static_cast<InstanceData*>(rc);
            auto* newPtr = new InstanceData(p->className);
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->fields.reserve(p->fields.size());
            for (const auto& kv : p->fields) {
                newPtr->fields.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_CLOSURE: {
            auto* p = static_cast<ClosureData*>(rc);
            auto* newPtr = new ClosureData(*p); // 浅拷贝 env/params/body/vmClosure
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->capturedVars.clear();
            for (const auto& kv : p->capturedVars) {
                newPtr->capturedVars.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_TUPLE: {
            // R98 元组与解构：immutable 元组深拷贝
            auto* p = static_cast<TupleData*>(rc);
            auto* newPtr = new TupleData();
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->elements.reserve(p->elements.size());
            for (const auto& elem : p->elements) {
                newPtr->elements.push_back(elem.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_ENUM_VARIANT: {
            // R99 枚举与 ADT：immutable enum variant 深拷贝
            auto* p = static_cast<EnumVariantData*>(rc);
            auto* newPtr = new EnumVariantData();
            newPtr->enumName = p->enumName;
            newPtr->variantName = p->variantName;
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->fields.reserve(p->fields.size());
            for (const auto& f : p->fields) {
                newPtr->fields.push_back(f.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        // R136 同步对象深拷贝：共享同一底层 Inner（同一通道/锁/线程，而非独立副本）
        // 语义：深拷贝 channel 后，原 channel 与拷贝仍是同一通道（send/recv 互通）。
        // 这与 array/dict 的深拷贝语义不同，但符合并发原语的预期行为。
        case ValueType::VAL_CHANNEL: {
            auto* p = static_cast<ChannelData*>(rc);
            auto* newPtr = new ChannelData();
            newPtr->inner = p->inner; // shared_ptr 共享
            return fromHeapPtr(newPtr);
        }
        case ValueType::VAL_MUTEX: {
            auto* p = static_cast<MutexData*>(rc);
            auto* newPtr = new MutexData();
            newPtr->inner = p->inner;
            return fromHeapPtr(newPtr);
        }
        case ValueType::VAL_RWLOCK: {
            auto* p = static_cast<RwLockData*>(rc);
            auto* newPtr = new RwLockData();
            newPtr->inner = p->inner;
            return fromHeapPtr(newPtr);
        }
        case ValueType::VAL_THREAD: {
            auto* p = static_cast<ThreadData*>(rc);
            auto* newPtr = new ThreadData();
            newPtr->inner = p->inner;
            return fromHeapPtr(newPtr);
        }
        // R164 协程/生成器：深拷贝共享同一协程状态（与同步对象语义一致：
        // 拷贝协程值后，原值与拷贝指向同一迭代器状态，.next() 在任一上调用
        // 都推进同一 currentYieldId。这与 array/dict 的独立副本语义不同，
        // 但符合迭代器的预期行为——迭代器是单消费者，拷贝的是引用而非状态快照）。
        case ValueType::VAL_COROUTINE: {
            auto* p = static_cast<CoroutineData*>(rc);
            auto* newPtr = new CoroutineData();
            newPtr->generatorDecl = p->generatorDecl;
            newPtr->closureEnv = p->closureEnv;
            newPtr->args = p->args;
            newPtr->currentYieldId = p->currentYieldId;
            newPtr->yieldCount = p->yieldCount;
            newPtr->done = p->done;
            newPtr->currentValueBox = p->currentValueBox; // vector<Value> 拷贝（每个 Value addRef）
            // R164 D.5: VM 路径字段（非拥有指针共享 + 闭包值 addRef）
            newPtr->vmChunk = p->vmChunk;
            newPtr->vmClosureBox = p->vmClosureBox; // R164 fixup: vector<Value> 拷贝
            // R164 D.6: RegisterVM 路径字段（非拥有指针共享）
            newPtr->regChunk = p->regChunk;
            return fromHeapPtr(newPtr);
        }
        default:
            return Value(); // null
        }
    }

public:
    // ---- 静态工厂方法 ----

    /// 返回 null 值（等价于默认构造的 Value，NaNBox 编码为 NULL_BITS）。
    static Value nullValue() { return Value(); }

    /// 构造一个空实例值，className 标识其所属类。字段表初始为空，
    /// 由解释器/VM 在构造后填充。用于 class 实例化表达式的结果。
    static Value makeInstance(const std::string& clsName) { return fromHeapPtr(new InstanceData(clsName)); }

    /// 构造闭包值，捕获 name/env/params/body。
    /// 注意 env 以 shared_ptr 传入，但 ClosureData 内部存为 weak_ptr<Environment>
    /// （V4 fix），用于打破 闭包→环境→闭包 的循环引用，避免引用计数泄漏。
    /// L3 fix: 每次调用分配全局唯一 functionId，用于闭包相等性判断。
    static Value makeClosure(const std::string& name, std::shared_ptr<Environment> env,
                             const std::vector<std::string>& params, std::shared_ptr<FunDecl> body = nullptr) {
        return fromHeapPtr(new ClosureData(name, env, params, std::move(body), nextFunctionId()));
    }

    /// L3 fix: 全局单调递增的 functionId 生成器（atomic 线程安全）。
    /// 每次 makeClosure 调用消耗一个 ID，保证独立创建的闭包永不判等。
    static uint64_t nextFunctionId() {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    /// R98 元组与解构：构造 immutable 元组值。
    /// 元组元素按位置存储，不可变；支持解构绑定 var (a,b) = t 与位置索引 t.0/t.1。
    static Value makeTuple(std::vector<Value> elems) { return fromHeapPtr(new TupleData(std::move(elems))); }

    /// R99 枚举与 ADT：构造 immutable 枚举 variant 值。
    /// enumName/variantName 标识所属 enum 与具体 variant，fields 为 ADT 参数（空 = 简单枚举）。
    static Value makeEnumVariant(const std::string& enumName, const std::string& variantName,
                                 std::vector<Value> fields) {
        return fromHeapPtr(new EnumVariantData(enumName, variantName, std::move(fields)));
    }

    /// R136 线程与并发原语：构造同步对象值（channel/mutex/rwlock/thread）
    /// 均使用 shared_ptr<Inner> 共享底层资源，Value 拷贝时共享同一底层。
    static Value makeChannel() { return fromHeapPtr(new ChannelData()); }
    static Value makeMutex() { return fromHeapPtr(new MutexData()); }
    static Value makeRwLock() { return fromHeapPtr(new RwLockData()); }
    static Value makeThread() { return fromHeapPtr(new ThreadData()); }

    /// R164 协程/生成器：构造协程值（生成器函数调用返回）。
    /// generatorDecl 为 fun* 声明的 AST，closureEnv 为定义时环境，
    /// args 为调用参数，yieldCount 从 FunDecl.yieldCount 复制。
    static Value makeCoroutine(std::shared_ptr<FunDecl> generatorDecl, std::shared_ptr<Environment> closureEnv,
                               std::vector<Value> args, int yieldCount) {
        auto* cd = new CoroutineData();
        cd->generatorDecl = std::move(generatorDecl);
        cd->closureEnv = std::move(closureEnv);
        cd->args = std::move(args);
        cd->yieldCount = yieldCount;
        return fromHeapPtr(cd);
    }

    /// R164 D.5: VM 路径协程构造（不依赖 AST/Environment）。
    /// vmChunk 指向 functionChunks_ 中的 BytecodeChunk（非拥有，VM 生命周期内有效），
    /// vmClosure 携带 upvalue 绑定（可为空 Value），yieldCount 从 BytecodeChunk.yieldCount 复制。
    static Value makeCoroutineVM(const BytecodeChunk* vmChunk, Value vmClosure, std::vector<Value> args,
                                 int yieldCount) {
        auto* cd = new CoroutineData();
        cd->vmChunk = vmChunk;
        cd->vmClosureBox = {std::move(vmClosure)}; // R164 fixup: 单元素容器赋值
        cd->args = std::move(args);
        cd->yieldCount = yieldCount;
        return fromHeapPtr(cd);
    }

    /// R164 D.6: RegisterVM 路径协程构造（与 makeCoroutineVM 对称，仅 chunk 类型不同）。
    /// regChunk 指向 functionChunks_ 中的 RegBytecodeChunk（非拥有，VM 生命周期内有效），
    /// vmClosure 携带 upvalue 绑定（可为空 Value），yieldCount 从 RegBytecodeChunk.yieldCount 复制。
    static Value makeCoroutineRegVM(const RegBytecodeChunk* regChunk, Value vmClosure, std::vector<Value> args,
                                    int yieldCount) {
        auto* cd = new CoroutineData();
        cd->regChunk = regChunk;
        cd->vmClosureBox = {std::move(vmClosure)}; // 单元素容器赋值
        cd->args = std::move(args);
        cd->yieldCount = yieldCount;
        return fromHeapPtr(cd);
    }

    // ---- 类型查询 ----

    ValueType getType() const {
        switch (box_.tag()) {
        case NaNBox::Tag::FLOAT:
            return ValueType::VAL_FLOAT;
        case NaNBox::Tag::INT:
            return ValueType::VAL_INT;
        case NaNBox::Tag::BOOL:
            return ValueType::VAL_BOOL;
        case NaNBox::Tag::NUL:
            return ValueType::VAL_NULL;
        case NaNBox::Tag::POINTER:
            return box_.asPtr<RefCounted>()->type;
        }
        return ValueType::VAL_NULL;
    }

    // 向后兼容：允许读取 .type
    ValueType type() const { return getType(); }

    bool isInt() const {
        if (box_.isInt())
            return true; // 内联 int48
        if (box_.isPointer()) {
            return box_.asPtr<RefCounted>()->type == ValueType::VAL_INT; // 装箱 int64
        }
        return false;
    }
    bool isFloat() const { return box_.isFloat(); }
    bool isBool() const { return box_.isBool(); }
    bool isNull() const { return box_.isNull(); }
    bool isString() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_STRING; }
    bool isArray() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_ARRAY; }
    bool isDict() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_DICT; }
    bool isInstance() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_INSTANCE; }
    bool isClosure() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_CLOSURE; }
    bool isTuple() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_TUPLE; }
    bool isEnumVariant() const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_ENUM_VARIANT;
    }
    // R136 线程与并发原语：同步对象类型查询
    bool isChannel() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_CHANNEL; }
    bool isMutex() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_MUTEX; }
    bool isRwLock() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_RWLOCK; }
    bool isThread() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_THREAD; }
    // R164 协程/生成器
    bool isCoroutine() const { return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_COROUTINE; }
    bool isNumber() const { return isInt() || isFloat(); }

    // ---- 堆指针访问（诊断/可视化用，P4-4: MemoryModelPanel 第 4 子页"实时动画"消费）----
    // 返回 true 当且仅当 NaN-box tag 为 POINTER（即承载堆对象）。
    // 涵盖 StringData/ArrayData/DictData/InstanceData/ClosureData/BoxedIntData。
    // 注意：BoxedIntData 的 type==VAL_INT，但通过 isPointer() 可与内联 int48 区分。
    bool isPointer() const { return box_.isPointer(); }

    // 返回底层 RefCounted* 指针（仅当 isPointer() 为 true 时有效，否则 nullptr）。
    // 调用方可通过 p->type 区分具体堆类型，p->useCount() 读取引用计数。
    // 不增加引用计数：调用方仅用于只读诊断展示，不持有指针所有权。
    RefCounted* asPointer() const { return box_.isPointer() ? box_.asPtr<RefCounted>() : nullptr; }

    // ---- 访问器 ----
    // 标量访问器返回 by value（NaNBox 内联存储，非地址able lvalue）
    // 堆类型访问器返回引用（需 dereference 指针）
    // 非 const 堆类型访问器调用 ensureUnique() 确保 COW 独占

    // -- intVal --
    int64_t intVal() const {
        // AUDIT-NANBOX fix: assert 在 Release 被剥离，改为运行时 abort（与 NaNBox::asXxx 一致）
        if (!isInt()) {
            std::abort();
        }
        if (box_.isInt())
            return box_.asInt();                  // 内联 int48
        return box_.asPtr<BoxedIntData>()->value; // 装箱 int64
    }

    // -- floatVal --
    double floatVal() const {
        if (!isFloat()) {
            std::abort();
        }
        return box_.asFloat();
    }

    // -- boolVal --
    bool boolVal() const {
        if (!isBool()) {
            std::abort();
        }
        return box_.asBool();
    }

    // -- stringVal --
    std::string& stringVal() {
        if (!isString()) {
            std::abort();
        }
        auto* sd = ensureUnique<StringData>();
        sd->cachedCodepointCount.reset(); // perf1 fix: 字符串可能被修改，使码位缓存失效
        sd->cachedIsAscii.reset();        // R97 #3 fix: 字符串可能被修改，使 isAscii 缓存失效
        return sd->value;
    }
    const std::string& stringVal() const {
        if (!isString()) {
            std::abort();
        }
        return box_.asPtr<StringData>()->value;
    }
    // perf1 fix: 返回字符串 UTF-8 码位数（带缓存，const 路径首次计算后复用）
    // O(n) 首次 → O(1) 后续，消除 len()/substr() 循环中的 O(n²) 重复扫描。
    int64_t codepointCount() const {
        if (!isString()) {
            std::abort();
        }
        StringData* sd = box_.asPtr<StringData>();
        if (!sd->cachedCodepointCount.has_value()) {
            sd->cachedCodepointCount = Utf8::codepointCount(sd->value);
        }
        return *sd->cachedCodepointCount;
    }

    /// R97 #3 fix: 返回字符串是否纯 ASCII（所有字节 < 0x80）。
    /// 替代两后端各自维护的 lastAsciiStr* 4 字段缓存。结果缓存在 StringData 中，
    /// 与字符串生命周期绑定，消除 BUG-015 的堆地址复用误命中风险。
    /// 仅对 string 类型调用，否则 std::abort。
    bool isAsciiString() const {
        if (!isString()) {
            std::abort();
        }
        StringData* sd = box_.asPtr<StringData>();
        if (!sd->cachedIsAscii.has_value()) {
            bool ascii = true;
            const std::string& s = sd->value;
            for (size_t i = 0; i < s.size(); ++i) {
                if (static_cast<unsigned char>(s[i]) >= 0x80) {
                    ascii = false;
                    break;
                }
            }
            sd->cachedIsAscii = ascii;
        }
        return *sd->cachedIsAscii;
    }

    // -- arrayVal --
    std::vector<Value>& arrayVal() {
        if (!isArray()) {
            std::abort();
        }
        return ensureUnique<ArrayData>()->elements;
    }
    const std::vector<Value>& arrayVal() const {
        if (!isArray()) {
            std::abort();
        }
        return box_.asPtr<ArrayData>()->elements;
    }

    // -- dictVal --
    // L4 fix: dictVal() 返回类型改为 DictKey 键的 map（原 string 键 map 已废弃）
    DictMap& dictVal() {
        if (!isDict()) {
            std::abort();
        }
        return ensureUnique<DictData>()->entries;
    }
    const DictMap& dictVal() const {
        if (!isDict()) {
            std::abort();
        }
        return box_.asPtr<DictData>()->entries;
    }

    // -- tupleVal --
    // R98 元组与解构：元组元素访问器（仅 const 版本——元组 immutable）
    // 非 const 版本不存在以在编译期阻止变异；写入需求应通过解构后重建实现。
    const std::vector<Value>& tupleVal() const {
        if (!isTuple()) {
            std::abort();
        }
        return box_.asPtr<TupleData>()->elements;
    }

    // -- enum variant 访问器 --
    // R99 枚举与 ADT：enum variant 值访问器（仅 const 版本——immutable）
    // enumVariantEnumName/enumVariantName/enumVariantFields 三个访问器分别返回
    // 所属 enum 名、variant 名、ADT 参数列表。match 表达式通过 variantName 比较
    // 决定匹配哪个 case，通过 fields 取出绑定到 case pattern 的变量。
    const std::string& enumVariantEnumName() const {
        if (!isEnumVariant()) {
            std::abort();
        }
        return box_.asPtr<EnumVariantData>()->enumName;
    }
    const std::string& enumVariantName() const {
        if (!isEnumVariant()) {
            std::abort();
        }
        return box_.asPtr<EnumVariantData>()->variantName;
    }
    const std::vector<Value>& enumVariantFields() const {
        if (!isEnumVariant()) {
            std::abort();
        }
        return box_.asPtr<EnumVariantData>()->fields;
    }

    // ============================================================
    // R136 线程与并发原语：同步对象访问器（仅 const，COW 不适用）
    // ============================================================
    // 同步对象的 inner 指针是 shared_ptr，拷贝 Value 时共享同一底层。
    // 所有变异通过 inner->mu 保护，不需要 ensureUnique detach。
    std::shared_ptr<ChannelInner> channelInner() const {
        if (!isChannel()) {
            std::abort();
        }
        return box_.asPtr<ChannelData>()->inner;
    }
    std::shared_ptr<std::mutex> mutexInner() const {
        if (!isMutex()) {
            std::abort();
        }
        return box_.asPtr<MutexData>()->inner;
    }
    std::shared_ptr<std::shared_mutex> rwlockInner() const {
        if (!isRwLock()) {
            std::abort();
        }
        return box_.asPtr<RwLockData>()->inner;
    }
    std::shared_ptr<ThreadInner> threadInner() const {
        if (!isThread()) {
            std::abort();
        }
        return box_.asPtr<ThreadData>()->inner;
    }

    // R164 协程/生成器：协程数据访问器（仅 const，状态通过 Interpreter 修改）
    // 返回 CoroutineData* 指针，调用方（Interpreter）直接读写其字段。
    // 不使用 ensureUnique/COW：协程是唯一所有权对象（生成器函数调用返回的
    // 协程值不应被复制共享，如需独立迭代器应调用生成器函数创建新协程）。
    // 注意：Value 拷贝（addRef）时多个 Value 共享同一 CoroutineData，
    // 但 .next() 只应在其中一个上调用（语义：迭代器是单消费者）。
    CoroutineData* coroutineData() const {
        if (!isCoroutine()) {
            std::abort();
        }
        return box_.asPtr<CoroutineData>();
    }

    // -- className（实例专用）--
    std::string& className() {
        if (!isInstance()) {
            std::abort();
        }
        return ensureUnique<InstanceData>()->className;
    }
    const std::string& className() const {
        if (!isInstance()) {
            std::abort();
        }
        return box_.asPtr<InstanceData>()->className;
    }

    // -- fields（实例字段）--
    std::unordered_map<std::string, Value>& fields() {
        if (!isInstance()) {
            std::abort();
        }
        return ensureUnique<InstanceData>()->fields;
    }
    const std::unordered_map<std::string, Value>& fields() const {
        if (!isInstance()) {
            std::abort();
        }
        return box_.asPtr<InstanceData>()->fields;
    }

    // -- 闭包字段 --
    std::string& closureName() {
        if (!isClosure()) {
            std::abort();
        }
        return ensureUnique<ClosureData>()->name;
    }
    const std::string& closureName() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->name;
    }

    std::shared_ptr<Environment> closureEnv() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->env.lock();
    }

    // AUDIT-P2-CORRECT fix: 设置闭包的环境 weak_ptr（用于沙箱深拷贝后断开与外层环境的共享）。
    // 通过 ensureUnique<ClosureData> 获取独占引用后修改 env 字段，COW 语义保证不影响其他引用。
    void setClosureEnv(std::shared_ptr<Environment> newEnv) {
        if (!isClosure()) {
            std::abort();
        }
        ensureUnique<ClosureData>()->env = newEnv;
    }

    std::vector<std::string>& closureParams() {
        if (!isClosure()) {
            std::abort();
        }
        return ensureUnique<ClosureData>()->params;
    }
    const std::vector<std::string>& closureParams() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->params;
    }

    FunDecl* closureBody() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->body.get();
    }
    std::shared_ptr<FunDecl> closureBodyShared() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->body;
    }

    std::unordered_map<std::string, Value>& capturedVars() {
        if (!isClosure()) {
            std::abort();
        }
        return ensureUnique<ClosureData>()->capturedVars;
    }
    const std::unordered_map<std::string, Value>& capturedVars() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->capturedVars;
    }

    // VM-05/06: VM 闭包数据访问器
    std::shared_ptr<VMClosureData>& vmClosure() {
        if (!isClosure()) {
            std::abort();
        }
        return ensureUnique<ClosureData>()->vmClosure;
    }
    const std::shared_ptr<VMClosureData>& vmClosure() const {
        if (!isClosure()) {
            std::abort();
        }
        return box_.asPtr<ClosureData>()->vmClosure;
    }

    // ============================================================
    // 原地变异辅助方法 — 跳过 COW detach 当 refCount==1
    // ============================================================

    std::string* tryGetMutableString() {
        if (!isString())
            return nullptr;
        auto* ptr = box_.asPtr<StringData>();
        return ptr->isUnique() ? &ptr->value : nullptr;
    }

    std::vector<Value>* tryGetMutableArray() {
        if (!isArray())
            return nullptr;
        auto* ptr = box_.asPtr<ArrayData>();
        return ptr->isUnique() ? &ptr->elements : nullptr;
    }

    // L4 fix: 返回类型改为 DictKey 键 map 指针
    DictMap* tryGetMutableDict() {
        if (!isDict())
            return nullptr;
        auto* ptr = box_.asPtr<DictData>();
        return ptr->isUnique() ? &ptr->entries : nullptr;
    }

    std::unordered_map<std::string, Value>* tryGetMutableFields() {
        if (!isInstance())
            return nullptr;
        auto* ptr = box_.asPtr<InstanceData>();
        return ptr->isUnique() ? &ptr->fields : nullptr;
    }

    bool isUniquelyOwned() const {
        if (!box_.isPointer())
            return true; // 标量始终独占
        return box_.asPtr<RefCounted>()->isUnique();
    }

    const void* gcRootPtr() const {
        if (isArray())
            return static_cast<const void*>(box_.asPtr<ArrayData>());
        if (isDict())
            return static_cast<const void*>(box_.asPtr<DictData>());
        if (isInstance())
            return static_cast<const void*>(box_.asPtr<InstanceData>());
        // BUG-REPL-AUDIT-7 fix: 闭包也需作为 GC 根——capturedVars 可能持有循环容器
        // （如 a.append(a) 后被闭包捕获）。原实现仅遍历 savedGlobalEnv 顶层变量，
        // 但 gcRootPtr() 对闭包返回 nullptr，导致闭包内部的 capturedVars 不会被 mark，
        // GcManager 误判为不可达循环孤岛并清空其 elements，破坏 REPL 状态。
        // ClosureData 本身不在 tracked_（GcManager.h 注释），不会被 sweep，
        // 但作为根可让 collectCycle 的 markPhase 进入 capturedVars 标记可达容器。
        if (isClosure())
            return static_cast<const void*>(box_.asPtr<ClosureData>());
        // R98 元组与解构：元组作为 GC 根，元素可能持有可变容器形成间接环。
        if (isTuple())
            return static_cast<const void*>(box_.asPtr<TupleData>());
        // R99 枚举与 ADT：enum variant 作为 GC 根，fields 可能持有可变容器形成间接环。
        if (isEnumVariant())
            return static_cast<const void*>(box_.asPtr<EnumVariantData>());
        return nullptr;
    }

    // ============================================================
    // 工具方法
    // ============================================================

    double toDouble() const {
        if (isInt())
            return static_cast<double>(intVal());
        if (isFloat())
            return box_.asFloat();
        return 0.0;
    }

    /// 返回类型的可读字符串名（如 "int"/"float"/"array"/"dict"，实例返回类名）。
    /// 用于错误诊断与 type() 内建函数；实例若类名为空则回落到 "instance"。
    std::string typeName() const {
        switch (getType()) {
        case ValueType::VAL_INT:
            return TypeName::INT;
        case ValueType::VAL_FLOAT:
            return TypeName::FLOAT;
        case ValueType::VAL_BOOL:
            return TypeName::BOOL;
        case ValueType::VAL_STRING:
            return TypeName::STRING;
        case ValueType::VAL_NULL:
            return TypeName::NULL_T;
        case ValueType::VAL_ARRAY:
            return TypeName::ARRAY;
        case ValueType::VAL_DICT:
            return TypeName::DICT;
        case ValueType::VAL_INSTANCE: {
            const auto& cn = className();
            return cn.empty() ? TypeName::INSTANCE : cn;
        }
        case ValueType::VAL_CLOSURE:
            return TypeName::CLOSURE;
        case ValueType::VAL_TUPLE:
            return TypeName::TUPLE;
        case ValueType::VAL_ENUM_VARIANT: {
            // R99 枚举与 ADT：返回 enum 类型名（如 "Color"/"Option"），用于类型注解匹配。
            // 注意：泛型类型参数在运行时擦除，Option<int> 与 Option<string> 都返回 "Option"。
            // 类型注解匹配由 typeMatchValue 处理，对 enum 注解直接比较 enumName。
            return enumVariantEnumName();
        }
        // R136 线程与并发原语
        case ValueType::VAL_CHANNEL:
            return TypeName::CHANNEL;
        case ValueType::VAL_MUTEX:
            return TypeName::MUTEX;
        case ValueType::VAL_RWLOCK:
            return TypeName::RWLOCK;
        case ValueType::VAL_THREAD:
            return TypeName::THREAD;
        // R164 协程/生成器
        case ValueType::VAL_COROUTINE:
            return TypeName::COROUTINE;
        }
        return "unknown";
    }

    // B5 fix: toString 递归深度上限
    static constexpr int MAX_TOSTRING_DEPTH = RuntimeLimits::MAX_TOSTRING_DEPTH;
    // P1-6/7 fix: equals 递归深度上限
    static constexpr int MAX_EQUALS_DEPTH = RuntimeLimits::MAX_EQUALS_DEPTH;

    /// 相等比较
    bool equals(const Value& other) const {
        bool thisScalar = isInt() || isFloat() || isBool() || isString() || isNull();
        bool otherScalar = other.isInt() || other.isFloat() || other.isBool() || other.isString() || other.isNull();
        if (thisScalar || otherScalar) {
            return equalsImpl(other, nullptr, 0);
        }
        thread_local std::unordered_set<const void*> tlsVisited;
        tlsVisited.clear();
        return equalsImpl(other, &tlsVisited, 0);
    }

private:
    bool equalsImpl(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    // ---- R133-B fix: equalsImpl 238 行拆为 thin dispatcher + 7 helper（按 ValueType 分组，
    // 与 R118 StackVM executeContainerOps / R133-A lowerContainerOps 拆分模式同构）----
    bool equalsImplScalar(const Value& other) const; // INT/FLOAT/BOOL/STRING/NULL（无环检测）
    bool equalsImplArray(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    bool equalsImplDict(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    bool equalsImplInstance(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    bool equalsImplTuple(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    bool equalsImplEnumVariant(const Value& other, std::unordered_set<const void*>* visited, int depth) const;
    bool equalsImplLeafObject(const Value& other) const; // CLOSURE/CHANNEL/MUTEX/RWLOCK/THREAD（叶子对象，无环检测）

public:
    /// 将值序列化为字符串表示。
    /// 标量（int/float/bool/string/null）与闭包走快速路径（直接格式化）；
    /// 数组/字典/实例走 toStringImpl，带环检测（visited 集合）与深度保护，
    /// 避免自引用容器（如 a.append(a)）无限递归。字符串元素在容器中加引号以区分。
    std::string toString() const {
        switch (getType()) {
        case ValueType::VAL_INT: {
            // Perf-Finding2: std::to_string 在 MSVC 上触发 locale 查询 + 堆分配，
            // 用 std::to_chars 写入栈缓冲消除 locale 开销（输出字节与 to_string 完全一致）
            char buf[32];
            auto res = std::to_chars(buf, buf + sizeof(buf), intVal());
            return std::string(buf, res.ptr);
        }
        case ValueType::VAL_FLOAT: {
            // AUDIT-BUG-I4 fix: 用 std::to_chars 替代 snprintf——locale-independent，
            // 避免非 "C" locale 下十进制分隔符变为 ',' 导致解析失败。
            // 与 VAL_INT 路径风格一致，使用 general 格式 + 17 位精度（round-trip 保证）。
            char buf[64];
            auto res = std::to_chars(buf, buf + sizeof(buf), box_.asFloat(), std::chars_format::general, 17);
            if (res.ec != std::errc{})
                return "nan";
            return std::string(buf, res.ptr);
        }
        case ValueType::VAL_BOOL:
            return box_.asBool() ? "true" : "false";
        case ValueType::VAL_STRING:
            return stringVal();
        case ValueType::VAL_NULL:
            return "null";
        case ValueType::VAL_CLOSURE:
            return "<fun:" + closureName() + ">";
        // R136 同步对象走快速路径（无需环检测，叶子节点）
        case ValueType::VAL_CHANNEL:
            return "<channel>";
        case ValueType::VAL_MUTEX:
            return "<mutex>";
        case ValueType::VAL_RWLOCK:
            return "<rwlock>";
        case ValueType::VAL_THREAD:
            return "<thread>";
        // R164 协程/生成器走快速路径
        case ValueType::VAL_COROUTINE:
            return "<coroutine>";
        default:
            break;
        }
        // R98 元组与解构：元组走 toStringImpl（带环检测）
        thread_local std::unordered_set<const void*> tlsVisited;
        tlsVisited.clear();
        return toStringImpl(tlsVisited, 0);
    }

private:
    std::string toStringImpl(std::unordered_set<const void*>& visited, int depth) const;
    // ---- R133-C fix: toStringImpl 186 行拆为 thin dispatcher + 7 helper（按 ValueType 分组，
    // 与 R133-B equalsImpl 拆分模式同构；CLOSURE/SYNC 虽走 toString() 快速路径不进入顶层
    // toStringImpl，但容器元素递归会调用 toStringImpl，故仍需 leaf helper 处理）----
    std::string toStringImplScalar() const; // INT/FLOAT/BOOL/STRING/NULL（无环检测）
    std::string toStringImplArray(std::unordered_set<const void*>& visited, int depth) const;
    std::string toStringImplDict(std::unordered_set<const void*>& visited, int depth) const;
    std::string toStringImplInstance(std::unordered_set<const void*>& visited, int depth) const;
    std::string toStringImplTuple(std::unordered_set<const void*>& visited, int depth) const;
    std::string toStringImplEnumVariant(std::unordered_set<const void*>& visited, int depth) const;
    std::string toStringImplLeafObject() const; // CLOSURE/CHANNEL/MUTEX/RWLOCK/THREAD（叶子对象，无环检测）

public:
    /// 将值序列化为 JSON 表示（R114 阶段 2）。
    /// 标量（int/float/bool/string/null）输出 JSON 原生类型；
    /// 容器（array/dict/instance/tuple/enum variant/closure）输出带类型标签的 JSON 对象，
    /// 格式 `{"_type":"<type>","value":...}`，便于跨语言交换与回放器重建。
    /// 带环检测与深度保护（与 toStringImpl 一致），环用 `{"_type":"cycle"}` 表示。
    std::string toJson() const {
        thread_local std::unordered_set<const void*> tlsJsonVisited;
        tlsJsonVisited.clear();
        return toJsonImpl(tlsJsonVisited, 0);
    }

private:
    std::string toJsonImpl(std::unordered_set<const void*>& visited, int depth) const;

public:
    /// 判断真值
    bool isTruthy() const {
        switch (getType()) {
        case ValueType::VAL_INT:
            return intVal() != 0;
        case ValueType::VAL_FLOAT:
            return box_.asFloat() != 0.0;
        case ValueType::VAL_BOOL:
            return box_.asBool();
        case ValueType::VAL_STRING:
            return !stringVal().empty();
        case ValueType::VAL_NULL:
            return false;
        case ValueType::VAL_ARRAY:
            return true;
        case ValueType::VAL_DICT:
            return true;
        case ValueType::VAL_INSTANCE:
            return true;
        case ValueType::VAL_CLOSURE:
            return true;
        case ValueType::VAL_TUPLE:
            return true;
        case ValueType::VAL_ENUM_VARIANT:
            return true;
        // R136 同步对象始终为真（与 closure/instance 一致：对象存在即真）
        case ValueType::VAL_CHANNEL:
            return true;
        case ValueType::VAL_MUTEX:
            return true;
        case ValueType::VAL_RWLOCK:
            return true;
        case ValueType::VAL_THREAD:
            return true;
        // R164 协程/生成器始终为真（对象存在即真）
        case ValueType::VAL_COROUTINE:
            return true;
        }
        return false;
    }
};

// ============================================================
// R136 同步对象 Inner 完整定义（需 Value 完整类型）
// ============================================================

// ChannelInner：阻塞式消息通道的共享状态
struct Value::ChannelInner {
    std::queue<Value> queue;    // 待传递的消息缓冲
    mutable std::mutex mu;      // 保护 queue + closed
    std::condition_variable cv; // 通知 send/recv
    bool closed = false;        // 通道是否已关闭
};

// ThreadInner：线程句柄的共享状态
// R136 fix: 延迟执行模式——spawn 不启动子线程（VM/Interpreter 非线程安全，
// 子线程并发调用 invokeClosureSync 会破坏 frames_/stack_ 等共享状态）。
// 闭包与参数存入 pending 字段，join() 时在主线程同步执行。
// pendingInvoker 使用 shared_ptr<void> 避免 Value.h 依赖 BuiltinMethods.h
// （ClosureInvoker 定义在那里），实际类型为 shared_ptr<ClosureInvoker>。
struct Value::ThreadInner {
    std::thread thr; // 保留用于未来真正异步（当前未使用）
    std::mutex stateMu;
    bool joined = false;
    bool detached = false;
    // 延迟执行字段
    Value pendingClosure;
    std::vector<Value> pendingArgs;
    std::shared_ptr<void> pendingInvoker; // 实际类型 shared_ptr<ClosureInvoker>
    int spawnLine = 0;
    int spawnCol = 0;
    bool hasPending = false;
};

// ChannelData / ThreadData 构造函数定义（需 Inner 完整类型）
inline Value::ChannelData::ChannelData()
    : RefCounted(ValueType::VAL_CHANNEL), inner(std::make_shared<ChannelInner>()) {}
inline Value::ThreadData::ThreadData() : RefCounted(ValueType::VAL_THREAD), inner(std::make_shared<ThreadInner>()) {}

// 编译期断言：Value 必须是 8 字节（NaN-boxing）
static_assert(sizeof(Value) == 8, "Value must be 8 bytes (NaN-boxed)");

// 包含数据载体模块（VMUpvalue 等，依赖完整 Value 类型）
#include "interpreter/ValueData.h"
