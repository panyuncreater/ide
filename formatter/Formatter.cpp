#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include "interpreter/Value.h"  // ARCH-01 fix: getValue().toString() 需要 Value 完整定义
#include <sstream>
#include <algorithm>  // F-P2-5 fix: std::sort

// ============================================================
// Formatter 代码格式化器实现
// ============================================================

// AUDIT-P2 fix: 提取公共字符串转义函数，供 formatStringLiteral 和
// formatInterpolatedString 共用，消除转义规则重复（原两处独立 switch
// 存在同步风险——Lexer 新增转义序列时需同时修改三处）。
static std::string escapeStringContent(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"':  escaped += "\\\""; break;
        case '\n': escaped += "\\n";  break;
        case '\t': escaped += "\\t";  break;
        case '\r': escaped += "\\r";  break;
        case '\0': escaped += "\\0";  break;
        case '\b': escaped += "\\b";  break;
        case '\f': escaped += "\\f";  break;
        case '\a': escaped += "\\a";  break;
        case '\v': escaped += "\\v";  break;
        default:   escaped += c;      break;
        }
    }
    return escaped;
}

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
    // AUDIT-P2 fix: std::sort 非稳定排序，同行多注释的相对顺序未定义。改用 stable_sort
    // 并增加列号作为次要排序键，保证同行注释按源码出现顺序排列，避免格式化后注释顺序颠倒。
    std::stable_sort(comments_.begin(), comments_.end(),
              [](const Token& a, const Token& b) {
                  if (a.line != b.line) return a.line < b.line;
                  return a.column < b.column;
              });
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

/// 生成当前缩进字符串：带缓存，仅当缩进级别变化时才重建（useTabs 时生成 tab，否则空格）。
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

/// 生成二元运算符文本：按 spaceAroundOperators 选项决定是否两侧加空格，带 MRU 缓存。
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

/// 生成逗号分隔符：按 spaceAfterComma 选项决定逗号后是否加空格，结果带缓存。
std::string Formatter::comma() {
    // P3 fix: 缓存逗号分隔符
    if (!commaCacheValid_) {
        commaCache_ = options_.spaceAfterComma ? ", " : ",";
        commaCacheValid_ = true;
    }
    return commaCache_;
}

/// 生成开括号：按 BraceStyle 选项返回同行 " {" 或换行后缩进的 "{"（Allman 风格）。
std::string Formatter::openBrace() const {
    if (options_.braceStyle == BraceStyle::NEXT_LINE) {
        return "\n" + indent() + "{";
    }
    return " {";
}

/// 格式化入口：重置缩进/递归深度/注释游标，格式化顶层语句块并返回标准代码文本。
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

// BUG-F-01 fix: 多行块注释的 lexeme 内含 '\n'，后续行继承源码原始缩进，
// 与格式化后的缩进不一致。此辅助函数在每个 '\n' 后插入当前 indent()，
// 使多行块注释的后续行对齐到格式化后的缩进级别。
/// 重新缩进多行块注释：在每个换行后插入当前缩进，使块注释后续行与格式化后的缩进对齐（避免 BUG-F-01）。
static std::string reindentBlockComment(const std::string& lexeme, const std::string& indent) {
    std::string result;
    result.reserve(lexeme.size() + 16);
    for (size_t i = 0; i < lexeme.size(); ++i) {
        char c = lexeme[i];
        result += c;
        // 在每个换行后（除最后一行外）插入当前缩进
        if (c == '\n' && i + 1 < lexeme.size()) {
            result += indent;
        }
    }
    return result;
}

/// 格式化任意 AST 节点：经 Visitor 模式分派到 visit*，带递归深度保护（超深返回占位），结果存入 lastFormatResult_。
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

void Formatter::visitBinaryOp(BinaryOp& node) {
    lastFormatResult_ = formatBinaryOp(node);
    return;
}

void Formatter::visitUnaryOp(UnaryOp& node) {
    lastFormatResult_ = formatUnaryOp(node);
    return;
}

void Formatter::visitNumberLiteral(NumberLiteral& node) {
    lastFormatResult_ = formatNumberLiteral(node);
    return;
}

void Formatter::visitStringLiteral(StringLiteral& node) {
    lastFormatResult_ = formatStringLiteral(node);
    return;
}

void Formatter::visitBoolLiteral(BoolLiteral& node) {
    lastFormatResult_ = formatBoolLiteral(node);
    return;
}

void Formatter::visitVarDecl(VarDecl& node) {
    lastFormatResult_ = formatVarDecl(node);
    return;
}

void Formatter::visitAssignment(Assignment& node) {
    lastFormatResult_ = formatAssignment(node);
    return;
}

void Formatter::visitVarRef(VarRef& node) {
    lastFormatResult_ = formatVarRef(node);
    return;
}

void Formatter::visitIfStmt(IfStmt& node) {
    lastFormatResult_ = formatIfStmt(node);
    return;
}

void Formatter::visitWhileStmt(WhileStmt& node) {
    lastFormatResult_ = formatWhileStmt(node);
    return;
}

void Formatter::visitForStmt(ForStmt& node) {
    lastFormatResult_ = formatForStmt(node);
    return;
}

void Formatter::visitFunDecl(FunDecl& node) {
    lastFormatResult_ = formatFunDecl(node);
    return;
}

void Formatter::visitFunCall(FunCall& node) {
    lastFormatResult_ = formatFunCall(node);
    return;
}

void Formatter::visitReturnStmt(ReturnStmt& node) {
    lastFormatResult_ = formatReturnStmt(node);
    return;
}

void Formatter::visitPrintStmt(PrintStmt& node) {
    lastFormatResult_ = formatPrintStmt(node);
    return;
}

void Formatter::visitBlock(Block& node) {
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
    return;
}

void Formatter::visitArrayLiteral(ArrayLiteral& node) {
    lastFormatResult_ = formatArrayLiteral(node);
    return;
}

void Formatter::visitDictLiteral(DictLiteral& node) {
    lastFormatResult_ = formatDictLiteral(node);
    return;
}

void Formatter::visitIndexAccess(IndexAccess& node) {
    lastFormatResult_ = formatIndexAccess(node);
    return;
}

void Formatter::visitIndexAssign(IndexAssign& node) {
    lastFormatResult_ = formatIndexAssign(node);
    return;
}

void Formatter::visitClassDecl(ClassDecl& node) {
    lastFormatResult_ = formatClassDecl(node);
    return;
}

void Formatter::visitMemberAccess(MemberAccess& node) {
    lastFormatResult_ = formatMemberAccess(node);
    return;
}

void Formatter::visitMemberAssign(MemberAssign& node) {
    lastFormatResult_ = formatMemberAssign(node);
    return;
}

void Formatter::visitMethodCall(MethodCall& node) {
    lastFormatResult_ = formatMethodCall(node);
    return;
}

void Formatter::visitNullLiteral(NullLiteral& node) {
    lastFormatResult_ = formatNullLiteral(node);
    return;
}

void Formatter::visitSuperExpr(SuperExpr& /*node*/) {
    // SuperExpr 无对应 format* 方法，保留原 formatNode 中 NODE_SUPER_EXPR 分支的行为
    lastFormatResult_ = "super";
    return;
}

void Formatter::visitBreakStmt(BreakStmt& /*node*/) {
    lastFormatResult_ = "break";
    return;
}

void Formatter::visitContinueStmt(ContinueStmt& /*node*/) {
    lastFormatResult_ = "continue";
    return;
}

void Formatter::visitThrowStmt(ThrowStmt& node) {
    std::string result = "throw";
    if (node.expression) {
        result += " " + formatNode(node.expression.get());
    } else {
        // BUG-F-08 fix: 空 throw（expression 为 nullptr）时输出 "throw null" 作为兜底。
        // Parser 强制要求表达式故正常路径不会产生此 AST，但外部构造的 AST 会触发。
        result += " null";
    }
    lastFormatResult_ = result;
    return;
}

void Formatter::visitTryStmt(TryStmt& node) {
    // P2 fix: visitBlock 以 " {" 开头，"try" 后无需额外空格
    // BUG-FE-AUDIT-1 fix: 同步 BUG-AUDIT-FINALLY-1 的 finally 语法支持。
    //   - catch 块可选（try-finally 无 catch 时不输出 " catch (...)"）
    //   - finally 块可选，存在时输出 " finally" + formatNode(finallyBlock)
    //   - 修复幂等性违反（原实现无条件输出 catch 且忽略 finally，导致 format 后重新 parse 报错）
    std::string result = "try" + formatNode(node.tryBlock.get());
    if (node.catchBlock) {
        result += " catch (" + node.catchVarName + ")" + formatNode(node.catchBlock.get());
    }
    if (node.finallyBlock) {
        result += " finally" + formatNode(node.finallyBlock.get());
    }
    lastFormatResult_ = result;
    return;
}

void Formatter::visitImportStmt(ImportStmt& node) {
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
    return;
}

void Formatter::visitExportStmt(ExportStmt& node) {
    std::string result = "export " + formatNode(node.declaration.get());
    lastFormatResult_ = result;
    return;
}

// C5 fix: 插值字符串格式化 — 重建 `"text {expr} more {expr2}"` 语法
void Formatter::visitInterpolatedString(InterpolatedString& node) {
    lastFormatResult_ = formatInterpolatedString(node);
    return;
}

// 运算符优先级表（数值越大优先级越高）
/// 运算符优先级查询：委托 BinaryOp::precedence 单一来源，避免与 Parser 重复优先级表。
int Formatter::opPrecedence(BinOpType opType) {
    // C14 fix: 委托给 BinaryOp::precedence 单一来源，消除 Formatter 与 Parser 的重复优先级表
    return BinaryOp::precedence(opType);
}

/// 右结合性查询：委托 BinaryOp::isRightAssociative 单一来源。
bool Formatter::isRightAssoc(BinOpType opType) {
    // C14 fix: 委托给 BinaryOp::isRightAssociative 单一来源
    return BinaryOp::isRightAssociative(opType);
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
            // 左结合：右操作数同优先级子表达式必须加括号以保持原始分组。
            // AUDIT-FMT-P0 fix: 原实现仅对 ADD/SUB/MUL/DIV/MOD 加括号（#15 fix），
            // EQ/NEQ/LT/GT/LTE/GTE/AND/OR 漏处理，导致 1 == (2 == 3) 被格式化为 1 == 2 == 3，
            // 重新解析得到 (1 == 2) == 3，AST 结构改变，往返不变量被破坏。
            // 修复方案：移除运算符特判，对所有左结合运算符的右操作数同优先级子表达式统一加括号。
            // 理由：
            //   - 浮点算术不满足结合律：(a+b)+c ≠ a+(b+c)（舍入误差）
            //   - SUB/DIV/MOD 本就不满足结合律
            //   - EQ/NEQ/LT/GT/LTE/GTE 返回 bool，重新分组会改变比较语义
            //     （如 (1 < 2) < 3 → true < 3 → true；1 < (2 < 3) → 1 < true → 1 < 1 → false）
            //   - AND/OR 虽短-circuit 行为在多数情况下等价，但 AST 分组改变违反保形变换原则
            //   - 左操作数同优先级无需括号（左结合天然保持分组）
            if (isRight) {
                return true;
            }
            return false;
        }
    }
    return false;
}

/// 格式化二元运算：递归格式化左右操作数，依据 needsParens 在子表达式优先级不足时补加括号，
/// 保证重新解析后 AST 结构与原树等价（往返不变量）。
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

/// 格式化一元运算（- / + / not）：当操作数本身是二元或一元表达式时加括号，
/// 避免 -(a + b) 被错误输出为 -a + b 这类优先级错乱。
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
    // BUG-FMT-P2-2 fix: UOP_UNKNOWN 输出 `/* unknown */` 注释会被 Lexer 剥离，
    // 重新解析后丢失 UnaryOp 节点，AST 结构从 UnaryOp 变为裸 operand。
    // 改为输出 UOP_PLUS（最接近"无操作"的语义），保持 AST 结构可重新解析。
    if (node.opType == UnaryOp::UnaryOpType::UOP_UNKNOWN) {
        return "+" + operand;
    }
    return "-" + operand;
}

/// 格式化数值字面量：整数直出；浮点若经 toString 丢失小数点（如 1.0 → "1"）
/// 则补 ".0"，否则重新解析会被识别为整型，破坏 AST 往返不变量。
std::string Formatter::formatNumberLiteral(NumberLiteral& node) {
    // AUDIT-BUG-P2 fix: float 值为整数时 toString 输出无小数点（如 1.0 → "1"），
    // 重新解析时被识别为 int，破坏 AST 往返不变量。检测并附加 ".0"。
    const Value& v = node.getValue();
    if (v.isFloat()) {
        std::string s = v.toString();
        // BUG-F-07 fix: NaN/Infinity 经 toString 返回 "nan"/"inf"/"-inf"，
        // 附加 ".0" 会产生 "nan.0"/"inf.0" 等非合法 NumberLiteral。
        // BUG-FMT-P2-3 fix: MiniLang Lexer 不支持 nan/inf 字面量，直接输出
        // "nan"/"inf" 会被重新解析为标识符（变量引用），破坏 AST 结构等价性。
        // 改为输出 "0.0" 并附注释占位，牺牲精确往返但保持可解析性与 AST 类型一致。
        if (s == "nan" || s == "inf" || s == "-inf") {
            return "0.0/* " + s + " */";
        }
        if (s.find('.') != std::string::npos ||
            s.find('e') != std::string::npos ||
            s.find('E') != std::string::npos) {
            return s;
        }
        return s + ".0";
    }
    return v.toString();  // A1 fix: getValue() 按需构造
}

/// 格式化字符串字面量：对源码字符串按 JSON 风格转义（\\、"、\n、\t、\r、\0 等），
/// 两端补双引号，确保输出可重新被 Lexer 正确切分为单个字符串 Token。
std::string Formatter::formatStringLiteral(StringLiteral& node) {
    // AUDIT-P2 fix: 复用 escapeStringContent 公共函数，消除转义规则重复
    return "\"" + escapeStringContent(node.value) + "\"";
}

std::string Formatter::formatBoolLiteral(BoolLiteral& node) {
    return node.value ? "true" : "false";
}

/// 格式化变量声明：有类型标注时输出 "Type name"，否则 "var name"；
/// 含初始化表达式时用 binOp("=") 生成带空格的赋值符。
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

/// 格式化赋值语句：name = value；value 为空（外部构造的 AST）时降级输出 "name = /* null */"
/// 而非崩溃，保持输出可解析。
std::string Formatter::formatAssignment(Assignment& node) {
    // F-P2-10 fix: 检查 value 空指针，避免输出 "x = null" 语义错误
    if (!node.value) return node.name + binOp("=") + "/* null */";
    return node.name + binOp("=") + formatNode(node.value.get());
}

/// 格式化变量引用：直接输出标识符名称（裸叶子节点，无需括号或空格）。
std::string Formatter::formatVarRef(VarRef& node) {
    return node.name;
}

/// 格式化 if 语句：保留单语句体原貌（无花括号）以保往返；复合体或 else 链才包裹花括号。
/// 依据 isSelfTerminating 决定是否在单语句体后补 ';'。
std::string Formatter::formatIfStmt(IfStmt& node) {
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构从 Stmt 变为 Block{Stmt}
    // 原 Bug：无条件 openBrace() 包裹，导致 if (cond) print(1); → if (cond) { print(1); }
    // 重新解析后 thenBranch 节点类型从 NODE_PRINT_STMT 变为 NODE_BLOCK，违反往返等价性
    const bool thenIsBlock = node.thenBranch && node.thenBranch->nodeType == NodeType::NODE_BLOCK;
    if (!thenIsBlock && node.thenBranch) {
        // 单语句体路径：无花括号，与 Parser 的无花括号语法对应
        std::string result = "if (" + formatNode(node.condition.get()) + ") ";
        result += formatNode(node.thenBranch.get());
        if (!isSelfTerminating(node.thenBranch.get())) result += ";";
        if (node.elseBranch) {
            if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
                result += " else " + formatNode(node.elseBranch.get());
            } else if (node.elseBranch->nodeType == NodeType::NODE_BLOCK) {
                auto* block = static_cast<Block*>(node.elseBranch.get());
                result += " else" + openBrace() + "\n";
                currentIndent_++;
                result += formatBlock(*block);
                currentIndent_--;
                result += indent() + "}";
            } else {
                // else 单语句体：同样无花括号
                result += " else " + formatNode(node.elseBranch.get());
                if (!isSelfTerminating(node.elseBranch.get())) result += ";";
            }
        }
        return result;
    }
    // Block 体路径：原有花括号逻辑
    std::string result = "if (" + formatNode(node.condition.get()) + ")" + openBrace() + "\n";
    currentIndent_++;
    // F-P2-3 fix: 检查 thenBranch 空指针，避免 formatNode 返回 "null" 语义错误
    if (!node.thenBranch) {
        result += indent() + "/* empty */\n";
    } else if (node.thenBranch->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断，O(1) vs O(RTTI)
        auto* block = static_cast<Block*>(node.thenBranch.get());
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.thenBranch.get())) {
        // L17 revert: 裸复合语句（if/while/for）的 formatNode 内部已用
        // currentIndent_++ 处理 body 缩进，此处再 +1 会导致双重缩进。
        result += indent() + formatNode(node.thenBranch.get()) + "\n";
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
        } else if (node.elseBranch->nodeType == NodeType::NODE_BLOCK) {
            // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
            auto* block = static_cast<Block*>(node.elseBranch.get());
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

/// 格式化 while 语句：与 if 一致的单语句体/复合体策略，保持 AST 往返等价。
std::string Formatter::formatWhileStmt(WhileStmt& node) {
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构改变
    if (node.body->nodeType != NodeType::NODE_BLOCK) {
        std::string result = "while (" + formatNode(node.condition.get()) + ") ";
        result += formatNode(node.body.get());
        if (!isSelfTerminating(node.body.get())) result += ";";
        return result;
    }
    std::string result = "while (" + formatNode(node.condition.get()) + ")" + openBrace() + "\n";
    currentIndent_++;
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        result += indent() + formatNode(node.body.get()) + "\n";
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

/// 格式化 for 语句：输出 "for (init; cond; update) body"，空 update/cond 时省略空格，
/// 单语句体与原貌一致、复合体包裹花括号。
std::string Formatter::formatForStmt(ForStmt& node) {
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构改变
    if (node.body->nodeType != NodeType::NODE_BLOCK) {
        std::string result = "for (";
        if (node.initializer) result += formatNode(node.initializer.get());
        result += ";";
        if (node.condition) result += " " + formatNode(node.condition.get());
        result += ";";
        if (node.update) result += " " + formatNode(node.update.get());
        result += ") ";
        result += formatNode(node.body.get());
        if (!isSelfTerminating(node.body.get())) result += ";";
        return result;
    }
    // F-P2-9 fix: 条件化添加分号和空格，避免 update 为空时产生 "for (init; cond; ) {" 多余空格
    std::string result = "for (";
    if (node.initializer) result += formatNode(node.initializer.get());
    result += ";";
    if (node.condition) result += " " + formatNode(node.condition.get());
    result += ";";
    if (node.update) result += " " + formatNode(node.update.get());
    result += ")" + openBrace() + "\n";
    currentIndent_++;
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        result += indent() + formatNode(node.body.get()) + "\n";
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

/// 格式化函数声明：输出 "fun name(params): retType { body }"，参数支持类型标注与默认值，
/// 函数体以 } 自终止，外部由 formatBlock 决定是否补前导空行。
std::string Formatter::formatFunDecl(FunDecl& node) {
    // PERF-27 fix: 预估输出大小（fun + name + params + body），避免反复 realloc
    std::string result;
    result.reserve(32 + node.params.size() * 16 + node.name.size());
    result += "fun " + node.name + "(";
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
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        result += formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        result += indent() + formatNode(node.body.get()) + "\n";
    } else {
        result += indent() + formatNode(node.body.get()) + ";\n";
    }
    currentIndent_--;
    result += indent() + "}";
    return result;
}

/// 格式化函数调用：优先输出 "name(args)"；存在表达式型 callee（链式/成员调用）时
/// 对低优先级 callee 加括号，保证重新解析得到与原 AST 一致的调用结构。
std::string Formatter::formatFunCall(FunCall& node) {
    // F-P2-4 fix: 预估大小避免循环内 realloc
    std::string result;
    result.reserve(node.arguments.size() * 16 + 16);
    if (node.callee) {
        // 链式调用 / 表达式调用
        std::string callee = formatNode(node.callee.get());
        // F-P2-12 fix: 移除冗余的 node.callee 二次检查（已在 if 分支内）
        // BUG-FMT-P1-2 fix: NODE_MEMBER_ACCESS 类型的 callee 也需加括号。
        // 原实现仅对 BINARY_OP/UNARY_OP 加括号，输出 `obj.field(args)` 会被
        // Parser 重新解析为 MethodCall（绑定 this），而非 FunCall(callee=MemberAccess)，
        // 破坏 AST 结构等价性与三后端语义。
        if (node.callee->nodeType == NodeType::NODE_BINARY_OP ||
            node.callee->nodeType == NodeType::NODE_UNARY_OP ||
            node.callee->nodeType == NodeType::NODE_MEMBER_ACCESS)
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

/// 格式化 return 语句：无返回值输出 "return"，否则 "return <expr>"。
std::string Formatter::formatReturnStmt(ReturnStmt& node) {
    if (node.value) {
        return "return " + formatNode(node.value.get());
    }
    return "return";
}

/// 格式化 print 语句：输出 "print(arg1, arg2, ...)"，参数用逗号分隔符连接。
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

/// 格式化语句块：逐条格式化语句，按行号注入独立注释与行内尾注，函数/类声明间补空行，
/// 块末尾刷新尾部注释。是往返不变量中"注释保留"的核心协调逻辑。
std::string Formatter::formatBlock(Block& node) {
    std::string result;
    // P3 fix: 预估输出大小，避免反复 realloc（每条语句平均约 40 字符）
    result.reserve(node.statements.size() * 40);
    for (size_t i = 0; i < node.statements.size(); ++i) {
        ASTNode* stmt = node.statements[i].get();
        // F-P1-3 fix: 跳过空语句指针，避免后续 stmt->line 解引用空指针崩溃
        if (!stmt) continue;

        // F1 fix: 输出当前语句之前的所有独立注释（行号严格小于语句行号）
        // BUG-F-01 fix: 多行块注释经 reindentBlockComment 重新缩进，使后续行对齐当前缩进
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line < stmt->line) {
            result += indent() + reindentBlockComment(comments_[commentIndex_].lexeme, indent()) + "\n";
            commentIndex_++;
        }

        // 函数/类声明前加空行（BUG1 fix: 原代码在任意自终止语句间插入空行）
        // BUG-F-04 fix: isFunOrClass 解包 NODE_EXPORT_STMT，使 export fun/class 之间也插入空行
        if (options_.blankLineBetweenFunctions && i > 0) {
            auto isFunOrClass = [](ASTNode* n) {
                if (!n) return false;
                if (n->nodeType == NodeType::NODE_FUN_DECL ||
                    n->nodeType == NodeType::NODE_CLASS_DECL) return true;
                if (n->nodeType == NodeType::NODE_EXPORT_STMT) {
                    auto* exp = static_cast<ExportStmt*>(n);
                    if (exp->declaration) {
                        auto innerType = exp->declaration->nodeType;
                        return innerType == NodeType::NODE_FUN_DECL ||
                               innerType == NodeType::NODE_CLASS_DECL;
                    }
                }
                return false;
            };
            if (isFunOrClass(stmt)) {
                result += "\n";
            }
        }

        // 格式化语句
        std::string stmtText;
        if (isSelfTerminating(stmt)) {
            std::string nodeText = formatNode(stmt);
            // BUG-F-03 fix: 独立块（NODE_BLOCK 作为语句）经 visitBlock 输出 " {..."，
            // K&R 风格下 openBrace() 返回 " {" 带前置空格，作为独立语句时首部多余空格。
            // 去除 NODE_BLOCK 节点格式化结果开头的前置空格。
            if (stmt->nodeType == NodeType::NODE_BLOCK &&
                !nodeText.empty() && nodeText[0] == ' ') {
                nodeText.erase(0, 1);
            }
            stmtText = indent() + nodeText;
        } else {
            stmtText = indent() + formatNode(stmt) + ";";
        }

        // F1+ fix: 同行行内注释追加到语句末尾
        // BUG-F-01 fix: 多行块注释经 reindentBlockComment 重新缩进
        std::string trailing;
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line == stmt->line) {
            trailing += " " + reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            commentIndex_++;
        }

        result += stmtText + trailing + "\n";
    }

    // F1 fix: 块末尾输出尾部注释
    // L18 fix: 对所有块都刷新尾部注释，不仅仅是顶层块
    // BUG-F-01 fix: 多行块注释经 reindentBlockComment 重新缩进
    if (node.closingBraceLine > 0) {
        // 非顶层块：输出 closingBraceLine 及之前的注释
        // BUG-FMT-P2-1 fix: 原实现用 `< closingBraceLine` 不消费与 '}' 同行的注释，
        // 导致这些注释被外层 formatBlock 消费，注释位置错误（跑到外层块）。
        // 改为 `<= closingBraceLine`：与 '}' 同行的注释在块末尾输出（'{' 之前），
        // 虽非完美（原文是 `} // comment`，格式化后注释在 '}' 之前一行），
        // 但保证注释留在正确的块内，不破坏 AST 结构等价性。
        while (commentIndex_ < comments_.size() &&
               comments_[commentIndex_].line <= node.closingBraceLine) {
            result += indent() + reindentBlockComment(comments_[commentIndex_].lexeme, indent()) + "\n";
            commentIndex_++;
        }
    } else {
        // 顶层块（closingBraceLine == 0）：输出所有剩余注释
        while (commentIndex_ < comments_.size()) {
            result += indent() + reindentBlockComment(comments_[commentIndex_].lexeme, indent()) + "\n";
            commentIndex_++;
        }
    }

    return result;
}

// ---- 新增节点格式化 ----

/// 格式化数组字面量：输出 "[e1, e2, ...]"，元素递归格式化并用逗号分隔符连接。
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

/// 格式化字典字面量：输出 "{k1: v1, k2: v2, ...}"，键值对递归格式化、冒号加空格。
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

/// 格式化下标访问：obj[index]；当 obj 为低优先级的二元/一元表达式时加括号避免歧义。
std::string Formatter::formatIndexAccess(IndexAccess& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix: 低优先级表达式需要括号
    return obj + "[" + formatNode(node.index.get()) + "]";
}

/// 格式化下标赋值：obj[index] = value；obj 为低优先级表达式时加括号。
std::string Formatter::formatIndexAssign(IndexAssign& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "[" + formatNode(node.index.get()) + "]" + binOp("=") + formatNode(node.value.get());
}

/// 格式化类声明：输出 "class Name extends Super { members }"，相邻方法成员间按
/// blankLineBetweenFunctions 选项补空行，成员自终止时无需额外 ';'。
std::string Formatter::formatClassDecl(ClassDecl& node) {
    // PERF-27 fix: 预估输出大小（class + name + members），避免反复 realloc
    std::string result;
    result.reserve(32 + node.name.size() + node.members.size() * 48);
    result += "class " + node.name;
    if (!node.superClassName.empty()) {
        result += " extends " + node.superClassName;
    }
    result += openBrace() + "\n";
    currentIndent_++;
    for (size_t i = 0; i < node.members.size(); ++i) {
        auto& member = node.members[i];
        // F-P2-8 fix: 跳过空成员指针，避免 formatNode 返回 "null" 作为类成员
        if (!member) continue;
        // BUG-F-05 fix: blankLineBetweenFunctions 选项传播到类成员，
        // 当前后两个成员都是方法（FunDecl）时插入空行
        if (options_.blankLineBetweenFunctions && i > 0) {
            auto& prev = node.members[i - 1];
            if (prev && prev->nodeType == NodeType::NODE_FUN_DECL &&
                member->nodeType == NodeType::NODE_FUN_DECL) {
                result += "\n";
            }
        }
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

/// 格式化成员访问：obj.field；obj 为低优先级表达式时加括号，避免被错误解析为方法调用。
std::string Formatter::formatMemberAccess(MemberAccess& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "." + node.fieldName;
}

/// 格式化成员赋值：obj.field = value；obj 为低优先级表达式时加括号。
std::string Formatter::formatMemberAssign(MemberAssign& node) {
    std::string obj = formatNode(node.object.get());
    if (node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                        node.object->nodeType == NodeType::NODE_UNARY_OP))
        obj = "(" + obj + ")";  // FMT-01 fix
    return obj + "." + node.fieldName + binOp("=") + formatNode(node.value.get());
}

/// 格式化方法调用：obj.method(args...)；obj 为低优先级表达式时加括号，
/// 否则形如 (a+b).foo() 会被错误重新解析为独立 FunCall。
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

/// 格式化 null 字面量：裸输出 "null" 关键字。
std::string Formatter::formatNullLiteral(NullLiteral& node) {
    return "null";
}

// C5 fix: 插值字符串格式化 — 重建 `"text {expr} more {expr2}"` 语法
// literals.size() == expressions.size() + 1
// 输出: "literals[0]{expressions[0]}literals[1]{expressions[1]}...literals[n]"
std::string Formatter::formatInterpolatedString(InterpolatedString& node) {
    // PERF-27 fix: 预估输出大小（literals + expressions），避免反复 realloc
    std::string result;
    size_t estimatedSize = 2;  // 引号
    for (const auto& lit : node.literals) estimatedSize += lit.size();
    estimatedSize += node.expressions.size() * 8;  // 每个 {expr} 平均 8 字符
    result.reserve(estimatedSize);
    result += '"';  // 开头引号

    // AUDIT-P2 fix: 复用 escapeStringContent 公共函数，消除转义规则重复

    // 首个字面量片段
    if (!node.literals.empty()) {
        result += escapeStringContent(node.literals[0]);
    }

    // 交替输出: {expr} literal
    for (size_t i = 0; i < node.expressions.size(); ++i) {
        result += '{';
        // BUG-FMT-P2-4 fix: 防御性检查 expressions[i] 是否为 nullptr。
        // 原实现直接解引用 node.expressions[i]->line，外部构造的 AST 可能含 nullptr。
        if (node.expressions[i]) {
            result += formatNode(node.expressions[i].get());
            // BUG-F-06 fix / BUG-FMT-P1-1 fix: formatNode 不消费 comments_ 数组，
            // 插值表达式行号范围内的注释会落入外层语句的"同行尾部注释"分支被误用。
            // 原实现用 `<= exprLine` 会错误跳过与表达式同行的外层语句尾部注释
            // （如 `var s = "hello ${x}"; // comment` 中的 `// comment`）。
            // 改为 `< exprLine`：仅跳过严格在表达式之前的注释，保留同行注释给外层。
            int exprLine = node.expressions[i]->line;
            while (commentIndex_ < comments_.size() &&
                   comments_[commentIndex_].line < exprLine) {
                ++commentIndex_;
            }
        }
        result += '}';
        if (i + 1 < node.literals.size()) {
            result += escapeStringContent(node.literals[i + 1]);
        }
    }

    result += '"';  // 结尾引号
    return result;
}
