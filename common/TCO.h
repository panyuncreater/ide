#pragma once

// ============================================================
// TCO.h — 尾调用优化（TCO）共享识别函数
// ------------------------------------------------------------
// 三后端（StackVM 直接路径 / IR 路径 / RegisterVM 路径 / Interpreter 路径）
// 共享的 AST 结构识别逻辑。仅检查"return 语句的值是直接尾递归自调用"的
// AST 形态（条件 1 与 2），不检查 try 块 / upvalue / 默认参数等运行时
// 上下文——这些条件由各后端根据自身状态自行检查。
//
// 识别条件（必须同时满足，才允许 TCO 优化）：
//   1. return 语句的值是 FunCall 节点（NODE_FUN_CALL）
//   2. FunCall.callee == nullptr（直接名称调用 f(args)，非链式 expr(args)）
//      且 FunCall.name == 当前函数名
//   3. 不在 try-finally 块内（由调用方检查 tryDepth_ == 0）
//   4. 不在 try-catch 块内（由调用方检查 tryDepth_ == 0）
//   5. 函数无闭包 upvalue（由调用方检查 currentUpvalues_.empty()）
//   6. 参数数量等于形参数量（无默认参数填充，由调用方检查）
//
// 设计原则：
//   - 保守识别：仅匹配最直接的尾递归自调用形态 `return f(args);`
//     不支持 mutual recursion、chain call、method call 等复杂形态
//   - 三后端一致：所有后端使用同一识别函数，确保语义等价
//   - 无副作用：纯函数，仅读取 AST 结构
// ============================================================

#include "ast/ASTNode.h"
#include <string>

namespace TCO {

/// 检查 return 语句是否为对指定函数名的直接尾递归自调用。
///
/// 仅检查 AST 结构（条件 1 与 2）。调用方需自行检查：
///   - tryDepth_ == 0（不在 try-finally / try-catch 块内）
///   - currentUpvalues_.empty()（函数无闭包 upvalue）
///   - call->arguments.size() == params.size()（无默认参数填充）
///
/// @param node ReturnStmt 节点（可为 nullptr，返回 false）
/// @param funcName 当前函数名
/// @return true 当且仅当 AST 结构匹配直接尾递归自调用 `return funcName(args);`
inline bool isTailRecursiveReturn(ReturnStmt* node, const std::string& funcName) {
    if (!node || !node->value)
        return false;
    if (node->value->nodeType != NodeType::NODE_FUN_CALL)
        return false;
    auto* call = static_cast<FunCall*>(node->value.get());
    // 仅支持直接名称调用：f(args)，不支持链式调用 expr(args)
    if (call->callee != nullptr)
        return false;
    return call->name == funcName;
}

} // namespace TCO
