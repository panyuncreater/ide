#pragma once

#include "ast/ASTNode.h"

// ============================================================
// Visitor 抽象基类
// ============================================================
// A1 fix: 所有 visit 方法返回 void，结果通过子类成员变量传递。
// 这打破了 Visitor 对 Value 的依赖，使 Formatter/Compiler/AstViewer 等
// 无需引入 interpreter/Value.h。Interpreter 用 lastValue_ 成员保存求值结果，
// Formatter 用 lastFormatResult_，Compiler 忽略返回值。

/// 访问者抽象基类，定义 25 个纯虚 visit 方法
class Visitor {
public:
    virtual ~Visitor() = default;

    // 原有 16 个 visit 方法
    virtual void visitBinaryOp(BinaryOp& node) = 0;
    virtual void visitUnaryOp(UnaryOp& node) = 0;
    virtual void visitNumberLiteral(NumberLiteral& node) = 0;
    virtual void visitStringLiteral(StringLiteral& node) = 0;
    virtual void visitBoolLiteral(BoolLiteral& node) = 0;
    virtual void visitVarDecl(VarDecl& node) = 0;
    virtual void visitAssignment(Assignment& node) = 0;
    virtual void visitVarRef(VarRef& node) = 0;
    virtual void visitIfStmt(IfStmt& node) = 0;
    virtual void visitWhileStmt(WhileStmt& node) = 0;
    virtual void visitForStmt(ForStmt& node) = 0;
    virtual void visitFunDecl(FunDecl& node) = 0;
    virtual void visitFunCall(FunCall& node) = 0;
    virtual void visitReturnStmt(ReturnStmt& node) = 0;
    virtual void visitPrintStmt(PrintStmt& node) = 0;
    virtual void visitBlock(Block& node) = 0;

    // 新增 9 个 visit 方法
    virtual void visitArrayLiteral(ArrayLiteral& node) = 0;
    virtual void visitDictLiteral(DictLiteral& node) = 0;
    virtual void visitIndexAccess(IndexAccess& node) = 0;
    virtual void visitIndexAssign(IndexAssign& node) = 0;
    virtual void visitClassDecl(ClassDecl& node) = 0;
    virtual void visitMemberAccess(MemberAccess& node) = 0;
    virtual void visitMemberAssign(MemberAssign& node) = 0;
    virtual void visitMethodCall(MethodCall& node) = 0;
    virtual void visitNullLiteral(NullLiteral& node) = 0;
    virtual void visitSuperExpr(SuperExpr& node) = 0;
    virtual void visitBreakStmt(BreakStmt& node) = 0;
    virtual void visitContinueStmt(ContinueStmt& node) = 0;
    virtual void visitTryStmt(TryStmt& node) = 0;
    virtual void visitThrowStmt(ThrowStmt& node) = 0;
    virtual void visitImportStmt(ImportStmt& node) = 0;
    virtual void visitExportStmt(ExportStmt& node) = 0;
    // C5 fix: 插值字符串节点
    virtual void visitInterpolatedString(InterpolatedString& node) = 0;
    // R98 元组与解构：元组字面量与解构绑定
    virtual void visitTupleLiteral(TupleLiteral& node) = 0;
    virtual void visitDestructureBinding(DestructureBinding& node) = 0;
    // R99 枚举与 ADT + match：enum 声明 / variant 构造 / match 表达式
    virtual void visitEnumDecl(EnumDecl& node) = 0;
    virtual void visitEnumVariantExpr(EnumVariantExpr& node) = 0;
    virtual void visitMatchExpr(MatchExpr& node) = 0;
    // R164 协程/生成器：yield 表达式
    virtual void visitYieldExpr(YieldExpr& node) = 0;
    // 七特性 MVP 阶段 2：宏系统（MacroDecl 运行期 no-op，MacroCallExpr 求值 expanded）
    virtual void visitMacroDecl(MacroDecl& node) = 0;
    virtual void visitMacroCallExpr(MacroCallExpr& node) = 0;
    // 七特性 MVP 阶段 3：Trait/Mixin（TraitDecl 运行期 no-op，方法 parse 期合入）
    virtual void visitTraitDecl(TraitDecl& node) = 0;
    // 七特性 MVP 阶段 4：async/await（await 驱动协程到完成并取最终值）
    virtual void visitAwaitExpr(AwaitExpr& node) = 0;
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
    virtual void defaultVisit(ASTNode& /*node*/) {
        // 默认无操作
    }

    // ---- 25 个 visit 方法的默认实现 ----

    void visitBinaryOp(BinaryOp& node) override { defaultVisit(node); }
    void visitUnaryOp(UnaryOp& node) override { defaultVisit(node); }
    void visitNumberLiteral(NumberLiteral& node) override { defaultVisit(node); }
    void visitStringLiteral(StringLiteral& node) override { defaultVisit(node); }
    void visitBoolLiteral(BoolLiteral& node) override { defaultVisit(node); }
    void visitVarDecl(VarDecl& node) override { defaultVisit(node); }
    void visitAssignment(Assignment& node) override { defaultVisit(node); }
    void visitVarRef(VarRef& node) override { defaultVisit(node); }
    void visitIfStmt(IfStmt& node) override { defaultVisit(node); }
    void visitWhileStmt(WhileStmt& node) override { defaultVisit(node); }
    void visitForStmt(ForStmt& node) override { defaultVisit(node); }
    void visitFunDecl(FunDecl& node) override { defaultVisit(node); }
    void visitFunCall(FunCall& node) override { defaultVisit(node); }
    void visitReturnStmt(ReturnStmt& node) override { defaultVisit(node); }
    void visitPrintStmt(PrintStmt& node) override { defaultVisit(node); }
    void visitBlock(Block& node) override { defaultVisit(node); }
    void visitArrayLiteral(ArrayLiteral& node) override { defaultVisit(node); }
    void visitDictLiteral(DictLiteral& node) override { defaultVisit(node); }
    void visitIndexAccess(IndexAccess& node) override { defaultVisit(node); }
    void visitIndexAssign(IndexAssign& node) override { defaultVisit(node); }
    void visitClassDecl(ClassDecl& node) override { defaultVisit(node); }
    void visitMemberAccess(MemberAccess& node) override { defaultVisit(node); }
    void visitMemberAssign(MemberAssign& node) override { defaultVisit(node); }
    void visitMethodCall(MethodCall& node) override { defaultVisit(node); }
    void visitNullLiteral(NullLiteral& node) override { defaultVisit(node); }
    void visitSuperExpr(SuperExpr& node) override { defaultVisit(node); }
    void visitBreakStmt(BreakStmt& node) override { defaultVisit(node); }
    void visitContinueStmt(ContinueStmt& node) override { defaultVisit(node); }
    void visitTryStmt(TryStmt& node) override { defaultVisit(node); }
    void visitThrowStmt(ThrowStmt& node) override { defaultVisit(node); }
    void visitImportStmt(ImportStmt& node) override { defaultVisit(node); }
    void visitExportStmt(ExportStmt& node) override { defaultVisit(node); }
    void visitInterpolatedString(InterpolatedString& node) override { defaultVisit(node); }
    void visitTupleLiteral(TupleLiteral& node) override { defaultVisit(node); }
    void visitDestructureBinding(DestructureBinding& node) override { defaultVisit(node); }
    void visitEnumDecl(EnumDecl& node) override { defaultVisit(node); }
    void visitEnumVariantExpr(EnumVariantExpr& node) override { defaultVisit(node); }
    void visitMatchExpr(MatchExpr& node) override { defaultVisit(node); }
    void visitYieldExpr(YieldExpr& node) override { defaultVisit(node); }
    // 七特性 MVP 阶段 2：宏系统默认实现。
    // 注：继承 DefaultVisitor 的访问者（Compiler/Formatter/LintPass/DocGenerator）
    // 若需语义处理 MacroCallExpr 必须自行 override（否则默认 no-op 丢失展开子树）。
    void visitMacroDecl(MacroDecl& node) override { defaultVisit(node); }
    void visitMacroCallExpr(MacroCallExpr& node) override { defaultVisit(node); }
    // 七特性 MVP 阶段 3：Trait/Mixin 默认实现（声明节点，多数访问者无需处理）。
    void visitTraitDecl(TraitDecl& node) override { defaultVisit(node); }
    // 七特性 MVP 阶段 4：async/await 默认实现。
    void visitAwaitExpr(AwaitExpr& node) override { defaultVisit(node); }
};
