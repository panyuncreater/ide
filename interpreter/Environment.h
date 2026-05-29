#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <functional>
#include "interpreter/Value.h"

// ============================================================
// Environment 作用域链
// ============================================================

/// 作用域环境，支持嵌套（作用域链）
class Environment {
public:
    /// 父作用域指针（全局环境为 nullptr）
    Environment* parent;

    /// 构造函数
    explicit Environment(Environment* parentEnv = nullptr)
        : parent(parentEnv) {}

    /// 在当前作用域定义变量
    void define(const std::string& name, const Value& val) {
        variables[name] = val;
    }

    /// 获取变量值（沿作用域链查找）
    Value get(const std::string& name) const {
        auto it = variables.find(name);
        if (it != variables.end()) {
            return it->second;
        }
        if (parent) {
            return parent->get(name);
        }
        // 未找到变量，返回 null（调用者应先检查 hasVariable）
        return Value::nullValue();
    }

    /// 设置变量值（沿作用域链查找并更新）
    bool set(const std::string& name, const Value& val) {
        auto it = variables.find(name);
        if (it != variables.end()) {
            it->second = val;
            return true;
        }
        if (parent) {
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

    /// 获取当前作用域及所有父作用域的变量快照
    std::vector<std::pair<std::string, Value>> allVariables() const {
        std::vector<std::pair<std::string, Value>> result;
        // 先收集父作用域
        if (parent) {
            auto parentVars = parent->allVariables();
            result.insert(result.end(), parentVars.begin(), parentVars.end());
        }
        // 再收集当前作用域
        for (const auto& kv : variables) {
            result.emplace_back(kv.first, kv.second);
        }
        return result;
    }

    /// 仅获取当前作用域变量（不含父作用域）
    const std::unordered_map<std::string, Value>& localVariables() const {
        return variables;
    }

private:
    std::unordered_map<std::string, Value> variables;
};
