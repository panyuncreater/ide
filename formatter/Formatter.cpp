#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include <sstream>

// ============================================================
// Formatter 代码格式化器实现
// ============================================================

Formatter::Formatter() {}

void Formatter::setComments(const std::vector<Token>& tokens) {
    comments_.clear();
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::TK_LINE_COMMENT) {
            comments_.push_back(tok);
        }
    }
    commentIndex_ = 0;
}

void Formatter::setIndentSize(int size) {
    options_.indentSize = size;
}

void Formatter::setOptions(const FormatOptions& options) {
    options_ = options;
}

const FormatOptions& Formatter::getOptions() const {
    return options_;
}

std::string Formatter::indent() const {
    if (options_.useTabs) {
        return std::string(currentIndent_, '\t');
    }
    return std::string(currentIndent_ * options_.indentSize, ' ');
}

std::string Formatter::binOp(const std::string& op) const {
    if (options_.spaceAroundOperators) {
        return " " + op + " ";
    }
    return op;
}

std::string Formatter::comma() const {
    return options_.spaceAfterComma ? ", " : ",";
}

std::string Formatter::openBrace() const {
    if (options_.braceStyle == BraceStyle::NEXT_LINE) {
        return "\n" + indent() + "{";
    }
    return " {";
}

std::string Formatter::format(Block& program) {
    currentIndent_ = 0;
    return formatBlock(program);
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
    case NodeType::NODE_BLOCK:
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
    case NodeType::NODE_BLOCK: {
        std::string result = "{\n";
        currentIndent_++;
        result += formatBlock(*static_cast<Block*>(node));
        currentIndent_--;
        result += indent() + "}";
        return result;
    }
    case NodeType::NODE_ARRAY_LITERAL:  return formatArrayLiteral(*static_cast<ArrayLiteral*>(node));
    case NodeType::NODE_DICT_LITERAL:   return formatDictLiteral(*static_cast<DictLiteral*>(node));
    case NodeType::NODE_INDEX_ACCESS:   return formatIndexAccess(*static_cast<IndexAccess*>(node));
    case NodeType::NODE_INDEX_ASSIGN:  return formatIndexAssign(*static_cast<IndexAssign*>(node));
    case NodeType::NODE_CLASS_DECL:     return formatClassDecl(*static_cast<ClassDecl*>(node));
    case NodeType::NODE_MEMBER_ACCESS: return formatMemberAccess(*static_cast<MemberAccess*>(node));
    case NodeType::NODE_MEMBER_ASSIGN: return formatMemberAssign(*static_cast<MemberAssign*>(node));
    case NodeType::NODE_METHOD_CALL:    return formatMethodCall(*static_cast<MethodCall*>(node));
    case NodeType::NODE_NULL_LITERAL:   return formatNullLiteral(*static_cast<NullLiteral*>(node));
    case NodeType::NODE_SUPER_EXPR:    return "super";
    }

    return "/* unknown node */";
}

// 运算符优先级表（数值越大优先级越高）
int Formatter::opPrecedence(BinOpType opType) {
    switch (opType) {
    case BinOpType::BIN_OR:  return 1;
    case BinOpType::BIN_AND: return 2;
    case BinOpType::BIN_EQ:
    case BinOpType::BIN_NEQ: return 3;
    case BinOpType::BIN_LT:
    case BinOpType::BIN_GT:
    case BinOpType::BIN_LTE:
    case BinOpType::BIN_GTE: return 4;
    case BinOpType::BIN_ADD:
    case BinOpType::BIN_SUB: return 5;
    case BinOpType::BIN_MUL:
    case BinOpType::BIN_DIV:
    case BinOpType::BIN_MOD: return 6;
    default: return 0;
    }
}

bool Formatter::isRightAssoc(BinOpType /*opType*/) {
    // 目前没有右结合的二元运算符（赋值不是 BinaryOp）
    return false;
}

/// 判断子表达式是否需要加括号
static bool needsParens(ASTNode* child, BinOpType parentOpType, bool isRight) {
    if (!child || child->nodeType != NodeType::NODE_BINARY_OP) return false;
    BinaryOp* childBin = static_cast<BinaryOp*>(child);
    int parentPrec = Formatter::opPrecedence(parentOpType);
    int childPrec  = Formatter::opPrecedence(childBin->opType);
    // 子优先级更低 → 需要括号
    if (childPrec < parentPrec) return true;
    // 同优先级时，右结合运算符的右操作数不需要括号，左操作数也不需要
    // 但对于左结合运算符的右操作数，如果子也是同优先级，需要括号（如 a - (b - c)）
    if (childPrec == parentPrec) {
        if (Formatter::isRightAssoc(parentOpType)) {
            return !isRight;  // 右结合：左操作数需要括号
        } else {
            return isRight;   // 左结合：右操作数需要括号（如 a-(b+c) 在同优先级时）
        }
    }
    return false;
}

std::string Formatter::formatBinaryOp(BinaryOp& node) {
    std::string left = formatNode(node.left.get());
    if (needsParens(node.left.get(), node.opType, false)) {
        left = "(" + left + ")";
    }
    std::string right = formatNode(node.right.get());
    if (needsParens(node.right.get(), node.opType, true)) {
        right = "(" + right + ")";
    }
    return left + binOp(BinaryOp::opTypeStr(node.opType)) + right;
}

std::string Formatter::formatUnaryOp(UnaryOp& node) {
    std::string operand = formatNode(node.operand.get());
    // BinaryOp 优先级低于一元运算符，必须加括号保持语义正确
    // 例如 -(a + b) 不能格式化为 -a + b
    if (node.operand && (node.operand->nodeType == NodeType::NODE_BINARY_OP ||
                         node.operand->nodeType == NodeType::NODE_UNARY_OP)) {
        operand = "(" + operand + ")";
    }
    if (node.opType == UnaryOp::UnaryOpType::UOP_NOT) {
        return "not " + operand;
    }
    return "-" + operand;
}

std::string Formatter::formatNumberLiteral(NumberLiteral& node) {
    return node.value.toString();
}

std::string Formatter::formatStringLiteral(StringLiteral& node) {
    std::string escaped;
    escaped.reserve(node.value.size() + 2);
    escaped += '"';
    for (char c : node.value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"':  escaped += "\\\""; break;
        case '\n': escaped += "\\n";  break;
        case '\t': escaped += "\\t";  break;
        case '\r': escaped += "\\r";  break;
        default:   escaped += c;      break;
        }
    }
    escaped += '"';
    return escaped;
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
        result += binOp("=") + formatNode(node.initializer.get());
    }
    return result;
}

std::string Formatter::formatAssignment(Assignment& node) {
    return node.name + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatVarRef(VarRef& node) {
    return node.name;
}

std::string Formatter::formatIfStmt(IfStmt& node) {
    std::string result = "if (" + formatNode(node.condition.get()) + ")" + openBrace() + "\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.thenBranch.get())) {
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.thenBranch.get())) {
        // L17 fix: 裸复合语句（if/while/for）作为 thenBranch 时，需要额外缩进层级
        // 注意：else-if 链不受影响，因为 else 分支中的 IfStmt 走下面的 "else " + formatIfStmt 路径
        currentIndent_++;
        result += indent() + formatNode(node.thenBranch.get()) + "\n";
        currentIndent_--;
    } else {
        result += indent() + formatNode(node.thenBranch.get()) + (options_.semicolons ? ";\n" : "\n");
    }
    currentIndent_--;
    result += indent() + "}";

    if (node.elseBranch) {
        // else if 分支：elseBranch 是 IfStmt，直接输出 "else if ..."
        if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
            result += " else " + formatIfStmt(*static_cast<IfStmt*>(node.elseBranch.get()));
        } else if (auto* block = dynamic_cast<Block*>(node.elseBranch.get())) {
            result += " else" + openBrace() + "\n";
            currentIndent_++;
            result += formatBlock(*block);
            currentIndent_--;
            result += indent() + "}";
        } else if (isSelfTerminating(node.elseBranch.get())) {
            result += " else" + openBrace() + "\n";
            currentIndent_++;
            result += indent() + formatNode(node.elseBranch.get()) + "\n";
            currentIndent_--;
            result += indent() + "}";
        } else {
            result += " else" + openBrace() + "\n";
            currentIndent_++;
            result += indent() + formatNode(node.elseBranch.get()) + (options_.semicolons ? ";\n" : "\n");
            currentIndent_--;
            result += indent() + "}";
        }
    }

    return result;
}

std::string Formatter::formatWhileStmt(WhileStmt& node) {
    std::string result = "while (" + formatNode(node.condition.get()) + ")" + openBrace() + "\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 fix: 裸复合语句需要额外缩进层级
        currentIndent_++;
        result += indent() + formatNode(node.body.get()) + "\n";
        currentIndent_--;
    } else {
        result += indent() + formatNode(node.body.get()) + (options_.semicolons ? ";\n" : "\n");
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
    result += ")" + openBrace() + "\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 fix: 裸复合语句需要额外缩进层级
        currentIndent_++;
        result += indent() + formatNode(node.body.get()) + "\n";
        currentIndent_--;
    } else {
        result += indent() + formatNode(node.body.get()) + (options_.semicolons ? ";\n" : "\n");
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatFunDecl(FunDecl& node) {
    std::string result = "fun " + node.name + "(";
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (i > 0) result += comma();
        result += node.params[i];
        if (!node.paramTypes.empty() && i < node.paramTypes.size() && !node.paramTypes[i].empty()) {
            result += ": " + node.paramTypes[i];
        }
    }
    result += ")";
    if (!node.returnType.empty()) {
        result += ": " + node.returnType;
    }
    result += openBrace() + "\n";
    currentIndent_++;
    if (auto* block = dynamic_cast<Block*>(node.body.get())) {
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 fix: 裸复合语句需要额外缩进层级
        currentIndent_++;
        result += indent() + formatNode(node.body.get()) + "\n";
        currentIndent_--;
    } else {
        result += indent() + formatNode(node.body.get()) + (options_.semicolons ? ";\n" : "\n");
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatFunCall(FunCall& node) {
    std::string result;
    if (node.callee) {
        // 链式调用 / 表达式调用
        result = formatNode(node.callee.get()) + "(";
    } else {
        result = node.name + "(";
    }
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0) result += comma();
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
        if (i > 0) result += comma();
        result += formatNode(node.values[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatBlock(Block& node) {
    std::string result;
    for (size_t i = 0; i < node.statements.size(); ++i) {
        ASTNode* stmt = node.statements[i].get();

        // F1 fix: 输出当前语句之前的所有独立注释（行号严格小于语句行号）
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line < stmt->line) {
            result += indent() + comments_[commentIndex_].lexeme + "\n";
            commentIndex_++;
        }

        // 函数/类声明之间加空行
        if (options_.blankLineBetweenFunctions && i > 0 && isSelfTerminating(stmt)) {
            ASTNode* prev = node.statements[i - 1].get();
            if (isSelfTerminating(prev)) {
                result += "\n";
            }
        }

        // 格式化语句
        std::string stmtText;
        if (isSelfTerminating(stmt)) {
            stmtText = indent() + formatNode(stmt);
        } else {
            stmtText = indent() + formatNode(stmt) + (options_.semicolons ? ";" : "");
        }

        // F1+ fix: 同行行内注释追加到语句末尾
        std::string trailing;
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line == stmt->line) {
            trailing += " " + comments_[commentIndex_].lexeme;
            commentIndex_++;
        }

        result += stmtText + trailing + "\n";
    }

    // F1 fix: 块末尾输出尾部注释
    // L18 fix: 对所有块都刷新尾部注释，不仅仅是顶层块
    if (node.closingBraceLine > 0) {
        // 非顶层块：输出 closingBraceLine 之前的注释
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line < node.closingBraceLine) {
            result += indent() + comments_[commentIndex_].lexeme + "\n";
            commentIndex_++;
        }
    } else {
        // 顶层块（closingBraceLine == 0）：输出所有剩余注释
        while (commentIndex_ < comments_.size()) {
            result += indent() + comments_[commentIndex_].lexeme + "\n";
            commentIndex_++;
        }
    }

    return result;
}

// ---- 新增节点格式化 ----

std::string Formatter::formatArrayLiteral(ArrayLiteral& node) {
    std::string result = "[";
    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.elements[i].get());
    }
    result += "]";
    return result;
}

std::string Formatter::formatDictLiteral(DictLiteral& node) {
    std::string result = "{";
    for (size_t i = 0; i < node.pairs.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.pairs[i].first.get()) + ": " + formatNode(node.pairs[i].second.get());
    }
    result += "}";
    return result;
}

std::string Formatter::formatIndexAccess(IndexAccess& node) {
    return formatNode(node.object.get()) + "[" + formatNode(node.index.get()) + "]";
}

std::string Formatter::formatIndexAssign(IndexAssign& node) {
    return formatNode(node.object.get()) + "[" + formatNode(node.index.get()) + "]" + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatClassDecl(ClassDecl& node) {
    std::string result = "class " + node.name;
    if (!node.superClassName.empty()) {
        result += " extends " + node.superClassName;
    }
    result += openBrace() + "\n";
    currentIndent_++;
    for (auto& member : node.members) {
        // 方法（FunDecl）以 } 结尾，不需要额外 ;
        if (isSelfTerminating(member.get())) {
            result += indent() + formatNode(member.get()) + "\n";
        } else {
            result += indent() + formatNode(member.get()) + (options_.semicolons ? ";\n" : "\n");
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
    return formatNode(node.object.get()) + "." + node.fieldName + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatMethodCall(MethodCall& node) {
    std::string result = formatNode(node.object.get()) + "." + node.methodName + "(";
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.arguments[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatNullLiteral(NullLiteral& node) {
    return "null";
}
