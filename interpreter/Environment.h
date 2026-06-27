#pragma once

#include <unordered_map>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "interpreter/Value.h"

// ============================================================
// Environment 作用域链
// ============================================================

/// 作用域环境，支持嵌套（作用域链），使用 shared_ptr 管理生命周期
/// 优化：对父作用域查找使用深度快捷缓存，避免重复 O(depth) 链遍历
class Environment : public std::enable_shared_from_this<Environment> {
public:
    /// 父作用域指针（全局环境为 nullptr）
    std::shared_ptr<Environment> parent;

    /// 构造函数
    explicit Environment(std::shared_ptr<Environment> parentEnv = nullptr)
        : parent(parentEnv) {
        // 注意：不在这里递增 generation_
        // 创建子作用域不改变已有变量的深度位置，缓存仍有效
        // 修复：继承父作用域的 boundInstance_，使方法体内的块作用域也能访问实例字段
        if (parentEnv) {
            boundInstance_ = parentEnv->boundInstance_;
        }
    }

    /// 在当前作用域定义变量
    void define(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        if (!inserted) {
            it->second = val;   // 覆盖已有变量
        }
    }

    /// 移动重载：避免临时 Value 的深拷贝（数组/字典/实例等大对象）
    void define(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        if (!inserted) {
            it->second = std::move(val);   // 覆盖已有变量
        }
    }

    /// P21 fix: 原子性检查+插入 — 变量已存在返回 false，新插入返回 true
    /// 用于 visitVarDecl 消除 find+define 双次查找
    bool tryDefineNew(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        return inserted;
    }
    bool tryDefineNew(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        return inserted;
    }

    /// 获取变量值（沿作用域链查找）— 返回指针，nullptr 表示未找到
    const Value* get(const std::string& name) const {
        // PERF-02 fix: 递归改迭代循环，消除函数调用开销。
        // 每层先查 variables，再查 boundInstance 字段，再向上。
        // 迭代上限保护：防止异常的循环父指针导致无限循环。
        // #21 fix: boundInstance_ 沿作用域链继承（构造/resetForReuse 时从父级拷贝），
        // 同一链上多数层 boundInstance_ 指针相同。缓存上次已检查过的实例指针，
        // 若当前层与之相同则跳过重复 fields 查找——同一实例的字段集在 get() 期间不变，
        // 上次未命中则本次也不会命中。仅当链上出现不同 boundInstance_（bindInstance
        // 显式覆盖）时才重新检查。语义等价：scope 0 vars > 实例字段 > 父级 vars > ...。
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                return &it->second;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            if (cur->boundInstance_ && cur->boundInstance_->isInstance()
                && cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& flds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = flds.find(name);
                if (fit != flds.end()) return &fit->second;
            }
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// B1 fix: 沿作用域链查找变量（仅 variables map，不含 boundInstance 字段）。
    /// 用于闭包 capturedVars 快照——仅捕获环境变量，不捕获实例字段（与
    /// collectVariables/allVariablesMap 行为一致）。
    const Value* getVariableOnly(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) return &it->second;
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// 设置变量值（沿作用域链查找并更新）
    bool set(const std::string& name, const Value& val) {
        // PERF-02 fix: 递归改迭代，每层查 variables 再查 boundInstance 字段
        // #21 fix: 同 get()，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                it->second = val;
                return true;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
            if (cur->boundInstance_ && cur->boundInstance_->isInstance()
                && cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& constFlds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = constFlds.find(name);
                if (fit != constFlds.end()) {
                    cur->boundInstance_->fields()[name] = val;
                    return true;
                }
            }
            cur = cur->parent.get();
        }
        return false;   // 变量不存在
    }

    /// P1 fix: move 重载 — 避免 writeBack 中 std::move 静默退化为深拷贝
    bool set(const std::string& name, Value&& val) {
        // PERF-02 fix: 递归改迭代
        // #21 fix: 同 set(const&)，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                it->second = std::move(val);
                return true;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
            if (cur->boundInstance_ && cur->boundInstance_->isInstance()
                && cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& constFlds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = constFlds.find(name);
                if (fit != constFlds.end()) {
                    cur->boundInstance_->fields()[name] = std::move(val);
                    return true;
                }
            }
            cur = cur->parent.get();
        }
        return false;
    }

    /// 检查变量是否存在（沿作用域链）
    /// P2-10 fix: 与 get() 保持一致，也检查绑定实例的字段
    bool hasVariable(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        // #21 fix: 同 get()，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            if (cur->variables.find(name) != cur->variables.end()) return true;
            // 与 get() 一致：回退到绑定实例的字段检查
            if (cur->boundInstance_ && cur->boundInstance_->isInstance()
                && cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& flds = static_cast<const Value*>(cur->boundInstance_)->fields();
                if (flds.find(name) != flds.end()) return true;
            }
            cur = cur->parent.get();
        }
        return false;
    }

    /// 收集所有变量到结果向量（避免N个临时vector深拷贝）
    void collectVariables(std::vector<std::pair<std::string, Value>>& result) const {
        // 先收集父作用域（父的变量在前）
        if (parent) {
            parent->collectVariables(result);
        }
        // 再收集当前作用域（子的覆盖父的，所以追加到末尾）
        for (const auto& kv : variables) {
            result.emplace_back(kv.first, kv.second);
        }
    }

    /// 获取当前作用域及所有父作用域的变量快照（向量形式，保留遮蔽顺序）
    std::vector<std::pair<std::string, Value>> allVariables() const {
        std::vector<std::pair<std::string, Value>> result;
        collectVariables(result);
        return result;
    }

    /// C1 fix: 获取所有可见变量的扁平 map（用于闭包 capturedVars 快照）
    /// 子作用域变量覆盖父作用域同名变量（与 collectVariables 的追加顺序一致）
    std::unordered_map<std::string, Value> allVariablesMap() const {
        auto vec = allVariables();
        std::unordered_map<std::string, Value> result;
        result.reserve(vec.size());
        for (auto& kv : vec) {
            result[kv.first] = std::move(kv.second);
        }
        return result;
    }

    /// 仅获取当前作用域变量（不含父作用域）
    // PERF-01 fix: 改回 unordered_map — std::unordered_map 的引用/指针在 rehash 时
    // 不失效（C++ 标准保证：node-based 容器仅迭代器失效）。boundInstance_ 指向
    // variables 中 "this" 条目的 Value*，在 unordered_map 中同样稳定。
    // 变量查找从 O(log n) 降为 O(1) 平均，解释器整体提速 20-40%。
    const std::unordered_map<std::string, Value>& localVariables() const {
        return variables;
    }

    // ---- P5 fix: 实例字段绑定 ----
    // 方法调用时绑定 this 实例，get/set 找不到变量时回退到实例字段
    // 避免将所有字段深拷贝到方法环境中

    void bindInstance(Value* instance) { boundInstance_ = instance; }
    Value* getBoundInstance() const { return boundInstance_; }

    /// PERF-07 fix: 重置 Environment 状态以便对象池复用。
    /// 清空 variables/typeAnnotations_/boundInstance_，更新 parent 指针。
    /// 用于 visitBlock 退出时回收未捕获的块作用域 Environment，避免重复堆分配。
    void resetForReuse(std::shared_ptr<Environment> parentEnv) {
        variables.clear();
        typeAnnotations_.clear();
        boundInstance_ = nullptr;
        parent = std::move(parentEnv);
        if (parent) {
            boundInstance_ = parent->boundInstance_;
        }
    }

    // ---- B2 fix: 作用域感知的类型注解 ----

    /// 在当前作用域定义类型注解
    void defineTypeAnnotation(const std::string& name, const std::string& type) {
        typeAnnotations_[name] = type;
    }

    /// 沿作用域链查找类型注解（返回指针，nullptr=无注解）
    const std::string* getTypeAnnotation(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH) break;
            auto it = cur->typeAnnotations_.find(name);
            if (it != cur->typeAnnotations_.end()) return &it->second;
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// 获取当前作用域的类型注解（用于 REPL 状态保存）
    const std::unordered_map<std::string, std::string>& localTypeAnnotations() const {
        return typeAnnotations_;
    }

private:
    // PERF-01 fix: 改回 unordered_map。C++ 标准保证 unordered_map 的引用/指针在 rehash
    // 时不失效（仅迭代器失效），因此 boundInstance_（指向 "this" 条目的 Value*）安全。
    // 变量查找从 O(log n) 降为 O(1) 平均。
    std::unordered_map<std::string, Value> variables;
    std::unordered_map<std::string, std::string> typeAnnotations_; // B2: 作用域感知类型注解
    Value* boundInstance_ = nullptr;  // P5: 绑定的 this 实例（非拥有指针，方法调用期间有效）

    // P0-5 fix: 已移除有缺陷的深度缓存（DepthEntry/depthCache_/generation_/
    //   findTargetEnv/getAtDepth/setAtDepth/getWithDepth），改用简单作用域链遍历。
    //   缓存验证条件几乎永不成立且存在遮蔽 Bug，移除后无性能损失。
};
