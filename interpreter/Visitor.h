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
    virtual Value visitSuperExpr(SuperExpr& node) = 0;
};

// ============================================================
// DefaultVisitor 默认访问者基类
// ============================================================
// 为所有 25 个 visit 方法提供默认实现，统一委托到 defaultVisit()。
// 子类只需覆盖 defaultVisit()（通用处理）或个别 visit 方法，
// 无需实现全部 25 个接口。适用于类型检查器、静态分析器、
// 代码格式分析等只需处理部分节点的场景。

class DefaultVisitor : public Visitor {
public:
    ~DefaultVisitor() override = default;

    /// 默认处理方法，子类可覆盖以实现通用行为
    virtual Value defaultVisit(ASTNode& /*node*/) {
        return Value::nullValue();
    }

    // ---- 25 个 visit 方法的默认实现 ----

    Value visitBinaryOp(BinaryOp& node) override { return defaultVisit(node); }
    Value visitUnaryOp(UnaryOp& node) override { return defaultVisit(node); }
    Value visitNumberLiteral(NumberLiteral& node) override { return defaultVisit(node); }
    Value visitStringLiteral(StringLiteral& node) override { return defaultVisit(node); }
    Value visitBoolLiteral(BoolLiteral& node) override { return defaultVisit(node); }
    Value visitVarDecl(VarDecl& node) override { return defaultVisit(node); }
    Value visitAssignment(Assignment& node) override { return defaultVisit(node); }
    Value visitVarRef(VarRef& node) override { return defaultVisit(node); }
    Value visitIfStmt(IfStmt& node) override { return defaultVisit(node); }
    Value visitWhileStmt(WhileStmt& node) override { return defaultVisit(node); }
    Value visitForStmt(ForStmt& node) override { return defaultVisit(node); }
    Value visitFunDecl(FunDecl& node) override { return defaultVisit(node); }
    Value visitFunCall(FunCall& node) override { return defaultVisit(node); }
    Value visitReturnStmt(ReturnStmt& node) override { return defaultVisit(node); }
    Value visitPrintStmt(PrintStmt& node) override { return defaultVisit(node); }
    Value visitBlock(Block& node) override { return defaultVisit(node); }
    Value visitArrayLiteral(ArrayLiteral& node) override { return defaultVisit(node); }
    Value visitDictLiteral(DictLiteral& node) override { return defaultVisit(node); }
    Value visitIndexAccess(IndexAccess& node) override { return defaultVisit(node); }
    Value visitIndexAssign(IndexAssign& node) override { return defaultVisit(node); }
    Value visitClassDecl(ClassDecl& node) override { return defaultVisit(node); }
    Value visitMemberAccess(MemberAccess& node) override { return defaultVisit(node); }
    Value visitMemberAssign(MemberAssign& node) override { return defaultVisit(node); }
    Value visitMethodCall(MethodCall& node) override { return defaultVisit(node); }
    Value visitNullLiteral(NullLiteral& node) override { return defaultVisit(node); }
    Value visitSuperExpr(SuperExpr& node) override { return defaultVisit(node); }
};
