#include "ast/ASTNode.h"
#include "interpreter/Visitor.h"
#include "interpreter/Value.h"  // A1 fix: getValue()/nodeName() 实现需要 Value 完整定义

// ============================================================
// AST 节点的 accept 实现
// ============================================================
// A1 fix: accept 返回 void，结果通过 Visitor 子类成员变量传递。

// A1 fix: 以下 getValue() / nodeName() 实现原本 inline 定义于 ASTNode.h，
// 但 ASTNode.h 现仅前向声明 Value（不再 include Value.h），故实现移至此处。

Value NumberLiteral::getValue() const {
    return isFloat_ ? Value(floatValue_) : Value(intValue_);
}

std::string NumberLiteral::nodeName() const {
    return "Number(" + getValue().toString() + ")";
}

Value StringLiteral::getValue() const {
    return Value(value);
}

Value BoolLiteral::getValue() const {
    return Value(value);
}

void BinaryOp::accept(Visitor& visitor) {
    visitor.visitBinaryOp(*this);
}

void UnaryOp::accept(Visitor& visitor) {
    visitor.visitUnaryOp(*this);
}

void NumberLiteral::accept(Visitor& visitor) {
    visitor.visitNumberLiteral(*this);
}

void StringLiteral::accept(Visitor& visitor) {
    visitor.visitStringLiteral(*this);
}

void BoolLiteral::accept(Visitor& visitor) {
    visitor.visitBoolLiteral(*this);
}

void VarDecl::accept(Visitor& visitor) {
    visitor.visitVarDecl(*this);
}

void Assignment::accept(Visitor& visitor) {
    visitor.visitAssignment(*this);
}

void VarRef::accept(Visitor& visitor) {
    visitor.visitVarRef(*this);
}

void IfStmt::accept(Visitor& visitor) {
    visitor.visitIfStmt(*this);
}

void WhileStmt::accept(Visitor& visitor) {
    visitor.visitWhileStmt(*this);
}

void ForStmt::accept(Visitor& visitor) {
    visitor.visitForStmt(*this);
}

void FunDecl::accept(Visitor& visitor) {
    visitor.visitFunDecl(*this);
}

void FunCall::accept(Visitor& visitor) {
    visitor.visitFunCall(*this);
}

void ReturnStmt::accept(Visitor& visitor) {
    visitor.visitReturnStmt(*this);
}

void PrintStmt::accept(Visitor& visitor) {
    visitor.visitPrintStmt(*this);
}

void Block::accept(Visitor& visitor) {
    visitor.visitBlock(*this);
}

// 新增节点的 accept 实现

void ArrayLiteral::accept(Visitor& visitor) {
    visitor.visitArrayLiteral(*this);
}

void DictLiteral::accept(Visitor& visitor) {
    visitor.visitDictLiteral(*this);
}

void IndexAccess::accept(Visitor& visitor) {
    visitor.visitIndexAccess(*this);
}

void IndexAssign::accept(Visitor& visitor) {
    visitor.visitIndexAssign(*this);
}

void ClassDecl::accept(Visitor& visitor) {
    visitor.visitClassDecl(*this);
}

void MemberAccess::accept(Visitor& visitor) {
    visitor.visitMemberAccess(*this);
}

void MemberAssign::accept(Visitor& visitor) {
    visitor.visitMemberAssign(*this);
}

void MethodCall::accept(Visitor& visitor) {
    visitor.visitMethodCall(*this);
}

void NullLiteral::accept(Visitor& visitor) {
    visitor.visitNullLiteral(*this);
}

void SuperExpr::accept(Visitor& visitor) {
    visitor.visitSuperExpr(*this);
}

void BreakStmt::accept(Visitor& visitor) {
    visitor.visitBreakStmt(*this);
}

void ContinueStmt::accept(Visitor& visitor) {
    visitor.visitContinueStmt(*this);
}

void TryStmt::accept(Visitor& visitor) {
    visitor.visitTryStmt(*this);
}

void ThrowStmt::accept(Visitor& visitor) {
    visitor.visitThrowStmt(*this);
}

void ImportStmt::accept(Visitor& visitor) {
    visitor.visitImportStmt(*this);
}

void ExportStmt::accept(Visitor& visitor) {
    visitor.visitExportStmt(*this);
}

// C5 fix: 插值字符串节点的 accept 实现
void InterpolatedString::accept(Visitor& visitor) {
    visitor.visitInterpolatedString(*this);
}
