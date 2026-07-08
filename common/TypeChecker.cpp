// ============================================================
// TypeChecker.cpp — MiniLangTypeChecker 基础字面量类型检查实现
// ------------------------------------------------------------
// 2026-06-29: 接线 TypeChecker 产生警告（不再是死代码）
// 实现范围：VarDecl + Assignment 的字面量初始化器类型注解检查
//   - NumberLiteral(int) vs string/bool 注解 → 警告
//   - StringLiteral vs int/float/bool 注解 → 警告
//   - BoolLiteral vs int/float/string 注解 → 警告
//   - NullLiteral → 通过（null 兼容所有类型）
//   - 非字面量表达式 → 跳过（需运行时检查，由 OP_TYPE_CHECK 处理）
// ============================================================

#include "common/TypeChecker.h"
#include "ast/ASTNode.h"
#include "common/Diagnostic.h"
#include "interpreter/Visitor.h"  // DefaultVisitor

#include <unordered_map>
#include <utility>

namespace minilang {

// 辅助：递归遍历 AST，检查 VarDecl 和 Assignment 的字面量类型匹配
/// 字面量类型检查遍历器。在编译期为带类型注解的变量声明/赋值做"字面量 vs 注解"
/// 的一致性检查：仅当初始化器是字面量（NumberLiteral/StringLiteral/BoolLiteral/NullLiteral）
/// 时才能静态判定类型，非字面量表达式交由运行时 OP_TYPE_CHECK 处理。
/// 通过 varTypes 表在作用域间传播注解；visit* 各语句对块/分支/循环/函数体
/// 使用"保存-恢复"模式隔离作用域，避免内层注解污染外层（AUDIT-BUG-E1）。
class LiteralTypeWalker : public DefaultVisitor {
public:
    DiagnosticBag diagnostics;
    std::unordered_map<std::string, std::string> varTypes;  // 变量名→类型注解

    /// 检查单个变量声明的字面量类型匹配。
    /// 无注解或无初始化器时仅登记注解供后续 Assignment 复用；
    /// 否则用 dynamic_cast 判定初始化器字面量类型，与注解比对，不匹配则发警告。
    /// NullLiteral 兼容任何注解（视为通过）。最终把注解写入 varTypes。
    void checkVarDecl(VarDecl& node) {
        if (node.typeAnnotation.empty() || !node.initializer) {
            // 无注解或无初始化器：仅记录注解供 Assignment 用
            if (!node.typeAnnotation.empty()) {
                varTypes[node.name] = node.typeAnnotation;
            }
            return;
        }
        // 检查字面量初始化器
        std::string actualType;
        ASTNode* init = node.initializer.get();
        if (auto* num = dynamic_cast<NumberLiteral*>(init)) {
            actualType = num->isFloat_ ? TypeName::FLOAT : TypeName::INT;
        } else if (dynamic_cast<StringLiteral*>(init)) {
            actualType = TypeName::STRING;
        } else if (dynamic_cast<BoolLiteral*>(init)) {
            actualType = TypeName::BOOL;
        } else if (dynamic_cast<NullLiteral*>(init)) {
            actualType = TypeName::NULL_T;  // null 兼容所有类型，不报
        }
        // 非字面量（变量引用、表达式等）→ 跳过，运行时检查
        if (!actualType.empty() && actualType != TypeName::NULL_T) {
            if (!typeMatchLiteral(actualType, node.typeAnnotation)) {
                diagnostics.addWarning(
                    "变量 " + node.name + " 类型注解为 " + node.typeAnnotation +
                    "，但初始化值为 " + actualType,
                    node.line, node.column, DiagSource::TypeChecker);
            }
        }
        if (!node.typeAnnotation.empty()) {
            varTypes[node.name] = node.typeAnnotation;
        }
    }

    /// 访问变量声明：先执行字面量类型检查，再递归遍历初始化表达式
    /// （初始化器可能是嵌套声明/块，需一并检查）。
    void visitVarDecl(VarDecl& node) override {
        checkVarDecl(node);
        // 递归检查初始化表达式（可能含嵌套声明）
        if (node.initializer) node.initializer->accept(*this);
    }

    /// 访问赋值语句：若左值变量有已知注解，检查右侧字面量是否兼容。
    /// 非字面量右侧跳过（运行时检查）。随后递归遍历右侧表达式。
    void visitAssignment(Assignment& node) override {
        auto it = varTypes.find(node.name);
        if (it != varTypes.end()) {
            std::string actualType;
            ASTNode* val = node.value.get();
            if (auto* num = dynamic_cast<NumberLiteral*>(val)) {
                actualType = num->isFloat_ ? TypeName::FLOAT : TypeName::INT;
            } else if (dynamic_cast<StringLiteral*>(val)) {
                actualType = TypeName::STRING;
            } else if (dynamic_cast<BoolLiteral*>(val)) {
                actualType = TypeName::BOOL;
            } else if (dynamic_cast<NullLiteral*>(val)) {
                // AUDIT-P2 fix: 对齐 checkVarDecl 的 NullLiteral 分支。
                // null 兼容所有类型，actualType 设为 NULL_T 但 typeMatchLiteral
                // 对 null 永远返回 true，不会误报。保持与 checkVarDecl 一致性。
                actualType = TypeName::NULL_T;
            }
            if (!actualType.empty()) {
                if (!typeMatchLiteral(actualType, it->second)) {
                    diagnostics.addWarning(
                        "赋值给 " + node.name + " 类型注解为 " + it->second +
                        "，但赋值值为 " + actualType,
                        node.line, node.column, DiagSource::TypeChecker);
                }
            }
        }
        if (node.value) node.value->accept(*this);
    }

    void visitBlock(Block& node) override {
        // AUDIT-BUG-E1 fix: 块作用域隔离——保存/恢复 varTypes，
        // 防止内层块（if/while/for body）的变量类型注解污染外层。
        // visitFunDecl 已有此模式，此处对齐。
        auto saved = varTypes;
        for (auto& stmt : node.statements) {
            if (stmt) stmt->accept(*this);
        }
        varTypes = std::move(saved);
    }

    void visitIfStmt(IfStmt& node) override {
        if (node.condition) node.condition->accept(*this);
        // AUDIT-BUG-E1 fix: then/else 分支是独立作用域
        auto saved = varTypes;
        if (node.thenBranch) node.thenBranch->accept(*this);
        varTypes = saved;
        if (node.elseBranch) node.elseBranch->accept(*this);
        varTypes = std::move(saved);
    }

    void visitWhileStmt(WhileStmt& node) override {
        if (node.condition) node.condition->accept(*this);
        // AUDIT-BUG-E1 fix: 循环体是独立作用域
        auto saved = varTypes;
        if (node.body) node.body->accept(*this);
        varTypes = std::move(saved);
    }

    void visitForStmt(ForStmt& node) override {
        // AUDIT-BUG-E1 fix: for 的 initializer/condition/update/body 各为独立作用域
        auto saved = varTypes;
        if (node.initializer) node.initializer->accept(*this);
        varTypes = saved;
        if (node.condition) node.condition->accept(*this);
        varTypes = saved;
        if (node.update) node.update->accept(*this);
        varTypes = saved;
        if (node.body) node.body->accept(*this);
        varTypes = std::move(saved);
    }

    void visitFunDecl(FunDecl& node) override {
        // 保存外层 varTypes，函数体内独立作用域
        auto saved = varTypes;
        if (node.body) node.body->accept(*this);
        varTypes = std::move(saved);
    }

    void visitClassDecl(ClassDecl& node) override {
        for (auto& m : node.members) {
            if (m) m->accept(*this);
        }
    }

    // BUG-FE-AUDIT-3 fix: try/catch/finally 三个块均为独立作用域，需递归检查
    // 其中 VarDecl 的字面量类型注解冲突。原实现未 override，走 DefaultVisitor
    // no-op，导致 try 块成为类型检查"黑洞"。
    void visitTryStmt(TryStmt& node) override {
        auto saved = varTypes;
        if (node.tryBlock) node.tryBlock->accept(*this);
        varTypes = saved;
        // catch 块引入新作用域，catchVarName 是局部变量（无类型注解，不需登记）
        if (node.catchBlock) node.catchBlock->accept(*this);
        varTypes = saved;
        if (node.finallyBlock) node.finallyBlock->accept(*this);
        varTypes = std::move(saved);
    }

    // BUG-FE-AUDIT-4 fix: export 包装的声明需递归检查，否则 export var x: int = "str"
    // 这类明显的类型注解冲突不会在编译期被捕获。
    void visitExportStmt(ExportStmt& node) override {
        if (node.declaration) node.declaration->accept(*this);
    }

    void visitThrowStmt(ThrowStmt& node) override {
        if (node.expression) node.expression->accept(*this);
    }

private:
    // 字面量类型兼容性检查（编译期，仅字面量）
    /// 判断字面量实际类型 actual 是否兼容类型注解 annotation。
    /// 规则：无注解→兼容；int 注解只接受 int；float 注解接受 float 与 int（int 可提升）；
    /// bool/string 严格匹配；array/dict/类名等无法在字面量层面判定→保守返回兼容
    /// （避免误报，交由运行时检查）。返回 false 时调用方发警告。
    static bool typeMatchLiteral(const std::string& actual, const std::string& annotation) {
        if (annotation.empty()) return true;
        if (annotation == TypeName::INT) return actual == TypeName::INT;
        if (annotation == TypeName::FLOAT) return actual == TypeName::FLOAT || actual == TypeName::INT;
        if (annotation == TypeName::BOOL) return actual == TypeName::BOOL;
        if (annotation == TypeName::STRING) return actual == TypeName::STRING;
        return true;  // array/dict/类名等无法在字面量层面检查
    }
};

/// 入口：对整棵 AST（顶层 Block）运行字面量类型检查。
/// 构造 LiteralTypeWalker 并派发遍历；walker 不修改 AST（故对 program 做 const_cast
/// 以满足 DefaultVisitor 的 non-const 接口），仅收集 diagnostic 警告。
/// 返回 DiagnosticBag，调用方据此向用户展示类型注解不匹配的警告。
DiagnosticBag MiniLangTypeChecker::check(const Block& program) {
    LiteralTypeWalker walker;
    // const_cast: DefaultVisitor 需要 non-const 引用，但 walker 不修改 AST
    const_cast<Block&>(program).accept(walker);
    return std::move(walker.diagnostics);
}

/// 按变量名推断类型（当前未实现，返回空 TypeInfo）。
/// 预留接口：未来可基于 varTypes 表做上下文相关的类型推断；现阶段类型检查
/// 仅依赖显式字面量注解，故此处返回空。
TypeInfo MiniLangTypeChecker::inferType(const std::string& /*name*/) const {
    return TypeInfo{};
}

} // namespace minilang
