#include "parser/Parser.h"
#include <algorithm>

// ============================================================
// Parser 递归下降语法分析器实现
// ============================================================

Parser::Parser() {}

std::unique_ptr<Block> Parser::parse(const std::vector<Token>& tokens) {
    tokens_ = &tokens;  // 存储指针，避免深拷贝整个 token 流
    current_ = 0;
    errors_.clear();
    diagnostics_.clear();

    std::vector<std::unique_ptr<ASTNode>> statements;

    while (!isAtEnd()) {
        try {
            auto decl = declaration();
            if (decl) {
                statements.push_back(std::move(decl));
            }
        } catch (const ParseError& e) {
            // 收集错误而非吞掉
            errors_.push_back(e);
            // 错误恢复：同步到下一个声明边界
            synchronize();
        }
    }

    // 将 ParseError 转换为 Diagnostic
    for (const auto& err : errors_) {
        diagnostics_.addError(err.what(), err.line, err.column, DiagSource::Parser);
    }

    return std::make_unique<Block>(std::move(statements), 1, 1);
}

// ---- 辅助方法 ----

const Token& Parser::peek() const {
    return (*tokens_)[current_];
}

const Token& Parser::previous() const {
    return (*tokens_)[current_ - 1];
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::TK_EOF;
}

const Token& Parser::advance() {
    if (!isAtEnd()) current_++;
    return previous();
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type == type;
}

bool Parser::checkNext(TokenType type) const {
    if (current_ + 1 >= (int)tokens_->size()) return false;
    return (*tokens_)[current_ + 1].type == type;
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
    if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
        check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
        check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
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

// ---- 声明与语句 ----

std::unique_ptr<ASTNode> Parser::declaration() {
    // var 声明
    if (check(TokenType::TK_VAR)) return varDecl();

    // fun / function / func 声明
    if (check(TokenType::TK_FUN) || check(TokenType::TK_FUNCTION) || check(TokenType::TK_FUNC)) return funDecl();

    // class 声明
    if (check(TokenType::TK_CLASS)) return classDecl();

    // 类型注解声明: int/float/bool/string/dict/array
    if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
        check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
        check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
        // 可能是: 类型注解变量声明(int a=1;) 或 带返回类型的函数声明(int fib(n){})
        // 保存当前位置以便回溯
        int savePos = current_;
        const Token& typeTok = advance();

        // 检查是否是数组类型注解，如 int[]
        std::string typeAnn = typeTok.lexeme;
        if (match(TokenType::TK_LBRACKET)) {
            consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
            typeAnn += "[]";
        }

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
        const Token& firstTok = advance();  // 类名

        if (check(TokenType::TK_IDENTIFIER)) {
            // 检查是否是带类类型的函数声明: ClassName funcName(
            if (checkNext(TokenType::TK_LPAREN)) {
                return typedFunDecl(firstTok.lexeme);
            }
            // ClassName varName — 类类型注解变量声明
            std::string typeAnn = firstTok.lexeme;
            return typedVarDecl(typeAnn);
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
    parseParamList(params, paramTypes);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 可选的返回值类型注解 : type 或 -> type（type 可能是关键字如 int/float）
    std::string returnType;
    if (match(TokenType::TK_COLON)) {
        if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
            check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
            check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
            const Token& typeTok = advance();
            returnType = typeTok.lexeme;
            if (match(TokenType::TK_LBRACKET)) {
                consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
                returnType += "[]";
            }
        } else {
            const Token& retTypeTok = consume(TokenType::TK_IDENTIFIER, "期望返回类型名");
            returnType = retTypeTok.lexeme;
        }
    } else if (check(TokenType::TK_MINUS) && checkNext(TokenType::TK_GT)) {
        // -> type 箭头返回类型语法
        advance(); // 消耗 '-'
        advance(); // 消耗 '>'
        if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
            check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
            check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
            const Token& typeTok = advance();
            returnType = typeTok.lexeme;
            if (match(TokenType::TK_LBRACKET)) {
                consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
                returnType += "[]";
            }
        } else {
            const Token& retTypeTok = consume(TokenType::TK_IDENTIFIER, "期望返回类型名");
            returnType = retTypeTok.lexeme;
        }
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    return std::make_unique<FunDecl>(name.lexeme, std::move(params),
                                     std::move(paramTypes), returnType,
                                     std::move(body), funTok.line, funTok.column);
}

std::unique_ptr<FunDecl> Parser::typedFunDecl(const std::string& returnType) {
    // 带返回类型的函数声明: int fib(int n) { ... }
    // 返回类型已由调用方提供，当前 token 是函数名
    const Token& name = consumeIdentifierOrType("期望函数名");
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    parseParamList(params, paramTypes);
    consume(TokenType::TK_RPAREN, "期望 ')'");

    // 返回类型已由调用方提供，不再解析 :type 或 ->type

    consume(TokenType::TK_LBRACE, "期望 '{'");
    auto body = block();

    return std::make_unique<FunDecl>(name.lexeme, std::move(params),
                                     std::move(paramTypes), returnType,
                                     std::move(body), name.line, name.column);
}

void Parser::parseParamList(std::vector<std::string>& params, std::vector<std::string>& paramTypes) {
    if (check(TokenType::TK_RPAREN)) return;
    do {
        std::string pType;
        std::string paramName;

        // 支持 C 风格类型注解: int a, float b 等
        if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
            check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
            check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
            const Token& typeTok = advance();
            pType = typeTok.lexeme;

            // 可选的数组类型: int[]
            if (match(TokenType::TK_LBRACKET)) {
                consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
                pType += "[]";
            }

            const Token& param = consume(TokenType::TK_IDENTIFIER, "期望参数名");
            paramName = param.lexeme;
        } else {
            // 参数名在前面: a 或 a: int
            const Token& param = consume(TokenType::TK_IDENTIFIER, "期望参数名");
            paramName = param.lexeme;

            // 可选的参数类型注解 : type
            if (match(TokenType::TK_COLON)) {
                // 接受标识符或内置类型关键字（int, float, bool, string, dict, array）
                if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
                    check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
                    check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY) ||
                    check(TokenType::TK_IDENTIFIER)) {
                    const Token& typeTok = advance();
                    pType = typeTok.lexeme;
                    // 可选的数组类型: int[]
                    if (match(TokenType::TK_LBRACKET)) {
                        consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
                        pType += "[]";
                    }
                } else {
                    const Token& tok = peek();
                    throw ParseError("期望参数类型名", tok.line, tok.column);
                }
            }
        }

        params.push_back(paramName);
        paramTypes.push_back(pType);
    } while (match(TokenType::TK_COMMA));
}

std::unique_ptr<ClassDecl> Parser::classDecl() {
    const Token& classTok = consume(TokenType::TK_CLASS, "期望 'class'");
    const Token& name = consumeIdentifierOrType("期望类名");

    // 可选的 extends SuperClassName 或 : SuperClassName
    std::string superClassName;
    if (match(TokenType::TK_EXTENDS) || match(TokenType::TK_COLON)) {
        const Token& superName = consume(TokenType::TK_IDENTIFIER, "期望父类名");
        superClassName = superName.lexeme;
    }

    consume(TokenType::TK_LBRACE, "期望 '{'");

    // 解析类成员
    std::vector<std::unique_ptr<ASTNode>> members;

    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        // 类成员可以是：
        // - var 声明（字段）
        // - fun/function 声明（方法）
        // - 带类型注解的声明
        // - 裸方法名定义: methodName() {} （不带 fun 关键字）
        if (check(TokenType::TK_VAR)) {
            members.push_back(varDecl());
        } else if (check(TokenType::TK_FUN) || check(TokenType::TK_FUNCTION) || check(TokenType::TK_FUNC)) {
            members.push_back(funDecl());
        } else if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
                   check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
                   check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
            // 带类型注解的字段声明（如 int count = 0;）或带返回类型的方法声明（如 int getValue() {}）
            int savePos = current_;
            const Token& typeTok = advance();

            std::string typeAnn = typeTok.lexeme;
            if (match(TokenType::TK_LBRACKET)) {
                consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
                typeAnn += "[]";
            }

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
            // 或类类型字段: ClassName fieldName;
            int savePos = current_;
            const Token& firstTok = advance();  // 方法名或类名

            // 检查是否是方法定义: name(
            if (check(TokenType::TK_LPAREN)) {
                // 这是一个裸方法定义
                consume(TokenType::TK_LPAREN, "期望 '('");

                std::vector<std::string> params;
                std::vector<std::string> paramTypes;
                parseParamList(params, paramTypes);
                consume(TokenType::TK_RPAREN, "期望 ')'");

                // 可选的返回类型注解
                std::string returnType;
                if (match(TokenType::TK_COLON)) {
                    if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
                        check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
                        check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
                        const Token& typeTok = advance();
                        returnType = typeTok.lexeme;
                    } else {
                        const Token& retTypeTok = consume(TokenType::TK_IDENTIFIER, "期望返回类型名");
                        returnType = retTypeTok.lexeme;
                    }
                }

                consume(TokenType::TK_LBRACE, "期望 '{'");
                auto body = block();

                members.push_back(std::make_unique<FunDecl>(firstTok.lexeme, std::move(params),
                                                             std::move(paramTypes), returnType,
                                                             std::move(body), firstTok.line, firstTok.column));
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
    }

    consume(TokenType::TK_RBRACE, "期望 '}'");

    return std::make_unique<ClassDecl>(name.lexeme, superClassName,
                                        std::move(members),
                                        classTok.line, classTok.column);
}

std::unique_ptr<ASTNode> Parser::statement() {
    if (check(TokenType::TK_IF))       return ifStmt();
    if (check(TokenType::TK_WHILE))    return whileStmt();
    if (check(TokenType::TK_FOR))      return forStmt();
    if (check(TokenType::TK_RETURN))   return returnStmt();
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
    } else if (check(TokenType::TK_INT) || check(TokenType::TK_FLOAT) ||
               check(TokenType::TK_BOOL) || check(TokenType::TK_STRING_TYPE) ||
               check(TokenType::TK_DICT) || check(TokenType::TK_ARRAY)) {
        // 类型注解声明，如 int j = 0;
        int savePos = current_;
        const Token& typeTok = advance();

        std::string typeAnn = typeTok.lexeme;
        if (match(TokenType::TK_LBRACKET)) {
            consume(TokenType::TK_RBRACKET, "期望 ']' 结束数组类型注解");
            typeAnn += "[]";
        }

        if (check(TokenType::TK_IDENTIFIER)) {
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

std::unique_ptr<PrintStmt> Parser::printStmt() {
    const Token& printTok = consume(TokenType::TK_PRINT, "期望 'print'");
    consume(TokenType::TK_LPAREN, "期望 '('");

    std::vector<std::unique_ptr<ASTNode>> values;
    if (!check(TokenType::TK_RPAREN)) {
        do {
            values.push_back(expression());
        } while (match(TokenType::TK_COMMA));
    }

    consume(TokenType::TK_RPAREN, "期望 ')'");
    consume(TokenType::TK_SEMICOLON, "期望 ';'");

    return std::make_unique<PrintStmt>(std::move(values), printTok.line, printTok.column);
}

std::unique_ptr<Block> Parser::block() {
    const Token& lbrace = previous();  // '{' 已被消耗

    std::vector<std::unique_ptr<ASTNode>> stmts;

    while (!check(TokenType::TK_RBRACE) && !isAtEnd()) {
        stmts.push_back(declaration());
    }

    consume(TokenType::TK_RBRACE, "期望 '}'");

    return std::make_unique<Block>(std::move(stmts), lbrace.line, lbrace.column);
}

std::unique_ptr<ASTNode> Parser::expressionStatement() {
    auto expr = expression();
    consume(TokenType::TK_SEMICOLON, "期望 ';'");
    return expr;
}

// ---- 表达式 ----

std::unique_ptr<ASTNode> Parser::expression() {
    return assignment();
}

std::unique_ptr<ASTNode> Parser::assignment() {
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
    if (match(TokenType::TK_NOT, TokenType::TK_MINUS)) {
        const Token& op = previous();
        auto operand = unary();
        auto uopType = (op.type == TokenType::TK_NOT) ? UnaryOp::UnaryOpType::UOP_NOT : UnaryOp::UnaryOpType::UOP_NEGATE;
        return std::make_unique<UnaryOp>(uopType, std::move(operand), op.line, op.column);
    }
    return call();
}

std::unique_ptr<ASTNode> Parser::call() {
    auto expr = primary();

    // 支持链式调用: obj.method(args).field[0]
    while (true) {
        // 函数调用: name(args) —— 仅当 expr 是 VarRef 时
        if (match(TokenType::TK_LPAREN)) {
            const Token& paren = previous();
            if (expr->nodeType == NodeType::NODE_VAR_REF) {
                auto* varRef = static_cast<VarRef*>(expr.get());
                std::vector<std::unique_ptr<ASTNode>> args;
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        args.push_back(expression());
                    } while (match(TokenType::TK_COMMA));
                }
                consume(TokenType::TK_RPAREN, "期望 ')' 结束参数列表");

                std::string funcName = varRef->name;
                int ln = varRef->line;
                int col = varRef->column;

                expr = std::make_unique<FunCall>(funcName, std::move(args), ln, col);
                continue;
            }
            // 不是 VarRef 的左括号——回溯
            current_--;
            break;
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
            const Token& fieldName = consume(TokenType::TK_IDENTIFIER, "期望成员名");

            // 检查是否是方法调用: obj.method(args)
            if (match(TokenType::TK_LPAREN)) {
                std::vector<std::unique_ptr<ASTNode>> args;
                if (!check(TokenType::TK_RPAREN)) {
                    do {
                        args.push_back(expression());
                    } while (match(TokenType::TK_COMMA));
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
        return std::make_unique<NumberLiteral>(tok.literal, tok.line, tok.column);
    }

    // 浮点字面量
    if (match(TokenType::TK_FLOAT_LIT)) {
        const Token& tok = previous();
        return std::make_unique<NumberLiteral>(tok.literal, tok.line, tok.column);
    }

    // 字符串字面量
    if (match(TokenType::TK_STRING_LIT)) {
        const Token& tok = previous();
        return std::make_unique<StringLiteral>(tok.literal.stringVal(), tok.line, tok.column);
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

    // 标识符
    if (match(TokenType::TK_IDENTIFIER)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(tok.lexeme, tok.line, tok.column);
    }

    // 类型关键字作为标识符使用（如 dict(), array() 函数调用）
    if (match(TokenType::TK_DICT)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(tok.lexeme, tok.line, tok.column);
    }
    if (match(TokenType::TK_ARRAY)) {
        const Token& tok = previous();
        return std::make_unique<VarRef>(tok.lexeme, tok.line, tok.column);
    }

    // 数组字面量 [e1, e2, e3]
    if (match(TokenType::TK_LBRACKET)) {
        const Token& bracket = previous();

        std::vector<std::unique_ptr<ASTNode>> elements;
        if (!check(TokenType::TK_RBRACKET)) {
            do {
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

        std::vector<std::pair<std::unique_ptr<ASTNode>, std::unique_ptr<ASTNode>>> pairs;

        if (!check(TokenType::TK_RBRACE)) {
            do {
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
    if (match(TokenType::TK_LPAREN)) {
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
    advance();

    while (!isAtEnd()) {
        // 分号标记语句结束
        if (previous().type == TokenType::TK_SEMICOLON) return;

        // 关键字标记声明开始
        switch (peek().type) {
        case TokenType::TK_VAR:
        case TokenType::TK_FUN:
        case TokenType::TK_FUNCTION:
        case TokenType::TK_FUNC:
        case TokenType::TK_CLASS:
        case TokenType::TK_IF:
        case TokenType::TK_WHILE:
        case TokenType::TK_FOR:
        case TokenType::TK_RETURN:
        case TokenType::TK_PRINT:
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
