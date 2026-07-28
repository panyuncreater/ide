// ============================================================
// TypeChecker.cpp — MiniLangTypeChecker 基础类型推断 + 字面量类型检查实现
// ------------------------------------------------------------
// 2026-06-29: 接线 TypeChecker 产生警告（不再是死代码）
// 2026-07-25: 扩展为基础类型推断引擎：
//   - inferExprType: 从表达式推断 TypeInfo
//   - 函数返回类型检查：FunDecl.returnType vs ReturnStmt 推断类型
//   - 赋值类型兼容性：对所有可推断类型的右值检查兼容性
// ============================================================

#include "common/TypeChecker.h"
#include "ast/ASTNode.h"
#include "common/Diagnostic.h"
#include "interpreter/Visitor.h" // DefaultVisitor

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
    std::unordered_map<std::string, std::string> varTypes; // 变量名→类型注解
    std::unordered_map<std::string, TypeInfo> varTypeInfos; // 变量名→推断类型
    std::unordered_map<std::string, std::string> funcReturnTypes; // 函数名→返回类型注解

    /// 从表达式 AST 节点推断类型
    TypeInfo inferExprType(ASTNode* expr) {
        if (!expr)
            return TypeInfo{TypeKind::UNKNOWN};
        // 字面量
        if (auto* num = dynamic_cast<NumberLiteral*>(expr))
            return TypeInfo{num->isFloat_ ? TypeKind::FLOAT : TypeKind::INT};
        if (dynamic_cast<StringLiteral*>(expr))
            return TypeInfo{TypeKind::STRING};
        if (dynamic_cast<BoolLiteral*>(expr))
            return TypeInfo{TypeKind::BOOL};
        if (dynamic_cast<NullLiteral*>(expr))
            return TypeInfo{TypeKind::NULL_T};
        if (dynamic_cast<TupleLiteral*>(expr))
            return TypeInfo{TypeKind::ARRAY}; // tuple 视为 array 子类型
        // 变量引用：查符号表
        if (auto* id = dynamic_cast<VarRef*>(expr)) {
            auto it = varTypeInfos.find(id->name);
            if (it != varTypeInfos.end())
                return it->second;
            return TypeInfo{TypeKind::UNKNOWN};
        }
        // 二元运算
        if (auto* bin = dynamic_cast<BinaryOp*>(expr)) {
            TypeInfo lhs = inferExprType(bin->left.get());
            TypeInfo rhs = inferExprType(bin->right.get());
            // 字符串拼接
            if (bin->opType == BinOpType::BIN_ADD && (lhs.kind == TypeKind::STRING || rhs.kind == TypeKind::STRING))
                return TypeInfo{TypeKind::STRING};
            // 数值运算：有 float 则结果为 float
            if (lhs.isNumeric() && rhs.isNumeric()) {
                if (lhs.kind == TypeKind::FLOAT || rhs.kind == TypeKind::FLOAT)
                    return TypeInfo{TypeKind::FLOAT};
                return TypeInfo{TypeKind::INT};
            }
            // 比较运算符返回 bool
            if (bin->opType == BinOpType::BIN_EQ || bin->opType == BinOpType::BIN_NEQ ||
                bin->opType == BinOpType::BIN_LT || bin->opType == BinOpType::BIN_GT ||
                bin->opType == BinOpType::BIN_LTE || bin->opType == BinOpType::BIN_GTE)
                return TypeInfo{TypeKind::BOOL};
            // 逻辑运算符返回操作数类型（MiniLang 短路返回原值）
            if (bin->opType == BinOpType::BIN_AND || bin->opType == BinOpType::BIN_OR)
                return lhs;
            return TypeInfo{TypeKind::UNKNOWN};
        }
        // 一元运算
        if (auto* unary = dynamic_cast<UnaryOp*>(expr)) {
            TypeInfo operand = inferExprType(unary->operand.get());
            if (unary->opType == UnaryOp::UnaryOpType::UOP_NEGATE)
                return operand; // 保持数值类型
            if (unary->opType == UnaryOp::UnaryOpType::UOP_NOT)
                return TypeInfo{TypeKind::BOOL};
            return operand;
        }
        // 函数调用：查函数返回类型注解
        if (auto* call = dynamic_cast<FunCall*>(expr)) {
            auto it = funcReturnTypes.find(call->name);
            if (it != funcReturnTypes.end())
                return TypeInfo::fromAnnotation(it->second);
            return TypeInfo{TypeKind::UNKNOWN};
        }
        return TypeInfo{TypeKind::UNKNOWN};
    }

    /// 检查单个变量声明的字面量类型匹配。
    /// 无注解或无初始化器时仅登记注解供后续 Assignment 复用；
    /// 否则用 dynamic_cast 判定初始化器字面量类型，与注解比对，不匹配则发警告。
    /// NullLiteral 兼容任何注解（视为通过）。最终把注解写入 varTypes。
    void checkVarDecl(VarDecl& node) {
        if (node.typeAnnotation.empty() || !node.initializer) {
            // 无注解或无初始化器：仅记录注解供 Assignment 用
            if (!node.typeAnnotation.empty()) {
                varTypes[node.name] = node.typeAnnotation;
                varTypeInfos[node.name] = TypeInfo::fromAnnotation(node.typeAnnotation);
            } else if (node.initializer) {
                // 无注解但有初始化器：从初始化表达式推断类型
                TypeInfo inferred = inferExprType(node.initializer.get());
                if (inferred.kind != TypeKind::UNKNOWN)
                    varTypeInfos[node.name] = inferred;
            }
            return;
        }
        // 有注解+有初始化器：用 inferExprType 推断右值类型并检查兼容性
        TypeInfo declaredType = TypeInfo::fromAnnotation(node.typeAnnotation);
        TypeInfo actualType = inferExprType(node.initializer.get());
        if (actualType.kind != TypeKind::UNKNOWN && actualType.kind != TypeKind::NULL_T) {
            if (!declaredType.isCompatible(actualType)) {
                diagnostics.addWarning("变量 " + node.name + " 类型注解为 " + node.typeAnnotation + "，但初始化值为 " +
                                           actualType.toString(),
                                       node.line, node.column, DiagSource::TypeChecker);
            }
        }
        varTypes[node.name] = node.typeAnnotation;
        varTypeInfos[node.name] = declaredType;
    }

    /// 访问变量声明：先执行字面量类型检查，再递归遍历初始化表达式
    /// （初始化器可能是嵌套声明/块，需一并检查）。
    void visitVarDecl(VarDecl& node) override {
        checkVarDecl(node);
        // 递归检查初始化表达式（可能含嵌套声明）
        if (node.initializer)
            node.initializer->accept(*this);
    }

    /// 访问赋值语句：若左值变量有已知注解，检查右侧字面量是否兼容。
    /// 非字面量右侧跳过（运行时检查）。随后递归遍历右侧表达式。
    void visitAssignment(Assignment& node) override {
        auto it = varTypes.find(node.name);
        if (it != varTypes.end()) {
            // 有类型注解：用 inferExprType 推断右值类型并检查兼容性
            TypeInfo declaredType = TypeInfo::fromAnnotation(it->second);
            TypeInfo actualType = inferExprType(node.value.get());
            if (actualType.kind != TypeKind::UNKNOWN && actualType.kind != TypeKind::NULL_T) {
                if (!declaredType.isCompatible(actualType)) {
                    diagnostics.addWarning("赋值给 " + node.name + " 类型注解为 " + it->second + "，但赋值值为 " +
                                               actualType.toString(),
                                           node.line, node.column, DiagSource::TypeChecker);
                }
            }
        }
        if (node.value)
            node.value->accept(*this);
    }

    void visitBlock(Block& node) override {
        // AUDIT-BUG-E1 fix: 块作用域隔离——保存/恢复 varTypes，
        // 防止内层块（if/while/for body）的变量类型注解污染外层。
        // visitFunDecl 已有此模式，此处对齐。
        auto saved = varTypes;
        auto savedInfos = varTypeInfos; // P1 #27 fix: 同时保存/恢复 varTypeInfos，防止块作用域推断类型泄漏
        for (auto& stmt : node.statements) {
            if (stmt)
                stmt->accept(*this);
        }
        varTypes = std::move(saved);
        varTypeInfos = std::move(savedInfos);
    }

    void visitIfStmt(IfStmt& node) override {
        if (node.condition)
            node.condition->accept(*this);
        // AUDIT-BUG-E1 fix: then/else 分支是独立作用域
        auto saved = varTypes;
        auto savedInfos = varTypeInfos; // P2 #63 fix: 对齐 visitBlock，同时保存/恢复 varTypeInfos
        if (node.thenBranch)
            node.thenBranch->accept(*this);
        varTypes = saved;
        varTypeInfos = savedInfos; // P2 #63 fix: 恢复 then 分支前的类型推断状态
        if (node.elseBranch)
            node.elseBranch->accept(*this);
        varTypes = std::move(saved);
        varTypeInfos = std::move(savedInfos); // P2 #63 fix: 恢复外层作用域
    }

    void visitWhileStmt(WhileStmt& node) override {
        if (node.condition)
            node.condition->accept(*this);
        // AUDIT-BUG-E1 fix: 循环体是独立作用域
        auto saved = varTypes;
        if (node.body)
            node.body->accept(*this);
        varTypes = std::move(saved);
    }

    void visitForStmt(ForStmt& node) override {
        // AUDIT-P3-CORRECT fix: for 循环 initializer/condition/update/body 共享一个作用域。
        // 原实现（AUDIT-BUG-E1 fix）错误地将各部分隔离，导致 initializer 声明的变量
        // （如 `var i: int = 0`）在 condition/update/body 中不可见，类型检查被跳过。
        // MiniLang（与 C/Java/JS 一致）中 for 循环四部分共享同一作用域，
        // 循环结束后该作用域销毁（i 不在外层可见）。
        auto saved = varTypes;
        if (node.initializer)
            node.initializer->accept(*this);
        // 不恢复 varTypes — initializer 声明的变量在 condition/update/body 中可见
        if (node.condition)
            node.condition->accept(*this);
        if (node.update)
            node.update->accept(*this);
        if (node.body)
            node.body->accept(*this);
        varTypes = std::move(saved); // 循环结束后恢复，i 不在外层可见
    }

    void visitFunDecl(FunDecl& node) override {
        // 记录函数返回类型注解（供 inferExprType 查询）
        if (!node.returnType.empty()) {
            funcReturnTypes[node.name] = node.returnType;
        }
        // 保存外层 varTypes/varTypeInfos，函数体内独立作用域
        auto savedVarTypes = varTypes;
        auto savedVarTypeInfos = varTypeInfos;
        // 注册参数类型到符号表
        for (size_t i = 0; i < node.params.size() && i < node.paramTypes.size(); ++i) {
            if (!node.paramTypes[i].empty()) {
                varTypes[node.params[i]] = node.paramTypes[i];
                varTypeInfos[node.params[i]] = TypeInfo::fromAnnotation(node.paramTypes[i]);
            }
        }
        // 返回类型检查：如果有 returnType 注解，检查函数体中 ReturnStmt 的返回值类型
        std::string currentReturnType;
        if (!node.returnType.empty()) {
            currentReturnType = node.returnType;
        }
        // 设置当前函数返回类型上下文（用于 visitReturnStmt）
        auto savedReturnType = currentCheckingReturnType_;
        currentCheckingReturnType_ = currentReturnType;
        if (node.body)
            node.body->accept(*this);
        currentCheckingReturnType_ = savedReturnType;
        varTypes = std::move(savedVarTypes);
        varTypeInfos = std::move(savedVarTypeInfos);
    }

    void visitReturnStmt(ReturnStmt& node) override {
        // 检查返回值类型与当前函数的 returnType 注解是否兼容
        if (!currentCheckingReturnType_.empty() && node.value) {
            TypeInfo declaredReturn = TypeInfo::fromAnnotation(currentCheckingReturnType_);
            TypeInfo actualReturn = inferExprType(node.value.get());
            if (actualReturn.kind != TypeKind::UNKNOWN && actualReturn.kind != TypeKind::NULL_T) {
                if (!declaredReturn.isCompatible(actualReturn)) {
                    diagnostics.addWarning("函数返回类型注解为 " + currentCheckingReturnType_ +
                                               "，但返回值为 " + actualReturn.toString(),
                                           node.line, node.column, DiagSource::TypeChecker);
                }
            }
        }
        if (node.value)
            node.value->accept(*this);
    }

    void visitClassDecl(ClassDecl& node) override {
        for (auto& m : node.members) {
            if (m)
                m->accept(*this);
        }
    }

    // BUG-FE-AUDIT-3 fix: try/catch/finally 三个块均为独立作用域，需递归检查
    // 其中 VarDecl 的字面量类型注解冲突。原实现未 override，走 DefaultVisitor
    // no-op，导致 try 块成为类型检查"黑洞"。
    void visitTryStmt(TryStmt& node) override {
        auto saved = varTypes;
        if (node.tryBlock)
            node.tryBlock->accept(*this);
        varTypes = saved;
        // catch 块引入新作用域，catchVarName 是局部变量（无类型注解，不需登记）
        if (node.catchBlock)
            node.catchBlock->accept(*this);
        varTypes = saved;
        if (node.finallyBlock)
            node.finallyBlock->accept(*this);
        varTypes = std::move(saved);
    }

    // BUG-FE-AUDIT-4 fix: export 包装的声明需递归检查，否则 export var x: int = "str"
    // 这类明显的类型注解冲突不会在编译期被捕获。
    void visitExportStmt(ExportStmt& node) override {
        if (node.declaration)
            node.declaration->accept(*this);
    }

    void visitThrowStmt(ThrowStmt& node) override {
        if (node.expression)
            node.expression->accept(*this);
    }

private:
    std::string currentCheckingReturnType_; // 当前正在检查的函数返回类型

    // 字面量类型兼容性检查（编译期，仅字面量）
    /// 判断字面量实际类型 actual 是否兼容类型注解 annotation。
    /// 规则：无注解→兼容；int 注解只接受 int；float 注解接受 float 与 int（int 可提升）；
    /// bool/string 严格匹配；array/dict/类名等无法在字面量层面判定→保守返回兼容
    /// （避免误报，交由运行时检查）。返回 false 时调用方发警告。
    static bool typeMatchLiteral(const std::string& actual, const std::string& annotation) {
        if (annotation.empty())
            return true;
        if (annotation == TypeName::INT)
            return actual == TypeName::INT;
        if (annotation == TypeName::FLOAT)
            return actual == TypeName::FLOAT || actual == TypeName::INT;
        if (annotation == TypeName::BOOL)
            return actual == TypeName::BOOL;
        if (annotation == TypeName::STRING)
            return actual == TypeName::STRING;
        // R98 元组与解构：tuple 注解只接受 tuple 字面量
        if (annotation == TypeName::TUPLE)
            return actual == TypeName::TUPLE;
        return true; // array/dict/类名等无法在字面量层面检查
    }
};

/// 入口：对整棵 AST（顶层 Block）运行字面量类型检查。
/// 构造 LiteralTypeWalker 并派发遍历；walker 不修改 AST（故对 program 做 const_cast
/// 以满足 DefaultVisitor 的 non-const 接口），仅收集 diagnostic 警告。
/// 返回 DiagnosticBag，调用方据此向用户展示类型注解不匹配的警告。
DiagnosticBag MiniLangTypeChecker::check(const Block& program) {
    lastWalkerTypeInfos_.clear();
    LiteralTypeWalker walker;
    // const_cast: DefaultVisitor 需要 non-const 引用，但 walker 不修改 AST
    const_cast<Block&>(program).accept(walker);
    lastWalkerTypeInfos_ = std::move(walker.varTypeInfos);
    return std::move(walker.diagnostics);
}

/// 按变量名推断类型（基于上次 check() 运行的符号表）。
TypeInfo MiniLangTypeChecker::inferType(const std::string& name) const {
    auto it = lastWalkerTypeInfos_.find(name);
    if (it != lastWalkerTypeInfos_.end())
        return it->second;
    return TypeInfo{};
}

} // namespace minilang
