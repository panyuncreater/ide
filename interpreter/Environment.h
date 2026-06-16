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
        // 注意：不在这里递增 sGeneration
        // 创建子作用域不改变已有变量的深度位置，缓存仍有效
    }

    /// 在当前作用域定义变量
    void define(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        if (!inserted) {
            it->second = val;   // 覆盖已有变量，不递增世代
        } else {
            ++sGeneration;      // 新变量插入才需要使缓存失效
        }
    }

    /// 移动重载：避免临时 Value 的深拷贝（数组/字典/实例等大对象）
    void define(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        if (!inserted) {
            it->second = std::move(val);   // 覆盖已有变量
        } else {
            ++sGeneration;
        }
    }

    /// 获取变量值（沿作用域链查找）— 返回 const 引用，避免深拷贝
    /// 优化路径：先查本地 O(1)，再用深度缓存跳过已知的中间作用域
    const Value& get(const std::string& name) const {
        // 快速路径：当前作用域直接命中
        auto it = variables.find(name);
        if (it != variables.end()) {
            return it->second;
        }
        // 慢路径：沿作用域链查找，使用深度缓存加速
        if (parent) {
            // 检查深度缓存是否有效
            auto cacheIt = depthCache_.find(name);
            if (cacheIt != depthCache_.end() && cacheIt->second.generation == sGeneration) {
                // 缓存命中：直接跳到目标深度
                return getAtDepth(name, cacheIt->second.depth);
            }
            // 缓存未命中或过期：正常遍历并记录深度
            int depth = 0;
            const Value& result = parent->getWithDepth(name, depth);
            if (depth >= 0) {
                depthCache_[name] = {depth + 1, sGeneration};  // +1: depth相对于parent，缓存相对于this
            }
            return result;
        }
        // 未找到变量，返回静态 null 哨兵
        return nullSentinel();
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
            if (cacheIt != depthCache_.end() && cacheIt->second.generation == sGeneration) {
                // 缓存命中：直接跳到目标深度
                return setAtDepth(name, val, cacheIt->second.depth);
            }
            return parent->set(name, val);
        }
        return false;   // 变量不存在
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

private:
    std::unordered_map<std::string, Value> variables;

    /// 静态 null 哨兵：避免未找到时构造临时 Value
    static const Value& nullSentinel() {
        static const Value null = Value::nullValue();
        return null;
    }

    /// 深度缓存条目：记录变量在作用域链中的深度位置
    struct DepthEntry {
        int depth;          // 变量所在作用域相对于当前作用域的深度（1=直接父级）
        uint32_t generation; // 缓存写入时的世代号
    };
    mutable std::unordered_map<std::string, DepthEntry> depthCache_;

    /// 全局世代计数器：任何环境变化都会递增，使缓存自动失效
    inline static uint32_t sGeneration = 0;

    /// 在指定深度查找变量（depth=1 表示直接父级）— 返回 const 引用
    const Value& getAtDepth(const std::string& name, int depth) const {
        const Environment* env = this;
        for (int i = 0; i < depth && env; ++i) {
            env = env->parent.get();
        }
        if (env) {
            auto it = env->variables.find(name);
            if (it != env->variables.end()) return it->second;
        }
        // 缓存过期（变量被遮蔽），回退到正常遍历
        return parent ? parent->get(name) : nullSentinel();
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

    /// 带深度记录的查找（返回时 depth 为变量所在深度，-1 表示未找到）— 返回 const 引用
    const Value& getWithDepth(const std::string& name, int& depth) const {
        auto it = variables.find(name);
        if (it != variables.end()) {
            depth = 0;
            return it->second;
        }
        if (parent) {
            int childDepth = 0;
            const Value& result = parent->getWithDepth(name, childDepth);
            depth = (childDepth >= 0) ? childDepth + 1 : -1;
            return result;
        }
        depth = -1;
        return nullSentinel();
    }
};
