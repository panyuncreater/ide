#include "ast/ASTNode.h"
#include "interpreter/Visitor.h"

// ============================================================
// AST 节点的 accept 实现
// ============================================================

Value BinaryOp::accept(Visitor& visitor) {
    return visitor.visitBinaryOp(*this);
}

Value UnaryOp::accept(Visitor& visitor) {
    return visitor.visitUnaryOp(*this);
}

Value NumberLiteral::accept(Visitor& visitor) {
    return visitor.visitNumberLiteral(*this);
}

Value StringLiteral::accept(Visitor& visitor) {
    return visitor.visitStringLiteral(*this);
}

Value BoolLiteral::accept(Visitor& visitor) {
    return visitor.visitBoolLiteral(*this);
}

Value VarDecl::accept(Visitor& visitor) {
    return visitor.visitVarDecl(*this);
}

Value Assignment::accept(Visitor& visitor) {
    return visitor.visitAssignment(*this);
}

Value VarRef::accept(Visitor& visitor) {
    return visitor.visitVarRef(*this);
}

Value IfStmt::accept(Visitor& visitor) {
    return visitor.visitIfStmt(*this);
}

Value WhileStmt::accept(Visitor& visitor) {
    return visitor.visitWhileStmt(*this);
}

Value ForStmt::accept(Visitor& visitor) {
    return visitor.visitForStmt(*this);
}

Value FunDecl::accept(Visitor& visitor) {
    return visitor.visitFunDecl(*this);
}

Value FunCall::accept(Visitor& visitor) {
    return visitor.visitFunCall(*this);
}

Value ReturnStmt::accept(Visitor& visitor) {
    return visitor.visitReturnStmt(*this);
}

Value PrintStmt::accept(Visitor& visitor) {
    return visitor.visitPrintStmt(*this);
}

Value Block::accept(Visitor& visitor) {
    return visitor.visitBlock(*this);
}

// 新增节点的 accept 实现

Value ArrayLiteral::accept(Visitor& visitor) {
    return visitor.visitArrayLiteral(*this);
}

Value DictLiteral::accept(Visitor& visitor) {
    return visitor.visitDictLiteral(*this);
}

Value IndexAccess::accept(Visitor& visitor) {
    return visitor.visitIndexAccess(*this);
}

Value IndexAssign::accept(Visitor& visitor) {
    return visitor.visitIndexAssign(*this);
}

Value ClassDecl::accept(Visitor& visitor) {
    return visitor.visitClassDecl(*this);
}

Value MemberAccess::accept(Visitor& visitor) {
    return visitor.visitMemberAccess(*this);
}

Value MemberAssign::accept(Visitor& visitor) {
    return visitor.visitMemberAssign(*this);
}

Value MethodCall::accept(Visitor& visitor) {
    return visitor.visitMethodCall(*this);
}

Value NullLiteral::accept(Visitor& visitor) {
    return visitor.visitNullLiteral(*this);
}

Value SuperExpr::accept(Visitor& visitor) {
    return visitor.visitSuperExpr(*this);
}
