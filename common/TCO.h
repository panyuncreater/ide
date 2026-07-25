#pragma once

// ============================================================
// TCO.h — 尾调用优化（TCO）共享识别函数
// ------------------------------------------------------------
// 三后端（StackVM 直接路径 / IR 路径 / RegisterVM 路径 / Interpreter 路径）
// 共享的 AST 结构识别逻辑。L15 扩展支持：
//   - 自递归函数调用 return f(args)（含 upvalue / 默认参数）
//   - 类方法自调用 return this.method(args)（保留 this/字段槽，覆盖参数槽）
//
// 识别条件（必须同时满足，才允许 TCO 优化）：
//   1. return 语句的值是 FunCall 或 MethodCall 节点
//   2. 对 FunCall：callee == nullptr（直接名称调用 f(args)，非链式 expr(args)）
//      且 FunCall.name == 当前函数名（SelfFunction）
//   3. 对 MethodCall：object 是 VarRef("this") 且 methodName == 当前方法名（SelfMethod）
//   4. 不在 try 块内（由调用方检查 tryDepth_ == 0）
//   5. currentFunctionDecl_ 非空（由调用方检查，用于获取形参/默认值）
//
// L15 放宽的限制（由调用方检查）：
//   - 闭包 upvalue：SelfFunction/SelfMethod 安全（同函数重用同一闭包，
//     upvalue 指向外层作用域，递归不覆盖外层栈槽）
//   - 默认参数：args.size() < params.size() 时用默认值填充缺失参数
//   - 类方法：SelfMethod 保留 slot 0 (this) 和 slot 1..N (字段)，仅覆盖参数槽
//
// 仍不支持的形态（需要 VM 层 OP_TAIL_CALL，留作后续工作）：
//   - 互递归 return g(args)（g != 当前函数名，跨 chunk 跳转）
//   - 链式调用 return f(x)(y)（callee != nullptr，闭包目标编译期未知）
//
// 设计原则：
//   - 保守识别：仅匹配可直接 JUMP 回当前函数入口的尾调用形态
//   - 三后端一致：所有后端使用同一识别函数，确保语义等价
//   - 无副作用：纯函数，仅读取 AST 结构
// ============================================================

#include "ast/ASTNode.h"
#include <string>

namespace TCO {

/// 尾调用识别结果
struct TailCallInfo {
    /// 尾调用类型
    enum class Kind {
        None,         ///< 非尾调用或不支持的形式
        SelfFunction, ///< return f(args) 自递归函数调用（f == 当前函数名）
        SelfMethod,   ///< return this.method(args) 类方法自调用（method == 当前方法名）
    };

    Kind kind = Kind::None;
    /// SelfFunction: 对应的 FunCall 节点（非拥有，AST 生命周期内有效）
    FunCall* call = nullptr;
    /// SelfMethod: 对应的 MethodCall 节点（非拥有，AST 生命周期内有效）
    MethodCall* methodCall = nullptr;
};

/// 检查 return 语句是否为可优化的尾调用。
///
/// 仅检查 AST 结构。调用方需自行检查：
///   - tryDepth_ == 0（不在 try-finally / try-catch 块内）
///   - currentFunctionDecl_ != nullptr（用于获取形参/默认值列表）
///
/// L15 放宽：不再要求 currentUpvalues_.empty()（SelfFunction/SelfMethod 安全）
///          不再要求 args.size() == params.size()（默认参数填充）
///          不再排除类方法（SelfMethod 保留 this/字段槽）
///
/// @param node ReturnStmt 节点（可为 nullptr，返回 Kind::None）
/// @param currentFuncName 当前函数/方法的简单名（不含 "ClassName." 前缀）
/// @param isMethod 当前是否在类方法体内（true 时识别 SelfMethod）
/// @return TailCallInfo 描述尾调用类型与节点指针
inline TailCallInfo identifyTailCall(ReturnStmt* node, const std::string& currentFuncName, bool isMethod) {
    TailCallInfo info;
    if (!node || !node->value || currentFuncName.empty()) {
        return info;
    }

    // SelfFunction: return f(args) — 直接名称调用且名称匹配当前函数
    if (node->value->nodeType == NodeType::NODE_FUN_CALL && !isMethod) {
        auto* call = static_cast<FunCall*>(node->value.get());
        // 仅支持直接名称调用：f(args)，不支持链式 expr(args)
        if (call->callee == nullptr && call->name == currentFuncName) {
            info.kind = TailCallInfo::Kind::SelfFunction;
            info.call = call;
            return info;
        }
    }

    // SelfMethod: return this.method(args) — 方法自调用
    // object 必须是 VarRef("this")，确保接收者就是当前实例（slot 0 不变）
    if (node->value->nodeType == NodeType::NODE_METHOD_CALL && isMethod) {
        auto* methodCall = static_cast<MethodCall*>(node->value.get());
        if (methodCall->methodName == currentFuncName && methodCall->object &&
            methodCall->object->nodeType == NodeType::NODE_VAR_REF) {
            auto* objVar = static_cast<VarRef*>(methodCall->object.get());
            if (objVar->name == "this") {
                info.kind = TailCallInfo::Kind::SelfMethod;
                info.methodCall = methodCall;
                return info;
            }
        }
    }

    return info;
}

/// [废弃] 旧的仅识别自递归函数调用的接口，保留向后兼容。
/// 新代码应使用 identifyTailCall。
inline bool isTailRecursiveReturn(ReturnStmt* node, const std::string& funcName) {
    if (!node || !node->value)
        return false;
    if (node->value->nodeType != NodeType::NODE_FUN_CALL)
        return false;
    auto* call = static_cast<FunCall*>(node->value.get());
    if (call->callee != nullptr)
        return false;
    return call->name == funcName;
}

} // namespace TCO
