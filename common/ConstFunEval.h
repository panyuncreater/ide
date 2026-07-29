#pragma once

// ============================================================
// ConstFunEval.h — L18 lang-constfun: const fun 编译期沙箱求值
// ------------------------------------------------------------
// 三后端共享的编译期求值 helper（header-only，Compiler / AstIRBuilder
// 在 visitFunCall 折叠点调用；Interpreter 运行时直接调用语义等价，无需折叠）。
//
// 折叠条件（全部满足）：
//   1. 直接名称调用 f(args)（callee == nullptr）且 f 是已声明的 const fun
//   2. 实参全部为字面量（number/string/bool/null）
//   3. arity 在 [requiredParamCount, params.size()] 范围内
//   4. 沙箱求值成功且无副作用观测（无 output/input、无运行时错误/异常）
//   5. 结果为原始类型（int/float/bool/string/null）
// 任一条件不满足 → 返回 nullopt，调用方回退为普通运行时调用（语义安全）。
//
// 副作用安全性：
//   - 沙箱是独立 Interpreter 实例，print/input 回调仅置 impure 标志
//   - 引用全局变量/未注册函数 → 沙箱内未定义 → 运行时错误 → 回退
//   - 死循环/深递归 → 沙箱内建迭代/递归上限报错 → 回退
// 已知限制（v1）：clock()/随机类内建在沙箱中会被固化为编译期值——
// 与 C++ constexpr 的确定性约定一致，const fun 用户意图即编译期求值。
// ============================================================

#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace ConstFunEval {

/// 判断 AST 节点是否为可编译期求值的字面量
inline bool isLiteralNode(const ASTNode* n) {
    if (!n)
        return false;
    switch (n->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
        return true;
    default:
        return false;
    }
}

/// 尝试编译期求值 const fun 调用。成功返回结果 Value，失败返回 nullopt（回退运行时）。
/// @param constFuns 已声明的 const fun 注册表（name → 非拥有 FunDecl 指针，AST 生命周期内有效）
/// @param call 调用点 FunCall 节点
inline std::optional<Value> tryEvaluate(const std::unordered_map<std::string, FunDecl*>& constFuns, FunCall& call) {
    if (call.callee != nullptr)
        return std::nullopt;
    auto it = constFuns.find(call.name);
    if (it == constFuns.end())
        return std::nullopt;
    FunDecl* decl = it->second;
    if (!decl || decl->isGenerator)
        return std::nullopt;
    if (call.arguments.size() < static_cast<size_t>(decl->requiredParamCount) ||
        call.arguments.size() > decl->params.size())
        return std::nullopt;
    for (auto& a : call.arguments) {
        if (!isLiteralNode(a.get()))
            return std::nullopt;
    }

    try {
        Interpreter sandbox;
        bool impure = false;
        sandbox.setOutputCallback([&](const std::string&) { impure = true; });
        sandbox.setInputCallback([&](const std::string&) -> std::string {
            impure = true;
            return "";
        });
        // 注册全部 const fun（互相调用支持）：非拥有 shared_ptr 别名，
        // AST 在编译期间存活，沙箱生命周期短于 AST，安全。
        std::vector<std::shared_ptr<ASTNode>> decls;
        decls.reserve(constFuns.size());
        for (const auto& kv : constFuns) {
            decls.push_back(std::shared_ptr<ASTNode>(kv.second, [](ASTNode*) {}));
        }
        Block declBlock(std::move(decls), call.line, call.column);
        sandbox.execute(declBlock);
        if (sandbox.getDiagnostics().hasErrors() || impure)
            return std::nullopt;
        Value result = sandbox.evaluateExpr(&call);
        if (sandbox.getDiagnostics().hasErrors() || impure)
            return std::nullopt;
        // 仅折叠原始类型（容器/闭包/实例不折叠——常量池语义与共享性复杂）
        if (!(result.isInt() || result.isFloat() || result.isBool() || result.isString() || result.isNull()))
            return std::nullopt;
        return result;
    } catch (...) {
        // 沙箱内任何异常（运行时错误/递归超限/迭代超限）→ 回退运行时调用
        return std::nullopt;
    }
}

} // namespace ConstFunEval
