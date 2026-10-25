#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include "interpreter/Value.h" // ARCH-01 fix: getValue().toString() 需要 Value 完整定义
#include <algorithm>           // F-P2-5 fix: std::sort
#include <cstdio>              // AUDIT-P3.11 fix: snprintf 用于控制字符 \xNN 与非 BMP \u{XXXXXX} 转义
#include <sstream>

// ============================================================
// Formatter 代码格式化器实现
// ============================================================

// AUDIT-P2 fix: 提取公共字符串转义函数，供 formatStringLiteral 和
// formatInterpolatedString 共用，消除转义规则重复（原两处独立 switch
// 存在同步风险——Lexer 新增转义序列时需同时修改三处）。
// AUDIT-P3.11 fix: 原实现只转义 10 个常见控制字符，其余 25 个 C0 控制字符
// (0x01-0x06, 0x0E-0x1F) 和 DEL (0x7F) 走 default 直接输出原始字节，导致
// 格式化后的字符串含不可见控制字符，往返等价性虽不破坏但输出不可读且可能
// 引入安全问题。改为用 \xNN 转义所有未显式处理的控制字符。
// AUDIT-P3.12 fix: 非 BMP 字符（码点 > 0xFFFF，如 emoji）以 4 字节 UTF-8
// 序列存储。原实现按单字节遍历将其拆为 4 个 >= 0x80 的字节直接输出，虽往返
// 等价但无法与 Lexer 的 \u{XXXXXX} 扩展语法对应。现解析 UTF-8 多字节序列，
// 对非 BMP 码点输出 \u{XXXXXX} 格式，BMP 码点（<= 0xFFFF）保持原字节输出。
static std::string escapeStringContent(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\0':
            escaped += "\\0";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\a':
            escaped += "\\a";
            break;
        case '\v':
            escaped += "\\v";
            break;
        case '{':
            // AUDIT-R3 P1 fix: 字面 '{' 必须转义——Lexer 在字符串内将裸 '{' 无条件
            // 视为插值起始（string() → handleInterpolation），且不支持 \\{ 转义。
            // 若原样输出，含 \\x7b/\\u{7b} 转义的字符串经格式化后重新解析会报
            // "未终止的插值表达式"或语义变为插值，破坏往返等价性。
            // 输出为 \\x7b（Lexer handleHexEscape 可回读）。
            escaped += "\\x7b";
            break;
        default:
            // AUDIT-P3.12 fix: 处理多字节 UTF-8 字符（非 ASCII）
            if (c >= 0x80) {
                uint32_t codepoint = 0;
                int extraBytes = 0;
                if ((c & 0xE0) == 0xC0) {
                    codepoint = c & 0x1F;
                    extraBytes = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    codepoint = c & 0x0F;
                    extraBytes = 2;
                } else if ((c & 0xF8) == 0xF0) {
                    codepoint = c & 0x07;
                    extraBytes = 3;
                } else {
                    // 无效 UTF-8 首字节，按单字节输出
                    escaped += static_cast<char>(c);
                    continue;
                }
                bool valid = true;
                for (int j = 0; j < extraBytes; ++j) {
                    if (i + 1 + j >= s.size()) {
                        valid = false;
                        break;
                    }
                    unsigned char next = static_cast<unsigned char>(s[i + 1 + j]);
                    if ((next & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 6) | (next & 0x3F);
                }
                if (valid) {
                    if (codepoint > 0xFFFF) {
                        // AUDIT-P3.12 fix: 非 BMP 字符输出 \u{XXXXXX}
                        char buf[12];
                        std::snprintf(buf, sizeof(buf), "\\u{%x}", codepoint);
                        escaped += buf;
                    } else {
                        // BMP 字符保持原字节输出（含中文等 3 字节 UTF-8）
                        escaped += s.substr(i, static_cast<size_t>(1 + extraBytes));
                    }
                    i += static_cast<size_t>(extraBytes); // 跳过多字节序列的剩余字节
                } else {
                    // 不完整 UTF-8 序列，按单字节输出
                    escaped += static_cast<char>(c);
                }
                continue; // 已处理，跳过下方控制字符检查
            }
            // AUDIT-P3.11 fix: 转义所有未显式处理的 C0 控制字符和 DEL
            if (c < 0x20 || c == 0x7F) {
                char buf[5];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                escaped += buf;
            } else {
                escaped += static_cast<char>(c);
            }
            break;
        }
    }
    return escaped;
}

Formatter::Formatter() {}

void Formatter::setComments(const std::vector<Token>& tokens) {
    comments_.clear();
    for (const auto& tok : tokens) {
        // P1 fix: 同时收集行注释和块注释，避免块注释格式化后丢失
        if (tok.type == TokenType::TK_LINE_COMMENT || tok.type == TokenType::TK_BLOCK_COMMENT) {
            comments_.push_back(tok);
        }
    }
    // F-P2-5 fix: 按行号排序，确保 formatBlock 中的 commentIndex_ 单调递增游标正确工作
    // AUDIT-P2 fix: std::sort 非稳定排序，同行多注释的相对顺序未定义。改用 stable_sort
    // 并增加列号作为次要排序键，保证同行注释按源码出现顺序排列，避免格式化后注释顺序颠倒。
    std::stable_sort(comments_.begin(), comments_.end(), [](const Token& a, const Token& b) {
        if (a.line != b.line)
            return a.line < b.line;
        return a.column < b.column;
    });
    commentIndex_ = 0;
}

void Formatter::setIndentSize(int size) {
    // F-P1-2 fix: 校验缩进大小，负值会导致 std::string 构造时 size_t 溢出触发 bad_alloc
    if (size < 0 || size > 16)
        return;
    options_.indentSize = size;
    cachedIndentLevel_ = -1; // P3: 使缩进缓存失效
}

void Formatter::setOptions(const FormatOptions& options) {
    options_ = options;
    // P3: 选项变更时使缓存失效
    cachedIndentLevel_ = -1;
    commaCacheValid_ = false;
    binOpKey_.clear(); // P30: 使 binOp 缓存失效
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
            if (spaces < 0)
                spaces = 0;
            indentCache_ = std::string(static_cast<size_t>(spaces), ' ');
        }
    }
    return indentCache_;
}

/// 生成二元运算符文本：按 spaceAroundOperators 选项决定是否两侧加空格，带 MRU 缓存。
std::string Formatter::binOp(const std::string& op) const {
    if (options_.spaceAroundOperators) {
        // P30 fix: MRU 缓存 — 同一运算符连续调用时直接返回缓存
        if (op == binOpKey_)
            return binOpVal_;
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
/// OPT-12: 唯一的公共输出入口。out_ 的清理/取走只在此处发生；格式化过程从不
/// 递归调用 format()（visit* 只走 formatNode），此处仍用 swap 保存/恢复缓冲，
/// 以保证将来若出现嵌套调用也不会污染外层输出。
std::string Formatter::format(Block& program) {
    std::string outer;
    outer.swap(out_); // 正常运行时不持有内容；嵌套调用时保存外层缓冲
    currentIndent_ = 0;
    formatDepth_ = 0;  // D5 fix: 重置递归深度计数器
    commentIndex_ = 0; // F-P1-4 fix: 重置注释游标，确保 Formatter 对象复用时注释正确输出
    formatBlock(program);
    std::string result;
    result.swap(out_); // 取出本次格式化结果，out_ 恢复为空
    out_.swap(outer);  // 恢复外层缓冲（无嵌套时为空）
    return result;
}

/// 判断节点类型是否是自终止的复合语句（以 } 结尾，不需要额外 ;）
static bool isSelfTerminating(ASTNode* node) {
    if (!node)
        return false;
    switch (node->nodeType) {
    case NodeType::NODE_IF_STMT:
    case NodeType::NODE_WHILE_STMT:
    case NodeType::NODE_FOR_STMT:
    case NodeType::NODE_FUN_DECL:
    case NodeType::NODE_CLASS_DECL:
    case NodeType::NODE_BLOCK:
    case NodeType::NODE_DESTRUCTURE_BINDING: // P1 #26 fix: 解构绑定自带 ';'，自终止，避免 formatBlock 双重分号
    case NodeType::NODE_ENUM_DECL:           // P1 #29 fix: enum 以 '}' 结尾，自终止
    case NodeType::NODE_MACRO_DECL:          // 七特性 MVP 阶段 2: 宏声明以 '}' 结尾，自终止
    case NodeType::NODE_TRAIT_DECL:          // 七特性 MVP 阶段 3: trait 声明以 '}' 结尾，自终止
    case NodeType::NODE_TRY_STMT:            // P0 fix: try/catch 以 } 结尾，自终止
        return true;
    case NodeType::NODE_IMPORT_STMT: // P0 fix: visitImportStmt 已自行添加 ;
        return true;
    case NodeType::NODE_EXPORT_STMT: {
        // P0 fix: export 的自终止性取决于内层声明类型
        // export var x = 1 需要额外 ;，export fun/class 不需要
        auto* exportNode = static_cast<ExportStmt*>(node);
        if (exportNode->declaration) {
            auto innerType = exportNode->declaration->nodeType;
            if (innerType == NodeType::NODE_FUN_DECL || innerType == NodeType::NODE_CLASS_DECL) {
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

/// 格式化任意 AST 节点：经 Visitor 模式分派到 visit*，带递归深度保护（超深写入占位），
/// 文本直接追加到 out_。
void Formatter::formatNode(ASTNode* node) {
    // P2 #61 fix: 返回注释形式而非裸 "null"，避免与用户代码中的 null 字面量混淆
    if (!node) {
        out_ += "/* null-node */";
        return;
    }

    // D5 fix: 递归深度保护，防止极端嵌套 AST 导致栈溢出
    if (formatDepth_ >= MAX_FORMAT_DEPTH) {
        out_ += "/* 嵌套过深 */";
        return;
    }
    formatDepth_++;
    struct DepthGuard {
        int& d;
        ~DepthGuard() { d--; }
    } guard{formatDepth_};

    // 统一通过 Visitor 模式分派：node->accept(*this) 调用对应的 visit* 方法，
    // visit* 方法把格式化文本追加到 out_，替代原 25 路 switch。
    // F-P2-6 fix（OPT-12 改写）: 用追加前后长度差判断 visit* 是否产出文本，
    // 避免未覆盖的节点类型静默输出空内容（旧实现清空 lastFormatResult_ 后判空）。
    const size_t start = out_.size();
    node->accept(*this);
    if (out_.size() == start) {
        out_ += "/* unhandled node */";
    }
}

// ============================================================
// Visitor 模式：visit* 方法实现
// 每个 visit* 方法调用对应的 format* 逻辑，将文本追加到 out_，返回 void。
// ============================================================

void Formatter::visitBinaryOp(BinaryOp& node) {
    formatBinaryOp(node);
}

void Formatter::visitUnaryOp(UnaryOp& node) {
    formatUnaryOp(node);
}

void Formatter::visitNumberLiteral(NumberLiteral& node) {
    formatNumberLiteral(node);
}

void Formatter::visitStringLiteral(StringLiteral& node) {
    formatStringLiteral(node);
}

void Formatter::visitBoolLiteral(BoolLiteral& node) {
    formatBoolLiteral(node);
}

void Formatter::visitVarDecl(VarDecl& node) {
    formatVarDecl(node);
}

void Formatter::visitAssignment(Assignment& node) {
    formatAssignment(node);
}

void Formatter::visitVarRef(VarRef& node) {
    formatVarRef(node);
}

void Formatter::visitIfStmt(IfStmt& node) {
    formatIfStmt(node);
}

void Formatter::visitWhileStmt(WhileStmt& node) {
    formatWhileStmt(node);
}

void Formatter::visitForStmt(ForStmt& node) {
    formatForStmt(node);
}

void Formatter::visitFunDecl(FunDecl& node) {
    formatFunDecl(node);
}

void Formatter::visitFunCall(FunCall& node) {
    formatFunCall(node);
}

void Formatter::visitReturnStmt(ReturnStmt& node) {
    formatReturnStmt(node);
}

void Formatter::visitPrintStmt(PrintStmt& node) {
    formatPrintStmt(node);
}

void Formatter::visitBlock(Block& node) {
    // 保留原 formatNode 中 NODE_BLOCK 分支的行为：用花括号包裹 formatBlock 输出。
    // formatBlock 只格式化语句列表，不包含外层花括号，此处补上。
    // FMT-05 fix: 使用 openBrace() 支持 BraceStyle 配置
    // F-P2-1 fix: openBrace() 后添加换行，与 formatIfStmt 等保持一致，避免 { 与首条语句同行
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    formatBlock(node);
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

void Formatter::visitArrayLiteral(ArrayLiteral& node) {
    formatArrayLiteral(node);
}

void Formatter::visitDictLiteral(DictLiteral& node) {
    formatDictLiteral(node);
}

// R98 元组与解构：元组字面量格式化
void Formatter::visitTupleLiteral(TupleLiteral& node) {
    formatTupleLiteral(node);
}

// R98 元组与解构：解构绑定格式化
void Formatter::visitDestructureBinding(DestructureBinding& node) {
    formatDestructureBinding(node);
}

// R99 枚举与 ADT：enum 声明格式化
void Formatter::visitEnumDecl(EnumDecl& node) {
    formatEnumDecl(node);
}

// R99 枚举与 ADT：enum variant 构造表达式格式化
void Formatter::visitEnumVariantExpr(EnumVariantExpr& node) {
    formatEnumVariantExpr(node);
}

// R99 枚举与 ADT：match 表达式格式化
void Formatter::visitMatchExpr(MatchExpr& node) {
    formatMatchExpr(node);
}

void Formatter::visitIndexAccess(IndexAccess& node) {
    formatIndexAccess(node);
}

void Formatter::visitIndexAssign(IndexAssign& node) {
    formatIndexAssign(node);
}

void Formatter::visitClassDecl(ClassDecl& node) {
    formatClassDecl(node);
}

void Formatter::visitMemberAccess(MemberAccess& node) {
    formatMemberAccess(node);
}

void Formatter::visitMemberAssign(MemberAssign& node) {
    formatMemberAssign(node);
}

void Formatter::visitMethodCall(MethodCall& node) {
    formatMethodCall(node);
}

void Formatter::visitNullLiteral(NullLiteral& node) {
    formatNullLiteral(node);
}

void Formatter::visitSuperExpr(SuperExpr& /*node*/) {
    // SuperExpr 无对应 format* 方法，保留原 formatNode 中 NODE_SUPER_EXPR 分支的行为
    out_ += "super";
}

void Formatter::visitBreakStmt(BreakStmt& /*node*/) {
    out_ += "break";
}

void Formatter::visitContinueStmt(ContinueStmt& /*node*/) {
    out_ += "continue";
}

void Formatter::visitThrowStmt(ThrowStmt& node) {
    out_ += "throw";
    if (node.expression) {
        out_ += ' ';
        formatNode(node.expression.get());
    } else {
        // BUG-F-08 fix: 空 throw（expression 为 nullptr）时输出 "throw null" 作为兜底。
        // Parser 强制要求表达式故正常路径不会产生此 AST，但外部构造的 AST 会触发。
        out_ += " null";
    }
}

void Formatter::visitYieldExpr(YieldExpr& node) {
    // AUDIT-R6 F7 fix: 补齐 R164 yield 表达式的 visitor 覆盖。原 Formatter 未 override
    // visitYieldExpr，DefaultVisitor 空实现不写输出，yield 节点被格式化为
    // 上一节点的残留文本（往返不等价，生成器函数 format 后语义损坏）。
    out_ += "yield";
    if (node.value) {
        out_ += ' ';
        formatNode(node.value.get());
    }
}

// 七特性 MVP 阶段 2：宏声明格式化——打印原始声明语法（body 模板未展开）。
// 以 '}' 结尾自终止（isSelfTerminating 已登记 NODE_MACRO_DECL）。
void Formatter::visitMacroDecl(MacroDecl& node) {
    out_ += "macro ";
    out_ += node.name;
    out_ += '(';
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (i > 0)
            out_ += ", ";
        out_ += node.params[i];
    }
    out_ += ')';
    // 七特性宏升级：语句宏的 body 是 Block，formatNode 已含 `{ ... }`，直接拼接；
    // 表达式宏的 body 是单表达式，包以 `{ expr }` 单行形式（保持原有往返）。
    if (node.isStatementMacro) {
        out_ += ' ';
        if (node.bodyExpr)
            formatNode(node.bodyExpr.get());
        else
            out_ += "{}";
    } else {
        out_ += " { ";
        if (node.bodyExpr)
            formatNode(node.bodyExpr.get());
        out_ += " }";
    }
}

// 七特性 MVP 阶段 2：宏调用格式化——打印 name!(args) 原始调用形式
// （而非展开后的子树，保证往返等价：重新 parse 时宏表重建、展开一致）。
void Formatter::visitMacroCallExpr(MacroCallExpr& node) {
    out_ += node.name;
    out_ += "!(";
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0)
            out_ += ", ";
        formatNode(node.arguments[i].get());
    }
    out_ += ')';
}

// 七特性 MVP 阶段 3：trait 声明格式化——打印完整 trait 体（方法列表）。
// 以 '}' 结尾自终止（isSelfTerminating 已登记 NODE_TRAIT_DECL）。
void Formatter::visitTraitDecl(TraitDecl& node) {
    out_ += "trait ";
    out_ += node.name;
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    for (const auto& method : node.methods) {
        if (!method)
            continue;
        out_ += indent();
        formatNode(method.get());
        out_ += '\n';
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

// 七特性 MVP 阶段 4：await 表达式格式化——打印 `await <operand>`。
void Formatter::visitAwaitExpr(AwaitExpr& node) {
    out_ += "await ";
    if (node.operand)
        formatNode(node.operand.get());
}

void Formatter::visitTryStmt(TryStmt& node) {
    // P2 fix: visitBlock 以 " {" 开头，"try" 后无需额外空格
    // BUG-FE-AUDIT-1 fix: 同步 BUG-AUDIT-FINALLY-1 的 finally 语法支持。
    //   - catch 块可选（try-finally 无 catch 时不输出 " catch (...)"）
    //   - finally 块可选，存在时输出 " finally" + formatNode(finallyBlock)
    //   - 修复幂等性违反（原实现无条件输出 catch 且忽略 finally，导致 format 后重新 parse 报错）
    // AUDIT-P2.9 fix: 在 tryBlock/catch/finally 之间注入独立注释。
    //   formatBlock 只消费 closingBraceLine 及之前的注释，块间注释（closingBraceLine
    //   与 catchKeywordLine/finallyKeywordLine 之间）原被外层 formatBlock 误消费
    //   到下一个语句前。现由 visitTryStmt 在块衔接处精确注入。
    out_ += "try";
    formatNode(node.tryBlock.get());
    if (node.catchBlock) {
        // AUDIT-P2.9 fix: 注入 tryBlock 闭合 '}' 与 catch 关键字之间的独立注释
        // tryBlock/catchBlock 是 ASTNode*，需 static_cast<Block*> 访问 closingBraceLine
        int tryEndLine = node.tryBlock ? static_cast<Block*>(node.tryBlock.get())->closingBraceLine : 0;
        int catchStartLine =
            node.catchKeywordLine > 0 ? node.catchKeywordLine : (node.catchBlock ? node.catchBlock->line : 0);
        if (tryEndLine > 0 && catchStartLine > 0) {
            while (commentIndex_ < comments_.size() && comments_[commentIndex_].line > tryEndLine &&
                   comments_[commentIndex_].line < catchStartLine) {
                // AUDIT-P1-ROUND49 fix: 补尾部 "\n"，对齐 formatBlock/formatClassDecl 的注释注入模式。
                // reindentBlockComment 不添加尾部换行，原实现缺少 "\n" 导致 catch 关键字
                // 被并入注释行（"/* comment */ catch ("）。
                out_ += '\n';
                out_ += indent();
                out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
                out_ += '\n';
                commentIndex_++;
            }
        }
        out_ += " catch (";
        out_ += node.catchVarName;
        out_ += ')';
        formatNode(node.catchBlock.get());
    }
    if (node.finallyBlock) {
        // AUDIT-P2.9 fix: 注入 catch/try 闭合 '}' 与 finally 关键字之间的独立注释
        int prevEndLine = 0;
        if (node.catchBlock)
            prevEndLine = static_cast<Block*>(node.catchBlock.get())->closingBraceLine;
        else if (node.tryBlock)
            prevEndLine = static_cast<Block*>(node.tryBlock.get())->closingBraceLine;
        int finallyStartLine = node.finallyKeywordLine > 0 ? node.finallyKeywordLine : node.finallyBlock->line;
        if (prevEndLine > 0 && finallyStartLine > 0) {
            while (commentIndex_ < comments_.size() && comments_[commentIndex_].line > prevEndLine &&
                   comments_[commentIndex_].line < finallyStartLine) {
                // AUDIT-P1-ROUND49 fix: 补尾部 "\n"，对齐 formatBlock/formatClassDecl 的注释注入模式。
                // 原实现缺少 "\n" 导致 finally 关键字被并入注释行。
                out_ += '\n';
                out_ += indent();
                out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
                out_ += '\n';
                commentIndex_++;
            }
        }
        out_ += " finally";
        formatNode(node.finallyBlock.get());
    }
}

void Formatter::visitImportStmt(ImportStmt& node) {
    out_ += "import ";
    // AUDIT-R5 BUG-04 fix: 命名空间导入（import * as ns from "path";）原被退化
    // 为裸 import，重新解析后 ns 绑定丢失，运行时报“未定义的变量: ns”——
    // P2-11 新增语法后未同步 Formatter，破坏往返等价性且改变语义。
    if (!node.namespaceAlias.empty()) {
        out_ += "* as ";
        out_ += node.namespaceAlias;
        out_ += " from ";
    } else if (!node.importAll && !node.names.empty()) {
        out_ += "{ ";
        for (size_t i = 0; i < node.names.size(); ++i) {
            if (i > 0)
                out_ += ", ";
            out_ += node.names[i];
        }
        out_ += " } from ";
    }
    out_ += '"';
    out_ += node.modulePath;
    out_ += "\";";
}

void Formatter::visitExportStmt(ExportStmt& node) {
    out_ += "export ";
    formatNode(node.declaration.get());
}

// C5 fix: 插值字符串格式化 — 重建 `"text {expr} more {expr2}"` 语法
void Formatter::visitInterpolatedString(InterpolatedString& node) {
    formatInterpolatedString(node);
}

// 运算符优先级表（数值越大优先级越高）
/// 运算符优先级查询：委托 BinaryOp::precedence 单一来源，避免与 Parser 重复优先级表。
int Formatter::opPrecedence(BinOpType opType) {
    // C14 fix: 委托给 BinaryOp::precedence 单一来源，消除 Formatter 与 Parser 的重复优先级表
    return BinaryOp::precedence(opType);
}

/// 右结合性查询：委托 BinaryOp::isRightAssociative 单一来源。
bool Formatter::isRightAssoc(BinOpType opType) {
    // C14 fix: 委托 BinaryOp::isRightAssociative 单一来源
    return BinaryOp::isRightAssociative(opType);
}

/// 判断子表达式是否需要加括号
static bool needsParens(ASTNode* child, BinOpType parentOpType, bool isRight) {
    if (!child || child->nodeType != NodeType::NODE_BINARY_OP)
        return false;
    BinaryOp* childBin = static_cast<BinaryOp*>(child);
    int parentPrec = Formatter::opPrecedence(parentOpType);
    int childPrec = Formatter::opPrecedence(childBin->opType);
    // 子优先级更低 → 需要括号
    if (childPrec < parentPrec)
        return true;
    // 同优先级时，右结合运算符的右操作数不需要括号，左操作数也不需要
    // 但对于左结合运算符的右操作数，如果子也是同优先级，需要括号（如 a - (b - c)）
    if (childPrec == parentPrec) {
        if (Formatter::isRightAssoc(parentOpType)) {
            return !isRight; // 右结合：左操作数需要括号
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
/// OPT-12: needsParens 是纯函数（只看 AST 节点类型），先判定再追加 '('，
/// 使输出保持纯尾部追加，无需在缓冲中回插。
void Formatter::formatBinaryOp(BinaryOp& node) {
    const bool parenLeft = needsParens(node.left.get(), node.opType, false);
    if (parenLeft)
        out_ += '(';
    formatNode(node.left.get());
    if (parenLeft)
        out_ += ')';
    out_ += binOp(BinaryOp::opTypeStr(node.opType));
    const bool parenRight = needsParens(node.right.get(), node.opType, true);
    if (parenRight)
        out_ += '(';
    formatNode(node.right.get());
    if (parenRight)
        out_ += ')';
}

/// 格式化一元运算（- / + / not）：当操作数本身是二元或一元表达式时加括号，
/// 避免 -(a + b) 被错误输出为 -a + b 这类优先级错乱。
/// OPT-12: 前缀由 opType 决定、括号由操作数节点类型决定，均可在追加操作数前确定。
void Formatter::formatUnaryOp(UnaryOp& node) {
    if (node.opType == UnaryOp::UnaryOpType::UOP_NOT) {
        out_ += "not ";
    } else if (node.opType == UnaryOp::UnaryOpType::UOP_PLUS) {
        out_ += '+';
    } else if (node.opType == UnaryOp::UnaryOpType::UOP_UNKNOWN) {
        // BUG-FMT-P2-2 fix: UOP_UNKNOWN 输出 `/* unknown */` 注释会被 Lexer 剥离，
        // 重新解析后丢失 UnaryOp 节点，AST 结构从 UnaryOp 变为裸 operand。
        // 改为输出 UOP_PLUS（最接近"无操作"的语义），保持 AST 结构可重新解析。
        out_ += '+';
    } else {
        out_ += '-';
    }
    // BinaryOp 优先级低于一元运算符，必须加括号保持语义正确
    // 例如 -(a + b) 不能格式化为 -a + b
    const bool paren = node.operand && (node.operand->nodeType == NodeType::NODE_BINARY_OP ||
                                        node.operand->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '(';
    formatNode(node.operand.get());
    if (paren)
        out_ += ')';
}

/// 格式化数值字面量：整数直出；浮点若经 toString 丢失小数点（如 1.0 → "1"）
/// 则补 ".0"，否则重新解析会被识别为整型，破坏 AST 往返不变量。
void Formatter::formatNumberLiteral(NumberLiteral& node) {
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
            out_ += "0.0/* ";
            out_ += s;
            out_ += " */";
            return;
        }
        if (s.find('.') != std::string::npos || s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
            out_ += s;
            return;
        }
        out_ += s;
        out_ += ".0";
        return;
    }
    out_ += v.toString(); // A1 fix: getValue() 按需构造
}

/// 格式化字符串字面量：对源码字符串按 JSON 风格转义（\\、"、\n、\t、\r、\0 等），
/// 两端补双引号，确保输出可重新被 Lexer 正确切分为单个字符串 Token。
void Formatter::formatStringLiteral(StringLiteral& node) {
    // AUDIT-P2 fix: 复用 escapeStringContent 公共函数，消除转义规则重复
    out_ += '"';
    out_ += escapeStringContent(node.value);
    out_ += '"';
}

void Formatter::formatBoolLiteral(BoolLiteral& node) {
    out_ += node.value ? "true" : "false";
}

/// 格式化变量声明：有类型标注时输出 "Type name"，否则 "var name"；
/// 含初始化表达式时用 binOp("=") 生成带空格的赋值符。
void Formatter::formatVarDecl(VarDecl& node) {
    if (!node.typeAnnotation.empty()) {
        out_ += node.typeAnnotation;
        out_ += ' ';
        out_ += node.name;
    } else {
        out_ += "var ";
        out_ += node.name;
    }
    if (node.initializer) {
        out_ += binOp("=");
        formatNode(node.initializer.get());
    }
}

/// 格式化赋值语句：name = value；value 为空（外部构造的 AST）时降级输出 "name = /* null */"
/// 而非崩溃，保持输出可解析。
void Formatter::formatAssignment(Assignment& node) {
    out_ += node.name;
    out_ += binOp("=");
    // F-P2-10 fix: 检查 value 空指针，避免输出 "x = null" 语义错误
    if (!node.value) {
        out_ += "/* null */";
        return;
    }
    formatNode(node.value.get());
}

/// 格式化变量引用：直接输出标识符名称（裸叶子节点，无需括号或空格）。
void Formatter::formatVarRef(VarRef& node) {
    out_ += node.name;
}

/// 格式化 if 语句：保留单语句体原貌（无花括号）以保往返；复合体或 else 链才包裹花括号。
/// 依据 isSelfTerminating 决定是否在单语句体后补 ';'。
void Formatter::formatIfStmt(IfStmt& node) {
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构从 Stmt 变为 Block{Stmt}
    // 原 Bug：无条件 openBrace() 包裹，导致 if (cond) print(1); → if (cond) { print(1); }
    // 重新解析后 thenBranch 节点类型从 NODE_PRINT_STMT 变为 NODE_BLOCK，违反往返等价性
    const bool thenIsBlock = node.thenBranch && node.thenBranch->nodeType == NodeType::NODE_BLOCK;
    if (!thenIsBlock && node.thenBranch) {
        // 单语句体路径：无花括号，与 Parser 的无花括号语法对应
        out_ += "if (";
        formatNode(node.condition.get());
        out_ += ") ";
        formatNode(node.thenBranch.get());
        if (!isSelfTerminating(node.thenBranch.get()))
            out_ += ';';
        if (node.elseBranch) {
            if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
                out_ += " else ";
                formatNode(node.elseBranch.get());
            } else if (node.elseBranch->nodeType == NodeType::NODE_BLOCK) {
                auto* block = static_cast<Block*>(node.elseBranch.get());
                out_ += " else";
                out_ += openBrace();
                out_ += '\n';
                currentIndent_++;
                formatBlock(*block);
                currentIndent_--;
                out_ += indent();
                out_ += '}';
            } else {
                // else 单语句体：同样无花括号
                out_ += " else ";
                formatNode(node.elseBranch.get());
                if (!isSelfTerminating(node.elseBranch.get()))
                    out_ += ';';
            }
        }
        return;
    }
    // Block 体路径：原有花括号逻辑
    out_ += "if (";
    formatNode(node.condition.get());
    out_ += ')';
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    // F-P2-3 fix: 检查 thenBranch 空指针，避免 formatNode 返回 "null" 语义错误
    if (!node.thenBranch) {
        out_ += indent();
        out_ += "/* empty */\n";
    } else if (node.thenBranch->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断，O(1) vs O(RTTI)
        auto* block = static_cast<Block*>(node.thenBranch.get());
        formatBlock(*block);
    } else if (isSelfTerminating(node.thenBranch.get())) {
        // L17 revert: 裸复合语句（if/while/for）的 formatNode 内部已用
        // currentIndent_++ 处理 body 缩进，此处再 +1 会导致双重缩进。
        out_ += indent();
        formatNode(node.thenBranch.get());
        out_ += '\n';
    } else {
        out_ += indent();
        formatNode(node.thenBranch.get());
        out_ += ";\n";
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';

    if (node.elseBranch) {
        // else if 分支：elseBranch 是 IfStmt
        if (node.elseBranch->nodeType == NodeType::NODE_IF_STMT) {
            // F-P1-1 fix: 改用 formatNode 分派，让 else-if 链也受 MAX_FORMAT_DEPTH 深度保护
            // 输出结果一致：" else " + "if (...) { ... }" = " else if (...) { ... }"
            out_ += " else ";
            formatNode(node.elseBranch.get());
        } else if (node.elseBranch->nodeType == NodeType::NODE_BLOCK) {
            // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
            auto* block = static_cast<Block*>(node.elseBranch.get());
            out_ += " else";
            out_ += openBrace();
            out_ += '\n';
            currentIndent_++;
            formatBlock(*block);
            currentIndent_--;
            out_ += indent();
            out_ += '}';
        } else if (isSelfTerminating(node.elseBranch.get())) {
            out_ += " else";
            out_ += openBrace();
            out_ += '\n';
            currentIndent_++;
            out_ += indent();
            formatNode(node.elseBranch.get());
            out_ += '\n';
            currentIndent_--;
            out_ += indent();
            out_ += '}';
        } else {
            out_ += " else";
            out_ += openBrace();
            out_ += '\n';
            currentIndent_++;
            out_ += indent();
            formatNode(node.elseBranch.get());
            out_ += ";\n";
            currentIndent_--;
            out_ += indent();
            out_ += '}';
        }
    }
}

/// 格式化 while 语句：与 if 一致的单语句体/复合体策略，保持 AST 往返等价。
void Formatter::formatWhileStmt(WhileStmt& node) {
    if (!node.body) {
        out_ += "/* empty */"; // P1 #28 fix: 空指针保护
        return;
    }
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构改变
    if (node.body->nodeType != NodeType::NODE_BLOCK) {
        out_ += "while (";
        formatNode(node.condition.get());
        out_ += ") ";
        formatNode(node.body.get());
        if (!isSelfTerminating(node.body.get()))
            out_ += ';';
        return;
    }
    out_ += "while (";
    formatNode(node.condition.get());
    out_ += ')';
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        out_ += indent();
        formatNode(node.body.get());
        out_ += '\n';
    } else {
        out_ += indent();
        formatNode(node.body.get());
        out_ += ";\n";
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

/// 格式化 for 语句：输出 "for (init; cond; update) body"，空 update/cond 时省略空格，
/// 单语句体与原貌一致、复合体包裹花括号。
void Formatter::formatForStmt(ForStmt& node) {
    if (!node.body) {
        out_ += "/* empty */"; // P1 #28 fix: 空指针保护
        return;
    }
    // P1-D fix: 保留单语句体原貌（无花括号），避免往返后 AST 结构改变
    if (node.body->nodeType != NodeType::NODE_BLOCK) {
        out_ += "for (";
        if (node.initializer)
            formatNode(node.initializer.get());
        out_ += ';';
        if (node.condition) {
            out_ += ' ';
            formatNode(node.condition.get());
        }
        out_ += ';';
        if (node.update) {
            out_ += ' ';
            formatNode(node.update.get());
        }
        out_ += ") ";
        formatNode(node.body.get());
        if (!isSelfTerminating(node.body.get()))
            out_ += ';';
        return;
    }
    // F-P2-9 fix: 条件化添加分号和空格，避免 update 为空时产生 "for (init; cond; ) {" 多余空格
    out_ += "for (";
    if (node.initializer)
        formatNode(node.initializer.get());
    out_ += ';';
    if (node.condition) {
        out_ += ' ';
        formatNode(node.condition.get());
    }
    out_ += ';';
    if (node.update) {
        out_ += ' ';
        formatNode(node.update.get());
    }
    out_ += ')';
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        out_ += indent();
        formatNode(node.body.get());
        out_ += '\n';
    } else {
        out_ += indent();
        formatNode(node.body.get());
        out_ += ";\n";
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

/// 格式化函数声明：输出 "fun name(params): retType { body }"，参数支持类型标注与默认值，
/// 函数体以 } 自终止，外部由 formatBlock 决定是否补前导空行。
void Formatter::formatFunDecl(FunDecl& node) {
    if (!node.body) {
        out_ += "/* empty */"; // P1 #28 fix: 空指针保护
        return;
    }
    // R98 W3: 匿名 lambda（name 为空）格式化为 `fun(params)`，具名函数为 `fun name(params)`
    // AUDIT-R6 F7b fix: 生成器函数保留 `fun*` 标记——原实现丢失 '*'，往返后
    // 函数体内的 yield 变为非法语法（"yield 只能出现在 fun* 生成器函数体内"）。
    // 七特性 MVP 阶段 4：async fun 打印 `async fun`（优先于 fun*，二者语义等价，
    // 恢复原声明形式；async fun 内部 isGenerator 也为 true，但不应打印 fun*）。
    std::string funKw;
    if (node.isConstFun)
        funKw = "const ";
    funKw += node.isAsync ? "async fun" : (node.isGenerator ? "fun*" : "fun");
    if (node.name.empty()) {
        out_ += funKw;
    } else {
        out_ += funKw;
        out_ += ' ';
        out_ += node.name;
    }
    // R163 泛型扩展：输出类型参数列表 <T, E, ...>（与 formatEnumDecl 一致）
    if (!node.typeParams.empty()) {
        out_ += '<';
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                out_ += comma();
            out_ += node.typeParams[i];
        }
        out_ += '>';
    }
    out_ += '(';
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (i > 0)
            out_ += comma();
        out_ += node.params[i];
        if (!node.paramTypes.empty() && i < node.paramTypes.size() && !node.paramTypes[i].empty()) {
            out_ += ": ";
            out_ += node.paramTypes[i];
        }
        // F10: 格式化默认参数值
        if (i < node.defaultValues.size() && node.defaultValues[i]) {
            out_ += " = ";
            formatNode(node.defaultValues[i].get());
        }
    }
    out_ += ')';
    if (!node.returnType.empty()) {
        out_ += ": ";
        out_ += node.returnType;
    }
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    if (node.body->nodeType == NodeType::NODE_BLOCK) {
        // PERF-26 fix: dynamic_cast 改 nodeType 分支判断
        auto* block = static_cast<Block*>(node.body.get());
        formatBlock(*block);
    } else if (isSelfTerminating(node.body.get())) {
        // L17 revert: formatNode 内部已处理 body 缩进，无需额外 +1
        out_ += indent();
        formatNode(node.body.get());
        out_ += '\n';
    } else {
        out_ += indent();
        formatNode(node.body.get());
        out_ += ";\n";
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

/// 格式化函数调用：优先输出 "name(args)"；存在表达式型 callee（链式/成员调用）时
/// 对低优先级 callee 加括号，保证重新解析得到与原 AST 一致的调用结构。
void Formatter::formatFunCall(FunCall& node) {
    if (node.callee) {
        // 链式调用 / 表达式调用
        // F-P2-12 fix: 移除冗余的 node.callee 二次检查（已在 if 分支内）
        // BUG-FMT-P1-2 fix: NODE_MEMBER_ACCESS 类型的 callee 也需加括号。
        // 原实现仅对 BINARY_OP/UNARY_OP 加括号，输出 `obj.field(args)` 会被
        // Parser 重新解析为 MethodCall（绑定 this），而非 FunCall(callee=MemberAccess)，
        // 破坏 AST 结构等价性与三后端语义。
        const bool paren = node.callee->nodeType == NodeType::NODE_BINARY_OP ||
                           node.callee->nodeType == NodeType::NODE_UNARY_OP ||
                           node.callee->nodeType == NodeType::NODE_MEMBER_ACCESS;
        if (paren)
            out_ += '(';
        formatNode(node.callee.get());
        if (paren)
            out_ += ')';
    } else {
        out_ += node.name;
    }
    out_ += '(';
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.arguments[i].get());
    }
    out_ += ')';
}

/// 格式化 return 语句：无返回值输出 "return"，否则 "return <expr>"。
void Formatter::formatReturnStmt(ReturnStmt& node) {
    out_ += "return";
    if (node.value) {
        out_ += ' ';
        formatNode(node.value.get());
    }
}

/// 格式化 print 语句：输出 "print(arg1, arg2, ...)"，参数用逗号分隔符连接。
void Formatter::formatPrintStmt(PrintStmt& node) {
    out_ += "print(";
    for (size_t i = 0; i < node.values.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.values[i].get());
    }
    out_ += ')';
}

/// 格式化语句块：逐条格式化语句，按行号注入独立注释与行内尾注，函数/类声明间补空行，
/// 块末尾刷新尾部注释。是往返不变量中"注释保留"的核心协调逻辑。
void Formatter::formatBlock(Block& node) {
    for (size_t i = 0; i < node.statements.size(); ++i) {
        ASTNode* stmt = node.statements[i].get();
        // F-P1-3 fix: 跳过空语句指针，避免后续 stmt->line 解引用空指针崩溃
        if (!stmt)
            continue;

        // F1 fix: 输出当前语句之前的所有独立注释（行号严格小于语句行号）
        // BUG-F-01 fix: 多行块注释经 reindentBlockComment 重新缩进，使后续行对齐当前缩进
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line < stmt->line) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }

        // 函数/类声明前加空行（BUG1 fix: 原代码在任意自终止语句间插入空行）
        // BUG-F-04 fix: isFunOrClass 解包 NODE_EXPORT_STMT，使 export fun/class 之间也插入空行
        if (options_.blankLineBetweenFunctions && i > 0) {
            auto isFunOrClass = [](ASTNode* n) {
                if (!n)
                    return false;
                if (n->nodeType == NodeType::NODE_FUN_DECL || n->nodeType == NodeType::NODE_CLASS_DECL)
                    return true;
                if (n->nodeType == NodeType::NODE_EXPORT_STMT) {
                    auto* exp = static_cast<ExportStmt*>(n);
                    if (exp->declaration) {
                        auto innerType = exp->declaration->nodeType;
                        return innerType == NodeType::NODE_FUN_DECL || innerType == NodeType::NODE_CLASS_DECL;
                    }
                }
                return false;
            };
            if (isFunOrClass(stmt)) {
                out_ += '\n';
            }
        }

        // 格式化语句（OPT-12: 直接追加到 out_，不再经 stmtText 中转）
        // AUDIT-P2.4 fix: 重置 lastNodeEndLine_ 为语句起始行。formatNode 内部的
        // formatInterpolatedString 会覆盖为 endLine（若含多行插值字符串）。
        lastNodeEndLine_ = stmt->line;
        out_ += indent();
        if (isSelfTerminating(stmt)) {
            const size_t nodeStart = out_.size();
            formatNode(stmt);
            // BUG-F-03 fix: 独立块（NODE_BLOCK 作为语句）经 visitBlock 输出 " {..."，
            // K&R 风格下 openBrace() 返回 " {" 带前置空格，作为独立语句时首部多余空格。
            // 去除 NODE_BLOCK 节点格式化结果开头的前置空格（只删除刚追加的尾段首字符）。
            if (stmt->nodeType == NodeType::NODE_BLOCK && out_.size() > nodeStart && out_[nodeStart] == ' ') {
                out_.erase(nodeStart, 1);
            }
        } else {
            formatNode(stmt);
            out_ += ';';
        }

        // F1+ fix: 同行行内注释追加到语句末尾
        // BUG-F-01 fix: 多行块注释经 reindentBlockComment 重新缩进
        // AUDIT-P2.4 fix: 用 <= lastNodeEndLine_ 替代 == stmt->line，
        // 消费多行语句结束行上的注释（如多行插值字符串闭合引号后的尾注）。
        // formatInterpolatedString 的循环已跳过 endLine 之前的内部注释，
        // 此处 commentIndex_ 指向 >= lastNodeEndLine_ 的注释，<= 仅消费结束行上的注释。
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line <= lastNodeEndLine_) {
            out_ += ' ';
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            commentIndex_++;
        }

        out_ += '\n';
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
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line <= node.closingBraceLine) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }
    } else {
        // 顶层块（closingBraceLine == 0）：输出所有剩余注释
        while (commentIndex_ < comments_.size()) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }
    }
}

// ---- 新增节点格式化 ----

/// 格式化数组字面量：输出 "[e1, e2, ...]"，元素递归格式化并用逗号分隔符连接。
void Formatter::formatArrayLiteral(ArrayLiteral& node) {
    out_ += '[';
    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.elements[i].get());
    }
    out_ += ']';
}

/// 格式化字典字面量：输出 "{k1: v1, k2: v2, ...}"，键值对递归格式化、冒号加空格。
void Formatter::formatDictLiteral(DictLiteral& node) {
    out_ += '{';
    for (size_t i = 0; i < node.pairs.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.pairs[i].first.get());
        out_ += ": ";
        formatNode(node.pairs[i].second.get());
    }
    out_ += '}';
}

// R98 元组与解构：格式化元组字面量
// 输出 "(e1, e2, ...)"；单元素元组保留尾逗号 "(e1,)" 以区别于分组表达式 "(e1)"
void Formatter::formatTupleLiteral(TupleLiteral& node) {
    out_ += '(';
    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.elements[i].get());
    }
    // 单元素元组保留尾逗号
    if (node.elements.size() == 1) {
        out_ += ',';
    }
    out_ += ')';
}

// R98 元组与解构：格式化解构绑定
// 输出 "var (a, b, c) = initializer;"
// 类型注解（如存在）输出为 "var (a, b, c): (int, string, int) = initializer;"
// L20 per-name 注解：输出 "var (a: int, b: string) = initializer;"（优先于 tupleTypeAnnotation）
void Formatter::formatDestructureBinding(DestructureBinding& node) {
    out_ += "var (";
    for (size_t i = 0; i < node.names.size(); ++i) {
        if (i > 0)
            out_ += comma();
        out_ += node.names[i];
        // L20: per-name 类型注解（优先于整体 tupleTypeAnnotation）
        if (node.hasNameTypeAnnotations()) {
            const std::string& nameAnn = node.nameTypeAt(i);
            if (!nameAnn.empty()) {
                out_ += ": ";
                out_ += nameAnn;
            }
        }
    }
    out_ += ')';
    // L20: 仅当无 per-name 注解时才输出整体 tupleTypeAnnotation（避免双重注解）
    if (!node.hasNameTypeAnnotations() && !node.tupleTypeAnnotation.empty()) {
        out_ += ": ";
        out_ += node.tupleTypeAnnotation;
    }
    if (node.initializer) {
        out_ += " = ";
        formatNode(node.initializer.get());
    }
    out_ += ';';
}

// R99 枚举与 ADT：格式化 enum 声明
// 输出 "enum Name<T, U> { Variant1, Variant2(T), Variant3(T, U) }"
// 缩进与 formatClassDecl 一致：currentIndent_+1 用于 variant 列表。
void Formatter::formatEnumDecl(EnumDecl& node) {
    out_ += "enum ";
    out_ += node.name;
    if (!node.typeParams.empty()) {
        out_ += '<';
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                out_ += comma();
            out_ += node.typeParams[i];
        }
        out_ += '>';
    }
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    for (size_t i = 0; i < node.variants.size(); ++i) {
        const auto& v = node.variants[i];
        // 注释注入：variant 行号前的独立注释
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line < node.line) {
            // 跳过 enum 声明行之前的注释（已被外层 formatBlock 处理）
            commentIndex_++;
        }
        out_ += indent();
        out_ += v.name;
        if (!v.paramTypes.empty()) {
            out_ += '(';
            for (size_t j = 0; j < v.paramTypes.size(); ++j) {
                if (j > 0)
                    out_ += comma();
                out_ += v.paramTypes[j];
            }
            out_ += ')';
        }
        // AUDIT-R5 BUG-05 fix: variant 分隔符改为 ','——Parser::enumDecl 仅接受
        // 逗号分隔（尾逗号可选，Parser.cpp L1210-1213），原输出 ';' 使所有
        // enum 声明格式化后不可再解析（报“期望 variant 名称”），违反
        // “formatter 不得输出不可再解析代码”约束。尾 variant 后的逗号合法（尾逗号可选）。
        out_ += ',';
        // P2 #62 known-limitation: EnumVariant 结构没有独立的 line 字段，
        // 无法精确匹配每个 variant 前后的注释。当前仅在 enum 尾部
        // （closingBraceLine 之前）统一输出残余注释。
        // 待 AST 为 EnumVariant 添加 line 字段后可实现逐 variant 注释注入。
        out_ += '\n';
    }
    // closingBraceLine 之前的注释（对齐 formatClassDecl）
    if (node.closingBraceLine > 0) {
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line <= node.closingBraceLine) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

// R99 枚举与 ADT：格式化 enum variant 构造表达式
// 输出 "EnumName.VariantName" 或 "EnumName.VariantName(arg1, arg2)"
void Formatter::formatEnumVariantExpr(EnumVariantExpr& node) {
    out_ += node.enumName;
    out_ += '.';
    out_ += node.variantName;
    if (!node.arguments.empty()) {
        out_ += '(';
        for (size_t i = 0; i < node.arguments.size(); ++i) {
            if (i > 0)
                out_ += comma();
            formatNode(node.arguments[i].get());
        }
        out_ += ')';
    }
}

// R99 枚举与 ADT：格式化 match 表达式
// 输出：
//   match scrutinee {
//       case Pattern1 => body1;
//       case Pattern2 => body2;
//       default => bodyDefault;
//   }
// pattern 格式：
//   - WILDCARD: `_`
//   - LITERAL: 字面量文本
//   - VARIANT: `EnumName.VariantName` 或 `EnumName.VariantName(b1, b2)`
// body 若为 Block 则按块格式化（自终止），否则补 `;`
void Formatter::formatMatchExpr(MatchExpr& node) {
    out_ += "match ";
    formatNode(node.scrutinee.get());
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    for (auto& mc : node.cases) {
        out_ += indent();
        if (mc.isDefault || !mc.pattern) {
            out_ += "default";
        } else {
            // R134: 递归格式化 pattern（支持 6 种：WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）
            out_ += "case ";
            formatMatchPattern(*mc.pattern);
            // R134: guard 表达式 "if <cond>"
            if (mc.guard) {
                out_ += " if ";
                formatNode(mc.guard.get());
            }
        }
        out_ += " => ";
        if (mc.body) {
            if (mc.body->nodeType == NodeType::NODE_BLOCK) {
                // Block 自终止，格式化时已含 {}
                formatNode(mc.body.get());
            } else {
                formatNode(mc.body.get());
                out_ += ';';
            }
        } else {
            out_ += ';';
        }
        out_ += '\n';
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

/// R134: 递归格式化 match pattern（6 种：WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）
/// 保证 round-trip 等价性——formatter 输出能被 Parser 重新解析为等价 AST。
/// OPT-12: 文本追加到 out_，不再返回临时串。
void Formatter::formatMatchPattern(const MatchPattern& p) {
    switch (p.kind) {
    case MatchPatternKind::WILDCARD:
        out_ += '_';
        return;
    case MatchPatternKind::LITERAL:
        formatNode(p.literal.get());
        return;
    case MatchPatternKind::VARIABLE:
        // R134: 绑定变量名（不带 case 前缀，由调用方添加）
        out_ += p.variableName;
        return;
    case MatchPatternKind::VARIANT: {
        // EnumName.VariantName(sub1, sub2, ...) — 递归格式化子 pattern
        out_ += p.enumName;
        out_ += '.';
        out_ += p.variantName;
        if (!p.subPatterns.empty()) {
            out_ += '(';
            for (size_t i = 0; i < p.subPatterns.size(); ++i) {
                if (i > 0)
                    out_ += comma();
                formatMatchPattern(*p.subPatterns[i]);
            }
            out_ += ')';
        }
        return;
    }
    case MatchPatternKind::TUPLE: {
        // (sub1, sub2, ...) — 递归格式化子 pattern
        out_ += '(';
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            if (i > 0)
                out_ += comma();
            formatMatchPattern(*p.subPatterns[i]);
        }
        out_ += ')';
        return;
    }
    case MatchPatternKind::OR: {
        // sub1 or sub2 or sub3 — 递归格式化子 pattern，用 " or " 连接
        // 注：MiniLang 用 'or' 关键字（与 and/or 短路语义一致），非 '|'
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            if (i > 0)
                out_ += " or ";
            formatMatchPattern(*p.subPatterns[i]);
        }
        return;
    }
    }
    out_ += '_'; // 不应到达
}

/// 格式化下标访问：obj[index]；当 obj 为低优先级的二元/一元表达式时加括号避免歧义。
void Formatter::formatIndexAccess(IndexAccess& node) {
    const bool paren = node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                                       node.object->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '('; // FMT-01 fix: 低优先级表达式需要括号
    formatNode(node.object.get());
    if (paren)
        out_ += ')';
    out_ += '[';
    formatNode(node.index.get());
    out_ += ']';
}

/// 格式化下标赋值：obj[index] = value；obj 为低优先级表达式时加括号。
void Formatter::formatIndexAssign(IndexAssign& node) {
    const bool paren = node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                                       node.object->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '('; // FMT-01 fix
    formatNode(node.object.get());
    if (paren)
        out_ += ')';
    out_ += '[';
    formatNode(node.index.get());
    out_ += ']';
    out_ += binOp("=");
    formatNode(node.value.get());
}

/// 格式化类声明：输出 "class Name extends Super { members }"，相邻方法成员间按
/// blankLineBetweenFunctions 选项补空行，成员自终止时无需额外 ';'。
void Formatter::formatClassDecl(ClassDecl& node) {
    out_ += "class ";
    out_ += node.name;
    // R163 泛型扩展：输出类型参数列表 <T, K, V, ...>（与 formatEnumDecl 一致）
    if (!node.typeParams.empty()) {
        out_ += '<';
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                out_ += comma();
            out_ += node.typeParams[i];
        }
        out_ += '>';
    }
    if (!node.superClassName.empty()) {
        out_ += " extends ";
        out_ += node.superClassName;
    }
    // 七特性 MVP 阶段 3：输出 with 混入子句（extends 之后）
    if (!node.traits.empty()) {
        out_ += " with ";
        for (size_t i = 0; i < node.traits.size(); ++i) {
            if (i > 0)
                out_ += comma();
            out_ += node.traits[i];
        }
    }
    out_ += openBrace();
    out_ += '\n';
    currentIndent_++;
    // 七特性 MVP 阶段 3：仅打印类自身声明的成员（前 ownMemberCount 个）——
    // trait 方法在 parse 期 append 到尾部，打印它们会导致往返重复合入。
    // ownMemberCount < 0 表示未设置（兼容无 trait 路径），打印全部。
    size_t printCount = node.ownMemberCount >= 0 ? static_cast<size_t>(node.ownMemberCount) : node.members.size();
    for (size_t i = 0; i < printCount && i < node.members.size(); ++i) {
        auto& member = node.members[i];
        // F-P2-8 fix: 跳过空成员指针，避免 formatNode 返回 "null" 作为类成员
        if (!member)
            continue;
        // AUDIT-P2.8 fix: 输出当前成员之前的所有独立注释（行号严格小于成员行号），
        // 复用 formatBlock 的注释注入模式。
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line < member->line) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }
        // BUG-F-05 fix: blankLineBetweenFunctions 选项传播到类成员，
        // 当前后两个成员都是方法（FunDecl）时插入空行
        if (options_.blankLineBetweenFunctions && i > 0) {
            auto& prev = node.members[i - 1];
            if (prev && prev->nodeType == NodeType::NODE_FUN_DECL && member->nodeType == NodeType::NODE_FUN_DECL) {
                out_ += '\n';
            }
        }
        // 方法（FunDecl）以 } 结尾，不需要额外 ;
        out_ += indent();
        if (isSelfTerminating(member.get())) {
            formatNode(member.get());
        } else {
            formatNode(member.get());
            out_ += ';';
        }
        // AUDIT-P2.8 fix: 同行行内注释追加到成员末尾
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line == member->line) {
            out_ += ' ';
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            commentIndex_++;
        }
        out_ += '\n';
    }
    // AUDIT-P2.8 fix: 类体末尾输出 closingBraceLine 及之前的注释（对齐 formatBlock）
    if (node.closingBraceLine > 0) {
        while (commentIndex_ < comments_.size() && comments_[commentIndex_].line <= node.closingBraceLine) {
            out_ += indent();
            out_ += reindentBlockComment(comments_[commentIndex_].lexeme, indent());
            out_ += '\n';
            commentIndex_++;
        }
    }
    currentIndent_--;
    out_ += indent();
    out_ += '}';
}

/// 格式化成员访问：obj.field；obj 为低优先级表达式时加括号，避免被错误解析为方法调用。
void Formatter::formatMemberAccess(MemberAccess& node) {
    const bool paren = node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                                       node.object->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '('; // FMT-01 fix
    formatNode(node.object.get());
    if (paren)
        out_ += ')';
    out_ += '.';
    out_ += node.fieldName;
}

/// 格式化成员赋值：obj.field = value；obj 为低优先级表达式时加括号。
void Formatter::formatMemberAssign(MemberAssign& node) {
    const bool paren = node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                                       node.object->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '('; // FMT-01 fix
    formatNode(node.object.get());
    if (paren)
        out_ += ')';
    out_ += '.';
    out_ += node.fieldName;
    out_ += binOp("=");
    formatNode(node.value.get());
}

/// 格式化方法调用：obj.method(args...)；obj 为低优先级表达式时加括号，
/// 否则形如 (a+b).foo() 会被错误重新解析为独立 FunCall。
void Formatter::formatMethodCall(MethodCall& node) {
    const bool paren = node.object && (node.object->nodeType == NodeType::NODE_BINARY_OP ||
                                       node.object->nodeType == NodeType::NODE_UNARY_OP);
    if (paren)
        out_ += '('; // FMT-01 fix
    formatNode(node.object.get());
    if (paren)
        out_ += ')';
    out_ += '.';
    out_ += node.methodName;
    out_ += '(';
    for (size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0)
            out_ += comma();
        formatNode(node.arguments[i].get());
    }
    out_ += ')';
}

/// 格式化 null 字面量：裸输出 "null" 关键字。
void Formatter::formatNullLiteral(NullLiteral& /*node*/) {
    out_ += "null";
}

// C5 fix: 插值字符串格式化 — 重建 `"text {expr} more {expr2}"` 语法
// literals.size() == expressions.size() + 1
// 输出: "literals[0]{expressions[0]}literals[1]{expressions[1]}...literals[n]"
void Formatter::formatInterpolatedString(InterpolatedString& node) {
    out_ += '"'; // 开头引号

    // AUDIT-P2 fix: 复用 escapeStringContent 公共函数，消除转义规则重复

    // 首个字面量片段
    if (!node.literals.empty()) {
        out_ += escapeStringContent(node.literals[0]);
    }

    // 交替输出: {expr} literal
    for (size_t i = 0; i < node.expressions.size(); ++i) {
        out_ += '{';
        // BUG-FMT-P2-4 fix: 防御性检查 expressions[i] 是否为 nullptr。
        // 原实现直接解引用 node.expressions[i]->line，外部构造的 AST 可能含 nullptr。
        if (node.expressions[i]) {
            formatNode(node.expressions[i].get());
            // BUG-F-06 fix / BUG-FMT-P1-1 fix: formatNode 不消费 comments_ 数组，
            // 插值表达式行号范围内的注释会落入外层语句的"同行尾部注释"分支被误用。
            // 原实现用 `<= exprLine` 会错误跳过与表达式同行的外层语句尾部注释
            // （如 `var s = "hello ${x}"; // comment` 中的 `// comment`）。
            // 改为 `< exprLine`：仅跳过严格在表达式之前的注释，保留同行注释给外层。
            int exprLine = node.expressions[i]->line;
            while (commentIndex_ < comments_.size() && comments_[commentIndex_].line < exprLine) {
                ++commentIndex_;
            }
        }
        out_ += '}';
        if (i + 1 < node.literals.size()) {
            out_ += escapeStringContent(node.literals[i + 1]);
        }
    }

    out_ += '"'; // 结尾引号
    // AUDIT-P2.9 fix: 消费最后一个表达式与闭合引号之间的独立注释（多行插值字符串场景）。
    //   插值字符串是单行构造，注释不能注入字符串内部（会破坏语法）。此处仅推进
    //   commentIndex_ 游标，跳过行号严格小于 endLine 的注释，避免多行插值字符串
    //   块内的注释泄漏到外层语句的同行尾部注释分支。单行插值字符串（line==endLine）
    //   此处为 no-op，不影响同行尾部注释归属。
    while (commentIndex_ < comments_.size() && comments_[commentIndex_].line < node.endLine) {
        ++commentIndex_;
    }
    // AUDIT-P2.4 fix: 记录结束行号，供外层 formatBlock 的同行注释循环使用。
    //   多行插值字符串（endLine > line）的闭合引号后注释（如 `"...${x\n+y}"; // cmt`）
    //   行号 == endLine > stmt->line，原 formatBlock 用 == stmt->line 不匹配导致注释
    //   被下一条语句的独立注释分支消费（位置错误）。设置 lastNodeEndLine_ = endLine
    //   后，formatBlock 用 <= lastNodeEndLine_ 正确消费为同行尾注。
    lastNodeEndLine_ = node.endLine;
}
