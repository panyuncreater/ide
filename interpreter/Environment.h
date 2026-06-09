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
class Environment : public std::enable_shared_from_this<Environment> {
public:
    /// 父作用域指针（全局环境为 nullptr）
    std::shared_ptr<Environment> parent;

    /// 构造函数
    explicit Environment(std::shared_ptr<Environment> parentEnv = nullptr)
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
};
