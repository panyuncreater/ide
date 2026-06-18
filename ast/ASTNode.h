#pragma once

#include <memory>
#include <string>
#include <vector>
#include "interpreter/Value.h"

// ============================================================
// AST 节点类型枚举（用于快速分发，替代 dynamic_cast）
// ============================================================

enum class NodeType {
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_NUMBER_LITERAL,
    NODE_STRING_LITERAL,
    NODE_BOOL_LITERAL,
    NODE_VAR_DECL,
    NODE_ASSIGNMENT,
    NODE_VAR_REF,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_FUN_DECL,
    NODE_FUN_CALL,
    NODE_RETURN_STMT,
    NODE_PRINT_STMT,
    NODE_BLOCK,
    NODE_ARRAY_LITERAL,
    NODE_DICT_LITERAL,
    NODE_INDEX_ACCESS,
    NODE_INDEX_ASSIGN,
    NODE_CLASS_DECL,
    NODE_MEMBER_ACCESS,
    NODE_MEMBER_ASSIGN,
    NODE_METHOD_CALL,
    NODE_NULL_LITERAL,
    NODE_SUPER_EXPR,
};

// ============================================================
// 前向声明 Visitor
// ============================================================
class Visitor;

// ============================================================
// AST 节点基类与所有子类声明
// ============================================================

/// AST 节点基类
class ASTNode {
public:
    int line = 0;       // 行号
    int column = 0;     // 列号
    NodeType nodeType = NodeType::NODE_NULL_LITERAL;  // 节点类型（用于快速分发）

    ASTNode() = default;
    ASTNode(int ln, int col) : line(ln), column(col) {}
    virtual ~ASTNode() = default;

    /// 访问者模式接受方法
    virtual Value accept(Visitor& visitor) = 0;

    /// 获取节点类型名称（用于可视化）
    virtual std::string nodeName() const = 0;

    /// 获取子节点列表（用于可视化遍历）
    virtual std::vector<ASTNode*> children() const = 0;
};

/// 二元运算符类型枚举（缓存字符串→枚举映射，避免运行时字符串比较）
enum class BinOpType {
    BIN_AND, BIN_OR,
    BIN_EQ, BIN_NEQ, BIN_LT, BIN_GT, BIN_LTE, BIN_GTE,
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD,
    BIN_UNKNOWN
};

/// 二元运算节点
class BinaryOp : public ASTNode {
public:
    BinOpType opType;                       // 运算符枚举
    std::unique_ptr<ASTNode> left;          // 左操作数
    std::unique_ptr<ASTNode> right;         // 右操作数

    BinaryOp(BinOpType opType, std::unique_ptr<ASTNode> l,
             std::unique_ptr<ASTNode> r, int ln = 0, int col = 0)
        : ASTNode(ln, col), opType(opType),
          left(std::move(l)), right(std::move(r)) { nodeType = NodeType::NODE_BINARY_OP; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return std::string("BinaryOp(") + opTypeStr(opType) + ")"; }
    std::vector<ASTNode*> children() const override {
        return { left.get(), right.get() };
    }

    /// 枚举 → 字符串（用于显示/格式化/错误信息）
    static const char* opTypeStr(BinOpType t) {
        switch (t) {
        case BinOpType::BIN_AND: return "and";
        case BinOpType::BIN_OR:  return "or";
        case BinOpType::BIN_EQ:  return "==";
        case BinOpType::BIN_NEQ: return "!=";
        case BinOpType::BIN_LT:  return "<";
        case BinOpType::BIN_GT:  return ">";
        case BinOpType::BIN_LTE: return "<=";
        case BinOpType::BIN_GTE: return ">=";
        case BinOpType::BIN_ADD: return "+";
        case BinOpType::BIN_SUB: return "-";
        case BinOpType::BIN_MUL: return "*";
        case BinOpType::BIN_DIV: return "/";
        case BinOpType::BIN_MOD: return "%";
        default: return "?";
        }
    }
};

/// 一元运算节点
class UnaryOp : public ASTNode {
public:
    enum class UnaryOpType { UOP_NEGATE, UOP_NOT, UOP_UNKNOWN };

    UnaryOpType opType;                     // 运算符类型枚举
    std::unique_ptr<ASTNode> operand;        // 操作数

    UnaryOp(UnaryOpType opType, std::unique_ptr<ASTNode> o,
            int ln = 0, int col = 0)
        : ASTNode(ln, col), opType(opType), operand(std::move(o)) { nodeType = NodeType::NODE_UNARY_OP; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return std::string("UnaryOp(") + opTypeStr(opType) + ")"; }
    std::vector<ASTNode*> children() const override {
        return { operand.get() };
    }

    /// 枚举 → 字符串
    static const char* opTypeStr(UnaryOpType t) {
        switch (t) {
        case UnaryOpType::UOP_NEGATE: return "-";
        case UnaryOpType::UOP_NOT:    return "not";
        default: return "?";
        }
    }
};

/// 数字字面量节点
class NumberLiteral : public ASTNode {
public:
    Value value;    // 保存 int 或 float 值

    NumberLiteral(const Value& v, int ln = 0, int col = 0)
        : ASTNode(ln, col), value(v) { nodeType = NodeType::NODE_NUMBER_LITERAL; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override {
        return "Number(" + value.toString() + ")";
    }
    std::vector<ASTNode*> children() const override { return {}; }
};

/// 字符串字面量节点
class StringLiteral : public ASTNode {
public:
    std::string value;

    StringLiteral(const std::string& v, int ln = 0, int col = 0)
        : ASTNode(ln, col), value(v), cachedValue_(Value(v)) { nodeType = NodeType::NODE_STRING_LITERAL; }

    /// 获取缓存的 Value（首次移动返回，零堆分配；后续从 string 重建）
    Value takeValue() {
        if (!cachedValue_.isNull()) {
            Value result = std::move(cachedValue_);
            cachedValue_ = Value();
            return result;
        }
        return Value(value);
    }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override {
        return "String(\"" + value + "\")";
    }
    std::vector<ASTNode*> children() const override { return {}; }

private:
    Value cachedValue_;  // 预构建的 Value，首次 takeValue() 移动返回
};

/// 布尔字面量节点
class BoolLiteral : public ASTNode {
public:
    bool value;

    BoolLiteral(bool v, int ln = 0, int col = 0)
        : ASTNode(ln, col), value(v) { nodeType = NodeType::NODE_BOOL_LITERAL; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override {
        return "Bool(" + std::string(value ? "true" : "false") + ")";
    }
    std::vector<ASTNode*> children() const override { return {}; }
};

/// 变量声明节点
class VarDecl : public ASTNode {
public:
    std::string name;
    std::string typeAnnotation;             // 类型注解（如 "int", "float", "int[]", "dict" 等）
    std::unique_ptr<ASTNode> initializer;   // 可为 nullptr

    VarDecl(const std::string& n, const std::string& typeAnn,
            std::unique_ptr<ASTNode> init, int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n), typeAnnotation(typeAnn), initializer(std::move(init)) { nodeType = NodeType::NODE_VAR_DECL; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override {
        if (typeAnnotation.empty()) return "VarDecl(" + name + ")";
        return "VarDecl(" + name + ":" + typeAnnotation + ")";
    }
    std::vector<ASTNode*> children() const override {
        if (initializer) return { initializer.get() };
        return {};
    }
};

/// 赋值节点
class Assignment : public ASTNode {
public:
    std::string name;
    std::unique_ptr<ASTNode> value;

    Assignment(const std::string& n, std::unique_ptr<ASTNode> v,
               int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n), value(std::move(v)) { nodeType = NodeType::NODE_ASSIGNMENT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "Assign(" + name + ")"; }
    std::vector<ASTNode*> children() const override {
        return { value.get() };
    }
};

/// 变量引用节点
class VarRef : public ASTNode {
public:
    std::string name;

    VarRef(const std::string& n, int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n) { nodeType = NodeType::NODE_VAR_REF; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "VarRef(" + name + ")"; }
    std::vector<ASTNode*> children() const override { return {}; }
};

/// if 语句节点
class IfStmt : public ASTNode {
public:
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<ASTNode> thenBranch;
    std::unique_ptr<ASTNode> elseBranch;    // 可为 nullptr

    IfStmt(std::unique_ptr<ASTNode> cond, std::unique_ptr<ASTNode> thenB,
           std::unique_ptr<ASTNode> elseB, int ln = 0, int col = 0)
        : ASTNode(ln, col), condition(std::move(cond)),
          thenBranch(std::move(thenB)), elseBranch(std::move(elseB)) { nodeType = NodeType::NODE_IF_STMT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "IfStmt"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        ch.push_back(condition.get());
        ch.push_back(thenBranch.get());
        if (elseBranch) ch.push_back(elseBranch.get());
        return ch;
    }
};

/// while 语句节点
class WhileStmt : public ASTNode {
public:
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<ASTNode> body;

    WhileStmt(std::unique_ptr<ASTNode> cond, std::unique_ptr<ASTNode> b,
              int ln = 0, int col = 0)
        : ASTNode(ln, col), condition(std::move(cond)), body(std::move(b)) { nodeType = NodeType::NODE_WHILE_STMT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "WhileStmt"; }
    std::vector<ASTNode*> children() const override {
        return { condition.get(), body.get() };
    }
};

/// for 语句节点
class ForStmt : public ASTNode {
public:
    std::unique_ptr<ASTNode> initializer;  // 可为 nullptr
    std::unique_ptr<ASTNode> condition;     // 可为 nullptr
    std::unique_ptr<ASTNode> update;        // 可为 nullptr
    std::unique_ptr<ASTNode> body;

    ForStmt(std::unique_ptr<ASTNode> init, std::unique_ptr<ASTNode> cond,
            std::unique_ptr<ASTNode> upd, std::unique_ptr<ASTNode> b,
            int ln = 0, int col = 0)
        : ASTNode(ln, col), initializer(std::move(init)),
          condition(std::move(cond)), update(std::move(upd)),
          body(std::move(b)) { nodeType = NodeType::NODE_FOR_STMT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "ForStmt"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        if (initializer) ch.push_back(initializer.get());
        if (condition) ch.push_back(condition.get());
        if (update) ch.push_back(update.get());
        ch.push_back(body.get());
        return ch;
    }
};

/// 函数声明节点
class FunDecl : public ASTNode {
public:
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> paramTypes;    // 参数类型注解
    std::string returnType;                 // 返回值类型注解
    std::unique_ptr<ASTNode> body;

    FunDecl(const std::string& n, std::vector<std::string> p,
            std::vector<std::string> pt, const std::string& rt,
            std::unique_ptr<ASTNode> b, int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n), params(std::move(p)),
          paramTypes(std::move(pt)), returnType(rt), body(std::move(b)) { nodeType = NodeType::NODE_FUN_DECL; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "FunDecl(" + name + ")"; }
    std::vector<ASTNode*> children() const override {
        return { body.get() };
    }
};

/// 函数调用节点
class FunCall : public ASTNode {
public:
    std::string name;
    std::vector<std::unique_ptr<ASTNode>> arguments;

    /// 链式调用：非 null 时表示被调用表达式（如 f(x)(y) 中的 f(x)）
    /// 此时 name 为空，调用 callee 求值结果（需为闭包）
    std::unique_ptr<ASTNode> callee;

    /// 缓存：首次调用解析后存储 FunDecl*，后续调用跳过查找
    FunDecl* resolvedDecl = nullptr;
    bool isResolved = false;
    int resolvedGen_ = -1;  // M7: 缓存时的 funRegistry 代数，不匹配则失效

    FunCall(const std::string& n, std::vector<std::unique_ptr<ASTNode>> args,
            int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n), arguments(std::move(args)) { nodeType = NodeType::NODE_FUN_CALL; }

    /// 链式调用构造：callee 为任意表达式
    FunCall(std::unique_ptr<ASTNode> calleeExpr, std::vector<std::unique_ptr<ASTNode>> args,
            int ln = 0, int col = 0)
        : ASTNode(ln, col), arguments(std::move(args)), callee(std::move(calleeExpr))
        { nodeType = NodeType::NODE_FUN_CALL; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "FunCall(" + (callee ? "expr" : name) + ")"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        if (callee) ch.push_back(callee.get());
        for (auto& arg : arguments) {
            ch.push_back(arg.get());
        }
        return ch;
    }
};

/// return 语句节点
class ReturnStmt : public ASTNode {
public:
    std::unique_ptr<ASTNode> value;     // 可为 nullptr

    ReturnStmt(std::unique_ptr<ASTNode> v, int ln = 0, int col = 0)
        : ASTNode(ln, col), value(std::move(v)) { nodeType = NodeType::NODE_RETURN_STMT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "ReturnStmt"; }
    std::vector<ASTNode*> children() const override {
        if (value) return { value.get() };
        return {};
    }
};

/// print 语句节点
class PrintStmt : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> values;

    PrintStmt(std::vector<std::unique_ptr<ASTNode>> v, int ln = 0, int col = 0)
        : ASTNode(ln, col), values(std::move(v)) { nodeType = NodeType::NODE_PRINT_STMT; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "PrintStmt"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        for (auto& v : values) ch.push_back(v.get());
        return ch;
    }
};

/// 代码块节点
class Block : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> statements;
    int closingBraceLine = 0;  // L18 fix: '}' 所在行号，用于格式化器刷新块尾部注释

    Block(std::vector<std::unique_ptr<ASTNode>> stmts, int ln = 0, int col = 0)
        : ASTNode(ln, col), statements(std::move(stmts)) { nodeType = NodeType::NODE_BLOCK; }

    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "Block"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        for (auto& s : statements) {
            ch.push_back(s.get());
        }
        return ch;
    }
};

// ============================================================
// 新增 AST 节点
// ============================================================

/// 数组字面量节点 [e1, e2, e3]
class ArrayLiteral : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> elements;
    ArrayLiteral(std::vector<std::unique_ptr<ASTNode>> elems, int ln = 0, int col = 0)
        : ASTNode(ln, col), elements(std::move(elems)) { nodeType = NodeType::NODE_ARRAY_LITERAL; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "ArrayLiteral"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        for (auto& e : elements) ch.push_back(e.get());
        return ch;
    }
};

/// 字典字面量节点 {"key": value, ...}
class DictLiteral : public ASTNode {
public:
    std::vector<std::pair<std::unique_ptr<ASTNode>, std::unique_ptr<ASTNode>>> pairs;
    DictLiteral(std::vector<std::pair<std::unique_ptr<ASTNode>, std::unique_ptr<ASTNode>>> p, int ln = 0, int col = 0)
        : ASTNode(ln, col), pairs(std::move(p)) { nodeType = NodeType::NODE_DICT_LITERAL; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "DictLiteral"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        for (auto& p : pairs) { ch.push_back(p.first.get()); ch.push_back(p.second.get()); }
        return ch;
    }
};

/// 索引访问节点 arr[index] 或 dict[key]
class IndexAccess : public ASTNode {
public:
    std::unique_ptr<ASTNode> object;
    std::unique_ptr<ASTNode> index;
    IndexAccess(std::unique_ptr<ASTNode> obj, std::unique_ptr<ASTNode> idx, int ln = 0, int col = 0)
        : ASTNode(ln, col), object(std::move(obj)), index(std::move(idx)) { nodeType = NodeType::NODE_INDEX_ACCESS; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "IndexAccess"; }
    std::vector<ASTNode*> children() const override { return {object.get(), index.get()}; }
};

/// 索引赋值节点 arr[index] = value 或 dict[key] = value
class IndexAssign : public ASTNode {
public:
    std::unique_ptr<ASTNode> object;
    std::unique_ptr<ASTNode> index;
    std::unique_ptr<ASTNode> value;
    IndexAssign(std::unique_ptr<ASTNode> obj, std::unique_ptr<ASTNode> idx, std::unique_ptr<ASTNode> val, int ln = 0, int col = 0)
        : ASTNode(ln, col), object(std::move(obj)), index(std::move(idx)), value(std::move(val)) { nodeType = NodeType::NODE_INDEX_ASSIGN; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "IndexAssign"; }
    std::vector<ASTNode*> children() const override { return {object.get(), index.get(), value.get()}; }
};

/// 类声明节点
class ClassDecl : public ASTNode {
public:
    std::string name;
    std::string superClassName;
    std::vector<std::unique_ptr<ASTNode>> members;
    ClassDecl(const std::string& n, const std::string& super,
              std::vector<std::unique_ptr<ASTNode>> mems, int ln = 0, int col = 0)
        : ASTNode(ln, col), name(n), superClassName(super), members(std::move(mems)) { nodeType = NodeType::NODE_CLASS_DECL; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "ClassDecl(" + name + ")"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        for (auto& m : members) ch.push_back(m.get());
        return ch;
    }
};

/// 成员访问节点 obj.field
class MemberAccess : public ASTNode {
public:
    std::unique_ptr<ASTNode> object;
    std::string fieldName;
    MemberAccess(std::unique_ptr<ASTNode> obj, const std::string& field, int ln = 0, int col = 0)
        : ASTNode(ln, col), object(std::move(obj)), fieldName(field) { nodeType = NodeType::NODE_MEMBER_ACCESS; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "MemberAccess(." + fieldName + ")"; }
    std::vector<ASTNode*> children() const override { return {object.get()}; }
};

/// 成员赋值节点 obj.field = value
class MemberAssign : public ASTNode {
public:
    std::unique_ptr<ASTNode> object;
    std::string fieldName;
    std::unique_ptr<ASTNode> value;
    MemberAssign(std::unique_ptr<ASTNode> obj, const std::string& field, std::unique_ptr<ASTNode> val, int ln = 0, int col = 0)
        : ASTNode(ln, col), object(std::move(obj)), fieldName(field), value(std::move(val)) { nodeType = NodeType::NODE_MEMBER_ASSIGN; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "MemberAssign(." + fieldName + ")"; }
    std::vector<ASTNode*> children() const override { return {object.get(), value.get()}; }
};

/// 方法调用节点 obj.method(args)
class MethodCall : public ASTNode {
public:
    std::unique_ptr<ASTNode> object;
    std::string methodName;
    std::vector<std::unique_ptr<ASTNode>> arguments;
    MethodCall(std::unique_ptr<ASTNode> obj, const std::string& method,
               std::vector<std::unique_ptr<ASTNode>> args, int ln = 0, int col = 0)
        : ASTNode(ln, col), object(std::move(obj)), methodName(method), arguments(std::move(args)) { nodeType = NodeType::NODE_METHOD_CALL; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "MethodCall(." + methodName + ")"; }
    std::vector<ASTNode*> children() const override {
        std::vector<ASTNode*> ch;
        ch.push_back(object.get());
        for (auto& a : arguments) ch.push_back(a.get());
        return ch;
    }
};

/// Null 字面量节点
class NullLiteral : public ASTNode {
public:
    NullLiteral(int ln = 0, int col = 0) : ASTNode(ln, col) { nodeType = NodeType::NODE_NULL_LITERAL; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "Null"; }
    std::vector<ASTNode*> children() const override { return {}; }
};

/// Super 表达式节点（引用父类方法/字段）
class SuperExpr : public ASTNode {
public:
    SuperExpr(int ln = 0, int col = 0) : ASTNode(ln, col) { nodeType = NodeType::NODE_SUPER_EXPR; }
    Value accept(Visitor& visitor) override;
    std::string nodeName() const override { return "Super"; }
    std::vector<ASTNode*> children() const override { return {}; }
};
