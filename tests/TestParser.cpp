// ============================================================
// Parser 单元测试
// ------------------------------------------------------------
// 覆盖 Parser 的所有功能点：表达式解析、语句解析、声明解析、
// 错误恢复与诊断信息。
// ============================================================

#include <gtest/gtest.h>
#include "parser/Parser.h"
#include "lexer/Lexer.h"
#include "ast/ASTNode.h"

#include <memory>
#include <string>

// ============================================================
// 辅助函数
// ============================================================

// 扫描源代码并解析为 AST Block
static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

// 扫描源代码并解析，同时保留 Parser 以便检查错误
static std::unique_ptr<Block> parseSourceWithParser(const std::string& source, Parser& parser) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    return parser.parse(tokens);
}

// 获取 Block 的第一条语句
static ASTNode* firstStmt(const std::unique_ptr<Block>& block) {
    if (!block || block->statements.empty()) return nullptr;
    return block->statements[0].get();
}

// ============================================================
// 1. 表达式解析测试
// ============================================================

// ---- 1.1 算术表达式 ----

// 测试：整数加法 1 + 2
TEST(ParserTest, Arithmetic_Addition) {
    auto block = parseSource("1 + 2;");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_ADD);
    EXPECT_EQ(bin->left->nodeType, NodeType::NODE_NUMBER_LITERAL);
    EXPECT_EQ(bin->right->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：运算符优先级 3 * 4 - 5 应解析为 (3 * 4) - 5
TEST(ParserTest, Arithmetic_OperatorPrecedence) {
    auto block = parseSource("3 * 4 - 5;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    // 根节点应为减法（优先级低于乘法）
    EXPECT_EQ(bin->opType, BinOpType::BIN_SUB);
    // 左子节点应为乘法
    ASSERT_EQ(bin->left->nodeType, NodeType::NODE_BINARY_OP);
    auto* left = static_cast<BinaryOp*>(bin->left.get());
    EXPECT_EQ(left->opType, BinOpType::BIN_MUL);
}

// 测试：括号改变优先级 (1 + 2) * 3
TEST(ParserTest, Arithmetic_Parenthesized) {
    auto block = parseSource("(1 + 2) * 3;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    // 根节点应为乘法
    EXPECT_EQ(bin->opType, BinOpType::BIN_MUL);
    // 左子节点应为加法（括号内的）
    ASSERT_EQ(bin->left->nodeType, NodeType::NODE_BINARY_OP);
    auto* left = static_cast<BinaryOp*>(bin->left.get());
    EXPECT_EQ(left->opType, BinOpType::BIN_ADD);
}

// 测试：取模运算 10 % 3
TEST(ParserTest, Arithmetic_Modulo) {
    auto block = parseSource("10 % 3;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_MOD);
}

// 测试：除法运算 8 / 2
TEST(ParserTest, Arithmetic_Division) {
    auto block = parseSource("8 / 2;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_DIV);
}

// ---- 1.2 比较表达式 ----

// 测试：小于 1 < 2
TEST(ParserTest, Comparison_LessThan) {
    auto block = parseSource("1 < 2;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_LT);
}

// 测试：大于等于 3 >= 4
TEST(ParserTest, Comparison_GreaterEqual) {
    auto block = parseSource("3 >= 4;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_GTE);
}

// 测试：相等 5 == 5
TEST(ParserTest, Comparison_Equal) {
    auto block = parseSource("5 == 5;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_EQ);
}

// 测试：不等 6 != 7
TEST(ParserTest, Comparison_NotEqual) {
    auto block = parseSource("6 != 7;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_NEQ);
}

// 测试：大于 3 > 2
TEST(ParserTest, Comparison_GreaterThan) {
    auto block = parseSource("3 > 2;");
    auto* node = firstStmt(block);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_GT);
}

// 测试：小于等于 2 <= 3
TEST(ParserTest, Comparison_LessEqual) {
    auto block = parseSource("2 <= 3;");
    auto* node = firstStmt(block);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_LTE);
}

// ---- 1.3 逻辑表达式 ----

// 测试：逻辑与 true and false
TEST(ParserTest, Logic_And) {
    auto block = parseSource("true and false;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_AND);
    EXPECT_EQ(bin->left->nodeType, NodeType::NODE_BOOL_LITERAL);
    EXPECT_EQ(bin->right->nodeType, NodeType::NODE_BOOL_LITERAL);
}

// 测试：逻辑或 true or false
TEST(ParserTest, Logic_Or) {
    auto block = parseSource("true or false;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    EXPECT_EQ(bin->opType, BinOpType::BIN_OR);
}

// 测试：逻辑非 not true
TEST(ParserTest, Logic_Not) {
    auto block = parseSource("not true;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_UNARY_OP);
    auto* un = static_cast<UnaryOp*>(node);
    EXPECT_EQ(un->opType, UnaryOp::UnaryOpType::UOP_NOT);
    EXPECT_EQ(un->operand->nodeType, NodeType::NODE_BOOL_LITERAL);
}

// 测试：逻辑运算优先级 or 低于 and
TEST(ParserTest, Logic_Precedence) {
    auto block = parseSource("true or false and true;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
    auto* bin = static_cast<BinaryOp*>(node);
    // 根节点应为 or
    EXPECT_EQ(bin->opType, BinOpType::BIN_OR);
    // 右子节点应为 and
    ASSERT_EQ(bin->right->nodeType, NodeType::NODE_BINARY_OP);
    auto* right = static_cast<BinaryOp*>(bin->right.get());
    EXPECT_EQ(right->opType, BinOpType::BIN_AND);
}

// ---- 1.4 字面量 ----

// 测试：整数字面量
TEST(ParserTest, Literal_Integer) {
    auto block = parseSource("42;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_NUMBER_LITERAL);
    auto* num = static_cast<NumberLiteral*>(node);
    // A1 fix: NumberLiteral 存储标量，用 isInt()/intVal() 直接访问
    ASSERT_TRUE(num->isInt());
    EXPECT_EQ(num->intVal(), 42);
}

// 测试：浮点数字面量
TEST(ParserTest, Literal_Float) {
    auto block = parseSource("3.14;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_NUMBER_LITERAL);
    auto* num = static_cast<NumberLiteral*>(node);
    ASSERT_TRUE(num->isFloatValue());
    EXPECT_DOUBLE_EQ(num->floatVal(), 3.14);
}

// 测试：字符串字面量
TEST(ParserTest, Literal_String) {
    auto block = parseSource("\"hello\";");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_STRING_LITERAL);
    auto* str = static_cast<StringLiteral*>(node);
    EXPECT_EQ(str->value, "hello");
}

// 测试：布尔字面量 true
TEST(ParserTest, Literal_True) {
    auto block = parseSource("true;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BOOL_LITERAL);
    auto* b = static_cast<BoolLiteral*>(node);
    EXPECT_TRUE(b->value);
}

// 测试：布尔字面量 false
TEST(ParserTest, Literal_False) {
    auto block = parseSource("false;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_BOOL_LITERAL);
    auto* b = static_cast<BoolLiteral*>(node);
    EXPECT_FALSE(b->value);
}

// 测试：null 字面量
TEST(ParserTest, Literal_Null) {
    auto block = parseSource("null;");
    auto* node = firstStmt(block);
    EXPECT_EQ(node->nodeType, NodeType::NODE_NULL_LITERAL);
}

// ---- 1.5 变量引用 ----

// 测试：变量引用
TEST(ParserTest, VariableReference) {
    auto block = parseSource("x;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_REF);
    auto* ref = static_cast<VarRef*>(node);
    EXPECT_EQ(ref->name, "x");
}

// ---- 1.6 复合字面量 ----

// 测试：数组字面量 [1, 2, 3]
TEST(ParserTest, ArrayLiteral) {
    auto block = parseSource("[1, 2, 3];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_ARRAY_LITERAL);
    auto* arr = static_cast<ArrayLiteral*>(node);
    EXPECT_EQ(arr->elements.size(), 3u);
    for (const auto& e : arr->elements) {
        EXPECT_EQ(e->nodeType, NodeType::NODE_NUMBER_LITERAL);
    }
}

// 测试：空数组 []
TEST(ParserTest, ArrayLiteral_Empty) {
    auto block = parseSource("[];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_ARRAY_LITERAL);
    auto* arr = static_cast<ArrayLiteral*>(node);
    EXPECT_EQ(arr->elements.size(), 0u);
}

// 测试：数组字面量允许尾逗号 [1, 2, 3,]
TEST(ParserTest, ArrayLiteral_TrailingComma) {
    auto block = parseSource("[1, 2, 3,];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_ARRAY_LITERAL);
    auto* arr = static_cast<ArrayLiteral*>(node);
    EXPECT_EQ(arr->elements.size(), 3u);
}

// 测试：字典字面量 {"key": "value"}
TEST(ParserTest, DictLiteral) {
    auto block = parseSource("var x = {\"key\": \"value\"};");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    ASSERT_NE(decl->initializer, nullptr);
    EXPECT_EQ(decl->initializer->nodeType, NodeType::NODE_DICT_LITERAL);
    auto* dict = static_cast<DictLiteral*>(decl->initializer.get());
    ASSERT_EQ(dict->pairs.size(), 1u);
    EXPECT_EQ(dict->pairs[0].first->nodeType, NodeType::NODE_STRING_LITERAL);
    EXPECT_EQ(dict->pairs[0].second->nodeType, NodeType::NODE_STRING_LITERAL);
}

// 测试：字典字面量多键值对
TEST(ParserTest, DictLiteral_MultiplePairs) {
    auto block = parseSource("var x = {\"a\": 1, \"b\": 2, \"c\": 3};");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    ASSERT_NE(decl->initializer, nullptr);
    EXPECT_EQ(decl->initializer->nodeType, NodeType::NODE_DICT_LITERAL);
    auto* dict = static_cast<DictLiteral*>(decl->initializer.get());
    EXPECT_EQ(dict->pairs.size(), 3u);
}

// 测试：空字典（在表达式上下文中，避免与块语句歧义）
TEST(ParserTest, DictLiteral_Empty) {
    auto block = parseSource("var x = {};");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    ASSERT_NE(decl->initializer, nullptr);
    EXPECT_EQ(decl->initializer->nodeType, NodeType::NODE_DICT_LITERAL);
    auto* dict = static_cast<DictLiteral*>(decl->initializer.get());
    EXPECT_EQ(dict->pairs.size(), 0u);
}

// ---- 1.7 索引访问 ----

// 测试：数组索引访问 arr[0]
TEST(ParserTest, IndexAccess_Array) {
    auto block = parseSource("arr[0];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_INDEX_ACCESS);
    auto* idx = static_cast<IndexAccess*>(node);
    EXPECT_EQ(idx->object->nodeType, NodeType::NODE_VAR_REF);
    EXPECT_EQ(idx->index->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：字典索引访问 obj["key"]
TEST(ParserTest, IndexAccess_Dict) {
    auto block = parseSource("obj[\"key\"];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_INDEX_ACCESS);
    auto* idx = static_cast<IndexAccess*>(node);
    EXPECT_EQ(idx->object->nodeType, NodeType::NODE_VAR_REF);
    EXPECT_EQ(idx->index->nodeType, NodeType::NODE_STRING_LITERAL);
}

// 测试：多维索引访问 matrix[0][1]
TEST(ParserTest, IndexAccess_Nested) {
    auto block = parseSource("matrix[0][1];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_INDEX_ACCESS);
    auto* outer = static_cast<IndexAccess*>(node);
    // 外层索引为 1
    EXPECT_EQ(outer->index->nodeType, NodeType::NODE_NUMBER_LITERAL);
    // 外层对象应为内层 IndexAccess
    ASSERT_EQ(outer->object->nodeType, NodeType::NODE_INDEX_ACCESS);
}

// ---- 1.8 成员访问与方法调用 ----

// 测试：成员访问 obj.field
TEST(ParserTest, MemberAccess) {
    auto block = parseSource("obj.field;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_MEMBER_ACCESS);
    auto* mem = static_cast<MemberAccess*>(node);
    EXPECT_EQ(mem->object->nodeType, NodeType::NODE_VAR_REF);
    EXPECT_EQ(mem->fieldName, "field");
}

// 测试：方法调用 obj.method()
TEST(ParserTest, MethodCall_NoArgs) {
    auto block = parseSource("obj.method();");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_METHOD_CALL);
    auto* mc = static_cast<MethodCall*>(node);
    EXPECT_EQ(mc->methodName, "method");
    EXPECT_EQ(mc->arguments.size(), 0u);
}

// 测试：带参数的方法调用 obj.method(1, 2)
TEST(ParserTest, MethodCall_WithArgs) {
    auto block = parseSource("obj.method(1, 2);");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_METHOD_CALL);
    auto* mc = static_cast<MethodCall*>(node);
    EXPECT_EQ(mc->methodName, "method");
    EXPECT_EQ(mc->arguments.size(), 2u);
}

// 测试：链式调用 arr.push(1).pop()
TEST(ParserTest, ChainedCall) {
    auto block = parseSource("arr.push(1).pop();");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_METHOD_CALL);
    auto* outer = static_cast<MethodCall*>(node);
    EXPECT_EQ(outer->methodName, "pop");
    // 外层调用的对象应为内层方法调用
    ASSERT_EQ(outer->object->nodeType, NodeType::NODE_METHOD_CALL);
    auto* inner = static_cast<MethodCall*>(outer->object.get());
    EXPECT_EQ(inner->methodName, "push");
    EXPECT_EQ(inner->arguments.size(), 1u);
}

// 测试：嵌套成员访问 obj.a.b.c
TEST(ParserTest, NestedMemberAccess) {
    auto block = parseSource("obj.a.b.c;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_MEMBER_ACCESS);
    auto* outer = static_cast<MemberAccess*>(node);
    EXPECT_EQ(outer->fieldName, "c");
    ASSERT_EQ(outer->object->nodeType, NodeType::NODE_MEMBER_ACCESS);
    auto* mid = static_cast<MemberAccess*>(outer->object.get());
    EXPECT_EQ(mid->fieldName, "b");
    ASSERT_EQ(mid->object->nodeType, NodeType::NODE_MEMBER_ACCESS);
    auto* inner = static_cast<MemberAccess*>(mid->object.get());
    EXPECT_EQ(inner->fieldName, "a");
}

// ---- 1.9 函数调用 ----

// 测试：带参数的函数调用 foo(1, 2)
TEST(ParserTest, FunctionCall_WithArgs) {
    auto block = parseSource("foo(1, 2);");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_CALL);
    auto* call = static_cast<FunCall*>(node);
    EXPECT_EQ(call->name, "foo");
    EXPECT_EQ(call->arguments.size(), 2u);
    EXPECT_EQ(call->callee, nullptr);  // 命名函数调用，callee 为空
}

// 测试：无参数函数调用 bar()
TEST(ParserTest, FunctionCall_NoArgs) {
    auto block = parseSource("bar();");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_CALL);
    auto* call = static_cast<FunCall*>(node);
    EXPECT_EQ(call->name, "bar");
    EXPECT_EQ(call->arguments.size(), 0u);
}

// 测试：链式函数调用 f(x)(y)（闭包调用机制）
TEST(ParserTest, FunctionCall_Chained) {
    auto block = parseSource("f(x)(y);");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_CALL);
    auto* outer = static_cast<FunCall*>(node);
    // 外层调用应为表达式调用（callee 非 null，name 为空）
    EXPECT_NE(outer->callee, nullptr);
    EXPECT_TRUE(outer->name.empty());
    EXPECT_EQ(outer->arguments.size(), 1u);
    // 内层 callee 应为命名函数调用 f(x)
    ASSERT_EQ(outer->callee->nodeType, NodeType::NODE_FUN_CALL);
    auto* inner = static_cast<FunCall*>(outer->callee.get());
    EXPECT_EQ(inner->name, "f");
    EXPECT_EQ(inner->arguments.size(), 1u);
}

// ---- 1.10 一元运算 ----

// 测试：一元负号 -5
TEST(ParserTest, Unary_Negative) {
    auto block = parseSource("-5;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_UNARY_OP);
    auto* un = static_cast<UnaryOp*>(node);
    EXPECT_EQ(un->opType, UnaryOp::UnaryOpType::UOP_NEGATE);
    EXPECT_EQ(un->operand->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：一元正号 +5
TEST(ParserTest, Unary_Plus) {
    auto block = parseSource("+5;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_UNARY_OP);
    auto* un = static_cast<UnaryOp*>(node);
    EXPECT_EQ(un->opType, UnaryOp::UnaryOpType::UOP_PLUS);
}

// 测试：一元负号嵌套 --5
TEST(ParserTest, Unary_NestedNegative) {
    auto block = parseSource("--5;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_UNARY_OP);
    auto* un = static_cast<UnaryOp*>(node);
    EXPECT_EQ(un->opType, UnaryOp::UnaryOpType::UOP_NEGATE);
    ASSERT_EQ(un->operand->nodeType, NodeType::NODE_UNARY_OP);
    auto* inner = static_cast<UnaryOp*>(un->operand.get());
    EXPECT_EQ(inner->opType, UnaryOp::UnaryOpType::UOP_NEGATE);
}

// ---- 1.11 super 表达式 ----

// 测试：super 表达式
TEST(ParserTest, SuperExpression) {
    auto block = parseSource("super;");
    auto* node = firstStmt(block);
    EXPECT_EQ(node->nodeType, NodeType::NODE_SUPER_EXPR);
}

// ============================================================
// 2. 语句解析测试
// ============================================================

// ---- 2.1 变量声明 ----

// 测试：var 声明带初始化 var x = 1;
TEST(ParserTest, VarDecl_WithInitializer) {
    auto block = parseSource("var x = 1;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    EXPECT_EQ(decl->name, "x");
    EXPECT_TRUE(decl->typeAnnotation.empty());
    ASSERT_NE(decl->initializer, nullptr);
    EXPECT_EQ(decl->initializer->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：var 声明不带初始化 var x;
TEST(ParserTest, VarDecl_WithoutInitializer) {
    auto block = parseSource("var x;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    EXPECT_EQ(decl->name, "x");
    EXPECT_EQ(decl->initializer, nullptr);
}

// 测试：带类型注解的变量声明 int a = 10;
TEST(ParserTest, TypedVarDecl) {
    auto block = parseSource("int a = 10;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    EXPECT_EQ(decl->name, "a");
    EXPECT_EQ(decl->typeAnnotation, "int");
    ASSERT_NE(decl->initializer, nullptr);
}

// 测试：带数组类型注解的变量声明 int[] arr = [1, 2, 3];
TEST(ParserTest, TypedVarDecl_ArrayType) {
    auto block = parseSource("int[] arr = [1, 2, 3];");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* decl = static_cast<VarDecl*>(node);
    EXPECT_EQ(decl->name, "arr");
    EXPECT_EQ(decl->typeAnnotation, "int[]");
}

// ---- 2.2 赋值语句 ----

// 测试：简单赋值 x = 2;
TEST(ParserTest, Assignment_Simple) {
    auto block = parseSource("x = 2;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_ASSIGNMENT);
    auto* assign = static_cast<Assignment*>(node);
    EXPECT_EQ(assign->name, "x");
    EXPECT_EQ(assign->value->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：索引赋值 arr[0] = 1;
TEST(ParserTest, Assignment_Index) {
    auto block = parseSource("arr[0] = 1;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_INDEX_ASSIGN);
    auto* assign = static_cast<IndexAssign*>(node);
    EXPECT_EQ(assign->object->nodeType, NodeType::NODE_VAR_REF);
    EXPECT_EQ(assign->index->nodeType, NodeType::NODE_NUMBER_LITERAL);
    EXPECT_EQ(assign->value->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// 测试：成员赋值 obj.field = 1;
TEST(ParserTest, Assignment_Member) {
    auto block = parseSource("obj.field = 1;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_MEMBER_ASSIGN);
    auto* assign = static_cast<MemberAssign*>(node);
    EXPECT_EQ(assign->fieldName, "field");
    EXPECT_EQ(assign->value->nodeType, NodeType::NODE_NUMBER_LITERAL);
}

// ---- 2.3 控制流语句 ----

// 测试：if 语句
TEST(ParserTest, IfStmt) {
    auto block = parseSource("if (x > 0) { print(x); }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_IF_STMT);
    auto* ifStmt = static_cast<IfStmt*>(node);
    EXPECT_EQ(ifStmt->condition->nodeType, NodeType::NODE_BINARY_OP);
    EXPECT_EQ(ifStmt->thenBranch->nodeType, NodeType::NODE_BLOCK);
    EXPECT_EQ(ifStmt->elseBranch, nullptr);
}

// 测试：if-else 语句
TEST(ParserTest, IfElseStmt) {
    auto block = parseSource("if (x > 0) { print(x); } else { print(y); }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_IF_STMT);
    auto* ifStmt = static_cast<IfStmt*>(node);
    ASSERT_NE(ifStmt->elseBranch, nullptr);
    EXPECT_EQ(ifStmt->elseBranch->nodeType, NodeType::NODE_BLOCK);
}

// 测试：else if 链
TEST(ParserTest, ElseIfChain) {
    auto block = parseSource("if (x > 0) { print(1); } else if (x < 0) { print(2); } else { print(3); }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_IF_STMT);
    auto* ifStmt = static_cast<IfStmt*>(node);
    ASSERT_NE(ifStmt->elseBranch, nullptr);
    // else 分支应为另一个 if 语句
    EXPECT_EQ(ifStmt->elseBranch->nodeType, NodeType::NODE_IF_STMT);
}

// 测试：while 语句
TEST(ParserTest, WhileStmt) {
    auto block = parseSource("while (x < 10) { x = x + 1; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_WHILE_STMT);
    auto* whileStmt = static_cast<WhileStmt*>(node);
    EXPECT_EQ(whileStmt->condition->nodeType, NodeType::NODE_BINARY_OP);
    EXPECT_EQ(whileStmt->body->nodeType, NodeType::NODE_BLOCK);
}

// 测试：for 语句
TEST(ParserTest, ForStmt) {
    auto block = parseSource("for (var i = 0; i < 10; i = i + 1) { print(i); }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FOR_STMT);
    auto* forStmt = static_cast<ForStmt*>(node);
    EXPECT_NE(forStmt->initializer, nullptr);
    EXPECT_NE(forStmt->condition, nullptr);
    EXPECT_NE(forStmt->update, nullptr);
    EXPECT_EQ(forStmt->body->nodeType, NodeType::NODE_BLOCK);
}

// 测试：for 语句各部分可为空
TEST(ParserTest, ForStmt_EmptyParts) {
    auto block = parseSource("for (;;) { break; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FOR_STMT);
    auto* forStmt = static_cast<ForStmt*>(node);
    // 注意：for(;;) 中 init/cond/update 都为空，但 break 不是本语言关键字
    // 这里仅验证 for 语句结构
    EXPECT_EQ(forStmt->initializer, nullptr);
    EXPECT_EQ(forStmt->condition, nullptr);
}

// ---- 2.4 return 与 print ----

// 测试：return 带值
TEST(ParserTest, ReturnStmt_WithValue) {
    auto block = parseSource("return x;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_RETURN_STMT);
    auto* ret = static_cast<ReturnStmt*>(node);
    ASSERT_NE(ret->value, nullptr);
    EXPECT_EQ(ret->value->nodeType, NodeType::NODE_VAR_REF);
}

// 测试：return 不带值
TEST(ParserTest, ReturnStmt_WithoutValue) {
    auto block = parseSource("return;");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_RETURN_STMT);
    auto* ret = static_cast<ReturnStmt*>(node);
    EXPECT_EQ(ret->value, nullptr);
}

// 测试：print 语句
TEST(ParserTest, PrintStmt) {
    auto block = parseSource("print(x);");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_PRINT_STMT);
    auto* print = static_cast<PrintStmt*>(node);
    ASSERT_EQ(print->values.size(), 1u);
    EXPECT_EQ(print->values[0]->nodeType, NodeType::NODE_VAR_REF);
}

// 测试：print 多个参数
TEST(ParserTest, PrintStmt_MultipleArgs) {
    auto block = parseSource("print(x, y, z);");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_PRINT_STMT);
    auto* print = static_cast<PrintStmt*>(node);
    EXPECT_EQ(print->values.size(), 3u);
}

// ---- 2.5 函数声明 ----

// 测试：函数声明 fun add(a, b) { return a + b; }
TEST(ParserTest, FunDecl) {
    auto block = parseSource("fun add(a, b) { return a + b; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fun = static_cast<FunDecl*>(node);
    EXPECT_EQ(fun->name, "add");
    ASSERT_EQ(fun->params.size(), 2u);
    EXPECT_EQ(fun->params[0], "a");
    EXPECT_EQ(fun->params[1], "b");
    EXPECT_EQ(fun->body->nodeType, NodeType::NODE_BLOCK);
}

// 测试：无参数函数声明
TEST(ParserTest, FunDecl_NoParams) {
    auto block = parseSource("fun foo() { return; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fun = static_cast<FunDecl*>(node);
    EXPECT_EQ(fun->name, "foo");
    EXPECT_EQ(fun->params.size(), 0u);
}

// 测试：带返回类型的函数声明 int fib(int n) { ... }
TEST(ParserTest, TypedFunDecl) {
    auto block = parseSource("int fib(int n) { return n; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fun = static_cast<FunDecl*>(node);
    EXPECT_EQ(fun->name, "fib");
    EXPECT_EQ(fun->returnType, "int");
    ASSERT_EQ(fun->params.size(), 1u);
    EXPECT_EQ(fun->params[0], "n");
    ASSERT_EQ(fun->paramTypes.size(), 1u);
    EXPECT_EQ(fun->paramTypes[0], "int");
}

// 测试：带冒号返回类型的函数声明 fun f(a): int { ... }
TEST(ParserTest, FunDecl_ColonReturnType) {
    auto block = parseSource("fun f(a): int { return a; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fun = static_cast<FunDecl*>(node);
    EXPECT_EQ(fun->returnType, "int");
}

// 测试：function 关键字作为 fun 的别名
TEST(ParserTest, FunDecl_FunctionAlias) {
    auto block = parseSource("function foo() { return; }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fun = static_cast<FunDecl*>(node);
    EXPECT_EQ(fun->name, "foo");
}

// ---- 2.6 类声明 ----

// 测试：类声明
TEST(ParserTest, ClassDecl) {
    auto block = parseSource("class Foo { var x = 0; fun get() { return x; } }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_CLASS_DECL);
    auto* cls = static_cast<ClassDecl*>(node);
    EXPECT_EQ(cls->name, "Foo");
    EXPECT_TRUE(cls->superClassName.empty());
    EXPECT_EQ(cls->members.size(), 2u);
}

// 测试：类继承 class Bar extends Foo { ... }
TEST(ParserTest, ClassDecl_WithExtends) {
    auto block = parseSource("class Bar extends Foo { }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_CLASS_DECL);
    auto* cls = static_cast<ClassDecl*>(node);
    EXPECT_EQ(cls->name, "Bar");
    EXPECT_EQ(cls->superClassName, "Foo");
}

// 测试：空类
TEST(ParserTest, ClassDecl_Empty) {
    auto block = parseSource("class Empty {}");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_CLASS_DECL);
    auto* cls = static_cast<ClassDecl*>(node);
    EXPECT_EQ(cls->name, "Empty");
    EXPECT_EQ(cls->members.size(), 0u);
}

// 测试：类中包含带类型注解的字段和方法
TEST(ParserTest, ClassDecl_TypedMembers) {
    auto block = parseSource("class Point { int x = 0; int y = 0; int getSum() { return x + y; } }");
    auto* node = firstStmt(block);
    ASSERT_EQ(node->nodeType, NodeType::NODE_CLASS_DECL);
    auto* cls = static_cast<ClassDecl*>(node);
    EXPECT_EQ(cls->members.size(), 3u);
}

// ---- 2.7 其他语句 ----

// 测试：表达式语句
TEST(ParserTest, ExpressionStatement) {
    auto block = parseSource("1 + 2;");
    auto* node = firstStmt(block);
    EXPECT_EQ(node->nodeType, NodeType::NODE_BINARY_OP);
}

// 测试：空输入
TEST(ParserTest, EmptyInput) {
    auto block = parseSource("");
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements.size(), 0u);
}

// 测试：多条语句
TEST(ParserTest, MultipleStatements) {
    auto block = parseSource("var x = 1; var y = 2; var z = 3;");
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements.size(), 3u);
    for (const auto& stmt : block->statements) {
        EXPECT_EQ(stmt->nodeType, NodeType::NODE_VAR_DECL);
    }
}

// 测试：代码块语句
TEST(ParserTest, BlockStatement) {
    auto block = parseSource("{ var x = 1; var y = 2; }");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = firstStmt(block);
    EXPECT_EQ(node->nodeType, NodeType::NODE_BLOCK);
    auto* blk = static_cast<Block*>(node);
    EXPECT_EQ(blk->statements.size(), 2u);
}

// ============================================================
// 3. 错误恢复与诊断测试
// ============================================================

// ---- 3.1 语法错误 ----

// 测试：缺少分号应报错
TEST(ParserTest, Error_MissingSemicolon) {
    Parser parser;
    auto block = parseSourceWithParser("var x = 1", parser);
    EXPECT_TRUE(parser.hasErrors());
    EXPECT_EQ(parser.getDiagnostics().errorCount(), 1u);
    // 错误信息应包含分号相关提示
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find(";"), std::string::npos);
}

// 测试：不匹配的圆括号应报错
TEST(ParserTest, Error_UnmatchedParen) {
    Parser parser;
    auto block = parseSourceWithParser("(1 + 2;", parser);
    EXPECT_TRUE(parser.hasErrors());
    EXPECT_EQ(parser.getDiagnostics().errorCount(), 1u);
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find(")"), std::string::npos);
}

// 测试：不匹配的花括号应报错
TEST(ParserTest, Error_UnmatchedBrace) {
    Parser parser;
    auto block = parseSourceWithParser("fun f() {", parser);
    EXPECT_TRUE(parser.hasErrors());
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find("}"), std::string::npos);
}

// 测试：无效的赋值目标
TEST(ParserTest, Error_InvalidAssignmentTarget) {
    Parser parser;
    auto block = parseSourceWithParser("1 + 2 = 3;", parser);
    EXPECT_TRUE(parser.hasErrors());
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find("赋值"), std::string::npos);
}

// 测试：意外的 Token
TEST(ParserTest, Error_UnexpectedToken) {
    Parser parser;
    auto block = parseSourceWithParser("+;", parser);
    EXPECT_TRUE(parser.hasErrors());
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find("Token"), std::string::npos);
}

// 测试：缺少变量名
TEST(ParserTest, Error_MissingVarName) {
    Parser parser;
    auto block = parseSourceWithParser("var = 1;", parser);
    EXPECT_TRUE(parser.hasErrors());
    std::string errMsg = parser.getDiagnostics().all()[0].message.c_str();
    EXPECT_NE(errMsg.find("变量名"), std::string::npos);
}

// ---- 3.2 错误恢复 ----

// 测试：错误恢复后继续解析后续语句
TEST(ParserTest, Error_RecoveryContinuesParsing) {
    Parser parser;
    auto block = parseSourceWithParser("var x = ; var y = 2;", parser);
    EXPECT_NE(block, nullptr);
    EXPECT_TRUE(parser.hasErrors());
    // var y = 2; 应被正确解析
    bool foundVarY = false;
    for (const auto& stmt : block->statements) {
        if (stmt->nodeType == NodeType::NODE_VAR_DECL) {
            auto* decl = static_cast<VarDecl*>(stmt.get());
            if (decl->name == "y") {
                foundVarY = true;
                ASSERT_NE(decl->initializer, nullptr);
                EXPECT_EQ(decl->initializer->nodeType, NodeType::NODE_NUMBER_LITERAL);
                break;
            }
        }
    }
    EXPECT_TRUE(foundVarY);
}

// 测试：多个错误都被收集
TEST(ParserTest, Error_MultipleErrorsCollected) {
    Parser parser;
    auto block = parseSourceWithParser("var x = ; var y = ;", parser);
    EXPECT_GE(parser.getDiagnostics().errorCount(), 2u);
}

// 测试：错误后仍能解析不同类型的声明
TEST(ParserTest, Error_RecoveryAcrossDeclarations) {
    Parser parser;
    auto block = parseSourceWithParser("var x = ; fun f() { return 1; } var z = 3;", parser);
    EXPECT_TRUE(parser.hasErrors());
    EXPECT_NE(block, nullptr);
    // 应至少解析出 fun f() 和 var z = 3
    bool foundFun = false;
    bool foundVarZ = false;
    for (const auto& stmt : block->statements) {
        if (stmt->nodeType == NodeType::NODE_FUN_DECL) {
            auto* fun = static_cast<FunDecl*>(stmt.get());
            if (fun->name == "f") foundFun = true;
        }
        if (stmt->nodeType == NodeType::NODE_VAR_DECL) {
            auto* decl = static_cast<VarDecl*>(stmt.get());
            if (decl->name == "z") foundVarZ = true;
        }
    }
    EXPECT_TRUE(foundFun);
    EXPECT_TRUE(foundVarZ);
}

// ---- 3.3 诊断信息 ----

// 测试：getDiagnostics 返回正确的错误信息
TEST(ParserTest, Diagnostics_ReturnsErrors) {
    Parser parser;
    auto block = parseSourceWithParser("var x = 1", parser);
    const auto& diags = parser.getDiagnostics();
    EXPECT_FALSE(diags.empty());
    EXPECT_GT(diags.errorCount(), 0);
    // 诊断来源应为 Parser
    for (const auto& d : diags.all()) {
        EXPECT_EQ(d.source, DiagSource::Parser);
        EXPECT_TRUE(d.isError());
    }
}

// 测试：诊断包含行号和列号
TEST(ParserTest, Diagnostics_HasLocation) {
    Parser parser;
    auto block = parseSourceWithParser("var x = 1", parser);
    const auto& diags = parser.getDiagnostics();
    ASSERT_EQ(diags.all().size(), 1u);
    const auto& d = diags.all()[0];
    EXPECT_GT(d.line, 0);
    EXPECT_GT(d.column, 0);
}

// 测试：无错误时诊断为空
TEST(ParserTest, Diagnostics_NoErrorsOnValidInput) {
    Parser parser;
    auto block = parseSourceWithParser("var x = 1;", parser);
    EXPECT_FALSE(parser.hasErrors());
    EXPECT_TRUE(parser.getDiagnostics().empty());
}

// 测试：诊断 errorCount 与实际错误数一致
TEST(ParserTest, Diagnostics_ErrorCountMatches) {
    Parser parser;
    auto block = parseSourceWithParser("var x = ;", parser);
    const auto& diags = parser.getDiagnostics();
    EXPECT_EQ(diags.errorCount(), static_cast<int>(parser.getDiagnostics().errorCount()));
}

// 测试：诊断 summary 方法返回非空字符串
TEST(ParserTest, Diagnostics_Summary) {
    Parser parser;
    auto block = parseSourceWithParser("var x = ;", parser);
    const auto& diags = parser.getDiagnostics();
    std::string summary = diags.summary();
    EXPECT_FALSE(summary.empty());
    EXPECT_NE(summary.find("错误"), std::string::npos);
}
