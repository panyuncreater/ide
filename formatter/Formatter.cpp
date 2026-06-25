#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include <sstream>
#include <algorithm>  // F-P2-5 fix: std::sort

// ============================================================
// Formatter 代码格式化器实现
// ============================================================

Formatter::Formatter() {}

void Formatter::setComments(const std::vector<Token>& tokens) {
    comments_.clear();
    for (const auto& tok : tokens) {
        // P1 fix: 同时收集行注释和块注释，避免块注释格式化后丢失
        if (tok.type == TokenType::TK_LINE_COMMENT ||
            tok.type == TokenType::TK_BLOCK_COMMENT) {
            comments_.push_back(tok);
        }
    }
    // F-P2-5 fix: 按行号排序，确保 formatBlock 中的 commentIndex_ 单调递增游标正确工作
    std::sort(comments_.begin(), comments_.end(),
              [](const Token& a, const Token& b) { return a.line < b.line; });
    commentIndex_ = 0;
}

void Formatter::setIndentSize(int size) {
    // F-P1-2 fix: 校验缩进大小，负值会导致 std::string 构造时 size_t 溢出触发 bad_alloc
    if (size < 0 || size > 16) return;
    options_.indentSize = size;
    cachedIndentLevel_ = -1;  // P3: 使缩进缓存失效
}

void Formatter::setOptions(const FormatOptions& options) {
    options_ = options;
    // P3: 选项变更时使缓存失效
    cachedIndentLevel_ = -1;
    commaCacheValid_ = false;
    binOpKey_.clear();  // P30: 使 binOp 缓存失效
}

const FormatOptions& Formatter::getOptions() const {
    return options_;
}

std::string Formatter::indent() const {
    // P3 fix: 缓存缩进字符串，仅在级别变化时重建
    if (cachedIndentLevel_ != currentIndent_) {
        cachedIndentLevel_ = currentIndent_;
        if (options_.useTabs) {
            indentCache_ = std::string(currentIndent_, '\t');
        } else {
            // F-P1-2 fix: 防御性检查，防止负值乘积溢出
            int spaces = currentIndent_ * options_.indentSize;
            if (spaces < 0) spaces = 0;
            indentCache_ = std::string(static_cast<size_t>(spaces), ' ');
        }
    }
    return indentCache_;
}

std::string Formatter::binOp(const std::string& op) const {
    if (options_.spaceAroundOperators) {
        // P30 fix: MRU 缓存 — 同一运算符连续调用时直接返回缓存
        if (op == binOpKey_) return binOpVal_;
        binOpKey_ = op;
        binOpVal_ = " " + op + " ";
        return binOpVal_;
    }
    return op;
}

std::string Formatter::comma() {
    // P3 fix: 缓存逗号分隔符
    if (!commaCacheValid_) {
        commaCache_ = options_.spaceAfterComma ? ", " : ",";
        commaCacheValid_ = true;
    }
    return commaCache_;
}

std::string Formatter::openBrace() const {
    if (options_.braceStyle == BraceStyle::NEXT_LINE) {
        return "\n" + indent() + "{";
    }
    return " {";
}

std::string Formatter::format(Block& program) {
    currentIndent_ = 0;
    formatDepth_ = 0;  // D5 fix: 重置递归深度计数器
    commentIndex_ = 0;  // F-P1-4 fix: 重置注释游标，确保 Formatter 对象复用时注释正确输出
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
    case NodeType::NODE_TRY_STMT:       // P0 fix: try/catch 以 } 结尾，自终止
        return true;
    case NodeType::NODE_IMPORT_STMT:    // P0 fix: visitImportStmt 已自行添加 ;
        return true;
    case NodeType::NODE_EXPORT_STMT: {
        // P0 fix: export 的自终止性取决于内层声明类型
        // export var x = 1 需要额外 ;，export fun/class 不需要
        auto* exportNode = static_cast<ExportStmt*>(node);
        if (exportNode->declaration) {
            auto innerType = exportNode->declaration->nodeType;
            if (innerType == NodeType::NODE_FUN_DECL ||
                innerType == NodeType::NODE_CLASS_DECL) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

std::string Formatter::formatNode(ASTNode* node) {
    if (!node) return "null";

    // D5 fix: 递归深度保护，防止极端嵌套 AST 导致栈溢出
    if (formatDepth_ >= MAX_FORMAT_DEPTH) return "/* 嵌套过深 */";
    formatDepth_++;
    struct DepthGuard { int& d; ~DepthGuard() { d--; } } guard{formatDepth_};

    // 统一通过 Visitor 模式分派：node->accept(*this) 调用对应的 visit* 方法，
    // visit* 方法将格式化结果存入 lastFormatResult_，替代原 25 路 switch。
    // F-P2-6 fix: 清空 lastFormatResult_，避免未覆盖的节点类型返回陈旧结果
    lastFormatResult_.clear();
    node->accept(*this);
    if (lastFormatResult_.empty()) {
        return "/* unhandled node */";
    }
    return std::move(lastFormatResult_);
}

// ============================================================
// Visitor 模式：visit* 方法实现
// 每个 visit* 方法调用对应的 format* 逻辑，将结果存入 lastFormatResult_，
// 返回 Value::nullValue()。
// ============================================================

Value Formatter::visitBinaryOp(BinaryOp& node) {
    lastFormatResult_ = formatBinaryOp(node);
    return Value::nullValue();
}

Value Formatter::visitUnaryOp(UnaryOp& node) {
    lastFormatResult_ = formatUnaryOp(node);
    return Value::nullValue();
}

Value Formatter::visitNumberLiteral(NumberLiteral& node) {
    lastFormatResult_ = formatNumberLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitStringLiteral(StringLiteral& node) {
    lastFormatResult_ = formatStringLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitBoolLiteral(BoolLiteral& node) {
    lastFormatResult_ = formatBoolLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitVarDecl(VarDecl& node) {
    lastFormatResult_ = formatVarDecl(node);
    return Value::nullValue();
}

Value Formatter::visitAssignment(Assignment& node) {
    lastFormatResult_ = formatAssignment(node);
    return Value::nullValue();
}

Value Formatter::visitVarRef(VarRef& node) {
    lastFormatResult_ = formatVarRef(node);
    return Value::nullValue();
}

Value Formatter::visitIfStmt(IfStmt& node) {
    lastFormatResult_ = formatIfStmt(node);
    return Value::nullValue();
}

Value Formatter::visitWhileStmt(WhileStmt& node) {
    lastFormatResult_ = formatWhileStmt(node);
    return Value::nullValue();
}

Value Formatter::visitForStmt(ForStmt& node) {
    lastFormatResult_ = formatForStmt(node);
    return Value::nullValue();
}

Value Formatter::visitFunDecl(FunDecl& node) {
    lastFormatResult_ = formatFunDecl(node);
    return Value::nullValue();
}

Value Formatter::visitFunCall(FunCall& node) {
    lastFormatResult_ = formatFunCall(node);
    return Value::nullValue();
}

Value Formatter::visitReturnStmt(ReturnStmt& node) {
    lastFormatResult_ = formatReturnStmt(node);
    return Value::nullValue();
}

Value Formatter::visitPrintStmt(PrintStmt& node) {
    lastFormatResult_ = formatPrintStmt(node);
    return Value::nullValue();
}

Value Formatter::visitBlock(Block& node) {
    // 保留原 formatNode 中 NODE_BLOCK 分支的行为：用花括号包裹 formatBlock 输出。
    // formatBlock 只格式化语句列表，不包含外层花括号，此处补上。
    // FMT-05 fix: 使用 openBrace() 支持 BraceStyle 配置
    // F-P2-1 fix: openBrace() 后添加换行，与 formatIfStmt 等保持一致，避免 { 与首条语句同行
    std::string result = openBrace() + "\n";
    currentIndent_++;
    result += formatBlock(node);
    currentIndent_--;
    result += indent() + "}";
    lastFormatResult_ = std::move(result);
    return Value::nullValue();
}

Value Formatter::visitArrayLiteral(ArrayLiteral& node) {
    lastFormatResult_ = formatArrayLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitDictLiteral(DictLiteral& node) {
    lastFormatResult_ = formatDictLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitIndexAccess(IndexAccess& node) {
    lastFormatResult_ = formatIndexAccess(node);
    return Value::nullValue();
}

Value Formatter::visitIndexAssign(IndexAssign& node) {
    lastFormatResult_ = formatIndexAssign(node);
    return Value::nullValue();
}

Value Formatter::visitClassDecl(ClassDecl& node) {
    lastFormatResult_ = formatClassDecl(node);
    return Value::nullValue();
}

Value Formatter::visitMemberAccess(MemberAccess& node) {
    lastFormatResult_ = formatMemberAccess(node);
    return Value::nullValue();
}

Value Formatter::visitMemberAssign(MemberAssign& node) {
    lastFormatResult_ = formatMemberAssign(node);
    return Value::nullValue();
}

Value Formatter::visitMethodCall(MethodCall& node) {
    lastFormatResult_ = formatMethodCall(node);
    return Value::nullValue();
}

Value Formatter::visitNullLiteral(NullLiteral& node) {
    lastFormatResult_ = formatNullLiteral(node);
    return Value::nullValue();
}

Value Formatter::visitSuperExpr(SuperExpr& /*node*/) {
    // SuperExpr 无对应 format* 方法，保留原 formatNode 中 NODE_SUPER_EXPR 分支的行为
    lastFormatResult_ = "super";
    return Value::nullValue();
}

Value Formatter::visitBreakStmt(BreakStmt& /*node*/) {
    lastFormatResult_ = "break";
    return Value::nullValue();
}

Value Formatter::visitContinueStmt(ContinueStmt& /*node*/) {
    lastFormatResult_ = "continue";
    return Value::nullValue();
}

Value Formatter::visitThrowStmt(ThrowStmt& node) {
    std::string result = "throw";
    if (node.expression) {
        result += " " + formatNode(node.expression.get());
    }
    lastFormatResult_ = result;
    return Value::nullValue();
}

Value Formatter::visitTryStmt(TryStmt& node) {
    // P2 fix: visitBlock 以 " {" 开头，"try" 后无需额外空格
    std::string result = "try" + formatNode(node.tryBlock.get());
    result += " catch (" + node.catchVarName + ")" + formatNode(node.catchBlock.get());
    lastFormatResult_ = result;
    return Value::nullValue();
}

Value Formatter::visitImportStmt(ImportStmt& node) {
    std::string result = "import ";
    if (!node.importAll && !node.names.empty()) {
        result += "{ ";
        for (size_t i = 0; i < node.names.size(); ++i) {
            if (i > 0) result += ", ";
            result += node.names[i];
        }
        result += " } from ";
    }
    result += "\"" + node.modulePath + "\";";
    lastFormatResult_ = result;
    return Value::nullValue();
}

Value Formatter::visitExportStmt(ExportStmt& node) {
    std::string result = "export " + formatNode(node.declaration.get());
    lastFormatResult_ = result;
    return Value::nullValue();
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
            // 左结合：SUB/DIV/MOD 不满足结合律，右操作数同优先级子表达式必须加括号
            // 例: a-(b+c) ≠ a-b+c, a/(b*c) ≠ a/b*c, a%(b-c) ≠ a%b-c
            // F-P2 fix: MUL 的右操作数为 DIV/MOD 也需括号，因为整数乘法不对整除/取模分配律
            // 例: a*(b/c) ≠ a*b/c, a*(b%c) ≠ a*b%c
            if (isRight) {
                if (parentOpType == BinOpType::BIN_SUB ||
                    parentOpType == BinOpType::BIN_DIV ||
                    parentOpType == BinOpType::BIN_MOD) {
                    return true;
                }
                if (parentOpType == BinOpType::BIN_MUL &&
                    (childBin->opType == BinOpType::BIN_DIV ||
                     childBin->opType == BinOpType::BIN_MOD)) {
                    return true;
                }
            }
            return false;   // ADD/MUL 满足结合律，同优先级无需括号
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
    if (node.opType == UnaryOp::UnaryOpType::UOP_PLUS) {
        return "+" + operand;
    }
    // FMT-03 fix: UOP_UNKNOWN 不应被格式化为 "-"
    if (node.opType == UnaryOp::UnaryOpType::UOP_UNKNOWN) {
        return "/* unknown */ " + operand;
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
        case '\0': escaped += "\\0";  break;  // FMT-02 fix
        case '\b': escaped += "\\b";  break;  // FMT-02 fix
        case '\f': escaped += "\\f";  break;  // FMT-02 fix
        case '\a': escaped += "\\a";  break;  // FMT-02 fix
        case '\v': escaped += "\\v";  break;  // FMT-02 fix
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
    // F-P2-10 fix: 检查 value 空指针，避免输出 "x = null" 语义错误
    if (!node.value) return node.name + binOp("=") + "/* null */";
    return node.name + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatVarRef(VarRef& node) {
    return node.name;
}

std::string Formatter::formatIfStmt(IfStmt& node) {
    std::string result = "if (" + formatNode(node.condition.get()) + ")" + openBrace() + "\n";
    currentIndent_++;
    // F-P2-3 fix: 检查 thenBranch 空指针，避免 formatNode 返回 "null" 语义错误
    if (!node.thenBranch) {
        result += indent() + "/* empty */\n";
    } else if (auto* block = dynamic_cast<Block*>(node.thenBranch.get())) {
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.thenBranch.get())) {
        // L17 fix: 裸复合语句（if/while/for）作为 thenBranch 时，需要额外缩进层级
        // 注意：else-if 链不受影响，因为 else 分支中的 IfStmt 走下面的 "else " + formatNode 路径
        currentIndent_++;
        result += indent() + formatNode(node.thenBranch.get()) + "\n";
        currentIndent_--;
    } else {
        result += indent() + formatNode(node.thenBranch.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";

    if (node.elseBranch) {
        // else if 分支：elseBranch 是 IfStmt
        if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
            // F-P1-1 fix: 改用 formatNode 分派，让 else-if 链也受 MAX_FORMAT_DEPTH 深度保护
            // 输出结果一致：" else " + "if (...) { ... }" = " else if (...) { ... }"
            result += " else " + formatNode(node.elseBranch.get());
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
            result += indent() + formatNode(node.elseBranch.get()) + ";\n";
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
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatForStmt(ForStmt& node) {
    // F-P2-9 fix: 条件化添加分号和空格，避免 update 为空时产生 "for (init; cond; ) {" 多余空格
    std::string result = "for (";
    if (node.initializer) result += formatNode(node.initializer.get());
    result += ";";
    if (node.condition) result += " " + formatNode(node.condition.get());
    result += ";";
    if (node.update) result += " " + formatNode(node.update.get());
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
        result += indent() + formatNode(node.body.get()) + ";\n";
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
        // F10: 格式化默认参数值
        if (i < node.defaultValues.size() && node.defaultValues[i]) {
            result += " = " + formatNode(node.defaultValues[i].get());
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
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

std::string Formatter::formatFunCall(FunCall& node) {
    // F-P2-4 fix: 预估大小避免循环内 realloc
    std::string result;
    result.reserve(node.arguments.size() * 16 + 16);
    if (node.callee) {
        // 链式调用 / 表达式调用
        std::string callee = formatNode(node.callee.get());
        // F-P2-12 fix: 移除冗余的 node.callee 二次检查（已在 if 分支内）
        if (node.callee->nodeType == NodeType::NODE_BINARY_OP ||
            node.callee->nodeType == NodeType::NODE_UNARY_OP)
            callee = "(" + callee + ")";  // FMT-01 fix
        result = callee + "(";
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
    // F-P2-4 fix: 预估大小避免循环内 realloc
    std::string result = "print(";
    result.reserve(node.values.size() * 16 + 8);
    for (size_t i = 0; i < node.values.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.values[i].get());
    }
    result += ")";
    return result;
}

std::string Formatter::formatBlock(Block& node) {
    std::string result;
    // P3 fix: 预估输出大小，避免反复 realloc（每条语句平均约 40 字符）
    result.reserve(node.statements.size() * 40);
    for (size_t i = 0; i < node.statements.size(); ++i) {
        ASTNode* stmt = node.statements[i].get();
        // F-P1-3 fix: 跳过空语句指针，避免后续 stmt->line 解引用空指针崩溃
        if (!stmt) continue;

        // F1 fix: 输出当前语句之前的所有独立注释（行号严格小于语句行号）
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line < stmt->line) {
            result += indent() + comments_[commentIndex_].lexeme + "\n";
            commentIndex_++;
        }

        // 函数/类声明前加空行（BUG1 fix: 原代码在任意自终止语句间插入空行）
        if (options_.blankLineBetweenFunctions && i > 0) {
            auto isFunOrClass = [](ASTNode* n) {
                return n && (n->nodeType == NodeType::NODE_FUN_DECL ||
                             n->nodeType == NodeType::NODE_CLASS_DECL);
            };
            if (isFunOrClass(stmt)) {
                result += "\n";
            }
        }

        // 格式化语句
        std::string stmtText;
        if (isSelfTerminating(stmt)) {
            stmtText = indent() + formatNode(stmt);
        } else {
            stmtText = indent() + formatNode(stmt) + ";";
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
    // F-P2-4 fix: 预估大小避免循环内 realloc
    std::string result = "[";
    result.reserve(node.elements.size() * 16 + 2);
    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.elements[i].get());
    }
    result += "]";
    return result;
}

std::string Formatter::formatDictLiteral(DictLiteral& node) {
    // F-P2-4 fix: 预估大小避免循环内 realloc
    std::string result = "{";
    result.reserve(node.pairs.size() * 32 + 2);
    for (size_t i = 0; i < node.pairs.size(); ++i) {
        if (i > 0) result += comma();
        result += formatNode(node.pairs[i].first.get()) + ": " + formatNode(node.pairs[i].second.get());
    }
    result += "}";
    return result;
}

std::string Formatter::formatIndexAccess(IndexAccess& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix: 低优先级表达式需要括号
    return obj + "[" + formatNode(node.index.get()) + "]";
}

std::string Formatter::formatIndexAssign(IndexAssign& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "[" + formatNode(node.index.get()) + "]" + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatClassDecl(ClassDecl& node) {
    std::string result = "class " + node.name;
    if (!node.superClassName.empty()) {
        result += " extends " + node.superClassName;
    }
    result += openBrace() + "\n";
    currentIndent_++;
    for (auto& member : node.members) {
        // F-P2-8 fix: 跳过空成员指针，避免 formatNode 返回 "null" 作为类成员
        if (!member) continue;
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
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "." + node.fieldName;
}

std::string Formatter::formatMemberAssign(MemberAssign& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "." + node.fieldName + binOp("=") + formatNode(node.value.get());
}

std::string Formatter::formatMethodCall(MethodCall& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    std::string result = obj + "." + node.methodName + "(";
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
