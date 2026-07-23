/**
 * @file parser/Parser.h
 * @brief MiniLang 递归下降语法分析器。
 *
 * 将 Token 流解析为 AST（抽象语法树），根节点为 Block。支持：
 *   - 表达式优先级链（assignment → or → and → equality → comparison
 *     → term → factor → unary → call → primary）
 *   - 控制流（if/elif/else、while、for-in、break、continue）
 *   - 函数声明与 lambda（含默认参数、类型注解）
 *   - 类与继承（class/extends、字段、方法、super）
 *   - 模块系统（import/export，含路径安全校验、循环依赖检测）
 *   - 异常处理（try/catch/finally/throw）
 *   - 字符串插值（解析 `\(expr)` 嵌套表达式为 InterpolatedString AST 节点）
 *
 * 错误恢复：
 *   - parse() 中 catch ParseError 后调用 synchronize() 跳到下个语句边界继续
 *   - 单条语句错误不影响后续解析，支持 IDE 多错误一次性展示
 *
 * DoS 防护：
 *   - MAX_PARSE_DEPTH（256）：递归深度上限，防止恶意嵌套栈溢出
 *   - MAX_BLOCK_DEPTH（256）：块嵌套深度上限
 *
 * @see Lexer ASTNode Formatter
 */
#pragma once

#include "Diagnostic.h"
#include "ast/ASTNode.h"
#include "common/RuntimeLimits.h"
#include "lexer/Token.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// ============================================================
// Parser 语法分析器
// ============================================================

/// 语法错误异常
class ParseError : public std::runtime_error {
public:
    int line;
    int column;

    ParseError(const std::string& msg, int ln = 0, int col = 0) : std::runtime_error(msg), line(ln), column(col) {}
};

/// 递归下降语法分析器
class Parser {
public:
    /// 构造函数
    Parser();

    /// 解析 Token 流，返回 AST 根节点（Block）
    std::unique_ptr<Block> parse(const std::vector<Token>& tokens);

    /// 是否有解析错误
    bool hasErrors() const { return diagnostics_.hasErrors(); }

    /// 获取解析过程中的诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    // C4 fix: 统一的递归深度 RAII 守卫，消除 6 处局部结构体重复定义。
    // 构造时递增深度计数器，析构时递减（异常路径也自动递减，防止深度计数泄漏）。
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    // R164 协程/生成器：yieldId 计数器 RAII 守卫。
    // 进入生成器函数 funDecl 时构造，保存旧值并重置为 0；析构时恢复旧值。
    // 支持嵌套生成器函数（外层 fun* 内定义内层 fun*）的独立 yieldId 计数。
    // 异常安全：ParseError 抛出时 ~YieldIdScope 自动恢复外层计数器。
    struct YieldIdScope {
        int& counter;
        int saved;
        explicit YieldIdScope(int& c) : counter(c), saved(c) {}
        ~YieldIdScope() { counter = saved; }
        YieldIdScope(const YieldIdScope&) = delete;
        YieldIdScope& operator=(const YieldIdScope&) = delete;
    };

    // R164 协程/生成器：bool 字段 RAII 守卫（异常安全保存/恢复）。
    // 用于 yieldInLoop_（whileStmt/forStmt 入口置 true）和
    // currentFunHasYieldInLoop_（funDecl 入口重置为 false）。
    struct BoolScope {
        bool& flag;
        bool saved;
        explicit BoolScope(bool& f) : flag(f), saved(f) {}
        ~BoolScope() { flag = saved; }
        BoolScope(const BoolScope&) = delete;
        BoolScope& operator=(const BoolScope&) = delete;
    };

private:
    const std::vector<Token>* tokens_ = nullptr; // Token 流（引用，避免深拷贝）
    int current_ = 0;                            // 当前位置
    DiagnosticBag diagnostics_;                  // 诊断收集器
    int parseDepth_ = 0;                         // P15 fix: 递归深度计数器
    int blockDepth_ = 0;                         // P0-1 fix: 块嵌套深度计数器
    // R164 协程/生成器：当前生成器函数内 yield 编号计数器。
    // -1 表示不在生成器上下文（普通函数/全局作用域），>=0 表示当前生成器已分配的 yield 数。
    // 由 YieldIdScope 在 funDecl 入口保存/重置/恢复，primary() 解析 yield 时分配并递增。
    int currentYieldId_ = -1;
    // R164 协程/生成器：当前是否处于循环体内部（while/for body）。
    // 由 BoolScope 在 whileStmt/forStmt 入口保存/置 true/恢复。用于检测 yield-in-loop：
    // 循环内 yield 的编译期 yieldCount 只是 AST 节点数（1），但运行时可能执行 N 次，
    // 导致 callCoroutineNext 的 `currentYieldId >= yieldCount` 提前判定 done。
    // 检测到此类生成器时，yieldCount 设为 INT_MAX（kDynamicYieldCount），
    // done 改由函数体自然结束路径判定。
    bool yieldInLoop_ = false;
    // R164 协程/生成器：当前生成器函数体内是否存在 yield-in-loop。
    // 由 BoolScope 在 funDecl 入口保存/重置/恢复。primary() 解析 yield 时若 yieldInLoop_
    // 为 true 则置本字段为 true。funDecl 结尾若本字段为 true，decl->yieldCount = INT_MAX。
    bool currentFunHasYieldInLoop_ = false;
    static constexpr int MAX_PARSE_DEPTH = RuntimeLimits::MAX_PARSE_DEPTH;
    static constexpr int MAX_BLOCK_DEPTH = RuntimeLimits::MAX_BLOCK_DEPTH;
    // BUG-PARSER-AUDIT-5: 错误数量上限，防止恶意输入触发 O(N) 诊断内存膨胀
    static constexpr int MAX_PARSE_ERRORS = RuntimeLimits::MAX_PARSE_ERRORS;

    // R99 枚举与 ADT: 已声明的 enum 名称集合。
    // call() 中用于区分 EnumName.VariantName（EnumVariantExpr）与
    // instance.field（MemberAccess）—— parser 侧无类型系统，
    // 通过维护已知 enum 名集合进行语法层消歧。enumDecl() 解析后插入。
    std::unordered_set<std::string> knownEnums_;

    // ---- 辅助方法 ----

    /// 当前 Token
    const Token& peek() const;

    /// 前一个 Token
    const Token& previous() const;

    /// 是否到达末尾
    bool isAtEnd() const;

    /// 前进一个 Token，返回前一个 Token
    const Token& advance();

    /// 检查当前 Token 是否为指定类型
    bool check(TokenType type) const;

    /// 检查下一个 Token 是否为指定类型（用于 -> 语法检测）
    bool checkNext(TokenType type) const;

    /// 如果当前 Token 匹配任一类型则前进（C++17 折叠表达式，零分配）
    template <typename... Ts> bool match(Ts... types) {
        return ((check(static_cast<TokenType>(types)) ? (advance(), true) : false) || ...);
    }

    /// 消耗当前 Token，必须匹配指定类型，否则抛异常
    const Token& consume(TokenType type, const std::string& message);

    /// 消耗标识符或类型关键字（允许 dict/array/int/float/string/bool 作为名称）
    const Token& consumeIdentifierOrType(const std::string& message);

    /// 检查当前 token 是否是标识符或类型关键字
    bool isIdentifierOrType() const;

    /// 检查当前 token 是否是类型关键字（int/float/bool/string/dict/array）
    /// 消除 6 处重复的类型关键字检查模式
    bool isTypeKeyword() const;

    /// 检查当前位置是否是类类型声明起始：ClassName paramName 或 ClassName[] paramName
    /// 用于 parseParamList / forStmt 初始化中识别 Point p / Point[] arr 模式
    bool isClassTypeDeclStart() const;

    /// 检查当前位置是否是函数类型声明起始：fun(params):ret paramName
    /// 用于 parseParamList 识别高阶函数参数类型注解（如 fun(int):string cb）
    /// ROUND56 fix: samples/02-types-and-operators/types_and_operators.mini 使用此语法
    bool isFunTypeDeclStart() const;

    /// 解析类型注解（内置类型或类名，可选 [] 数组后缀）
    /// 当前 token 必须是类型关键字或标识符，消耗类型 token 并可选地消耗 []
    /// 使用安全回溯：仅在 [ 后紧跟 ] 时才消费，否则回退 [
    /// 返回如 "int", "int[]", "ClassName", "ClassName[]" 等字符串
    std::string parseTypeAnnotation();

    // P2-1 fix: 移除未实现的 parseReturnType() 声明（死代码）
    // 实际的 -> type 解析逻辑在 funDecl() 和 classDecl() 中内联实现

    // ---- 声明与语句 ----

    /// 声明（变量声明 / 类型注解声明 / 函数声明 / 类声明 / 语句）
    std::unique_ptr<ASTNode> declaration();

    /// 变量声明: var name = expr;
    /// R98 元组与解构：可能返回 DestructureBinding（var (a,b) = expr）。
    std::unique_ptr<ASTNode> varDecl();

    /// 带类型注解的变量声明: int a = 10; int[] arr = [1,2,3]; dict d = {"a":1};
    std::unique_ptr<VarDecl> typedVarDecl(const std::string& typeAnn);

    /// 函数声明: fun name(params) { body } 或 function name(params): type { body }
    std::unique_ptr<FunDecl> funDecl();

    /// 带返回类型的函数声明: int fib(int n) { ... }
    std::unique_ptr<FunDecl> typedFunDecl(const std::string& returnType);

    /// R98 W3: Lambda 表达式: fun(params) { body }（匿名函数，作为表达式使用）
    /// 语法：`var f = fun(x) { return x * 2; };` 或 `var f = fun(x, y) { return x + y; };`
    /// 复用 FunDecl 节点，name 为空字符串（Compiler/Interpreter 内部用合成名 `$lambda_N`）
    std::unique_ptr<FunDecl> lambdaExpr();

    /// 类声明: class Name { members } 或 class Name extends Super { members }
    std::unique_ptr<ClassDecl> classDecl();

    /// 解析类声明的 extends / : SuperClassName 子句。
    /// 调用前应已 consume 类名，调用后 current_ 位于 '{' 之前。
    /// 返回父类名（空字符串表示无父类）。
    std::string parseClassExtends();

    /// 解析类成员循环（var / fun / 类型注解 / 裸方法 / 类类型字段）。
    /// 调用前应已 consume '{'，调用后 current_ 位于 '}' 之前。
    /// 保留 BUG-PARSER-AUDIT-2 fix 的 try/catch 错误恢复。
    /// 保留 BUG-LPA-04 fix 的 ClassName[] 数组字段支持。
    /// 保留 AUDIT-P2 fix 的多维数组字段。
    void parseClassMembers(std::vector<std::shared_ptr<ASTNode>>& members);

    // R99 枚举与 ADT + match
    /// enum 声明: enum Name<T, U> { Variant1, Variant2(T), ... }
    std::unique_ptr<EnumDecl> enumDecl();

    /// match 表达式: match scrutinee { case Pattern => body; ... default => body; }
    std::unique_ptr<MatchExpr> matchExpr();

    /// match 模式: _ / 字面量 / 变量 / EnumName.VariantName(...) / 元组 / OR pattern
    /// R134 扩展：支持嵌套 pattern（subPatterns 递归）+ guard 表达式。
    std::shared_ptr<MatchPattern> matchPattern();

    /// match 主 pattern（不含 OR）：_ / 字面量 / 变量 / EnumName.VariantName(...) / 元组
    /// R134 新增：matchPattern 的子 helper，递归下降解析的初级 pattern。
    std::shared_ptr<MatchPattern> matchPrimaryPattern();

    /// 解析参数列表（支持C风格类型注解 int a 和冒号风格 a: int）
    /// 假设调用前已 consume '('
    /// F10: defaultValues 收集默认参数值表达式（与 params 一一对应，无默认值时为 nullptr）
    void parseParamList(std::vector<std::string>& params, std::vector<std::string>& paramTypes,
                        std::vector<std::shared_ptr<ASTNode>>& defaultValues);

    /// 语句
    std::unique_ptr<ASTNode> statement();

    /// if 语句
    std::unique_ptr<IfStmt> ifStmt();

    /// while 语句
    std::unique_ptr<WhileStmt> whileStmt();

    /// for 语句
    std::unique_ptr<ForStmt> forStmt();

    /// return 语句
    std::unique_ptr<ReturnStmt> returnStmt();

    /// break 语句
    std::unique_ptr<BreakStmt> breakStmt();

    /// continue 语句
    std::unique_ptr<ContinueStmt> continueStmt();

    /// try-catch 语句
    std::unique_ptr<TryStmt> tryStmt();

    /// throw 语句
    std::unique_ptr<ThrowStmt> throwStmt();

    /// import 语句
    std::unique_ptr<ImportStmt> importStmt();

    /// export 语句
    std::unique_ptr<ExportStmt> exportStmt();

    /// print 语句
    std::unique_ptr<PrintStmt> printStmt();

    /// 代码块 { ... }
    std::unique_ptr<Block> block();

    /// 表达式语句（赋值）
    std::unique_ptr<ASTNode> expressionStatement();

    // ---- 表达式（按优先级从低到高）----

    /// 表达式入口
    std::unique_ptr<ASTNode> expression();

    /// 赋值（支持索引赋值和成员赋值）
    std::unique_ptr<ASTNode> assignment();

    /// or
    std::unique_ptr<ASTNode> or_();

    /// and
    std::unique_ptr<ASTNode> and_();

    /// 相等性 == !=
    std::unique_ptr<ASTNode> equality();

    /// 比较 < > <= >=
    std::unique_ptr<ASTNode> comparison();

    /// 加减 + -
    std::unique_ptr<ASTNode> term();

    /// 乘除 * / %
    std::unique_ptr<ASTNode> factor();

    /// 一元 not -
    std::unique_ptr<ASTNode> unary();

    /// 调用 fun(args)、obj.method(args)、arr[index]、obj.field
    std::unique_ptr<ASTNode> call();

    /// 基本字面量 / 标识符 / 分组
    std::unique_ptr<ASTNode> primary();

    /// F7: 解析插值字符串 "Hello {name}, age {age}"
    /// 将字符串字面量和表达式拼接为 BinaryOp(BIN_ADD) 链
    /// @param first 第一个字符串片段（已解析的 StringLiteral）
    std::unique_ptr<ASTNode> parseInterpolatedString(std::unique_ptr<ASTNode> first);

    // ---- 错误恢复 ----

    /// 同步到下一个声明边界
    void synchronize();
};
