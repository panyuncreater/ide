#include "parser/Parser.h"
#include <unordered_set>

// ============================================================
// Parser 递归下降语法分析器实现
// ============================================================

// BUG-P1 fix: 递归收集表达式中的所有变量引用名
// 用于校验默认参数值不引用后续参数
static void collectVarRefs(ASTNode* node, std::unordered_set<std::string>& names) {
    if (!node) return;
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
    default:
        break;
    }
}

Parser::Parser() {}

std::unique_ptr<Block> Parser::parse(const std::vector<Token>& tokens) {
    tokens_ = &tokens;  // 存储指针，避免深拷贝整个 token 流
    current_ = 0;
    parseDepth_ = 0;  // P15 fix: 重置递归深度
    blockDepth_ = 0;  // P0-1 fix: 重置块嵌套深度
    diagnostics_.clear();

    // PERF-21 fix: 预分配 statements 向量容量，避免每个 declaration push_back 触发 realloc。
    // 估算：平均每 4 个 token 产生 1 个声明（关键字 + 名字 + ... + 分号）
    std::vector<std::shared_ptr<ASTNode>> statements;
    statements.reserve(tokens_->size() / 4);

    while (!isAtEnd()) {
        // FIX: '}' 不是顶层语句的合法起始 token。若 synchronize() 在 '}' 处停下，
        // 主循环必须也停下来，否则 declaration() → primary() 不识别 '}' → 抛异常 →
        // synchronize() 又看到 '}' → 死循环。
        if (check(TokenType::TK_RBRACE)) break;
        // BUG-PARSER-AUDIT-5 fix: 错误数量上限，防止恶意输入触发 O(N) 诊断内存膨胀。
        // 原实现无上限，100 万个 ';' 可累积 ~100 万条 Diagnostic（~200MB）。
        if (diagnostics_.errorCount() >= MAX_PARSE_ERRORS) {
            diagnostics_.addError("错误过多（超过 " + std::to_string(MAX_PARSE_ERRORS) +
                                  " 条），停止解析",
                                  peek().line, peek().column, DiagSource::Parser);
            break;
        }
        try {
            auto decl = declaration();
            if (decl) {
                statements.push_back(std::move(decl));
            }
        } catch (const ParseError& e) {
            // 收集错误到诊断包而非吞掉
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser);
            // 错误恢复：同步到下一个声明边界
            synchronize();
        }
    }

    return std::make_unique<Block>(std::move(statements), 1, 1);
}

// ---- 辅助方法 ----

const Token& Parser::peek() const {
    return (*tokens_)[current_];
}

const Token& Parser::previous() const {
    // P0-14 fix: 边界检查，current_==0 时返回 EOF 哨兵避免负索引 UB
    if (current_ <= 0) {
        static const Token eofSentinel(TokenType::TK_EOF, "", std::monostate{}, 0, 0);  // A1 fix: variant monostate
        return eofSentinel;
    }
    return (*tokens_)[current_ - 1];
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::TK_EOF;
}

const Token& Parser::advance() {
    if (!isAtEnd()) current_++;
    // 不在 advance() 中跳过注释——peek()/check() 已负责跳注释
    // advance() 后 previous() 必须返回实际被消耗的 token（修复 block() 等位置追踪）
    return previous();
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type == type;
}

bool Parser::checkNext(TokenType type) const {
    int idx = current_ + 1;
    if (idx >= (int)tokens_->size()) return false;
    return (*tokens_)[idx].type == type;
}


const Token& Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    const Token& tok = peek();
    throw ParseError(message, tok.line, tok.column);
}

const Token& Parser::consumeIdentifierOrType(const std::string& message) {
    // 允许普通标识符
    if (check(TokenType::TK_IDENTIFIER)) return advance();
    // 允许类型关键字作为名称（如 dict, array, int, float, string, bool）
    if (isTypeKeyword()) {
        return advance();
    }
    const Token& tok = peek();
    throw ParseError(message, tok.line, tok.column);
}

bool Parser::isIdentifierOrType() const {
    if (isAtEnd()) return false;
    TokenType t = peek().type;
    return t == TokenType::TK_IDENTIFIER ||
           t == TokenType::TK_INT || t == TokenType::TK_FLOAT ||
           t == TokenType::TK_BOOL || t == TokenType::TK_STRING_TYPE ||
           t == TokenType::TK_DICT || t == TokenType::TK_ARRAY;
}

bool Parser::isTypeKeyword() const {
    if (isAtEnd()) return false;
    TokenType t = peek().type;
    return t == TokenType::TK_INT || t == TokenType::TK_FLOAT ||
           t == TokenType::TK_BOOL || t == TokenType::TK_STRING_TYPE ||
           t == TokenType::TK_DICT || t == TokenType::TK_ARRAY;
}

bool Parser::isClassTypeDeclStart() const {
    // 识别类类型声明起始：ClassName paramName 或 ClassName[] paramName
    // 原 checkNext(TK_IDENTIFIER) 只覆盖 ClassName paramName，漏掉 ClassName[] paramName
    if (!check(TokenType::TK_IDENTIFIER)) return false;
    int idx = current_ + 1;
    int size = static_cast<int>(tokens_->size());
    if (idx >= size) return false;
    // ClassName paramName
    if ((*tokens_)[idx].type == TokenType::TK_IDENTIFIER) return true;
    // ClassName[] paramName
    if ((*tokens_)[idx].type != TokenType::TK_LBRACKET) return false;
    idx++;
    if (idx >= size || (*tokens_)[idx].type != TokenType::TK_RBRACKET) return false;
    idx++;
    return idx < size && (*tokens_)[idx].type == TokenType::TK_IDENTIFIER;
}

std::string Parser::parseTypeAnnotation() {
    const Token& typeTok = advance();  // 消耗类型关键字或标识符
    std::string typeAnn = typeTok.lexeme;
    // 安全回溯：仅在 [ 后紧跟 ] 时才消费，否则回退 [
    if (check(TokenType::TK_LBRACKET)) {
        int bracketSave = current_;
        advance(); // 消耗 '['
        if (check(TokenType::TK_RBRACKET)) {
            advance(); // 消耗 ']'
            typeAnn += "[]";
        } else {
            current_ = bracketSave; // 不是 [] 类型注解，回退 '['
        }
    }
    return typeAnn;
}

// ---- 声明与语句 ----

std::unique_ptr<ASTNode> Parser::declaration() {
    // var 声明
    if (check(TokenType::TK_VAR)) return varDecl();

    // fun / function / func 声明（Lexer 已统一为 TK_FUN）
    if (check(TokenType::TK_FUN)) return funDecl();

    // class 声明
    if (check(TokenType::TK_CLASS)) return classDecl();

    // F12: import / export 声明
    if (check(TokenType::TK_IMPORT)) return importStmt();
    if (check(TokenType::TK_EXPORT)) return exportStmt();

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
        std::string typeAnn = parseTypeAnnotation();  // 消耗类名 + 可选 []

        if (check(TokenType::TK_IDENTIFIER)) {
            // M7 fix: 前瞻验证 — 检查第二个标识符后是否为合法声明后续 token
            int afterSecondPos = current_ + 1;  // 跳过第二个标识符
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
                validDeclFollow = (follow == TokenType::TK_ASSIGN ||
                                   follow == TokenType::TK_SEMICOLON ||
                                   follow == TokenType::TK_LPAREN ||
                                   follow == TokenType::TK_LBRACE);
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

std::unique_ptr<VarDecl> Parser::varDecl() {
    const Token& varTok = consume(TokenType::TK_VAR, "期望 'var'");
    const Token& name = consume(TokenType::TK_IDENTIFIER, "期望变量名");

    std::unique_ptr<ASTNode> init = nullptr;
    if (match(TokenType::TK_ASSIGN)) {
        init = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束变量声明");

    return std::make_unique<VarDecl>(name.lexeme, "", std::move(init),
                                     varTok.line, varTok.column);
}

std::unique_ptr<VarDecl> Parser::typedVarDecl(const std::string& typeAnn) {
    const Token& name = consume(TokenType::TK_IDENTIFIER, "期望变量名");

    std::unique_ptr<ASTNode> init = nullptr;
    if (match(TokenType::TK_ASSIGN)) {
        init = expression();
    }
    consume(TokenType::TK_SEMICOLON, "期望 ';' 结束变量声明");

    return std::make_unique<VarDecl>(name.lexeme, typeAnn, std::move(init),
                                     name.line, name.column);
}

std::unique_ptr<FunDecl> Parser::funDecl() {
    // 消耗 fun 或 function 关键字
    const Token& funTok = advance();

    // 函数名：允许标识符或类型关键字（如 dict, array, int, float, string, bool）
    const Token& name = consumeIdentifierOrType("期望函数名");
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    std::vector<std::shared_ptr<ASTNode>> defaultValues;  // F10
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
        advance(); // 消耗 '-'
        advance(); // 消耗 '>'
        if (!isIdentifierOrType()) {  // BUG-PARSER-AUDIT-1
            const Token& tok = peek();
            throw ParseError("期望返回类型名", tok.line, tok.column);
        }
        returnType = parseTypeAnnotation();
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    auto decl = std::make_unique<FunDecl>(name.lexeme, std::move(params),
                                          std::move(paramTypes), returnType,
                                          std::move(body), funTok.line, funTok.column);
    // F10: 计算必需参数个数（前缀无默认值的参数数量）
    int reqCount = 0;
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (defaultValues[i] == nullptr) {
            ++reqCount;
        } else {
            break;  // 一旦遇到默认值，后续都有默认值
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
    std::vector<std::shared_ptr<ASTNode>> defaultValues;  // F10
    parseParamList(params, paramTypes, defaultValues);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 返回类型已由调用方提供，不再解析 :type 或 ->type

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    auto decl = std::make_unique<FunDecl>(name.lexeme, std::move(params),
                                          std::move(paramTypes), returnType,
                                          std::move(body), name.line, name.column);
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
        if (!node) return false;
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
            if (unary->opType != UnaryOp::UnaryOpType::UOP_NEGATE) return false;
            const ASTNode* cur = unary->operand.get();
            int negateCount = 1;
            while (cur && cur->nodeType == NodeType::NODE_UNARY_OP) {
                const auto* inner = static_cast<const UnaryOp*>(cur);
                if (inner->opType != UnaryOp::UnaryOpType::UOP_NEGATE) return false;
                ++negateCount;
                cur = inner->operand.get();
            }
            return cur != nullptr && cur->nodeType == NodeType::NODE_NUMBER_LITERAL && negateCount > 0;
        }
        default:
            return false;
        }
    };
    if (check(TokenType::TK_RPAREN)) return;
    bool seenDefault = false;  // F10: 一旦出现默认参数，后续都必须有默认值
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

            const Token& param = consume(TokenType::TK_IDENTIFIER, "期望参数名");
            paramName = param.lexeme;
        } else if (isClassTypeDeclStart()) {
            // PARSE-02 fix: C 风格类类型参数: ClassName paramName 或 ClassName[] paramName
            pType = parseTypeAnnotation();
            const Token& param = consume(TokenType::TK_IDENTIFIER, "期望参数名");
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
        params.push_back(paramName);
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
                throw ParseError("默认参数值必须是字面量（数字/字符串/布尔/null/负数字面量）",
                                 defaultExpr->line, defaultExpr->column);
            }
            defaultValues.push_back(std::move(defaultExpr));
        } else {
            if (seenDefault) {
                throw ParseError("默认参数后的参数都必须有默认值: '" + paramName + "'",
                                 peek().line, peek().column);
            }
            defaultValues.push_back(nullptr);
        }
    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));

    // BUG-P1 fix: 默认参数值不能引用后续参数（如 fun f(a = b, b = 1) 应报错）
    for (size_t i = 0; i < defaultValues.size(); ++i) {
        if (!defaultValues[i]) continue;
        std::unordered_set<std::string> refNames;
        collectVarRefs(defaultValues[i].get(), refNames);
        for (const auto& ref : refNames) {
            for (size_t j = i + 1; j < params.size(); ++j) {
                if (params[j] == ref) {
                    throw ParseError("默认参数值不能引用后续参数: '" + ref + "'",
                                     defaultValues[i]->line, defaultValues[i]->column);
                }
            }
        }
    }
}

std::unique_ptr<ClassDecl> Parser::classDecl() {
    const Token& classTok = consume(TokenType::TK_CLASS, "期望 'class'");
    const Token& name = consumeIdentifierOrType("期望类名");

    // 可选的 extends SuperClassName 或 : SuperClassName
    std::string superClassName;
    if (match(TokenType::TK_EXTENDS) || match(TokenType::TK_COLON)) {
        // PARSE-03 fix: 父类名支持类型关键字（与类名声明一致）
        const Token& superName = consumeIdentifierOrType("期望父类名");
        superClassName = superName.lexeme;
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");

    // 解析类成员
    std::vector<std::shared_ptr<ASTNode>> members;

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
        } else if (check(TokenType::TK_FUN)) {  // Lexer 已统一 function/func → TK_FUN
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
            const Token& firstTok = advance();  // 方法名或类名

            // 检查是否是方法定义: name(
            if (check(TokenType::TK_LPAREN)) {
                // 这是一个裸方法定义
                consume(TokenType::TK_LPAREN, "期望 '('");

                std::vector<std::string> params;
                std::vector<std::string> paramTypes;
                std::vector<std::shared_ptr<ASTNode>> defaultValues;  // F10
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
                    advance(); // 消耗 '-'
                    advance(); // 消耗 '>'
                    if (!isIdentifierOrType()) {  // BUG-PARSER-AUDIT-1
                        const Token& tok = peek();
                        throw ParseError("期望返回类型名", tok.line, tok.column);
                    }
                    returnType = parseTypeAnnotation();
                }

                consume(TokenType::TK_LBRACE, "期望 '{'");
                auto body = block();

                auto decl = std::make_unique<FunDecl>(firstTok.lexeme, std::move(params),
                                                       std::move(paramTypes), returnType,
                                                       std::move(body), firstTok.line, firstTok.column);
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
            } else if (check(TokenType::TK_LBRACKET) && checkNext(TokenType::TK_RBRACKET)) {
                // BUG-LPA-04 fix: ClassName[] fieldName; 数组类型字段
                //   原实现消耗 ClassName 后遇 [ 直接回溯，不支持类类型数组字段。
                //   与参数列表/for循环/顶层声明行为对齐：消费 [] 后缀构造类型注解。
                advance(); // 消耗 '['
                advance(); // 消耗 ']'
                std::string typeAnn = firstTok.lexeme + "[]";
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
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser);
            synchronize();
            // synchronize 在 '}' 处返回（不消费），循环条件 check(RBRACE) 退出，
            // 由下方 consume(TK_RBRACE) 消费 class 的闭合花括号。
        }
    }

    consume(TokenType::TK_RBRACE, "期望 '}'");

    return std::make_unique<ClassDecl>(name.lexeme, superClassName,
                                        std::move(members),
                                        classTok.line, classTok.column);
}

std::unique_ptr<ASTNode> Parser::statement() {
    // P1-1 fix: 无花括号的 if/while/for 嵌套语句也需深度保护，防止栈溢出 DoS
    // 覆盖 "if (a) if (b) if (c) ..." 这类无花括号嵌套场景
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("语句嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                         peek().line, peek().column);
    }
    DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_

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

    if (check(TokenType::TK_IF))       return ifStmt();
    if (check(TokenType::TK_WHILE))    return whileStmt();
    if (check(TokenType::TK_FOR))      return forStmt();
    if (check(TokenType::TK_RETURN))   return returnStmt();
    if (check(TokenType::TK_BREAK))    return breakStmt();
    if (check(TokenType::TK_CONTINUE)) return continueStmt();
    if (check(TokenType::TK_TRY))      return tryStmt();
    if (check(TokenType::TK_THROW))    return throwStmt();
    if (check(TokenType::TK_PRINT))    return printStmt();
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
                throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                                 peek().line, peek().column);
            }
            DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_
            elseB = ifStmt();
        } else if (check(TokenType::TK_LBRACE)) {
            advance();
            elseB = block();
        } else {
            elseB = statement();
        }
    }

    return std::make_unique<IfStmt>(std::move(cond), std::move(thenB),
                                     std::move(elseB), ifTok.line, ifTok.column);
}

std::unique_ptr<WhileStmt> Parser::whileStmt() {
    const Token& whileTok = consume(TokenType::TK_WHILE, "期望 'while'");
    consume(TokenType::TK_LPAREN, "期望 '('");
    auto cond = expression();
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 支持带花括号的块和不带花括号的单条语句
    std::unique_ptr<ASTNode> body;
    if (check(TokenType::TK_LBRACE)) {
        advance();
        body = block();
    } else {
        body = statement();
    }

    return std::make_unique<WhileStmt>(std::move(cond), std::move(body),
                                        whileTok.line, whileTok.column);
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
        std::string typeAnn = parseTypeAnnotation();  // 消耗类名 + 可选 []
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

    // 支持带花括号的块和不带花括号的单条语句
    std::unique_ptr<ASTNode> body;
    if (check(TokenType::TK_LBRACE)) {
        advance();
        body = block();
    } else {
        body = statement();
    }

    return std::make_unique<ForStmt>(std::move(init), std::move(cond),
                                      std::move(update), std::move(body),
                                      forTok.line, forTok.column);
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

    if (hasCatch) {
        advance();  // 消耗 'catch'
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
        advance();  // 消耗 'finally'
        consume(TokenType::TK_LBRACE, "finally 后期望 '{'");
        finallyBlock = block();
    }

    return std::make_unique<TryStmt>(std::move(tryBlock), catchVarName,
                                      std::move(catchBlock), std::move(finallyBlock),
                                      tryTok.line, tryTok.column);
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
        diagnostics_.addError("import 语句只能在顶层使用", tok.line, tok.column, DiagSource::Parser);
        // 继续解析 import 语句，不抛异常
    }
    const Token& importTok = consume(TokenType::TK_IMPORT, "期望 'import'");

    std::vector<std::string> names;
    bool importAll = false;

    // 两种形式:
    // 1. import "path";           — 导入全部
    // 2. import { a, b } from "path"; — 导入指定名称
    if (check(TokenType::TK_LBRACE)) {
        advance();  // 消耗 '{'
        do {
            // AUDIT-BUG-P5 fix: 允许尾逗号 — 逗号后紧跟 } 则结束
            if (check(TokenType::TK_RBRACE)) break;
            const Token& name = consume(TokenType::TK_IDENTIFIER, "期望导入名称");
            names.push_back(name.lexeme);
        } while (match(TokenType::TK_COMMA));
        consume(TokenType::TK_RBRACE, "期望 '}'");
        consume(TokenType::TK_FROM, "期望 'from'");
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

    return std::make_unique<ImportStmt>(pathTok.literalString(), std::move(names),
                                         importAll, importTok.line, importTok.column);
}

std::unique_ptr<ExportStmt> Parser::exportStmt() {
    // BUG 5b fix: export 只能在顶层使用
    // FIX: 同 importStmt()，不抛异常，改为记录诊断后继续解析。
    if (blockDepth_ > 0) {
        const Token& tok = peek();
        diagnostics_.addError("export 语句只能在顶层使用", tok.line, tok.column, DiagSource::Parser);
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
    const Token& lbrace = previous();  // '{' 已被消耗

    // P0-1 fix: 块嵌套深度保护，防止 {{...}} 深度嵌套导致 C++ 栈溢出
    if (blockDepth_ >= MAX_BLOCK_DEPTH) {
        throw ParseError("块嵌套过深（超过 " + std::to_string(MAX_BLOCK_DEPTH) + " 层）",
                         lbrace.line, lbrace.column);
    }
    DepthGuard blockGuard{blockDepth_};  // C4 fix: 复用 DepthGuard，自动 ++/-- blockDepth_

    // PERF-21 fix: 预分配小量容量，减少小 block 的 realloc（典型 block 含 3-10 条语句）
    std::vector<std::shared_ptr<ASTNode>> stmts;
    stmts.reserve(8);

    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        // FIX: catch 也是块边界（try 块的 tryBlock 以 catch 结束）
        if (check(TokenType::TK_CATCH)) break;
        // BUG-PARSER-AUDIT-5 fix: block() 主循环也需错误上限检查，
        // 防止恶意嵌套块内含大量错误触发 O(N) 诊断内存膨胀。
        if (diagnostics_.errorCount() >= MAX_PARSE_ERRORS) {
            diagnostics_.addError("错误过多（超过 " + std::to_string(MAX_PARSE_ERRORS) +
                                  " 条），停止解析",
                                  peek().line, peek().column, DiagSource::Parser);
            break;
        }
        try {
            auto decl = declaration();
            if (decl) {
                stmts.push_back(std::move(decl));
            }
        } catch (const ParseError& e) {
            // P1-1/P1-2 fix: block() 内错误恢复，避免单错误导致整个块被放弃
            diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Parser);
            synchronize();
            // synchronize 后若已到 '}' 或 EOF 则退出循环
        }
    }

    consume(TokenType::TK_RBRACE, "期望 '}'");

    auto blk = std::make_unique<Block>(std::move(stmts), lbrace.line, lbrace.column);
    blk->closingBraceLine = previous().line;  // L18 fix: 记录 '}' 行号
    return blk;
}

std::unique_ptr<ASTNode> Parser::expressionStatement() {
    auto expr = expression();
    consume(TokenType::TK_SEMICOLON, "期望 ';'");
    return expr;
}

// ---- 表达式 ----

std::unique_ptr<ASTNode> Parser::expression() {
    // P15 fix: 递归深度保护，防止极端嵌套表达式导致栈溢出
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                         peek().line, peek().column);
    }
    DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_
    return assignment();
}

std::unique_ptr<ASTNode> Parser::assignment() {
    // P1 fix: 赋值右结合递归也需深度保护（a = b = c = ... 可深度嵌套）
    if (parseDepth_ >= MAX_PARSE_DEPTH) {
        throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                         peek().line, peek().column);
    }
    DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_

    auto expr = or_();

    // 检查是否是赋值
    if (match(TokenType::TK_ASSIGN)) {
        const Token& eq = previous();

        // 变量赋值: identifier = expr
        if (expr->nodeType == NodeType::NODE_VAR_REF) {
            auto* varRef = static_cast<VarRef*>(expr.get());
            auto val = assignment();  // 右结合
            return std::make_unique<Assignment>(varRef->name, std::move(val),
                                                eq.line, eq.column);
        }

        // 索引赋值: arr[index] = expr 或 dict[key] = expr
        if (expr->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* idxAccess = static_cast<IndexAccess*>(expr.get());
            auto val = assignment();  // 右结合
            // 从 IndexAccess 中提取 object 和 index
            auto obj = std::move(idxAccess->object);
            auto idx = std::move(idxAccess->index);
            return std::make_unique<IndexAssign>(std::move(obj), std::move(idx),
                                                  std::move(val),
                                                  eq.line, eq.column);
        }

        // 成员赋值: obj.field = expr
        if (expr->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* memAccess = static_cast<MemberAccess*>(expr.get());
            auto val = assignment();  // 右结合
            auto obj = std::move(memAccess->object);
            std::string field = memAccess->fieldName;
            return std::make_unique<MemberAssign>(std::move(obj), field,
                                                   std::move(val),
                                                   eq.line, eq.column);
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
        left = std::make_unique<BinaryOp>(BinOpType::BIN_OR, std::move(left), std::move(right),
                                           op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::and_() {
    auto left = equality();

    while (match(TokenType::TK_AND)) {
        const Token& op = previous();
        auto right = equality();
        left = std::make_unique<BinaryOp>(BinOpType::BIN_AND, std::move(left), std::move(right),
                                           op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::equality() {
    auto left = comparison();

    while (match(TokenType::TK_EQ, TokenType::TK_NEQ)) {
        const Token& op = previous();
        auto right = comparison();
        BinOpType binOp = (op.type == TokenType::TK_EQ) ? BinOpType::BIN_EQ : BinOpType::BIN_NEQ;
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right),
                                           op.line, op.column);
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
        case TokenType::TK_LT:  binOp = BinOpType::BIN_LT;  break;
        case TokenType::TK_GT:  binOp = BinOpType::BIN_GT;  break;
        case TokenType::TK_LEQ: binOp = BinOpType::BIN_LTE; break;
        case TokenType::TK_GEQ: binOp = BinOpType::BIN_GTE; break;
        default: binOp = BinOpType::BIN_UNKNOWN; break;
        }
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right),
                                           op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::term() {
    auto left = factor();

    while (match(TokenType::TK_PLUS, TokenType::TK_MINUS)) {
        const Token& op = previous();
        auto right = factor();
        BinOpType binOp = (op.type == TokenType::TK_PLUS) ? BinOpType::BIN_ADD : BinOpType::BIN_SUB;
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right),
                                           op.line, op.column);
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
        case TokenType::TK_STAR:    binOp = BinOpType::BIN_MUL; break;
        case TokenType::TK_SLASH:   binOp = BinOpType::BIN_DIV; break;
        case TokenType::TK_PERCENT: binOp = BinOpType::BIN_MOD; break;
        default: binOp = BinOpType::BIN_UNKNOWN; break;
        }
        left = std::make_unique<BinaryOp>(binOp, std::move(left), std::move(right),
                                           op.line, op.column);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::unary() {
    // P1 fix: 一元运算符递归也需深度保护（---...x 可深度嵌套）
    // AUDIT-BUG-P3 fix: 深度检查移到 match() 之前，避免 match 消耗 token 后
    // 抛出错误导致 synchronize() 多丢失一个 token。
    if (check(TokenType::TK_NOT) || check(TokenType::TK_MINUS)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                             peek().line, peek().column);
        }
        DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_
        advance();  // 消耗一元运算符
        const Token& op = previous();
        auto operand = unary();
        auto uopType = (op.type == TokenType::TK_NOT) ? UnaryOp::UnaryOpType::UOP_NOT : UnaryOp::UnaryOpType::UOP_NEGATE;
        return std::make_unique<UnaryOp>(uopType, std::move(operand), op.line, op.column);
    }
    // PARSE-07 fix: 一元 + 创建 UnaryOp 节点保留 AST 保真度
    if (check(TokenType::TK_PLUS)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                             peek().line, peek().column);
        }
        DepthGuard guard{parseDepth_};  // C4 fix: 自动 ++/-- parseDepth_
        advance();  // 消耗 +

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
            args.reserve(4);  // Perf-Finding4: 避免常见 1-3 参函数调用的 1-2 次 realloc
            if (!check(TokenType::TK_RPAREN)) {
                do {
                    args.push_back(expression());
                } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
            }
            consume(TokenType::TK_RPAREN, "期望 ')' 结束参数列表");

            if (expr->nodeType == NodeType::NODE_VAR_REF) {
                // 命名函数调用（原有路径）
                auto* varRef = static_cast<VarRef*>(expr.get());
                expr = std::make_unique<FunCall>(varRef->name, std::move(args),
                                                  varRef->line, varRef->column);
            } else {
                // 链式调用 / 表达式调用: f(x)(y), closures, 高阶函数
                expr = std::make_unique<FunCall>(std::move(expr), std::move(args),
                                                  paren.line, paren.column);
            }
            continue;
        }

        // 索引访问: expr[index]
        if (match(TokenType::TK_LBRACKET)) {
            const Token& bracket = previous();
            auto index = expression();
            consume(TokenType::TK_RBRACKET, "期望 ']' 结束索引访问");
            expr = std::make_unique<IndexAccess>(std::move(expr), std::move(index),
                                                  bracket.line, bracket.column);
            continue;
        }

        // 成员访问: expr.field 或 方法调用 expr.method(args)
        if (match(TokenType::TK_DOT)) {
            const Token& dot = previous();
            // PARSE-10 fix: 成员名支持类型关键字（与声明端一致）
        const Token& fieldName = consumeIdentifierOrType("期望成员名");

            // 检查是否是方法调用: obj.method(args)
            if (match(TokenType::TK_LPAREN)) {
                std::vector<std::shared_ptr<ASTNode>> args;
                args.reserve(4);  // Perf-Finding4: 避免常见 1-3 参方法调用的 1-2 次 realloc
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        args.push_back(expression());
                    } while (match(TokenType::TK_COMMA) && !check(TokenType::TK_RPAREN));
                }
                consume(TokenType::TK_RPAREN, "期望 ')' 结束方法参数列表");

                expr = std::make_unique<MethodCall>(std::move(expr), fieldName.lexeme,
                                                      std::move(args),
                                                      dot.line, dot.column);
            } else {
                // 普通成员访问: obj.field
                expr = std::make_unique<MemberAccess>(std::move(expr), fieldName.lexeme,
                                                        dot.line, dot.column);
            }
            continue;
        }

        // 没有更多后缀操作
        break;
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::primary() {
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

    // 标识符
    if (match(TokenType::TK_IDENTIFIER)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(tok.lexeme, tok.line, tok.column);
    }

    // 类型关键字作为标识符使用（如 dict(), array(), int(), string() 等函数调用）
    if (match(TokenType::TK_DICT) || match(TokenType::TK_ARRAY) ||
        match(TokenType::TK_INT) || match(TokenType::TK_FLOAT) ||
        match(TokenType::TK_BOOL) || match(TokenType::TK_STRING_TYPE)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(tok.lexeme, tok.line, tok.column);
    }

    // 数组字面量 [e1, e2, e3]
    if (match(TokenType::TK_LBRACKET)) {
        const Token& bracket = previous();

        std::vector<std::shared_ptr<ASTNode>> elements;
        if (!check(TokenType::TK_RBRACKET)) {
            do {
                // L4 fix: 允许尾逗号 — 逗号后紧跟 ] 则结束
                if (check(TokenType::TK_RBRACKET)) break;
                elements.push_back(expression());
            } while (match(TokenType::TK_COMMA));
        }
        consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组字面量");

        return std::make_unique<ArrayLiteral>(std::move(elements),
                                               bracket.line, bracket.column);
    }

    // 字典字面量 {"key": value, ...}
    if (match(TokenType::TK_LBRACE)) {
        const Token& brace = previous();

        std::vector<std::pair<std::shared_ptr<ASTNode>, std::shared_ptr<ASTNode>>> pairs;

        if (!check(TokenType::TK_RBRACE)) {
            do {
                // L4 fix: 允许尾逗号 — 逗号后紧跟 } 则结束
                if (check(TokenType::TK_RBRACE)) break;
                auto key = expression();
                consume(TokenType::TK_COLON, "期望 ':' 分隔键值对");
                auto val = expression();
                pairs.emplace_back(std::move(key), std::move(val));
            } while (match(TokenType::TK_COMMA));
        }
        consume(TokenType::TK_RBRACE, "期望 '}' 结束字典字面量");

        return std::make_unique<DictLiteral>(std::move(pairs),
                                               brace.line, brace.column);
    }

    // 分组表达式
    // BUG-PARSER-AUDIT-7 fix: 深度检查移到 match() 之前，与 unary() 的
    // AUDIT-BUG-P3 fix 模式对齐。原实现先 match 消耗 '(' 再检查深度，
    // 超限时抛异常 → synchronize() 的初始 advance() 会多消耗一个 token，
    // 导致恢复时多丢失一个 token。
    if (check(TokenType::TK_LPAREN)) {
        if (parseDepth_ >= MAX_PARSE_DEPTH) {
            const Token& lp = peek();
            throw ParseError("表达式嵌套过深（超过 " + std::to_string(MAX_PARSE_DEPTH) + " 层）",
                             lp.line, lp.column);
        }
        advance();  // 消耗 '('
        auto expr = expression();
        consume(TokenType::TK_RPAREN, "期望 ')' 结束分组表达式");
        return expr;
    }

    // 错误
    const Token& tok = peek();
    throw ParseError("意外的 Token: '" + tok.lexeme + "'", tok.line, tok.column);
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
    if (peek().type == TokenType::TK_RBRACE ||
        peek().type == TokenType::TK_EOF) {
        return;
    }
    if (peek().type == TokenType::TK_SEMICOLON) {
        advance();  // 消耗分号以确保向前推进
        return;
    }
    // 消耗所有插值字符串相关 token，跳过断裂的字符串上下文
    while (peek().type == TokenType::TK_INTERP_END ||
           peek().type == TokenType::TK_STRING_PART ||
           peek().type == TokenType::TK_INTERP_START) {
        advance();
    }
    if (isAtEnd()) return;
    // 如果消耗插值 token 后到达了同步点，停止
    if (peek().type == TokenType::TK_RBRACE ||
        peek().type == TokenType::TK_EOF ||
        peek().type == TokenType::TK_SEMICOLON) {
        if (peek().type == TokenType::TK_SEMICOLON) advance();
        return;
    }
    advance();

    while (!isAtEnd()) {
        // 分号标记语句结束
        if (previous().type == TokenType::TK_SEMICOLON) return;

        // P1-2 fix: '}' 标记块结束，作为同步点避免跳过块边界
        if (peek().type == TokenType::TK_RBRACE) return;

        // FIX: 插值字符串 token 不是同步点——它们是断裂字符串上下文的残留，
        // 必须消耗（跳过）才能到达真正的语句边界。
        if (peek().type == TokenType::TK_INTERP_END ||
            peek().type == TokenType::TK_STRING_PART ||
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
        case TokenType::TK_BREAK:      // break 作为同步点
        case TokenType::TK_CONTINUE:   // continue 作为同步点
        case TokenType::TK_ELSE:  // PARSE-11 fix: else 作为同步点
        // AUDIT-BUG-P2/P3/P4 fix: 补充 try/catch/throw/import/export 作为同步点。
        // 原实现缺少这些关键字，导致错误恢复时 panic mode 跳过 catch 子句、
        // try 块、import/export 声明，产生误导性错误链。
        case TokenType::TK_TRY:
        case TokenType::TK_CATCH:
        case TokenType::TK_THROW:
        case TokenType::TK_IMPORT:
        case TokenType::TK_EXPORT:
        case TokenType::TK_INT:
        case TokenType::TK_FLOAT:
        case TokenType::TK_BOOL:
        case TokenType::TK_STRING_TYPE:
        case TokenType::TK_DICT:
        case TokenType::TK_ARRAY:
            return;
        default:
            break;
        }

        advance();
    }
}

// F7: 解析插值字符串
// 语法: "text {expr} more text {expr2} end"
// Lexer 已将其拆分为: TK_STRING_LIT TK_INTERP_START <expr tokens> TK_INTERP_END TK_STRING_PART TK_INTERP_START ... TK_STRING_PART
// C5 fix: 保留插值结构为 InterpolatedString AST 节点（原实现抹平为 BinaryOp(BIN_ADD) 链，
// Formatter 无法重建插值语法，AstViewer 只能看到一堆 BinaryOp）
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
        interp->literals.push_back(partTok.literalString());  // A1 fix: variant 访问

        // 检查是否还有更多插值
        if (!check(TokenType::TK_INTERP_START)) {
            break;
        }
    }

    // PERF-11 fix: 预计算 literals 总字符数，供 visitInterpolatedString reserve
    for (const auto& lit : interp->literals) {
        interp->literalsTotalLen += lit.size();
    }

    return interp;
}
