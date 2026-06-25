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
        // 快速路径：当前作用域直接命中
        auto it = variables.find(name);
        if (it != variables.end()) {
            return &it->second;
        }
        // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
        if (boundInstance_ && boundInstance_->isInstance()) {
            const auto& flds = static_cast<const Value*>(boundInstance_)->fields();
            auto fit = flds.find(name);
            if (fit != flds.end()) return &fit->second;
        }
        // P0-5 fix: 移除有缺陷的深度缓存（验证条件几乎永不成立且存在遮蔽 Bug），
        // 改用简单的作用域链遍历，O(depth) 但正确性可靠
        if (parent) {
            return parent->get(name);
        }
        return nullptr;
    }

    /// B1 fix: 沿作用域链查找变量（仅 variables map，不含 boundInstance 字段）。
    /// 用于闭包 capturedVars 快照——仅捕获环境变量，不捕获实例字段（与
    /// collectVariables/allVariablesMap 行为一致）。
    const Value* getVariableOnly(const std::string& name) const {
        auto it = variables.find(name);
        if (it != variables.end()) return &it->second;
        if (parent) return parent->getVariableOnly(name);
        return nullptr;
    }

    /// 设置变量值（沿作用域链查找并更新）
    bool set(const std::string& name, const Value& val) {
        // 快速路径：当前作用域直接命中
        auto it = variables.find(name);
        if (it != variables.end()) {
            it->second = val;
            return true;
        }
        // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
        // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
        if (boundInstance_ && boundInstance_->isInstance()) {
            const auto& constFlds = static_cast<const Value*>(boundInstance_)->fields();
            auto fit = constFlds.find(name);
            if (fit != constFlds.end()) {
                // 字段确实存在，此时才触发 COW（必要时）
                boundInstance_->fields()[name] = val;
                return true;
            }
        }
        // P0-5 fix: 移除深度缓存，改用简单遍历
        if (parent) {
            return parent->set(name, val);
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
        // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
        // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
        if (boundInstance_ && boundInstance_->isInstance()) {
            const auto& constFlds = static_cast<const Value*>(boundInstance_)->fields();
            auto fit = constFlds.find(name);
            if (fit != constFlds.end()) {
                boundInstance_->fields()[name] = std::move(val);
                return true;
            }
        }
        if (parent) {
            return parent->set(name, std::move(val));
        }
        return false;
    }

    /// 检查变量是否存在（沿作用域链）
    /// P2-10 fix: 与 get() 保持一致，也检查绑定实例的字段
    bool hasVariable(const std::string& name) const {
        if (variables.find(name) != variables.end()) return true;
        // 与 get() 一致：回退到绑定实例的字段检查
        if (boundInstance_ && boundInstance_->isInstance()) {
            const auto& flds = static_cast<const Value*>(boundInstance_)->fields();
            if (flds.find(name) != flds.end()) return true;
        }
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
    // B2 fix: 改用 std::map — 插入不使引用失效，boundInstance_ 指向的 Value 不会因
    // 后续 define() 触发 rehash 而悬空。
    const std::map<std::string, Value>& localVariables() const {
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
    // B2 fix: 使用 std::map 替代 unordered_map。std::map 插入/删除不使已有元素的
    // 引用和迭代器失效，从而保证 boundInstance_（指向 variables 中 "this" 条目的
    // Value*）在后续 define() 调用后仍然有效，消除 rehash 悬垂指针风险。
    std::map<std::string, Value> variables;
    std::unordered_map<std::string, std::string> typeAnnotations_; // B2: 作用域感知类型注解
    Value* boundInstance_ = nullptr;  // P5: 绑定的 this 实例（非拥有指针，方法调用期间有效）

    // P0-5 fix: 已移除有缺陷的深度缓存（DepthEntry/depthCache_/generation_/
    //   findTargetEnv/getAtDepth/setAtDepth/getWithDepth），改用简单作用域链遍历。
    //   缓存验证条件几乎永不成立且存在遮蔽 Bug，移除后无性能损失。
};
