#pragma once

// ============================================================
// StringIntern — 字符串驻留池（PERF-05 fix）
// ------------------------------------------------------------
// 用于高频重复字符串（方法标记 "method:Class.field"、类名、字段名等），
// 避免每次 Value(std::string) 构造都分配 shared_ptr<StringData>。
//
// intern() 返回对池中稳定 std::string 的 const 引用，调用方可用
// Value(intern(...)) 构造，仍走 StringData 路径但 string 内容唯一稳定，
// 便于后续 equals 指针比较优化。
//
// 线程安全：mutex 保护 unordered_set。
// ============================================================

#include <mutex>
#include <string>
#include <unordered_set>

class StringIntern {
public:
    /// 驻留字符串：若池中已存在等价字符串，返回其引用；否则插入并返回。
    /// 返回的 const 引用在程序生命周期内稳定（池不删除条目）。
    static const std::string& intern(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex_());
        auto [it, inserted] = pool_().emplace(s);
        return *it;
    }

    /// 拼接后驻留（避免临时 string 构造后再 emplace 的二次拷贝）。
    /// C++17 起 unordered_set 支持 transparent emplace，但仍需先构造。
    /// 此重载用于 method marker 等拼接场景。
    static const std::string& internConcat(const std::string& a, const std::string& b) {
        std::string combined;
        combined.reserve(a.size() + b.size());
        combined += a;
        combined += b;
        return intern(combined);
    }

    /// 清空驻留池（仅在测试间调用，释放累积的字符串内存）。
    /// 注意：清空后之前 intern() 返回的引用全部失效，仅在所有 Interpreter/VM
    /// 实例已析构时调用（测试 fixture 的 OnTestEnd 保证此条件）。
    static void clear() {
        std::lock_guard<std::mutex> lock(mutex_());
        pool_().clear();
    }

private:
    // Meyers singleton，避免静态初始化顺序问题
    static std::mutex& mutex_() {
        static std::mutex m;
        return m;
    }
    static std::unordered_set<std::string>& pool_() {
        static std::unordered_set<std::string> p;
        return p;
    }
};
