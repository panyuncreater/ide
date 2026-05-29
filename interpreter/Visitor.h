#pragma once

#include "ast/ASTNode.h"

// ============================================================
// Visitor 抽象基类
// ============================================================

/// 访问者抽象基类，定义 25 个纯虚 visit 方法
class Visitor {
public:
    virtual ~Visitor() = default;

    // 原有 16 个 visit 方法
    virtual Value visitBinaryOp(BinaryOp& node) = 0;
    virtual Value visitUnaryOp(UnaryOp& node) = 0;
    virtual Value visitNumberLiteral(NumberLiteral& node) = 0;
    virtual Value visitStringLiteral(StringLiteral& node) = 0;
    virtual Value visitBoolLiteral(BoolLiteral& node) = 0;
    virtual Value visitVarDecl(VarDecl& node) = 0;
    virtual Value visitAssignment(Assignment& node) = 0;
    virtual Value visitVarRef(VarRef& node) = 0;
    virtual Value visitIfStmt(IfStmt& node) = 0;
    virtual Value visitWhileStmt(WhileStmt& node) = 0;
    virtual Value visitForStmt(ForStmt& node) = 0;
    virtual Value visitFunDecl(FunDecl& node) = 0;
    virtual Value visitFunCall(FunCall& node) = 0;
    virtual Value visitReturnStmt(ReturnStmt& node) = 0;
    virtual Value visitPrintStmt(PrintStmt& node) = 0;
    virtual Value visitBlock(Block& node) = 0;

    // 新增 9 个 visit 方法
    virtual Value visitArrayLiteral(ArrayLiteral& node) = 0;
    virtual Value visitDictLiteral(DictLiteral& node) = 0;
    virtual Value visitIndexAccess(IndexAccess& node) = 0;
    virtual Value visitIndexAssign(IndexAssign& node) = 0;
    virtual Value visitClassDecl(ClassDecl& node) = 0;
    virtual Value visitMemberAccess(MemberAccess& node) = 0;
    virtual Value visitMemberAssign(MemberAssign& node) = 0;
    virtual Value visitMethodCall(MethodCall& node) = 0;
    virtual Value visitNullLiteral(NullLiteral& node) = 0;
};
