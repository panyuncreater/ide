#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include <sstream>

// ============================================================
// Formatter 代码格式化器实现
// ============================================================

Formatter::Formatter() {}

void Formatter::setIndentSize(int size) {
    indentSize_ = size;
}

std::string Formatter::indent() const {
    return std::string(currentIndent_ * indentSize_, ' ');
}

std::string Formatter::format(Block& program) {
    currentIndent_ = 0;
    return formatBlock(program, true);
}

std::string Formatter::formatNode(ASTNode* node) {
    if (!node) return "null";

    if (auto* n = dynamic_cast<BinaryOp*>(node)) return formatBinaryOp(*n);
    if (auto* n = dynamic_cast<UnaryOp*>(node)) return formatUnaryOp(*n);
    if (auto* n = dynamic_cast<NumberLiteral*>(node)) return formatNumberLiteral(*n);
    if (auto* n = dynamic_cast<StringLiteral*>(node)) return formatStringLiteral(*n);
    if (auto* n = dynamic_cast<BoolLiteral*>(node)) return formatBoolLiteral(*n);
    if (auto* n = dynamic_cast<VarDecl*>(node)) return formatVarDecl(*n);
    if (auto* n = dynamic_cast<Assignment*>(node)) return formatAssignment(*n);
    if (auto* n = dynamic_cast<VarRef*>(node)) return formatVarRef(*n);
    if (auto* n = dynamic_cast<IfStmt*>(node)) return formatIfStmt(*n);
    if (auto* n = dynamic_cast<WhileStmt*>(node)) return formatWhileStmt(*n);
    if (auto* n = dynamic_cast<ForStmt*>(node)) return formatForStmt(*n);
    if (auto* n = dynamic_cast<FunDecl*>(node)) return formatFunDecl(*n);
    if (auto* n = dynamic_cast<FunCall*>(node)) return formatFunCall(*n);
    if (auto* n = dynamic_cast<ReturnStmt*>(node)) return formatReturnStmt(*n);
    if (auto* n = dynamic_cast<PrintStmt*>(node)) return formatPrintStmt(*n);
    if (auto* n = dynamic_cast<Block*>(node)) return formatBlock(*n);
    if (auto* n = dynamic_cast<ArrayLiteral*>(node)) return formatArrayLiteral(*n);
    if (auto* n = dynamic_cast<DictLiteral*>(node)) return formatDictLiteral(*n);
    if (auto* n = dynamic_cast<IndexAccess*>(node)) return formatIndexAccess(*n);
    if (auto* n = dynamic_cast<IndexAssign*>(node)) return formatIndexAssign(*n);
    if (auto* n = dynamic_cast<ClassDecl*>(node)) return formatClassDecl(*n);
    if (auto* n = dynamic_cast<MemberAccess*>(node)) return formatMemberAccess(*n);
    if (auto* n = dynamic_cast<MemberAssign*>(node)) return formatMemberAssign(*n);
    if (auto* n = dynamic_cast<MethodCall*>(node)) return formatMethodCall(*n);
    if (auto* n = dynamic_cast<NullLiteral*>(node)) return formatNullLiteral(*n);

    return "/* unknown node */";
}

std::string Formatter::formatBinaryOp(BinaryOp& node) {
    std::string left = formatNode(node.left.get());
    std::string right = formatNode(node.right.get());
    return left + " " + node.op + " " + right;
}

std::string Formatter::formatUnaryOp(UnaryOp& node) {
    std::string operand = formatNode(node.operand.get());
    if (node.op == "not") {
        return "not " + operand;
    }
    return "-" + operand;
}

std::string Formatter::formatNumberLiteral(NumberLiteral& node) {
    return node.value.toString();
}

std::string Formatter::formatStringLiteral(StringLiteral& node) {
    return "\"" + node.value + "\"";
}

std::string Formatter::formatBoolLiteral(BoolLiteral& node) {
    return node.value ? "true" : "false";
}

std::string Formatter::formatVarDecl(VarDecl& node) {
    std::string result;
    if (!node.typeAnnotation.empty()) {
        result = node.typeAnnotation + " " + node.name;
    } else {
        result = "var " + node.name;
    }
    if (node.initializer) {
        result += " = " + formatNode(node.initializer.get());
    }
    return result;
}

std::string Formatter::formatAssignment(Assignment& node) {
    return node.name + " = " + formatNode(node.value.get());
}

std::string Formatter::formatVarRef(VarRef& node) {
    return node.name;
}

std::string Formatter::formatIfStmt(IfStmt& node) {
    std::string result = "if (" + formatNode(node.condition.get()) + ") {\n";
    currentIndent_++;
    // 格式化 then 分支
    if (auto* block = dynamic_cast<Block*>(node.thenBranch.get())) {
        result += formatBlock(*block);
    } else {
        result += indent() + formatNode(node.thenBranch.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";

    if (node.elseBranch) {
        result += " else {\n";
        currentIndent_++;
        if (auto* block = dynamic_cast<Block*>(node.elseBranch.get())) {
            result += formatBlock(*block);
        } else {
            result += indent() + formatNode(node.elseBranch.get()) + ";\n";
        }
        currentIndent_--;
        result += indent() + "}";
    }

    return result;
}

std::string Formatter::formatWhileStmt(WhileStmt& node) {
    std::string result = "while (" + formatNode(node.condition.get()) + ") {\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatForStmt(ForStmt& node) {
    std::string result = "for (";
    if (node.initializer) result += formatNode(node.initializer.get());
    result += "; ";
    if (node.condition) result += formatNode(node.condition.get());
    result += "; ";
    if (node.update) result += formatNode(node.update.get());
    result += ") {\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatFunDecl(FunDecl& node) {
    std::string result = "fun " + node.name + "(";
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (i > 0) result += ", ";
        result += node.params[i];
        if (!node.paramTypes.empty() && i < node.paramTypes.size() && !node.paramTypes[i].empty()) {
            result += ": " + node.paramTypes[i];
        }
    }
    result += ")";
    if (!node.returnType.empty()) {
        result += ": " + node.returnType;
    }
    result += " {\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatFunCall(FunCall& node) {
    std::string result = node.name + "(";
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatNode(node.arguments[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatReturnStmt(ReturnStmt& node) {
    if (node.value) {
        return "return " + formatNode(node.value.get());
    }
    return "return";
}

std::string Formatter::formatPrintStmt(PrintStmt& node) {
    return "print(" + formatNode(node.value.get()) + ")";
}

std::string Formatter::formatBlock(Block& node, bool isTopLevel) {
    std::string result;
    for (size_t i = 0; i < node.statements.size(); ++i) {
        result += indent() + formatNode(node.statements[i].get()) + ";\n";
    }
    return result;
}

// ---- 新增节点格式化 ----

std::string Formatter::formatArrayLiteral(ArrayLiteral& node) {
    std::string result = "[";
    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatNode(node.elements[i].get());
    }
    result += "]";
    return result;
}

std::string Formatter::formatDictLiteral(DictLiteral& node) {
    std::string result = "{";
    for (size_t i = 0; i < node.pairs.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatNode(node.pairs[i].first.get()) + ": " + formatNode(node.pairs[i].second.get());
    }
    result += "}";
    return result;
}

std::string Formatter::formatIndexAccess(IndexAccess& node) {
    return formatNode(node.object.get()) + "[" + formatNode(node.index.get()) + "]";
}

std::string Formatter::formatIndexAssign(IndexAssign& node) {
    return formatNode(node.object.get()) + "[" + formatNode(node.index.get()) + "] = " + formatNode(node.value.get());
}

std::string Formatter::formatClassDecl(ClassDecl& node) {
    std::string result = "class " + node.name;
    if (!node.superClassName.empty()) {
        result += " extends " + node.superClassName;
    }
    result += " {\n";
    currentIndent_++;
    for (auto& member : node.members) {
        result += indent() + formatNode(member.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatMemberAccess(MemberAccess& node) {
    return formatNode(node.object.get()) + "." + node.fieldName;
}

std::string Formatter::formatMemberAssign(MemberAssign& node) {
    return formatNode(node.object.get()) + "." + node.fieldName + " = " + formatNode(node.value.get());
}

std::string Formatter::formatMethodCall(MethodCall& node) {
    std::string result = formatNode(node.object.get()) + "." + node.methodName + "(";
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatNode(node.arguments[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatNullLiteral(NullLiteral& node) {
    return "null";
}
