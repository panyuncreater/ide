#pragma once

// ============================================================
// ExprStmtPop.h — 表达式语句 POP 判定（栈式直接编译 + IR 路径共享）
// ------------------------------------------------------------
// AUDIT-R4 BUG-04 fix: 原实现为双份维护——
//   · Compiler::compileStatement（栈式直接路径）的 switch 清单
//   · AstIRBuilder 匿名命名空间的 needsPopForExprStmt（IR 路径）
// 历史上 R134 仅在 IR 路径补入 MATCH_EXPR/ENUM_VARIANT_EXPR/TUPLE_LITERAL
// 三种节点，栈式直接路径清单漂移遗漏（match 表达式/enum variant/元组字面量
// 作为裸表达式语句时结果未 POP，循环内逐次栈泄漏）。
// 本头文件为单一事实来源：新增表达式节点类型时只需修改此处，
// 两条编译路径自动同步。
//
// 语义：表达式语句（如 `foo();`、`a + b;`）会产生一个栈上返回值但不被
// 消费，需补发 POP 防止栈泄漏。声明/控制流节点（VarDecl/IfStmt/WhileStmt
// 等）已自行平衡栈，不需 POP。
// AUDIT-R5 BUG-01 fix: 匿名 lambda（NODE_FUN_DECL 且 name 为空）改由节点级
// 重载 needsPopForExprStmt(const ASTNode*) 统一判定——原注释声称“IR 路径在
// visitFunDecl 顶层路径内自行 POP”与实现不符（emitFunctionClosureRegistration
// 的 isLambda 分支直接返回 dest 无 POP），导致裸 lambda 表达式语句在 IR 路径
// 泄漏闭包值。两条编译路径统一改用节点级重载。
// AUDIT-R5 BUG-02 fix: 补入 NODE_YIELD_EXPR——OP_YIELD 重放未命中时保留栈顶值
// 作为 yield 表达式结果，裸 yield 语句（`yield x;`）不 POP 会在生成器重放
// 模型下随 next() 调用次数平方级泄漏，定长 Value[1024] 操作数栈很快溢出。
// ============================================================

#include "ast/ASTNode.h"

inline bool needsPopForExprStmt(NodeType nt) {
    switch (nt) {
    case NodeType::NODE_ASSIGNMENT:
    case NodeType::NODE_FUN_CALL:
    case NodeType::NODE_METHOD_CALL:
    case NodeType::NODE_BINARY_OP:
    case NodeType::NODE_UNARY_OP:
    case NodeType::NODE_VAR_REF:
    case NodeType::NODE_MEMBER_ACCESS:
    case NodeType::NODE_INDEX_ACCESS:
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
    case NodeType::NODE_ARRAY_LITERAL:
    case NodeType::NODE_DICT_LITERAL:
    case NodeType::NODE_INTERPOLATED_STRING:
    // R134 fix: match 表达式结果/enum variant 表达式/元组字面量
    case NodeType::NODE_MATCH_EXPR:
    case NodeType::NODE_ENUM_VARIANT_EXPR:
    case NodeType::NODE_TUPLE_LITERAL:
    // AUDIT-R5 BUG-02 fix: 裸 yield 语句的结果值需 POP（见文件头注释）
    case NodeType::NODE_YIELD_EXPR:
        return true;
    default:
        return false;
    }
}

/// AUDIT-R5 BUG-01 fix: 节点级重载——在类型谓词基础上覆盖匿名 lambda。
/// 匿名 lambda（`fun(x){...};` 作为裸表达式语句）会在栈上留下闭包值，
/// 需要 POP；具名函数声明自行平衡（存槽/全局或 POP），不需调用方 POP。
/// 两条编译路径（Compiler::compileStatement / AstIRBuilder 语句上下文）均用此重载。
inline bool needsPopForExprStmt(const ASTNode* node) {
    if (!node)
        return false;
    if (node->nodeType == NodeType::NODE_FUN_DECL)
        return static_cast<const FunDecl*>(node)->name.empty();
    return needsPopForExprStmt(node->nodeType);
}
