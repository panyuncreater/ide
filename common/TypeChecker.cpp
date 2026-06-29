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
class LiteralTypeWalker : public DefaultVisitor {
public:
    DiagnosticBag diagnostics;
    std::unordered_map<std::string, std::string> varTypes;  // 变量名→类型注解

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

    void visitVarDecl(VarDecl& node) override {
        checkVarDecl(node);
        // 递归检查初始化表达式（可能含嵌套声明）
        if (node.initializer) node.initializer->accept(*this);
    }

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
        for (auto& stmt : node.statements) {
            if (stmt) stmt->accept(*this);
        }
    }

    void visitIfStmt(IfStmt& node) override {
        if (node.condition) node.condition->accept(*this);
        if (node.thenBranch) node.thenBranch->accept(*this);
        if (node.elseBranch) node.elseBranch->accept(*this);
    }

    void visitWhileStmt(WhileStmt& node) override {
        if (node.condition) node.condition->accept(*this);
        if (node.body) node.body->accept(*this);
    }

    void visitForStmt(ForStmt& node) override {
        if (node.initializer) node.initializer->accept(*this);
        if (node.condition) node.condition->accept(*this);
        if (node.update) node.update->accept(*this);
        if (node.body) node.body->accept(*this);
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

private:
    // 字面量类型兼容性检查（编译期，仅字面量）
    static bool typeMatchLiteral(const std::string& actual, const std::string& annotation) {
        if (annotation.empty()) return true;
        if (annotation == TypeName::INT) return actual == TypeName::INT;
        if (annotation == TypeName::FLOAT) return actual == TypeName::FLOAT || actual == TypeName::INT;
        if (annotation == TypeName::BOOL) return actual == TypeName::BOOL;
        if (annotation == TypeName::STRING) return actual == TypeName::STRING;
        return true;  // array/dict/类名等无法在字面量层面检查
    }
};

DiagnosticBag MiniLangTypeChecker::check(const Block& program) {
    LiteralTypeWalker walker;
    // const_cast: DefaultVisitor 需要 non-const 引用，但 walker 不修改 AST
    const_cast<Block&>(program).accept(walker);
    return std::move(walker.diagnostics);
}

TypeInfo MiniLangTypeChecker::inferType(const std::string& /*name*/) const {
    return TypeInfo{};
}

} // namespace minilang
