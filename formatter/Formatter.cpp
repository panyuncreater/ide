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

/// 判断节点类型是否是自终止的复合语句（以 } 结尾，不需要额外 ;）
static bool isSelfTerminating(ASTNode* node) {
    if (!node) return false;
    switch (node->nodeType) {
    case NodeType::NODE_IF_STMT:
    case NodeType::NODE_WHILE_STMT:
    case NodeType::NODE_FOR_STMT:
    case NodeType::NODE_FUN_DECL:
    case NodeType::NODE_CLASS_DECL:
        return true;
    default:
        return false;
    }
}

std::string Formatter::formatNode(ASTNode* node) {
    if (!node) return "null";

    switch (node->nodeType) {
    case NodeType::NODE_BINARY_OP:     return formatBinaryOp(*static_cast<BinaryOp*>(node));
    case NodeType::NODE_UNARY_OP:      return formatUnaryOp(*static_cast<UnaryOp*>(node));
    case NodeType::NODE_NUMBER_LITERAL: return formatNumberLiteral(*static_cast<NumberLiteral*>(node));
    case NodeType::NODE_STRING_LITERAL: return formatStringLiteral(*static_cast<StringLiteral*>(node));
    case NodeType::NODE_BOOL_LITERAL:   return formatBoolLiteral(*static_cast<BoolLiteral*>(node));
    case NodeType::NODE_VAR_DECL:       return formatVarDecl(*static_cast<VarDecl*>(node));
    case NodeType::NODE_ASSIGNMENT:     return formatAssignment(*static_cast<Assignment*>(node));
    case NodeType::NODE_VAR_REF:       return formatVarRef(*static_cast<VarRef*>(node));
    case NodeType::NODE_IF_STMT:        return formatIfStmt(*static_cast<IfStmt*>(node));
    case NodeType::NODE_WHILE_STMT:     return formatWhileStmt(*static_cast<WhileStmt*>(node));
    case NodeType::NODE_FOR_STMT:       return formatForStmt(*static_cast<ForStmt*>(node));
    case NodeType::NODE_FUN_DECL:       return formatFunDecl(*static_cast<FunDecl*>(node));
    case NodeType::NODE_FUN_CALL:       return formatFunCall(*static_cast<FunCall*>(node));
    case NodeType::NODE_RETURN_STMT:    return formatReturnStmt(*static_cast<ReturnStmt*>(node));
    case NodeType::NODE_PRINT_STMT:     return formatPrintStmt(*static_cast<PrintStmt*>(node));
    case NodeType::NODE_BLOCK:          return formatBlock(*static_cast<Block*>(node));
    case NodeType::NODE_ARRAY_LITERAL:  return formatArrayLiteral(*static_cast<ArrayLiteral*>(node));
    case NodeType::NODE_DICT_LITERAL:   return formatDictLiteral(*static_cast<DictLiteral*>(node));
    case NodeType::NODE_INDEX_ACCESS:   return formatIndexAccess(*static_cast<IndexAccess*>(node));
    case NodeType::NODE_INDEX_ASSIGN:  return formatIndexAssign(*static_cast<IndexAssign*>(node));
    case NodeType::NODE_CLASS_DECL:     return formatClassDecl(*static_cast<ClassDecl*>(node));
    case NodeType::NODE_MEMBER_ACCESS: return formatMemberAccess(*static_cast<MemberAccess*>(node));
    case NodeType::NODE_MEMBER_ASSIGN: return formatMemberAssign(*static_cast<MemberAssign*>(node));
    case NodeType::NODE_METHOD_CALL:    return formatMethodCall(*static_cast<MethodCall*>(node));
    case NodeType::NODE_NULL_LITERAL:   return formatNullLiteral(*static_cast<NullLiteral*>(node));
    }

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
    if (auto* block = dynamic_cast<Block*>(node.thenBranch.get())) {
        result += formatBlock(*block);
    } else {
        result += indent() + formatNode(node.thenBranch.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";

    if (node.elseBranch) {
        // else if 分支：elseBranch 是 IfStmt，直接输出 "else if ..."
        if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
            result += " else " + formatIfStmt(*static_cast<IfStmt*>(node.elseBranch.get()));
        } else if (auto* block = dynamic_cast<Block*>(node.elseBranch.get())) {
            result += " else {\n";
            currentIndent_++;
            result += formatBlock(*block);
            currentIndent_--;
            result += indent() + "}";
        } else {
            result += " else {\n";
            currentIndent_++;
            result += indent() + formatNode(node.elseBranch.get()) + ";\n";
            currentIndent_--;
            result += indent() + "}";
        }
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
    std::string result = "print(";
    for (size_t i = 0; i < node.values.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatNode(node.values[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatBlock(Block& node, bool isTopLevel) {
    std::string result;
    for (size_t i = 0; i < node.statements.size(); ++i) {
        // 复合语句（if/while/for/fun/class）以 } 结尾，不需要额外 ;
        if (isSelfTerminating(node.statements[i].get())) {
            result += indent() + formatNode(node.statements[i].get()) + "\n";
        } else {
            result += indent() + formatNode(node.statements[i].get()) + ";\n";
        }
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
        // 方法（FunDecl）以 } 结尾，不需要额外 ;
        if (isSelfTerminating(member.get())) {
            result += indent() + formatNode(member.get()) + "\n";
        } else {
            result += indent() + formatNode(member.get()) + ";\n";
        }
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
