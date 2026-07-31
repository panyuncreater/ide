#include "parser/Parser.h"
#include "ast/MacroExpander.h" // 七特性 MVP 阶段 2：宏展开器
#include "common/ErrorMessages.h" // P2-12: DiagCodes 常量
#include "common/Logger.h"
#include "interpreter/StringIntern.h"
#include <unordered_set>

// ============================================================
// Parser 递归下降语法分析器实现
// ============================================================

// BUG-P1 fix: 递归收集表达式中的所有变量引用名
// 用于校验默认参数值不引用后续参数
static void collectVarRefs(ASTNode* node, std::unordered_set<std::string>& names) {
    if (!node)
        return;
    switch (node->nodeType) {
    case NodeType::NODE_VAR_REF:
        names.insert(static_cast<VarRef*>(node)->name);
        break;
    case NodeType::NODE_BINARY_OP: {
        auto* bin = static_cast<BinaryOp*>(node);
        collectVarRefs(bin->left.get(), names);
        collectVarRefs(bin->right.get(), names);
        break;
    }
    case NodeType::NODE_UNARY_OP:
        collectVarRefs(static_cast<UnaryOp*>(node)->operand.get(), names);
        break;
    case NodeType::NODE_FUN_CALL: {
        auto* call = static_cast<FunCall*>(node);
        for (auto& arg : call->arguments) {
            collectVarRefs(arg.get(), names);
        }
        break;
    }
    case NodeType::NODE_INDEX_ACCESS: {
        auto* idx = static_cast<IndexAccess*>(node);
        collectVarRefs(idx->object.get(), names);
        collectVarRefs(idx->index.get(), names);
        break;
    }
    case NodeType::NODE_MEMBER_ACCESS:
        collectVarRefs(static_cast<MemberAccess*>(node)->object.get(), names);
        break;
    case NodeType::NODE_METHOD_CALL: {
        auto* mc = static_cast<MethodCall*>(node);
        collectVarRefs(mc->object.get(), names);
        for (auto& arg : mc->arguments) {
            collectVarRefs(arg.get(), names);
        }
        break;
    }
    case NodeType::NODE_ARRAY_LITERAL: {
        auto* arr = static_cast<ArrayLiteral*>(node);
        for (auto& elem : arr->elements) {
            collectVarRefs(elem.get(), names);
        }
        break;
    }
    case NodeType::NODE_DICT_LITERAL: {
        auto* dict = static_cast<DictLiteral*>(node);
        for (auto& pair : dict->pairs) {
            collectVarRefs(pair.first.get(), names);
            collectVarRefs(pair.second.get(), names);
        }
        break;
    }
    // BUG-LPA-03 fix: 覆盖 NODE_INTERPOLATED_STRING。
    //   原实现落入 default: break，导致 fun f(a = "{b}", b = 1) 中的 b 引用
    //   不被检测，默认参数前向引用检查可被绕过。
    case NodeType::NODE_INTERPOLATED_STRING: {
        auto* interp = static_cast<InterpolatedString*>(node);
        for (auto& expr : interp->expressions) {
            collectVarRefs(expr.get(), names);
        }
        break;
    }
    // BUG-011 fix: 显式列举所有"叶节点"类型（无 VarRef 子树），落入 default 时
    // 发出 LOG_WARNING 以便未来新增表达式类型时及时察觉遗漏。
    // 当前 NodeType 全集：
    //   表达式（已覆盖）：BINARY_OP / UNARY_OP / FUN_CALL / INDEX_ACCESS /
    //     MEMBER_ACCESS / METHOD_CALL / ARRAY_LITERAL / DICT_LITERAL /
    //     INTERPOLATED_STRING / VAR_REF
    //   叶节点表达式（无子节点，无需递归）：NUMBER_LITERAL / STRING_LITERAL /
    //     BOOL_LITERAL / NULL_LITERAL / SUPER_EXPR
    //   语句节点（不应出现在默认参数表达式中，但若被错误构造也应记录）：
    //     VAR_DECL / ASSIGNMENT / IF_STMT / WHILE_STMT / FOR_STMT / FUN_DECL /
    //     RETURN_STMT / PRINT_STMT / BLOCK / INDEX_ASSIGN / CLASS_DECL /
    //     MEMBER_ASSIGN / BREAK_STMT / CONTINUE_STMT / TRY_STMT / THROW_STMT /
    //     IMPORT_STMT / EXPORT_STMT
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
        // 叶节点：无 VarRef 子树，无需递归
        break;
    // R164 协程/生成器：yield 表达式不应出现在默认参数中（Parser 会在 primary()
    // 报错"yield 只能出现在 fun* 体内"），但此处防御性覆盖避免 LOG_WARNING。
    case NodeType::NODE_YIELD_EXPR:
        if (auto* y = static_cast<YieldExpr*>(node); y->value)
            collectVarRefs(y->value.get(), names);
        break;
    // 七特性 MVP 阶段 2：宏调用——递归展开后的语义子树（实参子树已共享在内）。
    // 宏声明不应出现在默认参数表达式中，防御性覆盖避免 LOG_WARNING。
    case NodeType::NODE_MACRO_CALL:
        collectVarRefs(static_cast<MacroCallExpr*>(node)->expanded.get(), names);
        break;
    case NodeType::NODE_MACRO_DECL:
        break;
    // 七特性 MVP 阶段 3：trait 声明不应出现在默认参数表达式中，防御性覆盖。
    case NodeType::NODE_TRAIT_DECL:
        break;
    // 七特性 MVP 阶段 4：await 表达式——递归 operand（防御性覆盖）。
    case NodeType::NODE_AWAIT_EXPR:
        collectVarRefs(static_cast<AwaitExpr*>(node)->operand.get(), names);
        break;
    default:
        // BUG-011 fix: 未覆盖的节点类型——可能是未来新增的表达式类型。
        // 发出 LOG_WARNING 以便开发者及时补充覆盖，避免默认参数前向引用检查
        // 被静默绕过。注意：语句节点理论上不应出现在默认参数表达式中，
        // 若触发此警告说明 Parser 构造了非预期的 AST 结构。
        LOG_WARNING("collectVarRefs: 未覆盖的节点类型 " + std::to_string(static_cast<int>(node->nodeType)) +
                        "，默认参数前向引用检查可能不完整",
                    "Parser");
        break;
    }
}

Parser::Parser() {}

std::unique_ptr<Block> Parser::parse(const std::vector<Token>& tokens) {
    tokens_ = &tokens; // 存储指针，避免深拷贝整个 token 流
    current_ = 0;
    parseDepth_ = 0; // P15 fix: 重置递归深度
    blockDepth_ = 0; // P0-1 fix: 重置块嵌套深度
    diagnostics_.clear();
    knownEnums_.clear(); // R99: 重置已知 enum 名集合，防止 Parser 复用导致状态泄漏

    // PERF-21 fix: 预分配 statements 向量容量，避免每个 declaration push_back 触发 realloc。
    // 估算：平均每 4 个 token 产生 1 个声明（关键字 + 名字 + ... + 分号）
    std::vector<std::shared_ptr<ASTNode>> statements;
    statements.reserve(tokens_->size() / 4);

    while (!isAtEnd()) {
        // BUG-004 fix: 顶层出现 '}' 是非法 token（无 enclosing block）。
        // 原实现 break 会丢弃后续有效声明。改为：消耗 stray '}' + 诊断，
        // 然后继续解析后续声明。注意 synchronize() 仍在 '}' 处返回（不消费），
        // 但本循环已先消耗 '}'，故不会无限循环。
        // 安全性：nested block() 中的 '}' 不会被消耗到此处——block() 内部循环
        // 同样 check(RBRACE) 退出并由 consume(TK_RBRACE) 消费闭合花括号，
        // 不会回退到顶层 parse()。
        if (check(TokenType::TK_RBRACE)) {
            const Token& rbrace = peek();
            diagnostics_.addError("多余的 '}' 在顶层（无匹配的 '{'）", rbrace.line, rbrace.column, DiagSource::Parser,
                                  DiagCodes::kStrayBrace);
            advance(); // 消耗 stray '}'，避免 declaration() → primary() 不识别 → 死循环
            continue;
        }
        // BUG-PARSER-AUDIT-5 fix: 错误数量上限，防止恶意输入触发 O(N) 诊断内存膨胀。
        // 原实现无上限，100 万个 ';' 可累积 ~100 万条 Diagnostic（~200MB）。
        // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
        const int dynMaxParseErrors = RuntimeLimits::RuntimeConfig::instance().maxParseErrors();
        if (diagnostics_.errorCount() >= dynMaxParseErrors) {
            diagnostics_.addError("错误过多（超过 " + std::to_string(dynMaxParseErrors) + " 条），停止解析",
                                  peek().line, peek().column, DiagSource::Parser, DiagCodes::kTooManyErrors);
            break;
        }
        try {
            auto decl = declaration();
            if (decl) {
                statements.push_back(std::move(decl));
            }
        } catch (const ParseError& e) {
            // 收集错误到诊断包而非吞掉
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser, e.code);
            // 错误恢复：同步到下一个声明边界
            synchronize();
        }
    }

    return std::make_unique<Block>(std::move(statements), 1, 1);
}

// ---- 辅助方法 ----

const Token& Parser::peek() const {
    // AUDIT-P2 fix: 与 previous() 的 P0-14 fix 边界检查保持对称。
    // 原实现直接索引 tokens_[current_]，依赖"tokens_ 末尾必有 TK_EOF"的隐式不变量。
    // 正常路径下 Lexer::scan 总是追加 TK_EOF，但外部构造的 tokens_（测试夹具/API）
    // 或内部 bug 可能导致 current_ 越界。添加边界检查返回 EOF 哨兵，防御性编程。
    if (current_ < 0 || current_ >= static_cast<int>(tokens_->size())) {
        static const Token eofSentinel(TokenType::TK_EOF, "", std::monostate{}, 0, 0);
        return eofSentinel;
    }
    return (*tokens_)[current_];
}

const Token& Parser::previous() const {
    // P0-14 fix: 边界检查，current_==0 时返回 EOF 哨兵避免负索引 UB
    if (current_ <= 0) {
        static const Token eofSentinel(TokenType::TK_EOF, "", std::monostate{}, 0, 0); // A1 fix: variant monostate
        return eofSentinel;
    }
    return (*tokens_)[current_ - 1];
}

bool Parser::isAtEnd() const {
    // 仅需检查 peek() 是否为 EOF：所有扫描都保证 token 流以 TK_EOF 收尾，
    // 因此到达流末尾等价于"看到了哨兵 EOF token"。
    return peek().type == TokenType::TK_EOF;
}

const Token& Parser::advance() {
    if (!isAtEnd())
        current_++;
    // 不在 advance() 中跳过注释——peek()/check() 已负责跳注释
    // advance() 后 previous() 必须返回实际被消耗的 token（修复 block() 等位置追踪）
    return previous();
}

bool Parser::check(TokenType type) const {
    if (isAtEnd())
        return false;
    return peek().type == type;
}

bool Parser::checkNext(TokenType type) const {
    // 前瞻一个 token（不消耗）：用于区分"标识符后是 '(' "（函数声明）
    // 还是独立标识符（变量名）等需要多 token 上下文的语法判断。
    // 越界（已是最后一个 token）时视为不匹配，避免访问越界。
    int idx = current_ + 1;
    if (idx >= (int)tokens_->size())
        return false;
    return (*tokens_)[idx].type == type;
}

const Token& Parser::consume(TokenType type, const std::string& message, const std::string& diagCode) {
    if (check(type))
        return advance();
    const Token& tok = peek();
    // BUG-PARSER-MSG-1 fix (P2): 错误消息拼接实际得到的 token，与 primary() 风格一致。
    // 原实现仅输出调用方传入的 message（如"期望 ';'"），用户不知道下一个 token 是什么，
    // 难以判断问题位置。改为"期望 ';' 但得到 'var'"，提升诊断价值。
    // P2 fix (错误码优先匹配): 调用方未显式提供 diagCode 时，按 token 类型派生默认
    // 稳定诊断码（分号→missing-semicolon / 圆括号→unbalanced-paren / 花括号→unbalanced-brace），
    // 覆盖全部 consume 调用点，无需逐处手填。显式传入的 diagCode 优先。
    std::string code = diagCode;
    if (code.empty()) {
        switch (type) {
        case TokenType::TK_SEMICOLON:
            code = "missing-semicolon";
            break;
        case TokenType::TK_LPAREN:
        case TokenType::TK_RPAREN:
            code = "unbalanced-paren";
            break;
        case TokenType::TK_LBRACE:
        case TokenType::TK_RBRACE:
            code = "unbalanced-brace";
            break;
        default:
            break;
        }
    }
    throw ParseError(message + " 但得到 '" + tok.lexeme + "'", tok.line, tok.column, code);
}

const Token& Parser::consumeIdentifierOrType(const std::string& message, const std::string& diagCode) {
    // 允许普通标识符
    if (check(TokenType::TK_IDENTIFIER))
        return advance();
    // 允许类型关键字作为名称（如 dict, array, int, float, string, bool）
    if (isTypeKeyword()) {
        return advance();
    }
    const Token& tok = peek();
    // BUG-PARSER-MSG-1 fix: 同 consume，拼接实际 token
    throw ParseError(message + " 但得到 '" + tok.lexeme + "'", tok.line, tok.column, diagCode);
}

bool Parser::isIdentifierOrType() const {
    if (isAtEnd())
        return false;
    TokenType t = peek().type;
    return t == TokenType::TK_IDENTIFIER || t == TokenType::TK_INT || t == TokenType::TK_FLOAT ||
           t == TokenType::TK_BOOL || t == TokenType::TK_STRING_TYPE || t == TokenType::TK_DICT ||
           t == TokenType::TK_ARRAY;
}

bool Parser::isTypeKeyword() const {
    if (isAtEnd())
        return false;
    TokenType t = peek().type;
    return t == TokenType::TK_INT || t == TokenType::TK_FLOAT || t == TokenType::TK_BOOL ||
           t == TokenType::TK_STRING_TYPE || t == TokenType::TK_DICT || t == TokenType::TK_ARRAY;
}

bool Parser::isClassTypeDeclStart() const {
    // 识别类类型声明起始：ClassName paramName 或 ClassName[] paramName
    // 原 checkNext(TK_IDENTIFIER) 只覆盖 ClassName paramName，漏掉 ClassName[] paramName
    if (!check(TokenType::TK_IDENTIFIER))
        return false;
    int idx = current_ + 1;
    int size = static_cast<int>(tokens_->size());
    if (idx >= size)
        return false;
    // ClassName paramName
    if ((*tokens_)[idx].type == TokenType::TK_IDENTIFIER)
        return true;
    // ClassName[][] paramName（ROUND44 fix: 支持多维数组类型注解。
    // 第四十三轮已修复 parseTypeAnnotation 的多维支持，但此门控函数仍用单次 []
    // 检查，导致 ClassName[][] paramName 在函数参数处不会被识别为类类型声明，
    // 落入"名字在前"分支引发解析错误。改为 while 循环消费连续 [] 后缀。）
    if ((*tokens_)[idx].type != TokenType::TK_LBRACKET)
        return false;
    while (idx < size && (*tokens_)[idx].type == TokenType::TK_LBRACKET) {
        idx++;
        if (idx >= size || (*tokens_)[idx].type != TokenType::TK_RBRACKET)
            return false;
        idx++;
    }
    return idx < size && (*tokens_)[idx].type == TokenType::TK_IDENTIFIER;
}

bool Parser::isFunTypeDeclStart() const {
    // ROUND56 fix: 识别函数类型参数注解起始 fun(params):ret paramName
    //   仅匹配 TK_FUN 后紧跟 TK_LPAREN 的模式（函数类型注解），避免误识别
    //   普通函数声明 fun name(...) — 后者 TK_FUN 后是 TK_IDENTIFIER。
    if (!check(TokenType::TK_FUN))
        return false;
    return checkNext(TokenType::TK_LPAREN);
}

std::string Parser::parseTypeAnnotation() {
    // Bug #10 fix: 递归深度保护（dict[K:V] / fun(params):ret 递归调用自身）
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("类型注解嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                         peek().column);
    }
    DepthGuard guard{parseDepth_};

    // AUDIT-P2.7 fix: 支持 T? 可选类型、dict[K:V] 泛型字典、fun(params):ret 函数类型。
    //   Interpreter.typeMatch 已支持这三种格式的运行时类型检查，此处仅需生成对应字符串。
    const Token& typeTok = advance(); // 消耗类型关键字或标识符
    std::string typeAnn = typeTok.lexeme;

    // AUDIT-P2.7 fix: dict[K:V] 泛型字典类型 — 当基础类型为 dict 且紧跟 '[' 时，
    //   解析键类型 ':' 值类型 ']'，生成 "dict[keyType:valType]" 格式字符串。
    //   递归调用 parseTypeAnnotation 解析键/值类型，天然支持嵌套（如 dict[string:int?]）。
    //   消歧：仅当 '[' 后不是 ']' 时才进入泛型分支；dict[] 仍按数组后缀处理。
    if (typeTok.type == TokenType::TK_DICT && check(TokenType::TK_LBRACKET) && !checkNext(TokenType::TK_RBRACKET)) {
        advance(); // 消耗 '['
        std::string keyType = parseTypeAnnotation();
        consume(TokenType::TK_COLON, "期望 ':' 分隔字典的键类型与值类型");
        std::string valType = parseTypeAnnotation();
        consume(TokenType::TK_RBRACKET, "期望 ']' 结束字典类型注解");
        typeAnn = "dict[" + keyType + ":" + valType + "]";
    }
    // AUDIT-P2.7 fix: fun(params):ret 函数类型 — 当基础类型为 fun 且紧跟 '(' 时，
    //   解析参数类型列表 ')' ':' 返回类型，生成 "fun(paramTypes):retType" 格式字符串。
    //   递归调用 parseTypeAnnotation 解析每个参数类型和返回类型。
    else if (typeTok.type == TokenType::TK_FUN && check(TokenType::TK_LPAREN)) {
        advance(); // 消耗 '('
        std::string paramList;
        if (!check(TokenType::TK_RPAREN)) {
            do {
                std::string pType = parseTypeAnnotation();
                if (!paramList.empty())
                    paramList += ",";
                paramList += pType;
            } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
        }
        consume(TokenType::TK_RPAREN, "期望 ')' 结束函数类型参数列表");
        consume(TokenType::TK_COLON, "期望 ':' 分隔函数参数列表与返回类型");
        std::string retType = parseTypeAnnotation();
        typeAnn = "fun(" + paramList + "):" + retType;
    }

    // AUDIT-P2-CORRECT fix: 支持多维数组类型注解（int[][], int[][][]）。
    // AUDIT-P2.7 fix: 支持 [] 和 ? 后缀的任意顺序堆叠（如 int[]?, int?[], dict[string:int][]?）。
    //   循环消费连续的 [] 或 ? 后缀，直到两者都不再出现。
    //   [] 后缀：仅在 [ 后紧跟 ] 时才消费，否则回退 [（保留原安全回溯语义）。
    //   ? 后缀：Lexer 将 '?' 映射到已废弃的 TK_FUNC 槽位（见 Lexer.cpp scanToken）。
    while (true) {
        if (check(TokenType::TK_LBRACKET)) {
            int bracketSave = current_;
            advance(); // 消耗 '['
            if (check(TokenType::TK_RBRACKET)) {
                advance(); // 消耗 ']'
                typeAnn += "[]";
            } else {
                current_ = bracketSave; // 不是 [] 类型注解，回退 '['
                break;
            }
        } else if (check(TokenType::TK_FUNC)) {
            // AUDIT-P2.7 fix: T? 可选类型后缀。Interpreter.typeMatch 通过
            //   annotation.back()=='?' 识别可选类型，剥离后递归匹配。
            advance(); // 消耗 '?'
            typeAnn += "?";
        } else {
            break;
        }
    }
    return typeAnn;
}

// L20: 解析解构绑定中变量名后的可选 `: Type` 注解
std::string Parser::parseOptionalNameTypeAnnotation(int line, int col) {
    if (!match(TokenType::TK_COLON)) {
        return ""; // 无 `:`，返回空串表示该位置无注解
    }
    // 与 VarDecl `: Type` 路径对齐：允许 isIdentifierOrType 或 TK_ENUM（R99 泛型 enum 注解）
    if (!isIdentifierOrType() && !check(TokenType::TK_ENUM)) {
        const Token& tok = peek();
        throw ParseError("期望类型名", tok.line, tok.column);
    }
    (void)line;
    (void)col;
    return parseTypeAnnotation();
}

// ---- 声明与语句 ----

std::unique_ptr<ASTNode> Parser::declaration() {
    // var 声明
    if (check(TokenType::TK_VAR))
        return varDecl();

    // fun / function / func 声明（Lexer 已统一为 TK_FUN）
    // R98 W3: `fun name(...)` 是函数声明，`fun(...)` 是 lambda 表达式。
    // 通过 peekNext 区分：fun 后跟标识符 → funDecl；fun 后跟 `(` → lambda 表达式
    // （走 statement → expressionStatement → expression → primary → lambdaExpr）。
    // lambda 作为语句无意义（闭包值未被使用），但语法允许——用户可能写
    // `fun(x){...}(5);` 立即调用，此时走 expressionStatement 路径。
    if (check(TokenType::TK_FUN) && !checkNext(TokenType::TK_LPAREN))
        return funDecl();

    // L18 lang-constfun: const fun name(...) —— 编译期求值函数声明。
    // const 仅允许修饰具名 fun 声明（非 lambda/非其它语句）。
    if (check(TokenType::TK_CONST)) {
        const Token& constTok = peek();
        advance(); // 消耗 const
        if (check(TokenType::TK_FUN) && !checkNext(TokenType::TK_LPAREN)) {
            auto decl = funDecl();
            if (decl && decl->nodeType == NodeType::NODE_FUN_DECL) {
                static_cast<FunDecl*>(decl.get())->isConstFun = true;
            }
            return decl;
        }
        throw ParseError("const 仅可修饰具名函数声明（const fun name(...)）", constTok.line, constTok.column);
    }

    // class 声明
    if (check(TokenType::TK_CLASS))
        return classDecl();

    // R99 枚举与 ADT: enum 声明
    if (check(TokenType::TK_ENUM))
        return enumDecl();

    // 七特性 MVP 阶段 2：宏声明
    if (check(TokenType::TK_MACRO))
        return macroDecl();

    // 七特性 MVP 阶段 3：trait 声明
    if (check(TokenType::TK_TRAIT))
        return traitDecl();

    // 七特性 MVP 阶段 4：async fun 声明（async 仅可修饰具名 fun 声明）
    if (check(TokenType::TK_ASYNC)) {
        const Token& asyncTok = peek();
        advance(); // 消耗 async
        if (check(TokenType::TK_FUN) && !checkNext(TokenType::TK_LPAREN)) {
            return funDecl(/*isAsync=*/true);
        }
        throw ParseError("async 仅可修饰具名函数声明（async fun name(...)）", asyncTok.line, asyncTok.column);
    }

    // F12: import / export 声明
    if (check(TokenType::TK_IMPORT))
        return importStmt();
    if (check(TokenType::TK_EXPORT))
        return exportStmt();

    // 类型注解声明: int/float/bool/string/dict/array
    if (isTypeKeyword()) {
        // 可能是: 类型注解变量声明(int a=1;) 或 带返回类型的函数声明(int fib(n){})
        // 保存当前位置以便回溯
        int savePos = current_;
        std::string typeAnn = parseTypeAnnotation();

        if (isIdentifierOrType()) {
            // 检查是否是带返回类型的函数声明: int fib(
            if (checkNext(TokenType::TK_LPAREN)) {
                return typedFunDecl(typeAnn);
            }
            // 普通类型注解变量声明: int a = 10;
            if (check(TokenType::TK_IDENTIFIER)) {
                return typedVarDecl(typeAnn);
            }
            // 类型关键字作函数名但后面没有( — 无法构成有效声明，回溯
        }

        // 不是类型注解，回溯
        current_ = savePos;
    }

    // 类名类型注解声明: ClassName varName; 或 ClassName funcName() { ... }
    // 识别模式: 标识符 标识符 (类型名 变量名/函数名)
    if (check(TokenType::TK_IDENTIFIER)) {
        // 预读两个 token: 第一个是类名，第二个是变量名/函数名
        int savePos = current_;
        std::string typeAnn = parseTypeAnnotation(); // 消耗类名 + 可选 []

        if (check(TokenType::TK_IDENTIFIER)) {
            // M7 fix: 前瞻验证 — 检查第二个标识符后是否为合法声明后续 token
            int afterSecondPos = current_ + 1; // 跳过第二个标识符
            // 跳过可能的 [] 数组类型后缀
            if (afterSecondPos < static_cast<int>(tokens_->size()) &&
                (*tokens_)[afterSecondPos].type == TokenType::TK_LBRACKET &&
                afterSecondPos + 1 < static_cast<int>(tokens_->size()) &&
                (*tokens_)[afterSecondPos + 1].type == TokenType::TK_RBRACKET) {
                afterSecondPos += 2;
            }
            bool validDeclFollow = false;
            if (afterSecondPos < static_cast<int>(tokens_->size())) {
                TokenType follow = (*tokens_)[afterSecondPos].type;
                // 合法后续: '=' 初始化, ';' 结束, '(' 函数参数, '{' 函数体
                validDeclFollow = (follow == TokenType::TK_ASSIGN || follow == TokenType::TK_SEMICOLON ||
                                   follow == TokenType::TK_LPAREN || follow == TokenType::TK_LBRACE);
            }

            if (validDeclFollow) {
                // 检查是否是带类类型的函数声明: ClassName funcName(
                if (checkNext(TokenType::TK_LPAREN)) {
                    return typedFunDecl(typeAnn);
                }
                // ClassName varName — 类类型注解变量声明
                return typedVarDecl(typeAnn);
            }
        }

        // 不是类类型声明，回溯
        current_ = savePos;
    }

    return statement();
}

std::unique_ptr<ASTNode> Parser::varDecl() {
    const Token& varTok = consume(TokenType::TK_VAR, "期望 'var'");

    // R98 元组与解构：var (a, b, c) = expr 解构绑定
    // 检测 var 后紧跟 '(' 的模式——避免与 var (expr) 的合法语法歧义，
    // 解构绑定要求 '(' 后必须紧跟标识符（不能是表达式），且至少一个 ',' 分隔。
    // L20: 支持每个变量名后可选 `: Type` 注解，前瞻需跳过 ": <Type>" 序列。
    if (check(TokenType::TK_LPAREN)) {
        // 前瞻：'(' IDENTIFIER (':' <TypeTokens>)? (',' IDENTIFIER (':' <TypeTokens>)?)* ')' '='
        // 注：不强制要求逗号（var (a) = expr 也视为解构，与原实现一致）
        int lookahead = current_ + 1;
        bool isDestructure = false;
        if (lookahead < static_cast<int>(tokens_->size()) && (*tokens_)[lookahead].type == TokenType::TK_IDENTIFIER) {
            int scanPos = lookahead + 1;
            while (scanPos < static_cast<int>(tokens_->size())) {
                // L20: 跳过可选 ": <TypeTokens>"
                if ((*tokens_)[scanPos].type == TokenType::TK_COLON) {
                    ++scanPos; // 消耗 ':'
                    // 跳过类型 token 序列：标识符/类型关键字/enum/'['/']'/'?'/','/'('/')'/'{'/'}'（dict[K:V],
                    // fun():ret） 简化：跳过到下一个 ',' 或 ')'，不严格解析类型语法（实际解析在 parseTypeAnnotation）
                    while (scanPos < static_cast<int>(tokens_->size())) {
                        TokenType tt = (*tokens_)[scanPos].type;
                        if (tt == TokenType::TK_COMMA || tt == TokenType::TK_RPAREN)
                            break;
                        ++scanPos;
                    }
                    if (scanPos >= static_cast<int>(tokens_->size()))
                        break;
                }
                if ((*tokens_)[scanPos].type == TokenType::TK_COMMA) {
                    ++scanPos;
                    if (scanPos >= static_cast<int>(tokens_->size()) ||
                        (*tokens_)[scanPos].type != TokenType::TK_IDENTIFIER) {
                        break; // 不是合法解构模式
                    }
                    ++scanPos; // 消耗 IDENTIFIER
                    continue;
                }
                if ((*tokens_)[scanPos].type == TokenType::TK_RPAREN) {
                    ++scanPos;
                    // 注：原实现不强制要求逗号（var (a) = expr 也视为解构），此处保持兼容
                    if (scanPos < static_cast<int>(tokens_->size()) &&
                        (*tokens_)[scanPos].type == TokenType::TK_ASSIGN) {
                        isDestructure = true;
                    }
                    break;
                }
                break; // 不是合法解构模式
            }
        }

        if (isDestructure) {
            advance(); // 消耗 '('
            std::vector<std::string> names;
            std::vector<std::string> nameTypeAnns; // L20: per-name 类型注解
            bool anyNameTypeAnn = false;           // L20: 是否有任一位置带注解
            // L20: 解析第一个变量名 + 可选 `: Type`
            const Token& firstName = consume(TokenType::TK_IDENTIFIER, "期望变量名");
            names.push_back(firstName.lexeme);
            {
                std::string ann = parseOptionalNameTypeAnnotation(firstName.line, firstName.column);
                nameTypeAnns.push_back(ann);
                if (!ann.empty())
                    anyNameTypeAnn = true;
            }
            while (match(TokenType::TK_COMMA)) {
                const Token& nextName = consume(TokenType::TK_IDENTIFIER, "期望变量名");
                names.push_back(nextName.lexeme);
                std::string ann = parseOptionalNameTypeAnnotation(nextName.line, nextName.column);
                nameTypeAnns.push_back(ann);
                if (!ann.empty())
                    anyNameTypeAnn = true;
            }
            consume(TokenType::TK_RPAREN, "期望 ')' 结束解构绑定");
            consume(TokenType::TK_ASSIGN, "期望 '=' 初始化解构绑定");
            auto init = expression();
            consume(TokenType::TK_SEMICOLON, "期望 ';' 结束解构绑定");
            // L20: 若任一位置带类型注解，则 nameTypeAnnotations 全量保留（空串表示该位置无注解）；
            //      若全部无注解，保留空 vector 以维持向后兼容（hasNameTypeAnnotations() == false）
            auto node =
                std::make_unique<DestructureBinding>(std::move(names), std::move(init), varTok.line, varTok.column);
            if (anyNameTypeAnn) {
                node->nameTypeAnnotations = std::move(nameTypeAnns);
            }
            return node;
        }
    }

    const Token& name = consume(TokenType::TK_IDENTIFIER, "期望变量名");

    // R99 fix: 支持 var name: Type = expr; 语法（var-first 带类型注解）
    // 原 varDecl 只支持 var name = expr;（无注解），typedVarDecl 支持 Type name = expr;（类型在前）。
    // 但 `var c: Color = Color.Red;` 和 `var c: enum = Color.Red;` 落入 varDecl 路径，
    // 因 `:` 后非 `=`/`;` 触发 ParseError，导致变量未定义。
    std::string typeAnn;
    if (match(TokenType::TK_COLON)) {
        // 允许标识符（类名/enum 名）、类型关键字、TK_ENUM（R99 泛型 enum 注解）
        if (!isIdentifierOrType() && !check(TokenType::TK_ENUM)) {
            const Token& tok = peek();
            throw ParseError("期望类型名", tok.line, tok.column);
        }
        typeAnn = parseTypeAnnotation();
    }

    std::unique_ptr<ASTNode> init = nullptr;
    if (match(TokenType::TK_ASSIGN)) {
        init = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束变量声明");

    return std::make_unique<VarDecl>(StringIntern::intern(name.lexeme), typeAnn, std::move(init), varTok.line,
                                     varTok.column);
}

std::unique_ptr<VarDecl> Parser::typedVarDecl(const std::string& typeAnn) {
    const Token& name = consume(TokenType::TK_IDENTIFIER, "期望变量名");

    std::unique_ptr<ASTNode> init = nullptr;
    if (match(TokenType::TK_ASSIGN)) {
        init = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束变量声明");

    return std::make_unique<VarDecl>(StringIntern::intern(name.lexeme), typeAnn, std::move(init), name.line,
                                     name.column);
}

std::unique_ptr<FunDecl> Parser::funDecl(bool isAsync) {
    // 消耗 fun 或 function 关键字
    const Token& funTok = advance();

    // R164 协程/生成器：检测 fun* 标记生成器函数（类似 JS function* / Python def+yield）
    // 七特性 MVP 阶段 4：async fun 复用生成器机制（调用返回 Task/协程）
    bool isGenerator = isAsync;
    if (match(TokenType::TK_STAR)) {
        isGenerator = true;
    }

    // 函数名：允许标识符或类型关键字（如 dict, array, int, float, string, bool）
    const Token& name = consumeIdentifierOrType("期望函数名");

    // R163 泛型函数：可选类型参数列表 <T, E, ...>
    // 与 enumDecl 的泛型解析模式一致：函数名后、'(' 前解析 <T, E>
    std::vector<std::string> typeParams;
    if (match(TokenType::TK_LT)) {
        do {
            if (!isIdentifierOrType()) {
                const Token& tok = peek();
                throw ParseError("期望类型参数名", tok.line, tok.column);
            }
            const Token& tp = advance();
            typeParams.push_back(tp.lexeme);
        } while (match(TokenType::TK_COMMA));
        consume(TokenType::TK_GT, "期望 '>' 结束类型参数列表");
    }

    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    std::vector<std::shared_ptr<ASTNode>> defaultValues; // F10
    parseParamList(params, paramTypes, defaultValues);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 可选的返回值类型注解 : type 或 -> type
    std::string returnType;
    if (match(TokenType::TK_COLON)) {
        // BUG-PARSER-AUDIT-1 fix: 在 parseTypeAnnotation 前预检 token 类型，
        // 防止 `fun foo(): ;` 或 `fun foo(): {}` 中的 `;`/`{` 被吞作类型名，
        // 导致误导性错误消息与块结构污染。与 parseParamList 第 479-484 行模式对齐。
        if (!isIdentifierOrType()) {
            const Token& tok = peek();
            throw ParseError("期望返回类型名", tok.line, tok.column);
        }
        returnType = parseTypeAnnotation();
    } else if (check(TokenType::TK_MINUS) && checkNext(TokenType::TK_GT)) {
        advance();                   // 消耗 '-'
        advance();                   // 消耗 '>'
        if (!isIdentifierOrType()) { // BUG-PARSER-AUDIT-1
            const Token& tok = peek();
            throw ParseError("期望返回类型名", tok.line, tok.column);
        }
        returnType = parseTypeAnnotation();
    }

    // R164 协程/生成器：进入生成器函数体前重置 yieldId 计数器。
    // isGenerator=true 时重置为 0（开始计数），否则设为 -1（标记非生成器上下文，
    // primary() 遇到 yield 时报错）。YieldIdScope 保证异常路径下恢复外层计数器，
    // 支持嵌套生成器函数独立计数。
    YieldIdScope yieldScope{currentYieldId_};
    currentYieldId_ = isGenerator ? 0 : -1;
    // R164 协程/生成器：进入函数体前重置 yield-in-loop 追踪标志。
    // 嵌套函数（包括嵌套 fun*）有独立的追踪上下文：外层循环不影响内层函数的
    // yield-in-loop 判定。BoolScope 保证异常路径下恢复外层值。
    BoolScope loopCtxScope{yieldInLoop_};
    yieldInLoop_ = false;
    BoolScope hasLoopYieldScope{currentFunHasYieldInLoop_};
    currentFunHasYieldInLoop_ = false;
    // 七特性 MVP 阶段 4：async 上下文标记（await 仅在 async fun 体内合法）。
    // 嵌套函数独立判定：async fun 内的普通嵌套 fun 体内 await 非法。
    BoolScope asyncCtxScope{currentFunIsAsync_};
    currentFunIsAsync_ = isAsync;

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    auto decl = std::make_unique<FunDecl>(StringIntern::intern(name.lexeme), std::move(params), std::move(paramTypes),
                                          returnType, std::move(body), funTok.line, funTok.column);
    decl->typeParams = std::move(typeParams);
    // R164 协程/生成器：标记生成器函数（fun*），供 Interpreter/VM 在调用时
    // 返回 Coroutine 值而非直接执行函数体。
    decl->isGenerator = isGenerator;
    // 七特性 MVP 阶段 4：async fun 标记（Formatter 区分打印 async fun vs fun*）
    decl->isAsync = isAsync;
    // 记录生成器内 yield 总数（供 Interpreter 重放模式判断是否已耗尽）。
    if (isGenerator) {
        if (isAsync || currentFunHasYieldInLoop_) {
            // 循环内 yield：编译期节点数 ≠ 运行时执行数，用 INT_MAX 标记动态模式，
            // done 改由函数体自然结束路径判定（避免 currentYieldId >= yieldCount 误判）。
            // async fun 恒用动态模式：body 可能无 yield（首次 next() 直接跑完）、
            // 也可能含 await/yield 混合，静态计数不可靠。
            decl->yieldCount = FunDecl::kDynamicYieldCount;
        } else {
            decl->yieldCount = currentYieldId_;
        }
    }
    // F10: 计算必需参数个数（前缀无默认值的参数数量）
    int reqCount = 0;
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (defaultValues[i] == nullptr) {
            ++reqCount;
        } else {
            break; // 一旦遇到默认值，后续都有默认值
        }
    }
    decl->requiredParamCount = reqCount;
    decl->defaultValues = std::move(defaultValues);
    return decl;
}

std::unique_ptr<FunDecl> Parser::typedFunDecl(const std::string& returnType) {
    // 带返回类型的函数声明: int fib(int n) { ... }
    // 返回类型已由调用方提供，当前 token 是函数名
    const Token& name = consumeIdentifierOrType("期望函数名");
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    std::vector<std::shared_ptr<ASTNode>> defaultValues; // F10
    parseParamList(params, paramTypes, defaultValues);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 返回类型已由调用方提供，不再解析 :type 或 ->type

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    auto decl = std::make_unique<FunDecl>(StringIntern::intern(name.lexeme), std::move(params), std::move(paramTypes),
                                          returnType, std::move(body), name.line, name.column);
    // F10: 计算必需参数个数
    int reqCount = 0;
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (defaultValues[i] == nullptr) {
            ++reqCount;
        } else {
            break;
        }
    }
    decl->requiredParamCount = reqCount;
    decl->defaultValues = std::move(defaultValues);
    return decl;
}

// ============================================================
// R98 W3: Lambda 表达式解析
// ============================================================
// 语法：fun(params) { body }（匿名函数，作为表达式使用）
// 与 funDecl() 的差异：
//   1. 消耗 fun 关键字后直接期望 '('（不接受函数名）
//   2. 返回的 FunDecl 节点 name 为空字符串（Compiler/Interpreter 内部用合成名 `$lambda_N`）
//   3. 作为表达式求值，结果为闭包值（不注册到 funRegistry_）
// 复用：parseParamList（参数解析）、block（函数体解析）、类型注解解析
std::unique_ptr<FunDecl> Parser::lambdaExpr() {
    const Token& funTok = advance(); // 消耗 fun 关键字

    // Lambda 直接期望 '('（无函数名）
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    std::vector<std::shared_ptr<ASTNode>> defaultValues; // F10
    parseParamList(params, paramTypes, defaultValues);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 可选的返回值类型注解 : type 或 -> type（与 funDecl 一致）
    std::string returnType;
    if (match(TokenType::TK_COLON)) {
        if (!isIdentifierOrType()) {
            const Token& tok = peek();
            throw ParseError("期望返回类型名", tok.line, tok.column);
        }
        returnType = parseTypeAnnotation();
    } else if (check(TokenType::TK_MINUS) && checkNext(TokenType::TK_GT)) {
        advance(); // 消耗 '-'
        advance(); // 消耗 '>'
        if (!isIdentifierOrType()) {
            const Token& tok = peek();
            throw ParseError("期望返回类型名", tok.line, tok.column);
        }
        returnType = parseTypeAnnotation();
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    // 匿名 lambda：name 为空字符串
    auto decl = std::make_unique<FunDecl>("", std::move(params), std::move(paramTypes), returnType, std::move(body),
                                          funTok.line, funTok.column);
    // F10: 计算必需参数个数
    int reqCount = 0;
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (defaultValues[i] == nullptr) {
            ++reqCount;
        } else {
            break;
        }
    }
    decl->requiredParamCount = reqCount;
    decl->defaultValues = std::move(defaultValues);
    return decl;
}

void Parser::parseParamList(std::vector<std::string>& params, std::vector<std::string>& paramTypes,
                            std::vector<std::shared_ptr<ASTNode>>& defaultValues) {
    // BUG-DEF-1 fix: 默认参数值必须是字面量（数字/字符串/布尔/null/负数字面量）。
    // 三后端一致性：Interpreter 支持任意表达式默认值（b=a+1 在闭包环境求值），
    // 但 StackVM/RegisterVM 仅支持字面量（复杂表达式记录 0xFFFF 哨兵运行时报错）。
    // 在 Parser 层统一拒绝复杂表达式，确保三后端行为一致。
    // 支持的类型：NumberLiteral / StringLiteral / BoolLiteral / NullLiteral /
    //   UnaryOp(NEGATE) 嵌套包装 NumberLiteral（如 -42, --5）/
    //   InterpolatedString（字面量片段+表达式，表达式部分由 collectVarRefs 检查前向引用；
    //   VM 路径记录 0xFFFF 哨兵，运行时调用使用默认值会报错，但 Parser 层接受以保持
    //   BUG-LPA-03 回归测试语义——该测试验证 collectVarRefs 覆盖 InterpolatedString）。
    auto isLiteralDefaultExpr = [](const ASTNode* node) -> bool {
        if (!node)
            return false;
        switch (node->nodeType) {
        case NodeType::NODE_NUMBER_LITERAL:
        case NodeType::NODE_STRING_LITERAL:
        case NodeType::NODE_BOOL_LITERAL:
        case NodeType::NODE_NULL_LITERAL:
        case NodeType::NODE_INTERPOLATED_STRING:
            return true;
        case NodeType::NODE_UNARY_OP: {
            // 支持负数字面量: -42, -3.14, --5（双重否定）
            const auto* unary = static_cast<const UnaryOp*>(node);
            if (unary->opType != UnaryOp::UnaryOpType::UOP_NEGATE)
                return false;
            const ASTNode* cur = unary->operand.get();
            int negateCount = 1;
            while (cur && cur->nodeType == NodeType::NODE_UNARY_OP) {
                const auto* inner = static_cast<const UnaryOp*>(cur);
                if (inner->opType != UnaryOp::UnaryOpType::UOP_NEGATE)
                    return false;
                ++negateCount;
                cur = inner->operand.get();
            }
            return cur != nullptr && cur->nodeType == NodeType::NODE_NUMBER_LITERAL && negateCount > 0;
        }
        default:
            return false;
        }
    };
    if (check(TokenType::TK_RPAREN))
        return;
    bool seenDefault = false; // F10: 一旦出现默认参数，后续都必须有默认值
    // Perf-Finding4 + Bug-5: 用 unordered_set 替代每参数 O(n) 线性扫描去重，
    // 将 parseParamList 总复杂度从 O(n²) 降为 O(n)。初始化自 params 以兼容
    // 调用方预填充场景（虽然当前 3 个调用点均传入空 vector）。
    std::unordered_set<std::string> paramSet(params.begin(), params.end());
    // 预留常见容量，避免 1-2 次小幅 reallocation
    params.reserve(params.size() + 4);
    paramTypes.reserve(paramTypes.size() + 4);
    defaultValues.reserve(defaultValues.size() + 4);
    do {
        std::string pType;
        std::string paramName;

        // 支持 C 风格类型注解: int a, float b 等
        if (isTypeKeyword()) {
            pType = parseTypeAnnotation();

            const Token& param = consumeIdentifierOrType("期望参数名");
            paramName = param.lexeme;
        } else if (isClassTypeDeclStart()) {
            // PARSE-02 fix: C 风格类类型参数: ClassName paramName 或 ClassName[] paramName
            pType = parseTypeAnnotation();
            const Token& param = consumeIdentifierOrType("期望参数名");
            paramName = param.lexeme;
        } else if (isFunTypeDeclStart()) {
            // ROUND56 fix: 函数类型参数注解 fun(params):ret paramName
            //   samples/02-types-and-operators/types_and_operators.mini 使用 fun(int):string cb 作为
            //   高阶函数参数类型注解。原 parseParamList 仅识别 isTypeKeyword /
            //   isClassTypeDeclStart，遇到 TK_FUN 走入"参数名在前"分支抛
            //   "期望参数名 但得到 'fun'"。此处复用 parseTypeAnnotation 生成
            //   "fun(...):ret" 字符串，与 declaration() 的 dict/fun 分支语义一致。
            pType = parseTypeAnnotation();
            const Token& param = consumeIdentifierOrType("期望参数名");
            paramName = param.lexeme;
        } else {
            // 参数名在前面: a 或 a: int
            const Token& param = consume(TokenType::TK_IDENTIFIER, "期望参数名");
            paramName = param.lexeme;

            // 可选的参数类型注解 : type
            if (match(TokenType::TK_COLON)) {
                // 接受标识符或内置类型关键字（int, float, bool, string, dict, array）
                if (isIdentifierOrType()) {
                    pType = parseTypeAnnotation();
                } else {
                    const Token& tok = peek();
                    throw ParseError("期望参数类型名", tok.line, tok.column);
                }
            }
        }

        // M-新5 fix: 检测重复参数名（Perf-Finding4: O(1) hash 查找替代 O(n) 线性扫描）
        if (!paramSet.insert(paramName).second) {
            throw ParseError("重复的参数名 '" + paramName + "'", peek().line, peek().column);
        }
        params.push_back(StringIntern::intern(paramName));
        paramTypes.push_back(pType);

        // F10: 解析默认参数值 = expr
        if (match(TokenType::TK_ASSIGN)) {
            seenDefault = true;
            auto defaultExpr = expression();
            // BUG-DEF-1 fix: 三后端一致性——仅支持字面量默认值。
            // StackVM/RegisterVM 无法在函数入口求值复杂表达式（闭包环境访问、
            // 参数间引用等），Interpreter 虽支持但会造成三后端行为不一致。
            // 在 Parser 层统一拒绝，给出清晰的编译期错误而非 VM 运行时错误。
            if (!isLiteralDefaultExpr(defaultExpr.get())) {
                throw ParseError("默认参数值必须是字面量（数字/字符串/布尔/null/负数字面量）", defaultExpr->line,
                                 defaultExpr->column);
            }
            defaultValues.push_back(std::move(defaultExpr));
        } else {
            if (seenDefault) {
                throw ParseError("默认参数后的参数都必须有默认值: '" + paramName + "'", peek().line, peek().column);
            }
            defaultValues.push_back(nullptr);
        }
    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));

    // BUG-P1 fix: 默认参数值不能引用后续参数（如 fun f(a = b, b = 1) 应报错）
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (!defaultValues[i])
            continue;
        std::unordered_set<std::string> refNames;
        collectVarRefs(defaultValues[i].get(), refNames);
        for (const auto& ref : refNames) {
            for (size_t j = i + 1; j < params.size(); ++j) {
                if (params[j] == ref) {
                    throw ParseError("默认参数值不能引用后续参数: '" + ref + "'", defaultValues[i]->line,
                                     defaultValues[i]->column);
                }
            }
        }
    }
}

std::unique_ptr<ClassDecl> Parser::classDecl() {
    const Token& classTok = consume(TokenType::TK_CLASS, "期望 'class'");
    const Token& name = consumeIdentifierOrType("期望类名");

    // R163 泛型类：可选类型参数列表 <T, K, V, ...>
    // 与 enumDecl/funDecl 的泛型解析模式一致：类名后、extends/'{' 前解析 <T, K>
    std::vector<std::string> typeParams;
    if (match(TokenType::TK_LT)) {
        do {
            if (!isIdentifierOrType()) {
                const Token& tok = peek();
                throw ParseError("期望类型参数名", tok.line, tok.column);
            }
            const Token& tp = advance();
            typeParams.push_back(tp.lexeme);
        } while (match(TokenType::TK_COMMA));
        consume(TokenType::TK_GT, "期望 '>' 结束类型参数列表");
    }

    // 可选的 extends SuperClassName 或 : SuperClassName
    std::string superClassName = parseClassExtends();

    // 七特性 MVP 阶段 3：可选的 `with T1, T2` 混入子句（extends 之后、'{' 之前）
    std::vector<std::string> traits;
    if (match(TokenType::TK_WITH)) {
        do {
            const Token& traitName = consumeIdentifierOrType("期望 trait 名");
            // 重复混入检查（同一 trait 混入两次必然同名冲突，提前报更友好的错误）
            for (const auto& existing : traits) {
                if (existing == traitName.lexeme) {
                    throw ParseError("trait 重复混入: " + traitName.lexeme, traitName.line, traitName.column);
                }
            }
            traits.push_back(traitName.lexeme);
        } while (match(TokenType::TK_COMMA));
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");

    // 解析类成员
    std::vector<std::shared_ptr<ASTNode>> members;
    parseClassMembers(members);

    consume(TokenType::TK_RBRACE, "期望 '}'");

    // AUDIT-P2.8 fix: 记录闭合 '}' 所在行号，供 Formatter 注入类体末尾注释。
    int closingBraceLine = previous().line;

    // 七特性 MVP 阶段 3：记录自身成员数后合入 trait 方法（append 到 members 尾部）。
    // Formatter 仅打印前 ownMemberCount 个成员，保证往返等价。
    int ownMemberCount = static_cast<int>(members.size());
    if (!traits.empty()) {
        mergeTraitMethods(traits, members, classTok.line, classTok.column);
    }

    auto decl = std::make_unique<ClassDecl>(StringIntern::intern(name.lexeme), superClassName, std::move(members),
                                            classTok.line, classTok.column);
    decl->closingBraceLine = closingBraceLine;
    decl->typeParams = std::move(typeParams);
    decl->traits = std::move(traits);
    decl->ownMemberCount = ownMemberCount;
    return decl;
}

// ============================================================
// 七特性 MVP 阶段 3：Trait/Mixin（parse 期方法合入）
// ============================================================

std::unique_ptr<TraitDecl> Parser::traitDecl() {
    // trait T { fun m() {...} ... }（MVP 仅支持方法成员）
    const Token& traitTok = consume(TokenType::TK_TRAIT, "期望 'trait'");
    const Token& name = consumeIdentifierOrType("期望 trait 名");

    if (knownTraits_.count(name.lexeme)) {
        throw ParseError("trait 重复定义: " + name.lexeme, name.line, name.column);
    }

    consume(TokenType::TK_LBRACE, "期望 '{' 开始 trait 体");

    std::vector<std::shared_ptr<ASTNode>> methods;
    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        // 复用 parseClassMembers 的子集：仅允许 fun 方法（字段/其他成员报错，
        // MVP 限定 trait 为纯方法集，避免字段默认值/槽位分配的三后端合入复杂度）
        if (!check(TokenType::TK_FUN)) {
            const Token& tok = peek();
            throw ParseError("trait 体内仅支持方法声明（fun 开头）", tok.line, tok.column);
        }
        auto method = funDecl();
        // 同 trait 内方法重名检查
        for (const auto& existing : methods) {
            if (static_cast<FunDecl*>(existing.get())->name == method->name) {
                throw ParseError("trait " + name.lexeme + " 内方法重复定义: " + method->name, method->line,
                                 method->column);
            }
        }
        methods.push_back(std::shared_ptr<ASTNode>(std::move(method)));
    }

    consume(TokenType::TK_RBRACE, "期望 '}' 结束 trait 体");

    auto decl = std::make_unique<TraitDecl>(name.lexeme, std::move(methods), traitTok.line, traitTok.column);
    decl->closingBraceLine = previous().line;
    knownTraits_[decl->name] = decl.get();
    return decl;
}

void Parser::mergeTraitMethods(const std::vector<std::string>& traits,
                               std::vector<std::shared_ptr<ASTNode>>& members, int line, int col) {
    // 收集类自身方法名（自身方法优先于 trait 方法）
    std::unordered_set<std::string> ownMethodNames;
    for (const auto& m : members) {
        if (m && m->nodeType == NodeType::NODE_FUN_DECL) {
            ownMethodNames.insert(static_cast<FunDecl*>(m.get())->name);
        }
    }

    // 合入各 trait 方法；记录每个合入方法来自哪个 trait，用于 diamond 冲突诊断
    std::unordered_map<std::string, std::string> mergedFrom; // 方法名 → trait 名
    for (const auto& traitName : traits) {
        auto it = knownTraits_.find(traitName);
        if (it == knownTraits_.end()) {
            throw ParseError("未定义的 trait: " + traitName + "（trait 须先声明后使用）", line, col);
        }
        for (const auto& method : it->second->methods) {
            auto* fd = static_cast<FunDecl*>(method.get());
            // 类自身方法优先：跳过被覆盖的 trait 方法
            if (ownMethodNames.count(fd->name)) {
                continue;
            }
            // diamond 冲突：两个 trait 提供同名方法且类未覆盖
            auto conflictIt = mergedFrom.find(fd->name);
            if (conflictIt != mergedFrom.end()) {
                throw ParseError("trait 方法冲突（diamond）: 方法 " + fd->name + " 同时来自 trait " +
                                     conflictIt->second + " 与 " + traitName + "，类需自行覆盖以解决冲突",
                                 line, col);
            }
            mergedFrom[fd->name] = traitName;
            // shared_ptr 共享 FunDecl 节点：多个类混入同一 trait 共享同一方法 AST，
            // 三后端各自按普通类方法注册/编译（Interpreter 存 shared_ptr，
            // Compiler/IR 按类名生成独立 chunk，共享节点无状态冲突）。
            members.push_back(method);
        }
    }
}

std::string Parser::parseClassExtends() {
    // 可选的 extends SuperClassName 或 : SuperClassName
    // 调用前应已 consume 类名
    std::string superClassName;
    if (match(TokenType::TK_EXTENDS) || match(TokenType::TK_COLON)) {
        // PARSE-03 fix: 父类名支持类型关键字（与类名声明一致）
        const Token& superName = consumeIdentifierOrType("期望父类名");
        superClassName = StringIntern::intern(superName.lexeme);
    }
    return superClassName;
}

void Parser::parseClassMembers(std::vector<std::shared_ptr<ASTNode>>& members) {
    // 调用前应已 consume '{'，循环直到 '}' 或 EOF
    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        // BUG-PARSER-AUDIT-2 fix: classDecl 成员循环需 try/catch 错误恢复，
        // 与 block() 模式一致。原实现无恢复，单个坏成员抛异常会穿透到外层 block，
        // synchronize 在 class 的 '}' 处返回，外层 block 消费 class 的 '}' 作为
        // 自己的闭合，导致整个类丢失 + 外层块结构错乱。
        try {
            // 类成员可以是：
            // - var 声明（字段）
            // - fun/function 声明（方法）
            // - 带类型注解的声明
            // - 裸方法名定义: methodName() {} （不带 fun 关键字）
            if (check(TokenType::TK_VAR)) {
                members.push_back(varDecl());
            } else if (check(TokenType::TK_FUN)) { // Lexer 已统一 function/func → TK_FUN
                members.push_back(funDecl());
            } else if (isTypeKeyword()) {
                // 带类型注解的字段声明（如 int count = 0;）或带返回类型的方法声明（如 int getValue() {}）
                int savePos = current_;
                std::string typeAnn = parseTypeAnnotation();

                if (isIdentifierOrType()) {
                    // 检查是否是带返回类型的方法声明: int getValue(
                    if (checkNext(TokenType::TK_LPAREN)) {
                        members.push_back(typedFunDecl(typeAnn));
                    } else if (check(TokenType::TK_IDENTIFIER)) {
                        members.push_back(typedVarDecl(typeAnn));
                    } else {
                        current_ = savePos;
                        break;
                    }
                } else {
                    // 回溯
                    current_ = savePos;
                    break;
                }
            } else if (check(TokenType::TK_IDENTIFIER)) {
                // 裸方法定义: methodName(params) { body }
                // 或类类型字段: ClassName fieldName; / ClassName[] fieldName;
                // 或类类型方法: ClassName methodName() { body }
                int savePos = current_;
                const Token& firstTok = advance(); // 方法名或类名

                // 检查是否是方法定义: name(
                if (check(TokenType::TK_LPAREN)) {
                    // 这是一个裸方法定义
                    consume(TokenType::TK_LPAREN, "期望 '('");

                    std::vector<std::string> params;
                    std::vector<std::string> paramTypes;
                    std::vector<std::shared_ptr<ASTNode>> defaultValues; // F10
                    parseParamList(params, paramTypes, defaultValues);
                    consume(TokenType::TK_RPAREN, "期望 ')'");

                    // PARSE-06 fix: 可选的返回类型注解（支持 : type 和 -> type）
                    std::string returnType;
                    if (match(TokenType::TK_COLON)) {
                        // BUG-PARSER-AUDIT-1 fix: 裸方法 `:` 后预检类型 token
                        if (!isIdentifierOrType()) {
                            const Token& tok = peek();
                            throw ParseError("期望返回类型名", tok.line, tok.column);
                        }
                        returnType = parseTypeAnnotation();
                    } else if (check(TokenType::TK_MINUS) && checkNext(TokenType::TK_GT)) {
                        advance();                   // 消耗 '-'
                        advance();                   // 消耗 '>'
                        if (!isIdentifierOrType()) { // BUG-PARSER-AUDIT-1
                            const Token& tok = peek();
                            throw ParseError("期望返回类型名", tok.line, tok.column);
                        }
                        returnType = parseTypeAnnotation();
                    }

                    consume(TokenType::TK_LBRACE, "期望 '{'");
                    auto body = block();

                    auto decl = std::make_unique<FunDecl>(StringIntern::intern(firstTok.lexeme), std::move(params),
                                                          std::move(paramTypes), returnType, std::move(body),
                                                          firstTok.line, firstTok.column);
                    // F10: 计算必需参数个数
                    int reqCount = 0;
                    for (size_t i = 0; i < defaultValues.size(); ++i) {
                        if (defaultValues[i] == nullptr) {
                            ++reqCount;
                        } else {
                            break;
                        }
                    }
                    decl->requiredParamCount = reqCount;
                    decl->defaultValues = std::move(defaultValues);
                    members.push_back(std::move(decl));
                } else if (check(TokenType::TK_LBRACKET)) {
                    // BUG-LPA-04 fix: ClassName[] fieldName; 数组类型字段
                    //   原实现消耗 ClassName 后遇 [ 直接回溯，不支持类类型数组字段。
                    //   与参数列表/for循环/顶层声明行为对齐：消费 [] 后缀构造类型注解。
                    // AUDIT-P2 fix: 支持多维数组字段（ClassName[][] field），
                    //   对齐 parseTypeAnnotation 的 while 循环模式。原实现仅消费单对 []。
                    int bracketSave = current_;
                    std::string typeAnn = firstTok.lexeme;
                    while (check(TokenType::TK_LBRACKET)) {
                        advance(); // 消耗 '['
                        if (!check(TokenType::TK_RBRACKET)) {
                            current_ = bracketSave;
                            break;
                        }
                        advance(); // 消耗 ']'
                        typeAnn += "[]";
                    }
                    if (check(TokenType::TK_IDENTIFIER)) {
                        // ClassName[] fieldName — 类类型数组字段
                        // （ClassName[] methodName() 不合法，不支持类数组返回类型方法）
                        members.push_back(typedVarDecl(typeAnn));
                    } else {
                        // 无法识别，回溯
                        current_ = savePos;
                        break;
                    }
                } else if (check(TokenType::TK_IDENTIFIER)) {
                    // 可能是类类型字段: ClassName fieldName; 或类类型方法: ClassName methodName()
                    if (checkNext(TokenType::TK_LPAREN)) {
                        // ClassName methodName() — 带类类型的方法声明
                        members.push_back(typedFunDecl(firstTok.lexeme));
                    } else {
                        // ClassName fieldName — 类类型字段声明
                        // current_ 已在 advance() 后指向 fieldName，无需回溯
                        members.push_back(typedVarDecl(firstTok.lexeme));
                    }
                } else {
                    // 无法识别，回溯
                    current_ = savePos;
                    break;
                }
            } else {
                break;
            }
        } catch (const ParseError& e) {
            // BUG-PARSER-AUDIT-2 fix: 成员解析错误恢复
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser, e.code);
            synchronize();
            // synchronize 在 '}' 处返回（不消费），循环条件 check(RBRACE) 退出，
            // 由下方 consume(TK_RBRACE) 消费 class 的闭合花括号。
        }
    }
}

// ============================================================
// R99 枚举与 ADT + match 表达式
// ============================================================

std::unique_ptr<EnumDecl> Parser::enumDecl() {
    // enum Name<T, U> { Variant1, Variant2(T), Variant3(T, U), ... }
    const Token& enumTok = consume(TokenType::TK_ENUM, "期望 'enum'");
    const Token& name = consumeIdentifierOrType("期望 enum 名称");

    // 可选泛型类型参数列表 <T, U, ...>
    std::vector<std::string> typeParams;
    if (match(TokenType::TK_LT)) {
        do {
            if (!isIdentifierOrType()) {
                const Token& tok = peek();
                throw ParseError("期望类型参数名", tok.line, tok.column);
            }
            const Token& tp = advance();
            typeParams.push_back(tp.lexeme);
        } while (match(TokenType::TK_COMMA));
        consume(TokenType::TK_GT, "期望 '>' 结束类型参数列表");
    }

    consume(TokenType::TK_LBRACE, "期望 '{' 开始 enum 体");

    // 解析 variant 列表
    std::vector<EnumVariant> variants;
    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        try {
            if (!isIdentifierOrType()) {
                const Token& tok = peek();
                throw ParseError("期望 variant 名称", tok.line, tok.column);
            }
            const Token& varName = advance();
            EnumVariant variant;
            variant.name = varName.lexeme;

            // 可选的参数类型列表 (T1, T2, ...)
            if (match(TokenType::TK_LPAREN)) {
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        // variant 参数类型可以是任意类型注解
                        std::string paramType = parseTypeAnnotation();
                        variant.paramTypes.push_back(paramType);
                    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
                }
                consume(TokenType::TK_RPAREN, "期望 ')' 结束 variant 参数列表");
            }

            variants.push_back(std::move(variant));

            // variant 之间用 ',' 分隔，尾逗号可选
            if (!match(TokenType::TK_COMMA)) {
                break;
            }
        } catch (const ParseError& e) {
            // 错误恢复：与 classDecl 一致
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser, e.code);
            synchronize();
            // synchronize 在 '}' 处返回（不消费），循环条件 check(RBRACE) 退出
        }
    }

    consume(TokenType::TK_RBRACE, "期望 '}' 结束 enum 体");

    int closingBraceLine = previous().line;
    auto decl = std::make_unique<EnumDecl>(name.lexeme, std::move(typeParams), std::move(variants), enumTok.line,
                                           enumTok.column);
    decl->closingBraceLine = closingBraceLine;

    // 注册 enum 名到 knownEnums_，供 call() 区分 EnumVariantExpr 与 MemberAccess
    knownEnums_.insert(name.lexeme);

    return decl;
}

// ============================================================
// 七特性 MVP 阶段 2：宏系统（parse 期展开）
// ============================================================

std::unique_ptr<MacroDecl> Parser::macroDecl() {
    // macro name(p1, p2) { <expr> }
    const Token& macroTok = consume(TokenType::TK_MACRO, "期望 'macro'");
    const Token& name = consumeIdentifierOrType("期望宏名");
    std::string macroName = name.lexeme;

    // 重定义检查（宏表是 parse 期全局单一命名空间，重名会静默改变已解析调用的语义）
    if (knownMacros_.count(macroName)) {
        throw ParseError("宏重复定义: " + macroName, name.line, name.column);
    }

    consume(TokenType::TK_LPAREN, "期望 '('");
    std::vector<std::string> params;
    if (!check(TokenType::TK_RPAREN)) {
        do {
            if (!isIdentifierOrType()) {
                const Token& tok = peek();
                throw ParseError("期望宏参数名", tok.line, tok.column);
            }
            const Token& p = advance();
            // 重复参数名检查（重名参数会使替换表静默丢失前一个实参）
            for (const auto& existing : params) {
                if (existing == p.lexeme) {
                    throw ParseError("宏参数名重复: " + p.lexeme, p.line, p.column);
                }
            }
            params.push_back(p.lexeme);
        } while (match(TokenType::TK_COMMA));
    }
    consume(TokenType::TK_RPAREN, "期望 ')'");

    consume(TokenType::TK_LBRACE, "期望 '{' 开始宏体");
    // 七特性宏升级：区分语句宏与表达式宏。
    // 若首 token 为语句起始关键字（var/if/while/for/return/print/break/continue/throw/try，
    // 均不是合法表达式起始，无歧义）→ 解析为语句块（Block）；否则解析为单表达式。
    TokenType bt = peek().type;
    bool isStmtBody = (bt == TokenType::TK_VAR || bt == TokenType::TK_IF || bt == TokenType::TK_WHILE ||
                       bt == TokenType::TK_FOR || bt == TokenType::TK_RETURN || bt == TokenType::TK_PRINT ||
                       bt == TokenType::TK_BREAK || bt == TokenType::TK_CONTINUE || bt == TokenType::TK_THROW ||
                       bt == TokenType::TK_TRY);
    std::shared_ptr<ASTNode> body;
    if (isStmtBody) {
        // 语句宏：复用 block()（'{' 已消耗，previous() 为 '{'）解析语句列表并消耗 '}'。
        body = std::shared_ptr<ASTNode>(block());
    } else {
        // 表达式宏：body 限单个表达式；尾部分号可选
        auto expr = expression();
        match(TokenType::TK_SEMICOLON);
        consume(TokenType::TK_RBRACE, "期望 '}' 结束宏体（表达式宏限单个表达式）");
        body = std::shared_ptr<ASTNode>(std::move(expr));
    }

    auto decl = std::make_unique<MacroDecl>(std::move(macroName), std::move(params), std::move(body), macroTok.line,
                                            macroTok.column);
    decl->isStatementMacro = isStmtBody;
    // body 解析完成后才登记 → 递归宏（自引用）在 body 解析时报"未定义的宏"，
    // 从根上消除无限展开。
    knownMacros_[decl->name] = decl.get();
    return decl;
}

std::unique_ptr<ASTNode> Parser::macroCall(const Token& nameTok) {
    // 调用前提：nameTok 已消耗，当前位置 '!' '('（调用方已 check 确认）
    advance(); // 消耗 '!'（TK_NOT）
    advance(); // 消耗 '('

    auto it = knownMacros_.find(nameTok.lexeme);
    if (it == knownMacros_.end()) {
        throw ParseError("未定义的宏: " + nameTok.lexeme + "!（宏须先声明后使用）", nameTok.line, nameTok.column);
    }
    MacroDecl* decl = it->second;

    // 解析实参列表
    std::vector<std::shared_ptr<ASTNode>> args;
    if (!check(TokenType::TK_RPAREN)) {
        do {
            args.push_back(expression());
        } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
    }
    consume(TokenType::TK_RPAREN, "期望 ')' 结束宏调用参数列表");

    if (args.size() != decl->params.size()) {
        throw ParseError("宏 " + decl->name + "! 期望 " + std::to_string(decl->params.size()) + " 个参数，实际 " +
                             std::to_string(args.size()) + " 个",
                         nameTok.line, nameTok.column);
    }

    // 构建替换表并克隆 body 模板（实参子树 shared_ptr 共享，不克隆实参本身）
    MacroExpander::SubstMap subst;
    for (size_t i = 0; i < decl->params.size(); ++i) {
        subst[decl->params[i]] = args[i];
    }
    std::string unsupported;
    auto expanded = MacroExpander::cloneWithSubstitution(decl->bodyExpr.get(), subst, 0, &unsupported);
    if (!expanded) {
        if (unsupported == "<depth-limit>") {
            throw ParseError("宏展开嵌套过深（超过 " + std::to_string(RuntimeLimits::MAX_MACRO_EXPANSION_DEPTH) +
                                 " 层）",
                             nameTok.line, nameTok.column);
        }
        throw ParseError("宏 " + decl->name + "! 的 body 含不支持的构造: " + unsupported +
                             "（表达式/语句模板宏仅支持表达式与基础语句子集）",
                         nameTok.line, nameTok.column);
    }

    auto callNode =
        std::make_unique<MacroCallExpr>(nameTok.lexeme, std::move(args), std::move(expanded), nameTok.line,
                                        nameTok.column);
    // 七特性宏升级：语句宏展开为 Block（不产生表达式值），后端执行后补 null
    // 作为表达式值保证栈平衡；表达式宏 producesValue=true。
    callNode->producesValue = !decl->isStatementMacro;
    return callNode;
}

std::unique_ptr<MatchExpr> Parser::matchExpr() {
    // match scrutinee { case Pattern [if guard] => body; ... default => body; }
    const Token& matchTok = consume(TokenType::TK_MATCH, "期望 'match'");
    DepthGuard guard{parseDepth_};

    // scrutinee 必须用括号包裹以避免与 { case ... } 的歧义
    consume(TokenType::TK_LPAREN, "期望 '(' 开始 match 表达式");
    auto scrutinee = expression();
    // R134 fix: scrutinee 支持元组字面量 (e1, e2, ...)。
    // expression() 不会跨 ',' 解析元组（元组字面量由 primary 处理），
    // 因此当首个 expression 后紧跟 ',' 时需切换到元组解析模式。
    // 否则 `match (3, 4) { case (a, b) => ... }` 会因 consume(RPAREN) 看到 ',' 而抛错。
    if (check(TokenType::TK_COMMA)) {
        std::vector<std::shared_ptr<ASTNode>> elements;
        elements.push_back(std::move(scrutinee));
        while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN)) {
            elements.push_back(expression());
        }
        scrutinee = std::make_unique<TupleLiteral>(std::move(elements), matchTok.line, matchTok.column);
    }
    consume(TokenType::TK_RPAREN, "期望 ')' 结束 match 表达式");

    consume(TokenType::TK_LBRACE, "期望 '{' 开始 match 体");

    std::vector<MatchCase> cases;
    bool hasDefault = false;

    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        try {
            MatchCase mc;

            if (match(TokenType::TK_DEFAULT)) {
                // default => body
                if (hasDefault) {
                    const Token& tok = previous();
                    throw ParseError("match 表达式不能有多个 default 分支", tok.line, tok.column);
                }
                hasDefault = true;
                mc.isDefault = true;
                mc.pattern = nullptr;
            } else if (match(TokenType::TK_CASE)) {
                // case Pattern => body  (R134: Pattern 支持 OR/TUPLE/VARIABLE/嵌套)
                mc.pattern = matchPattern();
                mc.isDefault = false;
            } else {
                // 兼容裸 Pattern => body（无 case 关键字）
                mc.pattern = matchPattern();
                mc.isDefault = false;
            }

            // R134: 可选 guard 表达式 `if cond`
            if (match(TokenType::TK_IF)) {
                mc.guard = expression();
            }

            consume(TokenType::TK_ARROW, "期望 '=>' 分隔 match 模式与体");

            // case 体：单表达式（以 ';' 结束）或块语句
            if (check(TokenType::TK_LBRACE)) {
                advance(); // 消耗 '{'
                mc.body = block();
            } else {
                auto expr = expression();
                // 可选 ';'（match case 体可能不强制分号）
                match(TokenType::TK_SEMICOLON);
                mc.body = std::move(expr);
            }

            cases.push_back(std::move(mc));
        } catch (const ParseError& e) {
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser, e.code);
            // AUDIT-R6 F1 fix (P0): matchExpr 错误恢复死循环。synchronize() 在同步关键字
            // （var/fun/class 等，BUG-PARSER-SYNC-1）处不消耗 token 直接返回，但本循环无法
            // 处理这些关键字（既非 case/default 也非 '}'），裸 pattern 分支再次抛错→
            // synchronize 仍不前进→死循环挂死（如 `match (1) { case n: { var f = 1; } }`，
            // 用户写错 `case X:` 而非 `case X =>` 即触发）。修复：同步后无进展则强制
            // 前进一个 token，保证循环收敛（'}'/EOF 由循环条件终止）。
            int beforeSync = current_;
            synchronize();
            if (current_ == beforeSync && !isAtEnd() && !check(TokenType::TK_RBRACE)) {
                advance();
            }
        }
    }

    consume(TokenType::TK_RBRACE, "期望 '}' 结束 match 体");

    return std::make_unique<MatchExpr>(std::move(scrutinee), std::move(cases), matchTok.line, matchTok.column);
}

// R134 模式匹配扩展：matchPattern 改为递归下降解析器。
// 解析优先级（从低到高）：
//   orPattern:   andPattern ('|' andPattern)*    -- OR pattern
//   andPattern:  primaryPattern                   -- 当前无 and pattern，留接口
//   primaryPattern:
//     | '_'                                -- WILDCARD
//     | INT_LIT/FLOAT_LIT/STRING_LIT/...   -- LITERAL
//     | '(' pattern (',' pattern)* ')'     -- TUPLE pattern (或裸 (pattern))
//     | Identifier                          -- VARIABLE pattern（绑定整个 scrutinee）
//     | Identifier '.' Identifier '(' ... ')'  -- VARIANT pattern（嵌套子 pattern）
//
// 注意：VARIANT 的子 pattern 不再是字符串变量名，而是完整的 pattern，
// 支持 `Some(Some(x))` / `Pair(Some(a), None)` 等嵌套形式。
std::shared_ptr<MatchPattern> Parser::matchPattern() {
    auto left = matchPrimaryPattern();
    // OR pattern: left or right or ...（MiniLang 用 'or' 关键字而非 '|'）
    while (match(TokenType::TK_OR)) {
        auto right = matchPrimaryPattern();
        // 合并到 OR pattern：若 left 已是 OR，则追加；否则新建 OR
        if (left->kind == MatchPatternKind::OR) {
            left->subPatterns.push_back(std::move(right));
        } else {
            auto orPattern = std::make_shared<MatchPattern>(MatchPatternKind::OR, left->line, left->column);
            orPattern->subPatterns.push_back(std::move(left));
            orPattern->subPatterns.push_back(std::move(right));
            left = orPattern;
        }
    }
    return left;
}

std::shared_ptr<MatchPattern> Parser::matchPrimaryPattern() {
    // Bug #9 fix: 互递归深度保护（matchPattern <-> matchPrimaryPattern）
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("模式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line, peek().column);
    }
    DepthGuard guard{parseDepth_};

    const Token& tok = peek();

    // 1. 通配符 _
    if (check(TokenType::TK_IDENTIFIER) && tok.lexeme == "_") {
        advance();
        return std::make_shared<MatchPattern>(MatchPatternKind::WILDCARD, tok.line, tok.column);
    }

    // 2. 字面量模式（INT_LIT / FLOAT_LIT / STRING_LIT / TRUE / FALSE / NULL）
    if (check(TokenType::TK_INT_LIT) || check(TokenType::TK_FLOAT_LIT) || check(TokenType::TK_STRING_LIT) ||
        check(TokenType::TK_TRUE) || check(TokenType::TK_FALSE) || check(TokenType::TK_NULL)) {
        auto literalExpr = primary();
        auto p = std::make_shared<MatchPattern>(MatchPatternKind::LITERAL, tok.line, tok.column);
        p->literal = std::move(literalExpr);
        return p;
    }

    // 3. 元组模式：(pattern, pattern, ...) 或裸 (pattern)
    if (check(TokenType::TK_LPAREN)) {
        advance(); // 消耗 '('
        // 空 () 视为 0 元组（不支持，要求至少 1 个元素）
        if (check(TokenType::TK_RPAREN)) {
            throw ParseError("match 元组模式要求至少 1 个元素", tok.line, tok.column);
        }
        auto p = std::make_shared<MatchPattern>(MatchPatternKind::TUPLE, tok.line, tok.column);
        do {
            p->subPatterns.push_back(matchPattern());
        } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
        consume(TokenType::TK_RPAREN, "期望 ')' 结束元组模式");
        // 单元素 (x) 不是真正的元组，回退为裸 pattern
        if (p->subPatterns.size() == 1) {
            return std::move(p->subPatterns[0]);
        }
        return p;
    }

    // 4. 标识符：可能是 VARIABLE pattern 或 VARIANT pattern
    if (check(TokenType::TK_IDENTIFIER)) {
        const std::string& name = tok.lexeme;
        // 4a. VARIANT pattern: EnumName.VariantName(...)
        if (knownEnums_.find(name) != knownEnums_.end()) {
            advance(); // 消耗 enum 名
            consume(TokenType::TK_DOT, "期望 '.' 分隔 enum 名与 variant 名");
            if (!isIdentifierOrType()) {
                const Token& v = peek();
                throw ParseError("期望 variant 名", v.line, v.column);
            }
            const Token& varNameTok = advance();

            auto p = std::make_shared<MatchPattern>(MatchPatternKind::VARIANT, tok.line, tok.column);
            p->enumName = name;
            p->variantName = varNameTok.lexeme;

            // 可选的子 pattern 列表 (subPattern1, subPattern2, ...)
            // 子 pattern 是完整 pattern（递归 matchPattern），支持嵌套
            if (match(TokenType::TK_LPAREN)) {
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        p->subPatterns.push_back(matchPattern());
                    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
                }
                consume(TokenType::TK_RPAREN, "期望 ')' 结束 variant 子 pattern 列表");
            }
            return p;
        }
        // 4b. VARIABLE pattern: x（绑定整个 scrutinee 到变量 x）
        // 注意：变量名不能是关键字（match/case/default/if 等）
        advance(); // 消耗变量名
        auto p = std::make_shared<MatchPattern>(MatchPatternKind::VARIABLE, tok.line, tok.column);
        p->variableName = name;
        return p;
    }

    throw ParseError("期望 match 模式（'_' / 字面量 / Identifier / EnumName.VariantName(...) / (p1, p2, ...)）",
                     tok.line, tok.column);
}

std::unique_ptr<ASTNode> Parser::statement() {
    // P1-1 fix: 无花括号的 if/while/for 嵌套语句也需深度保护，防止栈溢出 DoS
    // 覆盖 "if (a) if (b) if (c) ..." 这类无花括号嵌套场景
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("语句嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line, peek().column);
    }
    DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_

    // BUG-LPA-05 fix: import/export 在无花括号单语句体中显式拒绝。
    //   原实现 statement() 不识别 TK_IMPORT/TK_EXPORT，落入 expressionStatement()
    //   → primary() 抛"意外的 Token"，错误消息误导用户。
    //   blockDepth_ 检查仅覆盖块内场景，无花括号体（if/while/for 无 {}）走
    //   statement() 路径不递增 blockDepth_，需在此显式拒绝。
    if (check(TokenType::TK_IMPORT)) {
        const Token& tok = peek();
        throw ParseError("import 语句只能在顶层使用", tok.line, tok.column);
    }
    if (check(TokenType::TK_EXPORT)) {
        const Token& tok = peek();
        throw ParseError("export 语句只能在顶层使用", tok.line, tok.column);
    }

    if (check(TokenType::TK_IF))
        return ifStmt();
    if (check(TokenType::TK_WHILE))
        return whileStmt();
    if (check(TokenType::TK_FOR))
        return forStmt();
    if (check(TokenType::TK_RETURN))
        return returnStmt();
    if (check(TokenType::TK_BREAK))
        return breakStmt();
    if (check(TokenType::TK_CONTINUE))
        return continueStmt();
    if (check(TokenType::TK_TRY))
        return tryStmt();
    if (check(TokenType::TK_THROW))
        return throwStmt();
    if (check(TokenType::TK_PRINT))
        return printStmt();
    if (check(TokenType::TK_LBRACE)) {
        advance();
        return block();
    }
    return expressionStatement();
}

std::unique_ptr<IfStmt> Parser::ifStmt() {
    const Token& ifTok = consume(TokenType::TK_IF, "期望 'if'");
    consume(TokenType::TK_LPAREN, "期望 '('");
    auto cond = expression();
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 支持带花括号的块和不带花括号的单条语句
    std::unique_ptr<ASTNode> thenB;
    if (check(TokenType::TK_LBRACE)) {
        advance();
        thenB = block();
    } else {
        thenB = statement();
    }

    std::unique_ptr<ASTNode> elseB = nullptr;
    if (match(TokenType::TK_ELSE)) {
        if (check(TokenType::TK_IF)) {
            // else if — else 分支是另一个 if 语句
            // P1 fix: else-if 链递归也需深度保护（if..else if..else if.. 可深度嵌套）
            if (parseDepth_ >= MAX_PARSE_DEPTH) {
                throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                                 peek().column);
            }
            DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_
            elseB = ifStmt();
        } else if (check(TokenType::TK_LBRACE)) {
            advance();
            elseB = block();
        } else {
            elseB = statement();
        }
    }

    return std::make_unique<IfStmt>(std::move(cond), std::move(thenB), std::move(elseB), ifTok.line, ifTok.column);
}

std::unique_ptr<WhileStmt> Parser::whileStmt() {
    const Token& whileTok = consume(TokenType::TK_WHILE, "期望 'while'");
    consume(TokenType::TK_LPAREN, "期望 '('");
    auto cond = expression();
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // R164 协程/生成器：标记循环体解析期间 yieldInLoop_=true，供 primary() 检测
    // yield-in-loop。BoolScope 保证异常路径下恢复外层值（支持嵌套循环与函数边界）。
    BoolScope loopScope{yieldInLoop_};
    yieldInLoop_ = true;

    // 支持带花括号的块和不带花括号的单条语句
    std::unique_ptr<ASTNode> body;
    if (check(TokenType::TK_LBRACE)) {
        advance();
        body = block();
    } else {
        body = statement();
    }

    return std::make_unique<WhileStmt>(std::move(cond), std::move(body), whileTok.line, whileTok.column);
}

std::unique_ptr<ForStmt> Parser::forStmt() {
    const Token& forTok = consume(TokenType::TK_FOR, "期望 'for'");
    consume(TokenType::TK_LPAREN, "期望 '('");

    // 初始化部分
    std::unique_ptr<ASTNode> init = nullptr;
    if (check(TokenType::TK_VAR)) {
        init = varDecl();
        // varDecl 已经消耗了分号
    } else if (isTypeKeyword()) {
        // 类型注解声明，如 int j = 0;
        int savePos = current_;
        std::string typeAnn = parseTypeAnnotation();

        if (check(TokenType::TK_IDENTIFIER)) {
            init = typedVarDecl(typeAnn);
            // typedVarDecl 已经消耗了分号
        } else {
            // 回溯，当做表达式处理
            current_ = savePos;
            init = expression();
            consume(TokenType::TK_SEMICOLON, "期望 ';'");
        }
    } else if (isClassTypeDeclStart()) {
        // H2 fix: 类名类型注解声明，如 Point p = create(); 或 Point[] arr = build();
        int savePos = current_;
        std::string typeAnn = parseTypeAnnotation(); // 消耗类名 + 可选 []
        if (check(TokenType::TK_IDENTIFIER) && checkNext(TokenType::TK_LPAREN)) {
            // ClassName funcName( — 函数声明不应出现在 for 初始化中，回溯当表达式处理
            current_ = savePos;
            init = expression();
            consume(TokenType::TK_SEMICOLON, "期望 ';'");
        } else if (check(TokenType::TK_IDENTIFIER)) {
            init = typedVarDecl(typeAnn);
            // typedVarDecl 已经消耗了分号
        } else {
            // 回溯，当做表达式处理
            current_ = savePos;
            init = expression();
            consume(TokenType::TK_SEMICOLON, "期望 ';'");
        }
    } else if (!check(TokenType::TK_SEMICOLON)) {
        init = expression();
        consume(TokenType::TK_SEMICOLON, "期望 ';'");
    } else {
        consume(TokenType::TK_SEMICOLON, "期望 ';'");
    }

    // 条件部分
    std::unique_ptr<ASTNode> cond = nullptr;
    if (!check(TokenType::TK_SEMICOLON)) {
        cond = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';'");

    // 更新部分
    std::unique_ptr<ASTNode> update = nullptr;
    if (!check(TokenType::TK_RPAREN)) {
        update = expression();
    }
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // R164 协程/生成器：标记循环体解析期间 yieldInLoop_=true（与 whileStmt 对称）。
    BoolScope loopScope{yieldInLoop_};
    yieldInLoop_ = true;

    // 支持带花括号的块和不带花括号的单条语句
    std::unique_ptr<ASTNode> body;
    if (check(TokenType::TK_LBRACE)) {
        advance();
        body = block();
    } else {
        body = statement();
    }

    return std::make_unique<ForStmt>(std::move(init), std::move(cond), std::move(update), std::move(body), forTok.line,
                                     forTok.column);
}

std::unique_ptr<ReturnStmt> Parser::returnStmt() {
    const Token& retTok = consume(TokenType::TK_RETURN, "期望 'return'");

    std::unique_ptr<ASTNode> val = nullptr;
    if (!check(TokenType::TK_SEMICOLON)) {
        val = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';'");

    return std::make_unique<ReturnStmt>(std::move(val), retTok.line, retTok.column);
}

std::unique_ptr<BreakStmt> Parser::breakStmt() {
    const Token& tok = consume(TokenType::TK_BREAK, "期望 'break'");
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束 break 语句");
    return std::make_unique<BreakStmt>(tok.line, tok.column);
}

std::unique_ptr<ContinueStmt> Parser::continueStmt() {
    const Token& tok = consume(TokenType::TK_CONTINUE, "期望 'continue'");
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束 continue 语句");
    return std::make_unique<ContinueStmt>(tok.line, tok.column);
}

std::unique_ptr<TryStmt> Parser::tryStmt() {
    const Token& tryTok = consume(TokenType::TK_TRY, "期望 'try'");
    consume(TokenType::TK_LBRACE, "try 后期望 '{'");
    auto tryBlock = block();

    // BUG-AUDIT-FINALLY-1: catch 子句可选（支持 try-finally 无 catch 语法）。
    // 至少需有 catch 或 finally 之一，否则报错。
    std::string catchVarName;
    std::unique_ptr<Block> catchBlock;
    std::unique_ptr<Block> finallyBlock;

    bool hasCatch = check(TokenType::TK_CATCH);
    bool hasFinally = check(TokenType::TK_FINALLY);
    if (!hasCatch && !hasFinally) {
        throw ParseError("try 语句后必须跟 catch 或 finally", tryTok.line, tryTok.column);
    }

    // AUDIT-P2.9 fix: 记录 catch/finally 关键字行号，供 Formatter 注入块间注释。
    int catchKeywordLine = 0;
    int finallyKeywordLine = 0;

    if (hasCatch) {
        advance(); // 消耗 'catch'
        catchKeywordLine = previous().line;
        consume(TokenType::TK_LPAREN, "catch 后期望 '('");
        const Token& varTok = consume(TokenType::TK_IDENTIFIER, "期望 catch 变量名");
        consume(TokenType::TK_RPAREN, "期望 ')'");
        consume(TokenType::TK_LBRACE, "catch 后期望 '{'");
        catchBlock = block();
        catchVarName = varTok.lexeme;
        // catch 后可选 finally
        if (check(TokenType::TK_FINALLY)) {
            hasFinally = true;
        }
    }

    if (hasFinally) {
        advance(); // 消耗 'finally'
        finallyKeywordLine = previous().line;
        consume(TokenType::TK_LBRACE, "finally 后期望 '{'");
        finallyBlock = block();
    }

    auto stmt = std::make_unique<TryStmt>(std::move(tryBlock), catchVarName, std::move(catchBlock),
                                          std::move(finallyBlock), tryTok.line, tryTok.column);
    stmt->catchKeywordLine = catchKeywordLine;
    stmt->finallyKeywordLine = finallyKeywordLine;
    return stmt;
}

std::unique_ptr<ThrowStmt> Parser::throwStmt() {
    const Token& throwTok = consume(TokenType::TK_THROW, "期望 'throw'");
    auto expr = expression();
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束 throw 语句");
    return std::make_unique<ThrowStmt>(std::move(expr), throwTok.line, throwTok.column);
}

std::unique_ptr<ImportStmt> Parser::importStmt() {
    // BUG 5b fix: import 只能在顶层使用
    // FIX: 不抛异常，改为记录诊断后继续解析。抛异常会导致 synchronize()
    // 跳过 try 块内的 catch 子句，使 tryStmt() 找不到 catch 而进入死循环。
    if (blockDepth_ > 0) {
        const Token& tok = peek();
        diagnostics_.addError("import 语句只能在顶层使用", tok.line, tok.column, DiagSource::Parser,
                              DiagCodes::kImportNotAtTopLevel);
        // 继续解析 import 语句，不抛异常
    }
    const Token& importTok = consume(TokenType::TK_IMPORT, "期望 'import'");

    std::vector<std::string> names;
    bool importAll = false;
    std::string namespaceAlias; // P2-11: import * as ns 模式的命名空间别名

    // 三种形式:
    // 1. import "path";                    — 导入全部到当前作用域
    // 2. import { a, b } from "path";      — 导入指定名称
    // 3. import * as ns from "path";       — 导入全部到命名空间对象 ns（P2-11）
    if (check(TokenType::TK_LBRACE)) {
        advance(); // 消耗 '{'
        do {
            const Token& name = consume(TokenType::TK_IDENTIFIER, "期望导入名称");
            names.push_back(name.lexeme);
        } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RBRACE));
        consume(TokenType::TK_RBRACE, "期望 '}'");
        consume(TokenType::TK_FROM, "期望 'from'");
    } else if (check(TokenType::TK_STAR)) {
        // P2-11: import * as ns from "path"
        advance(); // 消耗 '*'
        consume(TokenType::TK_AS, "期望 'as'（import * as ns 语法）");
        const Token& aliasTok = consume(TokenType::TK_IDENTIFIER, "期望命名空间别名");
        namespaceAlias = aliasTok.lexeme;
        consume(TokenType::TK_FROM, "期望 'from'");
        importAll = true; // namespace 模式也是导入全部
    } else {
        importAll = true;
    }

    const Token& pathTok = consume(TokenType::TK_STRING_LIT, "期望模块路径字符串");
    // BUG 4a fix: 空模块路径校验
    // A1 fix: Token.literal 是 variant，用 literalString() 访问
    if (pathTok.literalString().empty()) {
        throw ParseError("模块路径不能为空", pathTok.line, pathTok.column);
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束 import 语句");

    // P2-11: namespace 模式用专用构造函数
    if (!namespaceAlias.empty()) {
        return std::make_unique<ImportStmt>(pathTok.literalString(), namespaceAlias, importTok.line, importTok.column);
    }
    return std::make_unique<ImportStmt>(pathTok.literalString(), std::move(names), importAll, importTok.line,
                                        importTok.column);
}

std::unique_ptr<ExportStmt> Parser::exportStmt() {
    // BUG 5b fix: export 只能在顶层使用
    // FIX: 同 importStmt()，不抛异常，改为记录诊断后继续解析。
    if (blockDepth_ > 0) {
        const Token& tok = peek();
        diagnostics_.addError("export 语句只能在顶层使用", tok.line, tok.column, DiagSource::Parser,
                              DiagCodes::kExportNotAtTopLevel);
    }
    const Token& exportTok = consume(TokenType::TK_EXPORT, "期望 'export'");

    // export 后必须是声明: var / fun / class / 类型注解
    std::unique_ptr<ASTNode> decl;
    if (check(TokenType::TK_VAR)) {
        decl = varDecl();
    } else if (check(TokenType::TK_FUN)) {
        decl = funDecl();
    } else if (check(TokenType::TK_CLASS)) {
        decl = classDecl();
    } else if (isTypeKeyword() || check(TokenType::TK_IDENTIFIER)) {
        // 类型注解声明: int x = 1; 或 ClassName obj;
        int savePos = current_;
        std::string typeAnn = parseTypeAnnotation();
        if (isIdentifierOrType()) {
            if (checkNext(TokenType::TK_LPAREN)) {
                decl = typedFunDecl(typeAnn);
            } else if (check(TokenType::TK_IDENTIFIER)) {
                decl = typedVarDecl(typeAnn);
            } else {
                current_ = savePos;
                throw ParseError("export 后期望声明", peek().line, peek().column);
            }
        } else {
            current_ = savePos;
            throw ParseError("export 后期望声明", peek().line, peek().column);
        }
    } else {
        throw ParseError("export 后期望 var/fun/class 声明", peek().line, peek().column);
    }

    return std::make_unique<ExportStmt>(std::move(decl), exportTok.line, exportTok.column);
}

std::unique_ptr<PrintStmt> Parser::printStmt() {
    const Token& printTok = consume(TokenType::TK_PRINT, "期望 'print'");
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::shared_ptr<ASTNode>> values;
    if (!check(TokenType::TK_RPAREN)) {
        do {
            values.push_back(expression());
        } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
    }

    consume(TokenType::TK_RPAREN, "期望 ')'");
    consume(TokenType::TK_SEMICOLON, "期望 ';'");

    return std::make_unique<PrintStmt>(std::move(values), printTok.line, printTok.column);
}

std::unique_ptr<Block> Parser::block() {
    const Token& lbrace = previous(); // '{' 已被消耗

    // P0-1 fix: 块嵌套深度保护，防止 {{...}} 深度嵌套导致 C++ 栈溢出
    if (blockDepth_ >= MAX_BLOCK_DEPTH) {
        throw ParseError("块嵌套过深（超过 " + std::to_string(MAX_BLOCK_DEPTH) + " 层）", lbrace.line, lbrace.column);
    }
    DepthGuard blockGuard{blockDepth_}; // C4 fix: 复用 DepthGuard，自动 ++/-- blockDepth_

    // PERF-21 fix: 预分配小量容量，减少小 block 的 realloc（典型 block 含 3-10 条语句）
    std::vector<std::shared_ptr<ASTNode>> stmts;
    stmts.reserve(8);

    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        // FIX: catch 也是块边界（try 块的 tryBlock 以 catch 结束）
        if (check(TokenType::TK_CATCH))
            break;
        // BUG-PARSER-SYNC-2 fix (P1): finally 也是 try 块边界，与 catch 同等处理。
        // 原实现仅识别 catch，try { x = 1 finally { ... } } 在 x 缺分号时，
        // finally 被 synchronize() 吞掉，整个 finally 块丢失。
        if (check(TokenType::TK_FINALLY))
            break;
        // BUG-PARSER-SYNC-4 fix (P2): else 是 if 块边界。
        // if (x) { y = 1 else { ... } } 在 y 缺分号时，else 需作为块边界让 ifStmt 处理。
        // 必须与 SYNC-1 一组修复，否则 synchronize() 不再吞 else 但 block() 不中断会无限循环。
        if (check(TokenType::TK_ELSE))
            break;
        // BUG-PARSER-AUDIT-5 fix: block() 主循环也需错误上限检查，
        // 防止恶意嵌套块内含大量错误触发 O(N) 诊断内存膨胀。
        // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
        const int dynMaxParseErrors = RuntimeLimits::RuntimeConfig::instance().maxParseErrors();
        if (diagnostics_.errorCount() >= dynMaxParseErrors) {
            diagnostics_.addError("错误过多（超过 " + std::to_string(dynMaxParseErrors) + " 条），停止解析",
                                  peek().line, peek().column, DiagSource::Parser, DiagCodes::kTooManyErrors);
            break;
        }
        try {
            auto decl = declaration();
            if (decl) {
                stmts.push_back(std::move(decl));
            }
        } catch (const ParseError& e) {
            // P1-1/P1-2 fix: block() 内错误恢复，避免单错误导致整个块被放弃
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser, e.code);
            synchronize();
            // synchronize 后若已到 '}' 或 EOF 则退出循环
        }
    }

    // BUG-PARSER-SYNC-2/4 fix: 当 block() 因 catch/finally/else 中断时，
    // peek() 不是 '}'（用户缺 '}'），不应强制 consume 抛错导致整个语句丢失。
    // 这些关键字是结构性块边界，由调用方（tryStmt/ifStmt）处理。
    if (check(TokenType::TK_RBRACE)) {
        advance(); // 正常消耗 '}'
    } else if (check(TokenType::TK_CATCH) || check(TokenType::TK_FINALLY) || check(TokenType::TK_ELSE) ||
               check(TokenType::TK_EOF)) {
        // 结构性块边界或 EOF：不消耗，让调用方处理 catch/finally/else
    } else {
        // 其他情况（如错误上限 break 后 peek 非 '}'）：记录诊断但不抛错
        diagnostics_.addError("期望 '}'", peek().line, peek().column, DiagSource::Parser, DiagCodes::kUnbalancedBrace);
    }

    auto blk = std::make_unique<Block>(std::move(stmts), lbrace.line, lbrace.column);
    blk->closingBraceLine = (previous().type == TokenType::TK_RBRACE) ? previous().line : peek().line;
    return blk;
}

std::unique_ptr<ASTNode> Parser::expressionStatement() {
    auto expr = expression();
    consume(TokenType::TK_SEMICOLON, "期望 ';'");
    return expr;
}

// ---- 表达式 ----
// 运算符优先级"攀登"（precedence climbing）采用递归下降实现：
// 每一层函数只处理"本级及更低优先级"的运算符，遇到更高优先级就下沉到下一层函数。
// 层级从松到紧依次为：
//   assignment(右结合) → or_ → and_ → equality → comparison → term → factor
//   → unary → call → primary
// 其中 or_(1) < and_(2) < equality(3) < comparison(4) < term(+|-,5)
//   < factor(*|/|%,6)，数值与 BinaryOp::precedence 完全一致（单一来源）。
// 左结合性由每层的 while(match(...)) 循环天然保证：a - b - c 被解析为
// ((a-b)-c)——循环不断把"右侧同级子表达式"挂到已构建的左树上。
// 右结合（赋值）则在 assignment() 中通过"递归调用 assignment() 而非循环"实现：
// a = b = c 解析为 a = (b = c)。
// 每层入口均做 parseDepth_ 深度保护（配合 DepthGuard 自动回退），
// 防止极端嵌套表达式触发 C++ 递归栈溢出导致 DoS。

std::unique_ptr<ASTNode> Parser::expression() {
    // P15 fix: 递归深度保护，防止极端嵌套表达式导致栈溢出
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                         peek().column);
    }
    DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_
    return assignment();
}

std::unique_ptr<ASTNode> Parser::assignment() {
    // P1 fix: 赋值右结合递归也需深度保护（a = b = c = ... 可深度嵌套）
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                         peek().column);
    }
    DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_

    auto expr = or_();

    // 检查是否是赋值
    if (match(TokenType::TK_ASSIGN)) {
        const Token& eq = previous();

        // 变量赋值: identifier = expr
        if (expr->nodeType == NodeType::NODE_VAR_REF) {
            auto* varRef = static_cast<VarRef*>(expr.get());
            auto val = assignment(); // 右结合
            return std::make_unique<Assignment>(varRef->name, std::move(val), eq.line, eq.column);
        }

        // 索引赋值: arr[index] = expr 或 dict[key] = expr
        if (expr->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* idxAccess = static_cast<IndexAccess*>(expr.get());
            auto val = assignment(); // 右结合
            // 从 IndexAccess 中提取 object 和 index
            auto obj = std::move(idxAccess->object);
            auto idx = std::move(idxAccess->index);
            return std::make_unique<IndexAssign>(std::move(obj), std::move(idx), std::move(val), eq.line, eq.column);
        }

        // 成员赋值: obj.field = expr
        if (expr->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* memAccess = static_cast<MemberAccess*>(expr.get());
            auto val = assignment(); // 右结合
            auto obj = std::move(memAccess->object);
            std::string field = memAccess->fieldName;
            return std::make_unique<MemberAssign>(std::move(obj), field, std::move(val), eq.line, eq.column);
        }

        throw ParseError("无效的赋值目标", eq.line, eq.column);
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::or_() {
    auto left = and_();

    while (match(TokenType::TK_OR)) {
        const Token& op = previous();
        auto right = and_();
        left = std::make_unique<BinaryOp>(BinOpType::BIN_OR, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::and_() {
    auto left = equality();

    while (match(TokenType::TK_AND)) {
        const Token& op = previous();
        auto right = equality();
        left = std::make_unique<BinaryOp>(BinOpType::BIN_AND, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::equality() {
    auto left = comparison();

    while (match(TokenType::TK_EQ, TokenType::TK_NEQ)) {
        const Token& op = previous();
        auto right = comparison();
        BinOpType binOp = (op.type == TokenType::TK_EQ) ? BinOpType::BIN_EQ : BinOpType::BIN_NEQ;
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::comparison() {
    auto left = term();

    while (match(TokenType::TK_LT, TokenType::TK_GT, TokenType::TK_LEQ, TokenType::TK_GEQ)) {
        const Token& op = previous();
        auto right = term();
        BinOpType binOp;
        switch (op.type) {
        case TokenType::TK_LT:
            binOp = BinOpType::BIN_LT;
            break;
        case TokenType::TK_GT:
            binOp = BinOpType::BIN_GT;
            break;
        case TokenType::TK_LEQ:
            binOp = BinOpType::BIN_LTE;
            break;
        case TokenType::TK_GEQ:
            binOp = BinOpType::BIN_GTE;
            break;
        default:
            binOp = BinOpType::BIN_UNKNOWN;
            break;
        }
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::term() {
    auto left = factor();

    while (match(TokenType::TK_PLUS, TokenType::TK_MINUS)) {
        const Token& op = previous();
        auto right = factor();
        BinOpType binOp = (op.type == TokenType::TK_PLUS) ? BinOpType::BIN_ADD : BinOpType::BIN_SUB;
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::factor() {
    auto left = unary();

    while (match(TokenType::TK_STAR, TokenType::TK_SLASH, TokenType::TK_PERCENT)) {
        const Token& op = previous();
        auto right = unary();
        BinOpType binOp;
        switch (op.type) {
        case TokenType::TK_STAR:
            binOp = BinOpType::BIN_MUL;
            break;
        case TokenType::TK_SLASH:
            binOp = BinOpType::BIN_DIV;
            break;
        case TokenType::TK_PERCENT:
            binOp = BinOpType::BIN_MOD;
            break;
        default:
            binOp = BinOpType::BIN_UNKNOWN;
            break;
        }
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right), op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::unary() {
    // P1 fix: 一元运算符递归也需深度保护（---...x 可深度嵌套）
    // AUDIT-BUG-P3 fix: 深度检查移到 match() 之前，避免 match 消耗 token 后
    // 抛出错误导致 synchronize() 多丢失一个 token。
    if (check(TokenType::TK_NOT) || check(TokenType::TK_MINUS)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                             peek().column);
        }
        DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_
        advance();                     // 消耗一元运算符
        const Token& op = previous();
        auto operand = unary();
        auto uopType =
            (op.type == TokenType::TK_NOT) ? UnaryOp::UnaryOpType::UOP_NOT : UnaryOp::UnaryOpType::UOP_NEGATE;
        return std::make_unique<UnaryOp>(uopType, std::move(operand), op.line, op.column);
    }
    // PARSE-07 fix: 一元 + 创建 UnaryOp 节点保留 AST 保真度
    if (check(TokenType::TK_PLUS)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", peek().line,
                             peek().column);
        }
        DepthGuard guard{parseDepth_}; // C4 fix: 自动 ++/-- parseDepth_
        advance();                     // 消耗 +

        const Token& op = previous();
        auto operand = unary();
        return std::make_unique<UnaryOp>(UnaryOp::UnaryOpType::UOP_PLUS, std::move(operand), op.line, op.column);
    }
    return call();
}

std::unique_ptr<ASTNode> Parser::call() {
    auto expr = primary();

    // 支持链式调用: obj.method(args).field[0]
    while (true) {
        // 函数调用: expr(args)
        if (match(TokenType::TK_LPAREN)) {
            const Token& paren = previous();
            // 解析参数列表
            std::vector<std::shared_ptr<ASTNode>> args;
            args.reserve(4); // Perf-Finding4: 避免常见 1-3 参函数调用的 1-2 次 realloc
            if (!check(TokenType::TK_RPAREN)) {
                do {
                    args.push_back(expression());
                } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
            }
            consume(TokenType::TK_RPAREN, "期望 ')' 结束参数列表");

            if (expr->nodeType == NodeType::NODE_VAR_REF) {
                // 命名函数调用（原有路径）
                auto* varRef = static_cast<VarRef*>(expr.get());
                expr = std::make_unique<FunCall>(varRef->name, std::move(args), varRef->line, varRef->column);
            } else {
                // 链式调用 / 表达式调用: f(x)(y), closures, 高阶函数
                expr = std::make_unique<FunCall>(std::move(expr), std::move(args), paren.line, paren.column);
            }
            continue;
        }

        // 索引访问: expr[index]
        if (match(TokenType::TK_LBRACKET)) {
            const Token& bracket = previous();
            auto index = expression();
            consume(TokenType::TK_RBRACKET, "期望 ']' 结束索引访问");
            expr = std::make_unique<IndexAccess>(std::move(expr), std::move(index), bracket.line, bracket.column);
            continue;
        }

        // 成员访问: expr.field 或 方法调用 expr.method(args)
        if (match(TokenType::TK_DOT)) {
            const Token& dot = previous();
            // PARSE-10 fix: 成员名支持类型关键字（与声明端一致）
            const Token& fieldName = consumeIdentifierOrType("期望成员名");

            // R99 枚举与 ADT: EnumName.VariantName 或 EnumName.VariantName(args)
            // 若 expr 是 VarRef 且其 name 在 knownEnums_ 中，则生成 EnumVariantExpr
            // 而非 MemberAccess/MethodCall。这样后续 Interpreter/VM 可直接构造
            // enum variant 值而无需运行时查表消歧。
            if (expr->nodeType == NodeType::NODE_VAR_REF) {
                auto* varRef = static_cast<VarRef*>(expr.get());
                if (knownEnums_.find(varRef->name) != knownEnums_.end()) {
                    // 收集可选的构造参数
                    std::vector<std::shared_ptr<ASTNode>> args;
                    if (match(TokenType::TK_LPAREN)) {
                        args.reserve(4);
                        if (!check(TokenType::TK_RPAREN)) {
                            do {
                                args.push_back(expression());
                            } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
                        }
                        consume(TokenType::TK_RPAREN, "期望 ')' 结束 enum variant 构造参数列表");
                    }
                    expr = std::make_unique<EnumVariantExpr>(varRef->name, fieldName.lexeme, std::move(args),
                                                             varRef->line, varRef->column);
                    continue;
                }
            }

            // 检查是否是方法调用: obj.method(args)
            if (match(TokenType::TK_LPAREN)) {
                std::vector<std::shared_ptr<ASTNode>> args;
                args.reserve(4); // Perf-Finding4: 避免常见 1-3 参方法调用的 1-2 次 realloc
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        args.push_back(expression());
                    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
                }
                consume(TokenType::TK_RPAREN, "期望 ')' 结束方法参数列表");

                expr = std::make_unique<MethodCall>(std::move(expr), StringIntern::intern(fieldName.lexeme),
                                                    std::move(args), dot.line, dot.column);
            } else {
                // 普通成员访问: obj.field
                expr = std::make_unique<MemberAccess>(std::move(expr), StringIntern::intern(fieldName.lexeme), dot.line,
                                                      dot.column);
            }
            continue;
        }

        // 拓展二期·语言：? 传播运算符 expr? —— 脱糖为共享内建
        // __qmark_unwrap(expr)（BuiltinMethods.cpp builtinFunctionRegistry，
        // 三后端自动一致）。Lexer 将 '?' 映射到死槽位 TK_FUNC；表达式上下文
        // 无三元运算符，与 parseTypeAnnotation 的类型上下文消费互不冲突。
        // 语义：Ok(v)/Some(v) → v；Err(e)/None → 运行时错误（可被 try/catch
        // 捕获，异常式传播；Rust 返回式传播需语句级支持，MiniLang 无语句
        // 表达式，选择与 try/catch 体系自洽的抛出语义）。配套 std/result。
        if (match(TokenType::TK_FUNC)) {
            const Token& q = previous();
            std::vector<std::shared_ptr<ASTNode>> qargs;
            qargs.push_back(std::shared_ptr<ASTNode>(std::move(expr)));
            expr = std::make_unique<FunCall>("__qmark_unwrap", std::move(qargs), q.line, q.column);
            continue;
        }

        // 没有更多后缀操作
        break;
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::primary() {
    // 七特性 MVP 阶段 4：await 表达式（仅在 async fun 体内合法）
    // 语法：await <unary>（前缀，结合紧度高于二元运算：await t + 1 = (await t) + 1）
    // 语义：同步 drain——协程驱动到 done 取最终值；非协程值恒等返回。
    if (check(TokenType::TK_AWAIT)) {
        const Token& awaitTok = peek();
        if (!currentFunIsAsync_) {
            throw ParseError("await 只能出现在 async fun 声明的函数体内", awaitTok.line, awaitTok.column);
        }
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", awaitTok.line,
                             awaitTok.column);
        }
        advance(); // 消耗 await
        DepthGuard guard{parseDepth_};
        auto operand = unary();
        return std::make_unique<AwaitExpr>(std::move(operand), awaitTok.line, awaitTok.column);
    }

    // R99 枚举与 ADT: match 表达式
    if (check(TokenType::TK_MATCH)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            const Token& m = peek();
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", m.line, m.column);
        }
        return matchExpr();
    }

    // R164 协程/生成器：yield 表达式
    // 语法：yield expr（挂起并返回值）或 yield（无值，等价于 yield null）
    // 语义：挂起当前生成器，将 value 返回给调用者；下次 .next() 从此处恢复执行。
    // 约束：yield 只能出现在 fun* 声明的函数体内（currentYieldId_ >= 0）。
    //       普通函数/lambda/全局作用域中出现 yield 报错。
    // yieldId 分配：按源代码出现顺序递增（0,1,2,...），供 Interpreter 重放模式使用。
    if (check(TokenType::TK_YIELD)) {
        const Token& yieldTok = peek();
        if (currentYieldId_ < 0) {
            throw ParseError("yield 只能出现在 fun* 声明的生成器函数体内", yieldTok.line, yieldTok.column);
        }
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", yieldTok.line,
                             yieldTok.column);
        }
        advance(); // 消耗 yield 关键字

        // 判断 yield 后是否跟表达式：
        // - 若紧跟语句/参数终止符（; ) } , ] 或 EOF），视为无值 yield（等价于 yield null）
        // - 否则解析后续表达式作为 yield 的值
        std::shared_ptr<ASTNode> value = nullptr;
        bool followedByTerminator = check(TokenType::TK_SEMICOLON) || check(TokenType::TK_RPAREN) ||
                                    check(TokenType::TK_RBRACE) || check(TokenType::TK_COMMA) ||
                                    check(TokenType::TK_RBRACKET) || check(TokenType::TK_EOF);
        if (!followedByTerminator) {
            DepthGuard guard{parseDepth_}; // C4 fix: 递归深度保护
            value = expression();
        }

        // 分配递增 yieldId 并递增计数器（供同函数内后续 yield 使用）
        int assignedId = currentYieldId_++;
        // R164 协程/生成器：若当前处于循环体内，标记生成器为动态 yieldCount 模式。
        // 循环内 yield 的编译期节点数 ≠ 运行时执行数（如 while 内 yield i 执行 N 次），
        // 用 INT_MAX 标记使 done 判定改为函数体自然结束路径。
        if (yieldInLoop_) {
            currentFunHasYieldInLoop_ = true;
        }
        auto yieldNode = std::make_unique<YieldExpr>(std::move(value), yieldTok.line, yieldTok.column);
        yieldNode->yieldId = assignedId;
        return yieldNode;
    }

    // 整数字面量
    if (match(TokenType::TK_INT_LIT)) {
        const Token& tok = previous();
        // A1 fix: Token.literal 是 variant，Parser 直接用标量构造 AST 节点
        return std::make_unique<NumberLiteral>(tok.literalInt(), tok.line, tok.column);
    }

    // 浮点字面量
    if (match(TokenType::TK_FLOAT_LIT)) {
        const Token& tok = previous();
        // A1 fix: Token.literal 是 variant，Parser 直接用标量构造 AST 节点
        return std::make_unique<NumberLiteral>(tok.literalFloat(), tok.line, tok.column);
    }

    // 字符串字面量（含 F7 字符串插值支持）
    if (match(TokenType::TK_STRING_LIT)) {
        const Token& tok = previous();
        auto result = std::make_unique<StringLiteral>(tok.literalString(), tok.line, tok.column);

        // F7: 检查是否为插值字符串（后跟 TK_INTERP_START）
        if (check(TokenType::TK_INTERP_START)) {
            return parseInterpolatedString(std::move(result));
        }
        return result;
    }

    // F7: 插值字符串的后续片段（不应在 primary 顶层出现，由 parseInterpolatedString 内部处理）
    if (match(TokenType::TK_STRING_PART)) {
        const Token& tok = previous();
        throw ParseError("字符串片段出现在非插值上下文", tok.line, tok.column);
    }

    // 布尔字面量
    if (match(TokenType::TK_TRUE)) {
        const Token& tok = previous();
        return std::make_unique<BoolLiteral>(true, tok.line, tok.column);
    }
    if (match(TokenType::TK_FALSE)) {
        const Token& tok = previous();
        return std::make_unique<BoolLiteral>(false, tok.line, tok.column);
    }

    // null 字面量
    if (match(TokenType::TK_NULL)) {
        const Token& tok = previous();
        return std::make_unique<NullLiteral>(tok.line, tok.column);
    }

    // super 关键字
    if (match(TokenType::TK_SUPER)) {
        const Token& tok = previous();
        return std::make_unique<SuperExpr>(tok.line, tok.column);
    }

    // R98 W3: Lambda 表达式 fun(params) { body }
    // 在 primary 中识别 `fun` 后跟 `(`（lambda 表达式）vs `fun` 后跟标识符（函数声明，
    // 仅在 declaration 上下文处理，不会进入 primary）。通过 peekNext 判断：
    //   fun (  → lambda 表达式
    //   fun id → 函数声明（不应进入 primary，但若用户在表达式上下文误用 fun name()，
    //            报错"期望 '('"由 lambdaExpr 触发，提示用户使用声明语法）
    if (check(TokenType::TK_FUN) && checkNext(TokenType::TK_LPAREN)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            const Token& m = peek();
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", m.line, m.column);
        }
        return lambdaExpr();
    }

    // 标识符
    if (match(TokenType::TK_IDENTIFIER)) {
        const Token& tok = previous();
        // 七特性 MVP 阶段 2：宏调用 name!(args)。
        // `IDENT ! (` 三 token 序列在表达式上下文无其他合法解释
        // （`!` 单独出现时为 TK_NOT，而 not 是前缀一元运算符不可中缀），
        // 因此无条件进入宏调用解析，未定义宏报错比回退 VarRef 更友好。
        if (check(TokenType::TK_NOT) && checkNext(TokenType::TK_LPAREN)) {
            if (parseDepth_ >= MAX_PARSE_DEPTH) {
                throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", tok.line,
                                 tok.column);
            }
            DepthGuard guard{parseDepth_};
            return macroCall(tok);
        }
        return std::make_unique<VarRef>(StringIntern::intern(tok.lexeme), tok.line, tok.column);
    }

    // 类型关键字作为标识符使用（如 dict(), array(), int(), string() 等函数调用）
    if (match(TokenType::TK_DICT) || match(TokenType::TK_ARRAY) || match(TokenType::TK_INT) ||
        match(TokenType::TK_FLOAT) || match(TokenType::TK_BOOL) || match(TokenType::TK_STRING_TYPE)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(StringIntern::intern(tok.lexeme), tok.line, tok.column);
    }

    // 数组字面量 [e1, e2, e3]
    if (match(TokenType::TK_LBRACKET)) {
        const Token& bracket = previous();

        std::vector<std::shared_ptr<ASTNode>> elements;
        if (!check(TokenType::TK_RBRACKET)) {
            do {
                elements.push_back(expression());
            } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RBRACKET));
        }
        consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组字面量");

        return std::make_unique<ArrayLiteral>(std::move(elements), bracket.line, bracket.column);
    }

    // 字典字面量 {"key": value, ...}
    if (match(TokenType::TK_LBRACE)) {
        const Token& brace = previous();

        std::vector<std::pair<std::shared_ptr<ASTNode>, std::shared_ptr<ASTNode>>> pairs;

        if (!check(TokenType::TK_RBRACE)) {
            do {
                auto key = expression();
                consume(TokenType::TK_COLON, "期望 ':' 分隔键值对");
                auto val = expression();
                pairs.emplace_back(std::move(key), std::move(val));
            } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RBRACE));
        }
        consume(TokenType::TK_RBRACE, "期望 '}' 结束字典字面量");

        return std::make_unique<DictLiteral>(std::move(pairs), brace.line, brace.column);
    }

    // 分组表达式 / 元组字面量
    // BUG-PARSER-AUDIT-7 fix: 深度检查移到 match() 之前，与 unary() 的
    // AUDIT-BUG-P3 fix 模式对齐。原实现先 match 消耗 '(' 再检查深度，
    // 超限时抛异常 → synchronize() 的初始 advance() 会多消耗一个 token，
    // 导致恢复时多丢失一个 token。
    //
    // R98 元组与解构：'(e1, e2, ...)' 为元组字面量，'(e)' 为分组表达式。
    // 解析策略：消耗 '(' 后解析第一个 expression，若紧跟 ',' 则切换到元组解析模式。
    // 单元素元组需显式尾逗号：'(e,)'。空元组 '()' 由后续特例处理。
    if (check(TokenType::TK_LPAREN)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            const Token& lp = peek();
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）", lp.line, lp.column);
        }
        const Token& lp = peek();
        advance(); // 消耗 '('

        // 空元组 ()
        if (check(TokenType::TK_RPAREN)) {
            advance(); // 消耗 ')'
            return std::make_unique<TupleLiteral>(std::vector<std::shared_ptr<ASTNode>>{}, lp.line, lp.column);
        }

        auto firstExpr = expression();

        // 元组字面量：(e1, e2, ...) 或 (e,) 单元素元组
        if (check(TokenType::TK_COMMA)) {
            std::vector<std::shared_ptr<ASTNode>> elements;
            elements.push_back(std::move(firstExpr));
            while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN)) {
                elements.push_back(expression());
            }
            consume(TokenType::TK_RPAREN, "期望 ')' 结束元组字面量");
            return std::make_unique<TupleLiteral>(std::move(elements), lp.line, lp.column);
        }

        // 分组表达式
        consume(TokenType::TK_RPAREN, "期望 ')' 结束分组表达式");
        return firstExpr;
    }

    // 错误
    const Token& tok = peek();
    throw ParseError("意外的 Token: '" + tok.lexeme + "'", tok.line, tok.column, "unexpected-token");
}

// ---- 错误恢复 ----

void Parser::synchronize() {
    // 不跳过块边界/EOF——advance() 前先检查当前 token 是否已是同步点。
    // 原 advance() 无条件跳过当前 token，若当前 token 是 }，
    // 会被跳过而 previous() 检查仅识别 TK_SEMICOLON，导致块边界丢失产生级联错误。
    //
    // FIX-1: 分号必须消耗（advance），否则主循环再次调用 declaration() 时
    // primary() 不识别 ';' 抛异常 → synchronize() 又看到 ';' 直接 return → 死循环。
    // FIX-2: 插值字符串 token（TK_INTERP_END/TK_STRING_PART/TK_INTERP_START）
    // 只在插值字符串上下文中有意义，顶层 parse 循环中无法处理。
    // 若不消耗而直接 return，primary() 不识别这些 token → 抛异常 → synchronize() 又
    // 看到同样 token → 死循环。必须消耗所有插值相关 token 以跳过断裂的字符串上下文。
    // '}' 是结构边界，不能消耗；EOF 无需消耗（isAtEnd 会终止循环）。
    if (peek().type == TokenType::TK_RBRACE || peek().type == TokenType::TK_EOF) {
        return;
    }
    if (peek().type == TokenType::TK_SEMICOLON) {
        advance(); // 消耗分号以确保向前推进
        return;
    }
    // 消耗所有插值字符串相关 token，跳过断裂的字符串上下文
    while (peek().type == TokenType::TK_INTERP_END || peek().type == TokenType::TK_STRING_PART ||
           peek().type == TokenType::TK_INTERP_START) {
        advance();
    }
    if (isAtEnd())
        return;
    // 如果消耗插值 token 后到达了同步点，停止
    if (peek().type == TokenType::TK_RBRACE || peek().type == TokenType::TK_EOF ||
        peek().type == TokenType::TK_SEMICOLON) {
        if (peek().type == TokenType::TK_SEMICOLON)
            advance();
        return;
    }
    // BUG-PARSER-SYNC-1 fix (P1): 在 advance() 前检查当前 token 是否已是同步关键字。
    // 原实现无条件 advance()，当错误恰好发生在同步关键字位置（如前一条语句缺分号，
    // 下一个 token 是新声明起始关键字 var/fun/class/...）时，该关键字被当作"错误 token"
    // 吞掉，导致下一条声明整体从 AST 中丢失（用户只看到"期望 ';'"，但下一条声明消失）。
    // 修复：同步关键字作为同步点直接 return，不消耗，让主循环正常解析下一条声明。
    // 注意：必须与 SYNC-2/SYNC-3/SYNC-4 一组修复，否则 block() 在 finally/else 处会无限循环。
    switch (peek().type) {
    case TokenType::TK_VAR:
    case TokenType::TK_FUN:
    case TokenType::TK_CLASS:
    case TokenType::TK_IF:
    case TokenType::TK_WHILE:
    case TokenType::TK_FOR:
    case TokenType::TK_RETURN:
    case TokenType::TK_PRINT:
    case TokenType::TK_BREAK:
    case TokenType::TK_CONTINUE:
    case TokenType::TK_ELSE:
    case TokenType::TK_TRY:
    case TokenType::TK_CATCH:
    case TokenType::TK_FINALLY: // BUG-PARSER-SYNC-3 fix (P2): 补充 finally 作为同步点
    case TokenType::TK_THROW:
    case TokenType::TK_IMPORT:
    case TokenType::TK_EXPORT:
    case TokenType::TK_INT:
    case TokenType::TK_FLOAT:
    case TokenType::TK_BOOL:
    case TokenType::TK_STRING_TYPE:
    case TokenType::TK_DICT:
    case TokenType::TK_ARRAY:
    case TokenType::TK_ENUM:  // Bug #39 fix: enum 作为同步点
    case TokenType::TK_MATCH: // Bug #39 fix: match 作为同步点
    case TokenType::TK_MACRO: // 七特性 MVP 阶段 2：macro 作为同步点
    case TokenType::TK_TRAIT: // 七特性 MVP 阶段 3：trait 作为同步点
    case TokenType::TK_ASYNC: // 七特性 MVP 阶段 4：async 作为同步点
    case TokenType::TK_FROM:  // AUDIT-P1-CORRECT fix: from 作为同步点，避免 import 错误恢复时吞掉 from
        return;
    default:
        break;
    }
    advance();

    while (!isAtEnd()) {
        // 分号标记语句结束
        if (previous().type == TokenType::TK_SEMICOLON)
            return;

        // P1-2 fix: '}' 标记块结束，作为同步点避免跳过块边界
        if (peek().type == TokenType::TK_RBRACE)
            return;

        // FIX: 插值字符串 token 不是同步点——它们是断裂字符串上下文的残留，
        // 必须消耗（跳过）才能到达真正的语句边界。
        if (peek().type == TokenType::TK_INTERP_END || peek().type == TokenType::TK_STRING_PART ||
            peek().type == TokenType::TK_INTERP_START) {
            advance();
            continue;
        }

        // 关键字标记声明开始
        switch (peek().type) {
        case TokenType::TK_VAR:
        case TokenType::TK_FUN:
        case TokenType::TK_CLASS:
        case TokenType::TK_IF:
        case TokenType::TK_WHILE:
        case TokenType::TK_FOR:
        case TokenType::TK_RETURN:
        case TokenType::TK_PRINT:
        case TokenType::TK_BREAK:    // break 作为同步点
        case TokenType::TK_CONTINUE: // continue 作为同步点
        case TokenType::TK_ELSE:     // PARSE-11 fix: else 作为同步点
        // AUDIT-BUG-P2/P3/P4 fix: 补充 try/catch/throw/import/export 作为同步点。
        // 原实现缺少这些关键字，导致错误恢复时 panic mode 跳过 catch 子句、
        // try 块、import/export 声明，产生误导性错误链。
        case TokenType::TK_TRY:
        case TokenType::TK_CATCH:
        case TokenType::TK_FINALLY: // BUG-PARSER-SYNC-3 fix: while 循环内同步补充 finally
        case TokenType::TK_THROW:
        case TokenType::TK_IMPORT:
        case TokenType::TK_EXPORT:
        case TokenType::TK_INT:
        case TokenType::TK_FLOAT:
        case TokenType::TK_BOOL:
        case TokenType::TK_STRING_TYPE:
        case TokenType::TK_DICT:
        case TokenType::TK_ARRAY:
        case TokenType::TK_ENUM:  // Bug #39 fix: enum 作为同步点
        case TokenType::TK_MATCH: // Bug #39 fix: match 作为同步点
        case TokenType::TK_MACRO: // 七特性 MVP 阶段 2：macro 作为同步点
        case TokenType::TK_TRAIT: // 七特性 MVP 阶段 3：trait 作为同步点
        case TokenType::TK_ASYNC: // 七特性 MVP 阶段 4：async 作为同步点
        case TokenType::TK_FROM:  // AUDIT-P1-CORRECT fix: from 作为同步点
            return;
        default:
            break;
        }

        advance();
    }
}

// F7: 解析插值字符串
// 语法: "text {expr} more text {expr2} end"
// Lexer 已将其拆分为: TK_STRING_LIT TK_INTERP_START <expr tokens> TK_INTERP_END TK_STRING_PART TK_INTERP_START ...
// TK_STRING_PART C5 fix: 保留插值结构为 InterpolatedString AST 节点（原实现抹平为 BinaryOp(BIN_ADD) 链， Formatter
// 无法重建插值语法，AstViewer 只能看到一堆 BinaryOp）
std::unique_ptr<ASTNode> Parser::parseInterpolatedString(std::unique_ptr<ASTNode> first) {
    int startLine = first ? first->line : 0;
    int startCol = first ? first->column : 0;

    auto interp = std::make_unique<InterpolatedString>(startLine, startCol);

    // 首个字符串片段（来自 first，即 TK_STRING_LIT 的值）
    if (first && first->nodeType == NodeType::NODE_STRING_LITERAL) {
        interp->literals.push_back(static_cast<StringLiteral*>(first.get())->value);
    } else {
        // 理论上 first 始终是 StringLiteral，防御性处理
        interp->literals.push_back(std::string());
    }

    // 循环处理 {expr} text 片段
    while (true) {
        if (!match(TokenType::TK_INTERP_START)) {
            const Token& tok = peek();
            throw ParseError("期望插值起始 '{'", tok.line, tok.column);
        }

        // AUDIT-BUG-P1 fix: 空插值表达式 "{}" 应报错而非静默接受。
        // 原实现用 try/catch 吞掉所有 ParseError 并替换为空 StringLiteral，
        // 导致 "{}" 和 "{bad syntax}" 都被静默接受为空字符串。
        // 现改为：显式检查 TK_INTERP_END（空插值）并报错；其他 ParseError 正常传播。
        if (check(TokenType::TK_INTERP_END)) {
            const Token& tok = peek();
            throw ParseError("插值表达式不能为空", tok.line, tok.column);
        }
        auto expr = expression();

        interp->expressions.push_back(std::move(expr));

        // 消耗 TK_INTERP_END
        if (!match(TokenType::TK_INTERP_END)) {
            const Token& tok = peek();
            throw ParseError("期望插值结束 '}'", tok.line, tok.column);
        }

        // 期望下一个 token 是 TK_STRING_PART（字符串剩余部分）
        if (!match(TokenType::TK_STRING_PART)) {
            const Token& tok = peek();
            throw ParseError("期望字符串片段", tok.line, tok.column);
        }

        const Token& partTok = previous();
        interp->literals.push_back(partTok.literalString()); // A1 fix: variant 访问

        // 检查是否还有更多插值
        if (!check(TokenType::TK_INTERP_START)) {
            break;
        }
    }

    // PERF-11 fix: 预计算 literals 总字符数，供 visitInterpolatedString reserve
    for (const auto& lit : interp->literals) {
        interp->literalsTotalLen += lit.size();
    }

    // AUDIT-P2.9 fix: 记录闭合 '"' 所在行号（最后一个 TK_STRING_PART 的行号），
    // 供 Formatter 在表达式行号范围内注入独立注释。
    interp->endLine = previous().line;

    return interp;
}
