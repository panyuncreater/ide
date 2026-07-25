#pragma once

// ============================================================
// BuiltinModules — 内建标准库模块注册表
// ------------------------------------------------------------
// P2-11 子项 4: 实现 `import { foo } from "std/math"` 语法。
//
// 设计：moduleLoader 拦截 `std/` 前缀，返回内建模块的 MiniLang 源码。
// 三后端零侵入——moduleLoader 是统一注入点，Interpreter/Compiler/AstIRBuilder
// 均通过 moduleLoader_ 加载模块源码，无需修改核心解析逻辑。
//
// 内建模块以纯 MiniLang 源码实现，包装现有内置函数 + 提供常用算法。
// 后续可演进为原生注册（方案 B），当前 MVP 用源码方式快速落地。
// ============================================================

#include <string>
#include <unordered_map>

/// 内建标准库模块注册表
class BuiltinModuleRegistry {
public:
    /// 查询模块路径是否为内建标准库模块（以 "std/" 开头）
    static bool isBuiltinModule(const std::string& modulePath);

    /// 获取内建模块的 MiniLang 源码。
    /// @param modulePath 模块路径（如 "std/math"）
    /// @return 模块源码字符串；非内建模块返回空串
    static const std::string& getSource(const std::string& modulePath);

    /// 获取所有已注册的内建模块路径列表
    static const std::vector<std::string>& listModules();
};
