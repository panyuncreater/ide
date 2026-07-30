/**
 * @file ast/MacroExpander.h
 * @brief 七特性 MVP 阶段 2：宏展开器——表达式模板克隆 + 参数替换。
 *
 * Parser 遇到 `name!(args)` 时调用 cloneWithSubstitution 克隆宏 body 模板，
 * 将模板中与形参同名的 VarRef 节点替换为对应实参 AST（shared_ptr 共享，
 * 不克隆实参本身——同一形参多次出现时共享同一实参子树，同 C 宏语义）。
 *
 * 支持的节点类型限定为"表达式子集"（宏 body 是单个表达式）：
 *   BinaryOp / UnaryOp / NumberLiteral / StringLiteral / BoolLiteral /
 *   NullLiteral / VarRef / FunCall / IndexAccess / MemberAccess / MethodCall /
 *   ArrayLiteral / DictLiteral / TupleLiteral / InterpolatedString /
 *   EnumVariantExpr / MacroCallExpr（嵌套宏调用，克隆其 expanded）
 * 遇到不支持的节点类型返回 nullptr 并写出 unsupportedNodeName，
 * 由调用方（Parser）转为 ParseError。
 *
 * 深度保护：克隆递归深度上限 RuntimeLimits::MAX_MACRO_EXPANSION_DEPTH，
 * 超限返回 nullptr 且 unsupportedNodeName 置为 "<depth-limit>"。
 *
 * @see Parser::macroDecl Parser::primary MacroDecl MacroCallExpr
 */
#pragma once

#include "ast/ASTNode.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace MacroExpander {

/// 参数替换表：形参名 → 实参 AST（shared_ptr 共享）
using SubstMap = std::unordered_map<std::string, std::shared_ptr<ASTNode>>;

/// 克隆 node 子树并替换形参 VarRef。
/// @param node 宏 body 模板（或其子表达式）
/// @param subst 形参名 → 实参 AST 映射
/// @param depth 当前递归深度（调用方传 0）
/// @param unsupportedNodeName [out] 失败时写入不支持的节点名或 "<depth-limit>"
/// @return 克隆后的子树；失败返回 nullptr
std::shared_ptr<ASTNode> cloneWithSubstitution(const ASTNode* node, const SubstMap& subst, int depth,
                                               std::string* unsupportedNodeName);

} // namespace MacroExpander
