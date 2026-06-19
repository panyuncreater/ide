#pragma once

#include <unordered_map>
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
    }

    /// 在当前作用域定义变量
    void define(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        if (!inserted) {
            it->second = val;   // 覆盖已有变量，不递增世代
        } else {
            ++generation_;      // P2 fix: per-environment generation
        }
    }

    /// 移动重载：避免临时 Value 的深拷贝（数组/字典/实例等大对象）
    void define(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        if (!inserted) {
            it->second = std::move(val);   // 覆盖已有变量
        } else {
            ++generation_;
        }
    }

    /// P21 fix: 原子性检查+插入 — 变量已存在返回 false，新插入返回 true
    /// 用于 visitVarDecl 消除 find+define 双次查找
    bool tryDefineNew(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        if (inserted) {
            ++generation_;
            return true;
        }
        return false;  // 已存在，不覆盖
    }
    bool tryDefineNew(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        if (inserted) {
            ++generation_;
            return true;
        }
        return false;
    }

    /// 获取变量值（沿作用域链查找）— 返回指针，nullptr 表示未找到
    /// 优化路径：先查本地 O(1)，再用深度缓存跳过已知的中间作用域
    const Value* get(const std::string& name) const {
        // 快速路径：当前作用域直接命中
        auto it = variables.find(name);
        if (it != variables.end()) {
            return &it->second;
        }
        // 慢路径：沿作用域链查找，使用深度缓存加速
        if (parent) {
            // 检查深度缓存是否有效（P2: 使用目标环境的 per-instance generation）
            auto cacheIt = depthCache_.find(name);
            if (cacheIt != depthCache_.end() && cacheIt->second.target &&
                cacheIt->second.generation == cacheIt->second.target->generation_) {
                // 缓存命中：直接跳到目标深度
                return getAtDepth(name, cacheIt->second.depth);
            }
            // 缓存未命中或过期：正常遍历并记录深度
            int depth = 0;
            const Value* result = parent->getWithDepth(name, depth);
            if (depth >= 0) {
                // 找到变量所在的目标环境（const_cast 安全：底层对象非 const，仅缓存使用）
                Environment* target = const_cast<Environment*>(findTargetEnv(name));
                depthCache_[name] = {depth + 1, target ? target->generation_ : generation_, target};
            }
            return result;
        }
        // 未找到变量
        // P5 fix: 回退到绑定实例的字段（方法调用时避免字段深拷贝）
        // A1: 使用 const 访问器避免触发 COW detach
        if (boundInstance_ && boundInstance_->isInstance()) {
            const auto& flds = static_cast<const Value*>(boundInstance_)->fields();
            auto fit = flds.find(name);
            if (fit != flds.end()) return &fit->second;
        }
        return nullptr;
    }

    /// 设置变量值（沿作用域链查找并更新）
    /// 优化路径：先查本地 O(1)，再用深度缓存跳过已知的中间作用域
    bool set(const std::string& name, const Value& val) {
        // 快速路径：当前作用域直接命中
        auto it = variables.find(name);
        if (it != variables.end()) {
            it->second = val;
            return true;
        }
        if (parent) {
            // 检查深度缓存是否有效
            auto cacheIt = depthCache_.find(name);
            if (cacheIt != depthCache_.end() && cacheIt->second.target &&
                cacheIt->second.generation == cacheIt->second.target->generation_) {
                // 缓存命中：直接跳到目标深度
                return setAtDepth(name, val, cacheIt->second.depth);
            }
            // 缓存未命中：正常遍历并记录深度，供后续 set 使用
            int depth = 0;
            const Value* found = parent->getWithDepth(name, depth);
            if (found) {
                // 找到变量所在的目标环境
                Environment* target = findTargetEnv(name);
                depthCache_[name] = {depth + 1, target ? target->generation_ : generation_, target};
                // 直接通过深度路径写入，避免重复遍历
                return setAtDepth(name, val, depth + 1);
            }
            return false;
        }
        // P5 fix: 回退到绑定实例的字段
        if (boundInstance_ && boundInstance_->isInstance()) {
            auto fit = boundInstance_->fields().find(name);
            if (fit != boundInstance_->fields().end()) {
                fit->second = val;
                return true;
            }
        }
        return false;   // 变量不存在
    }

    /// P1 fix: move 重载 — 避免 writeBack 中 std::move 静默退化为深拷贝
    bool set(const std::string& name, Value&& val) {
        auto it = variables.find(name);
        if (it != variables.end()) {
            it->second = std::move(val);
            return true;
        }
        if (parent) {
            auto cacheIt = depthCache_.find(name);
            if (cacheIt != depthCache_.end() && cacheIt->second.target &&
                cacheIt->second.generation == cacheIt->second.target->generation_) {
                return setAtDepth(name, std::move(val), cacheIt->second.depth);
            }
            int depth = 0;
            const Value* found = parent->getWithDepth(name, depth);
            if (found) {
                Environment* target = findTargetEnv(name);
                depthCache_[name] = {depth + 1, target ? target->generation_ : generation_, target};
                return setAtDepth(name, std::move(val), depth + 1);
            }
            return false;
        }
        // P5 fix: 回退到绑定实例的字段
        if (boundInstance_ && boundInstance_->isInstance()) {
            auto fit = boundInstance_->fields().find(name);
            if (fit != boundInstance_->fields().end()) {
                fit->second = std::move(val);
                return true;
            }
        }
        return false;
    }

    /// 检查变量是否存在（沿作用域链）
    bool hasVariable(const std::string& name) const {
        if (variables.find(name) != variables.end()) return true;
        if (parent) return parent->hasVariable(name);
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

    /// 获取当前作用域及所有父作用域的变量快照
    std::vector<std::pair<std::string, Value>> allVariables() const {
        std::vector<std::pair<std::string, Value>> result;
        collectVariables(result);
        return result;
    }

    /// 仅获取当前作用域变量（不含父作用域）
    const std::unordered_map<std::string, Value>& localVariables() const {
        return variables;
    }

    // ---- P5 fix: 实例字段绑定 ----
    // 方法调用时绑定 this 实例，get/set 找不到变量时回退到实例字段
    // 避免将所有字段深拷贝到方法环境中

    void bindInstance(Value* instance) { boundInstance_ = instance; }
    Value* getBoundInstance() const { return boundInstance_; }

    // ---- B2 fix: 作用域感知的类型注解 ----

    /// 在当前作用域定义类型注解
    void defineTypeAnnotation(const std::string& name, const std::string& type) {
        typeAnnotations_[name] = type;
    }

    /// 沿作用域链查找类型注解（返回指针，nullptr=无注解）
    const std::string* getTypeAnnotation(const std::string& name) const {
        auto it = typeAnnotations_.find(name);
        if (it != typeAnnotations_.end()) return &it->second;
        if (parent) return parent->getTypeAnnotation(name);
        return nullptr;
    }

    /// 获取当前作用域的类型注解（用于 REPL 状态保存）
    const std::unordered_map<std::string, std::string>& localTypeAnnotations() const {
        return typeAnnotations_;
    }

private:
    std::unordered_map<std::string, Value> variables;
    std::unordered_map<std::string, std::string> typeAnnotations_; // B2: 作用域感知类型注解
    Value* boundInstance_ = nullptr;  // P5: 绑定的 this 实例（非拥有指针，方法调用期间有效）

    /// 深度缓存条目：记录变量在作用域链中的深度位置
    struct DepthEntry {
        int depth;          // 变量所在作用域相对于当前作用域的深度（1=直接父级）
        uint32_t generation; // 目标环境的世代号（P2: per-environment）
        Environment* target; // P2: 变量所在的目标环境指针（用于验证 generation）
    };
    mutable std::unordered_map<std::string, DepthEntry> depthCache_;

    /// P2 fix: per-environment 世代计数器（替代全局 sGeneration）
    /// 仅在当前环境 define 新变量时递增，不影响其他环境的缓存
    uint32_t generation_ = 0;

    /// P2: 沿作用域链查找变量所在的目标环境（返回原始指针）
    Environment* findTargetEnv(const std::string& name) {
        auto it = variables.find(name);
        if (it != variables.end()) return this;
        if (parent) return parent->findTargetEnv(name);
        return nullptr;
    }
    const Environment* findTargetEnv(const std::string& name) const {
        auto it = variables.find(name);
        if (it != variables.end()) return this;
        if (parent) return parent->findTargetEnv(name);
        return nullptr;
    }

    /// 在指定深度查找变量（depth=1 表示直接父级）— 返回指针，nullptr=未找到
    const Value* getAtDepth(const std::string& name, int depth) const {
        const Environment* env = this;
        for (int i = 0; i < depth && env; ++i) {
            env = env->parent.get();
        }
        if (env) {
            auto it = env->variables.find(name);
            if (it != env->variables.end()) return &it->second;
        }
        // 缓存过期（变量被遮蔽），回退到正常遍历
        return parent ? parent->get(name) : nullptr;
    }

    /// 在指定深度设置变量（depth=1 表示直接父级）
    bool setAtDepth(const std::string& name, const Value& val, int depth) {
        Environment* env = this;
        for (int i = 0; i < depth && env; ++i) {
            env = env->parent.get();
        }
        if (env) {
            auto it = env->variables.find(name);
            if (it != env->variables.end()) {
                it->second = val;
                return true;
            }
        }
        // 缓存过期（变量被遮蔽），回退到正常遍历
        return parent ? parent->set(name, val) : false;
    }

    /// P1 fix: setAtDepth move 重载
    bool setAtDepth(const std::string& name, Value&& val, int depth) {
        Environment* env = this;
        for (int i = 0; i < depth && env; ++i) {
            env = env->parent.get();
        }
        if (env) {
            auto it = env->variables.find(name);
            if (it != env->variables.end()) {
                it->second = std::move(val);
                return true;
            }
        }
        return parent ? parent->set(name, std::move(val)) : false;
    }

    /// 带深度记录的查找（返回时 depth 为变量所在深度，-1 表示未找到）— 返回指针
    const Value* getWithDepth(const std::string& name, int& depth) const {
        auto it = variables.find(name);
        if (it != variables.end()) {
            depth = 0;
            return &it->second;
        }
        if (parent) {
            int childDepth = 0;
            const Value* result = parent->getWithDepth(name, childDepth);
            depth = (childDepth >= 0) ? childDepth + 1 : -1;
            return result;
        }
        depth = -1;
        return nullptr;
    }
};
