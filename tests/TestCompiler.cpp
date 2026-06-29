// ============================================================
// Compiler 模块单元测试
// ------------------------------------------------------------
// 测试流程：源码 → Lexer::scan() → Parser::parse() → Compiler::compile()
// 验证 BytecodeChunk 中的指令序列、常量池内容、全局槽位分配。
// 覆盖功能点：
//   1. 基本表达式字节码生成（字面量、算术）
//   2. 变量声明与赋值
//   3. 控制流语句（if/while/for）
//   4. 函数定义与调用
//   5. 类定义与实例化
//   6. 常量池内容与去重
//   7. 全局槽位分配
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "ast/ASTNode.h"
#include "interpreter/Value.h"

#include <string>
#include <vector>

// ============================================================
// 辅助函数
// ============================================================

/// 编译源码，返回 CompileResult
static CompileResult compileSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_NE(ast, nullptr);
    if (!ast) return CompileResult{};
    Compiler compiler;
    return compiler.compile(*ast);
}

/// 按指令边界遍历字节码，收集所有 OpCode 序列
/// D7 fix: 使用 instructionSizeAt 统一处理 OP_CLOSURE 变长指令
static std::vector<OpCode> collectOps(const BytecodeChunk& chunk) {
    std::vector<OpCode> ops;
    size_t offset = 0;
    while (offset < chunk.code.size()) {
        OpCode op = static_cast<OpCode>(chunk.code[offset]);
        ops.push_back(op);
        offset += chunk.instructionSizeAt(offset);
    }
    return ops;
}

/// 检查字节码是否包含指定 OpCode
static bool containsOp(const BytecodeChunk& chunk, OpCode op) {
    auto ops = collectOps(chunk);
    for (auto o : ops) {
        if (o == op) return true;
    }
    return false;
}

/// 检查 OpCode 序列是否包含指定子序列
static bool containsOpSequence(const BytecodeChunk& chunk, const std::vector<OpCode>& seq) {
    auto ops = collectOps(chunk);
    if (seq.empty() || ops.size() < seq.size()) return false;
    for (size_t i = 0; i + seq.size() <= ops.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < seq.size(); ++j) {
            if (ops[i + j] != seq[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/// 检查 OpCode 序列是否以指定前缀开头
static bool startsWithOps(const BytecodeChunk& chunk, const std::vector<OpCode>& prefix) {
    auto ops = collectOps(chunk);
    if (ops.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (ops[i] != prefix[i]) return false;
    }
    return true;
}

/// 从字节码偏移处读取 16 位小端序整数
static uint16_t readShort(const std::vector<uint8_t>& code, size_t offset) {
    if (offset + 1 >= code.size()) return 0xFFFF;
    return code[offset] | (code[offset + 1] << 8);
}

// ============================================================
// 1. 基本表达式字节码生成
// ============================================================

/// 整数字面量 `42` → 验证生成 OP_INT，常量池包含 42
TEST(CompilerBasicTest, IntegerLiteral) {
    auto result = compileSource("42;");
    auto& chunk = result.mainChunk;
    // 顶层表达式语句：OP_INT + idx, OP_POP, 然后是尾声 OP_NULL, OP_RETURN
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_INT}));
    // 常量池应包含整数 42
    ASSERT_FALSE(chunk.constants.empty());
    EXPECT_TRUE(chunk.constants[0].isInt());
    EXPECT_EQ(chunk.constants[0].intVal(), 42);
}

/// 浮点字面量 `3.14` → 验证生成 OP_FLOAT
TEST(CompilerBasicTest, FloatLiteral) {
    auto result = compileSource("3.14;");
    auto& chunk = result.mainChunk;
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_FLOAT}));
    ASSERT_FALSE(chunk.constants.empty());
    EXPECT_TRUE(chunk.constants[0].isFloat());
    EXPECT_DOUBLE_EQ(chunk.constants[0].floatVal(), 3.14);
}

/// 字符串字面量 `"hello"` → 验证生成 OP_STRING
TEST(CompilerBasicTest, StringLiteral) {
    auto result = compileSource("\"hello\";");
    auto& chunk = result.mainChunk;
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_STRING}));
    ASSERT_FALSE(chunk.constants.empty());
    EXPECT_TRUE(chunk.constants[0].isString());
    EXPECT_EQ(chunk.constants[0].stringVal(), "hello");
}

/// 布尔字面量 `true` → 验证生成 OP_TRUE
TEST(CompilerBasicTest, TrueLiteral) {
    auto result = compileSource("true;");
    auto& chunk = result.mainChunk;
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_TRUE}));
}

/// 布尔字面量 `false` → 验证生成 OP_FALSE
TEST(CompilerBasicTest, FalseLiteral) {
    auto result = compileSource("false;");
    auto& chunk = result.mainChunk;
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_FALSE}));
}

/// null 字面量 → 验证生成 OP_NULL
TEST(CompilerBasicTest, NullLiteral) {
    auto result = compileSource("null;");
    auto& chunk = result.mainChunk;
    // null 表达式语句：OP_NULL, OP_POP, OP_NULL(尾声), OP_RETURN
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_NULL, OpCode::OP_POP}));
}

/// 算术表达式 `1 + 2` → 常量折叠为 OP_INT 3
/// 注意：编译器启用常量折叠，两个字面量运算在编译期求值
TEST(CompilerBasicTest, ConstantFoldedAddition) {
    auto result = compileSource("1 + 2;");
    auto& chunk = result.mainChunk;
    // 常量折叠后生成单个 OP_INT，常量值为 3
    ASSERT_TRUE(startsWithOps(chunk, {OpCode::OP_INT}));
    ASSERT_FALSE(chunk.constants.empty());
    EXPECT_TRUE(chunk.constants[0].isInt());
    EXPECT_EQ(chunk.constants[0].intVal(), 3);
    // 不应生成 OP_ADD（已被折叠）
    EXPECT_FALSE(containsOp(chunk, OpCode::OP_ADD));
}

/// 变量算术表达式 → 验证生成 OP_ADD 序列
/// 使用变量避免常量折叠，验证完整的二元运算字节码
TEST(CompilerBasicTest, VariableAddition) {
    auto result = compileSource("var a = 1; var b = 2; a + b;");
    auto& chunk = result.mainChunk;
    // 顶层 var 预扫描分配全局槽位，使用 OP_DEFINE_GLOBAL
    // a + b → OP_GET_GLOBAL, OP_GET_GLOBAL, OP_ADD
    EXPECT_TRUE(containsOpSequence(chunk, {OpCode::OP_GET_GLOBAL, OpCode::OP_GET_GLOBAL, OpCode::OP_ADD}));
}

// ============================================================
// 2. 变量声明与赋值
// ============================================================

/// `var x = 1;` → 验证生成 OP_INT OP_DEFINE_GLOBAL
TEST(CompilerVariableTest, VarDeclWithInit) {
    auto result = compileSource("var x = 1;");
    auto& chunk = result.mainChunk;
    // 顶层 var 预扫描分配全局槽位，使用 OP_DEFINE_GLOBAL
    EXPECT_TRUE(containsOpSequence(chunk, {OpCode::OP_INT, OpCode::OP_DEFINE_GLOBAL}));
    // 常量池应包含整数 1
    ASSERT_FALSE(chunk.constants.empty());
    bool found = false;
    for (auto& v : chunk.constants) {
        if (v.isInt() && v.intVal() == 1) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

/// `var x;` → 验证生成 OP_NULL OP_DEFINE_GLOBAL
TEST(CompilerVariableTest, VarDeclWithoutInit) {
    auto result = compileSource("var x;");
    auto& chunk = result.mainChunk;
    // 无初始化器时先 push null，再定义全局变量
    EXPECT_TRUE(containsOpSequence(chunk, {OpCode::OP_NULL, OpCode::OP_DEFINE_GLOBAL}));
}

/// `var x; x = 1;` → 验证赋值生成 OP_INT OP_DUP OP_SET_GLOBAL
TEST(CompilerVariableTest, VarAssignment) {
    auto result = compileSource("var x; x = 1;");
    auto& chunk = result.mainChunk;
    // 赋值表达式：编译值 → DUP → SET_GLOBAL
    EXPECT_TRUE(containsOpSequence(chunk, {OpCode::OP_INT, OpCode::OP_DUP, OpCode::OP_SET_GLOBAL}));
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_SET_GLOBAL));
}

/// 变量引用 `x` → 验证生成 OP_GET_GLOBAL
TEST(CompilerVariableTest, VarRef) {
    auto result = compileSource("var x = 1; x;");
    auto& chunk = result.mainChunk;
    // 顶层变量引用使用 OP_GET_GLOBAL
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_GET_GLOBAL));
}

// ============================================================
// 3. 控制流语句
// ============================================================

/// `if (x) { print(1); }` → 验证生成 OP_JUMP_IF_FALSE ... OP_PRINT OP_JUMP
/// 注：使用非常量条件 x，以验证完整的 if 跳转结构（C17 死代码消除会跳过常量条件）
TEST(CompilerControlFlowTest, IfStmt) {
    auto result = compileSource("var x = 1; if (x) { print(1); }");
    auto& chunk = result.mainChunk;
    // 条件跳转
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_JUMP_IF_FALSE));
    // then 分支中的 print
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_PRINT));
    // 跳过 else 的无条件跳转
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_JUMP));
}

/// `if (true) { print(1); } else { print(2); }` → C17 死代码消除：
/// 常量 true 条件下仅编译 then 分支，不生成条件求值与跳转指令，else 分支被消除
TEST(CompilerControlFlowTest, IfStmtConstantFoldTrue) {
    auto result = compileSource("if (true) { print(1); } else { print(2); }");
    auto& chunk = result.mainChunk;
    // then 分支的 print(1) 被保留
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_PRINT));
    // 常量条件消除：不生成条件求值与跳转
    EXPECT_FALSE(containsOp(chunk, OpCode::OP_JUMP_IF_FALSE));
    EXPECT_FALSE(containsOp(chunk, OpCode::OP_TRUE));
}

/// `if (false) { print(1); } else { print(2); }` → C17 死代码消除：
/// 常量 false 条件下仅编译 else 分支，then 分支被消除
TEST(CompilerControlFlowTest, IfStmtConstantFoldFalse) {
    auto result = compileSource("if (false) { print(1); } else { print(2); }");
    auto& chunk = result.mainChunk;
    // else 分支的 print(2) 被保留
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_PRINT));
    // 常量条件消除：不生成跳转，then 分支的 OP_PRINT 数量不应因 print(1) 独有...
    // 这里验证不生成条件跳转即可
    EXPECT_FALSE(containsOp(chunk, OpCode::OP_JUMP_IF_FALSE));
}

/// `while (true) { print(1); }` → 验证生成 OP_LOOP 和 OP_JUMP_IF_FALSE
TEST(CompilerControlFlowTest, WhileStmt) {
    auto result = compileSource("while (true) { print(1); }");
    auto& chunk = result.mainChunk;
    // 条件跳转
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_JUMP_IF_FALSE));
    // 回跳指令
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_LOOP));
    // 循环体中的 print
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_PRINT));
}

/// `for` 循环 → 验证跳转指令
TEST(CompilerControlFlowTest, ForStmt) {
    auto result = compileSource("for (var i = 0; i < 1; i = i + 1) { print(i); }");
    auto& chunk = result.mainChunk;
    // for 循环条件跳转
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_JUMP_IF_FALSE));
    // 回跳指令
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_LOOP));
    // 循环体中的 print
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_PRINT));
    // 比较运算 OP_LESS (i < 1)
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_LESS));
}

// ============================================================
// 4. 函数定义与调用
// ============================================================

/// `fun f() { return 1; }` → 验证生成 OP_CLOSURE 和函数体字节码
TEST(CompilerFunctionTest, FunDecl) {
    auto result = compileSource("fun f() { return 1; }");
    auto& chunk = result.mainChunk;
    // 主 chunk 中应生成 OP_CLOSURE
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_CLOSURE));
    // 函数体应存在于 functionChunks 中
    auto it = result.functionChunks.find("f");
    ASSERT_NE(it, result.functionChunks.end());
    // 函数 chunk 名称和参数个数
    EXPECT_EQ(it->second.name, "f");
    EXPECT_EQ(it->second.arity, 0);
}

/// `f()` → 验证生成 OP_CALL
TEST(CompilerFunctionTest, FunCall) {
    auto result = compileSource("fun f() { return 1; } f();");
    auto& chunk = result.mainChunk;
    // 函数调用应生成 OP_CALL
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_CALL));
}

/// `return 1;` → 验证函数体生成 OP_RETURN
TEST(CompilerFunctionTest, ReturnStmt) {
    auto result = compileSource("fun f() { return 1; }");
    auto& chunk = result.mainChunk;
    // 函数体应存在于 functionChunks 中
    auto it = result.functionChunks.find("f");
    ASSERT_NE(it, result.functionChunks.end());
    auto& funChunk = it->second;
    // return 1 → OP_INT, OP_RETURN
    EXPECT_TRUE(containsOp(funChunk, OpCode::OP_INT));
    EXPECT_TRUE(containsOp(funChunk, OpCode::OP_RETURN));
    // 函数体末尾还有隐式的 OP_NULL, OP_RETURN
    auto ops = collectOps(funChunk);
    int returnCount = 0;
    for (auto op : ops) {
        if (op == OpCode::OP_RETURN) returnCount++;
    }
    // 显式 return + 隐式 return
    EXPECT_GE(returnCount, 2);
}

// ============================================================
// 5. 类定义
// ============================================================

/// `class Foo { var x = 1; }` → 验证生成 OP_DEFINE_CLASS 和 OP_INIT_FIELD
TEST(CompilerClassTest, ClassDecl) {
    auto result = compileSource("class Foo { var x = 1; }");
    auto& chunk = result.mainChunk;
    // 类定义指令
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_DEFINE_CLASS));
    // 字段初始化指令
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_INIT_FIELD));
    // 类构造指令（创建模板实例）
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_CLASS_NEW));
    // 常量池应包含类名 "Foo" 和字段名 "x"
    bool foundFoo = false, foundX = false;
    for (auto& v : chunk.constants) {
        if (v.isString()) {
            if (v.stringVal() == "Foo") foundFoo = true;
            if (v.stringVal() == "x") foundX = true;
        }
    }
    EXPECT_TRUE(foundFoo);
    EXPECT_TRUE(foundX);
}

/// 类实例化 `Foo()` → 验证生成 OP_CALL（VM 运行时处理类构造）
TEST(CompilerClassTest, ClassInstantiation) {
    auto result = compileSource("class Foo { var x = 1; } Foo();");
    auto& chunk = result.mainChunk;
    // Foo() 编译为 OP_CALL，VM 在运行时检测到名称是类时执行实例化
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_CALL));
}

// ============================================================
// 6. 常量池内容验证
// ============================================================

/// 验证 BytecodeChunk::constants 包含正确的常量值
TEST(CompilerConstantPoolTest, ConstantValues) {
    auto result = compileSource("var s = \"world\"; var n = 42; var f = 2.5;");
    auto& chunk = result.mainChunk;
    // 常量池应包含字符串 "world"、整数 42、浮点 2.5
    bool foundStr = false, foundInt = false, foundFloat = false;
    for (auto& v : chunk.constants) {
        if (v.isString() && v.stringVal() == "world") foundStr = true;
        if (v.isInt() && v.intVal() == 42) foundInt = true;
        if (v.isFloat() && v.floatVal() == 2.5) foundFloat = true;
    }
    EXPECT_TRUE(foundStr);
    EXPECT_TRUE(foundInt);
    EXPECT_TRUE(foundFloat);
}

/// 验证常量去重（相同常量只存储一次）
TEST(CompilerConstantPoolTest, ConstantDedup) {
    auto result = compileSource("var a = 1; var b = 1; var c = 1;");
    auto& chunk = result.mainChunk;
    // 整数 1 应只出现一次（通过 addConstant 的哈希去重）
    int count = 0;
    for (auto& v : chunk.constants) {
        if (v.isInt() && v.intVal() == 1) count++;
    }
    EXPECT_EQ(count, 1);
}

/// 验证字符串常量去重
TEST(CompilerConstantPoolTest, StringConstantDedup) {
    auto result = compileSource("var a = \"dup\"; var b = \"dup\";");
    auto& chunk = result.mainChunk;
    int count = 0;
    for (auto& v : chunk.constants) {
        if (v.isString() && v.stringVal() == "dup") count++;
    }
    EXPECT_EQ(count, 1);
}

// ============================================================
// 7. 全局槽位分配
// ============================================================

/// 验证 globalSlotCount 和 globalSlotNames 返回正确的全局变量
TEST(CompilerGlobalSlotTest, SlotAllocation) {
    auto result = compileSource("var x = 1; var y = 2; var z = 3;");
    // 三个顶层 var 声明应分配 3 个全局槽位
    EXPECT_EQ(result.globalSlotCount, 3);
    EXPECT_EQ(result.globalSlotNames.size(), 3u);
}

/// 验证全局变量名到槽位的映射
TEST(CompilerGlobalSlotTest, SlotNames) {
    auto result = compileSource("var alpha = 1; var beta = 2;");
    // 槽位名应按声明顺序排列
    ASSERT_EQ(result.globalSlotNames.size(), 2u);
    EXPECT_EQ(result.globalSlotNames[0], "alpha");
    EXPECT_EQ(result.globalSlotNames[1], "beta");
}

/// 验证类声明也分配全局槽位
TEST(CompilerGlobalSlotTest, ClassSlotAllocation) {
    auto result = compileSource("class Foo { var x = 1; }");
    // 类声明应分配一个全局槽位
    EXPECT_GE(result.globalSlotCount, 1);
    bool foundFoo = false;
    for (auto& name : result.globalSlotNames) {
        if (name == "Foo") foundFoo = true;
    }
    EXPECT_TRUE(foundFoo);
}

/// 验证 OP_DEFINE_GLOBAL 的操作数指向正确的槽位
TEST(CompilerGlobalSlotTest, DefineGlobalSlotOperand) {
    auto result = compileSource("var x = 1; var y = 2;");
    auto& chunk = result.mainChunk;
    // 遍历字节码，找到 OP_DEFINE_GLOBAL 指令并验证槽位号
    auto ops = collectOps(chunk);
    std::vector<int> definedSlots;
    size_t offset = 0;
    while (offset < chunk.code.size()) {
        OpCode op = static_cast<OpCode>(chunk.code[offset]);
        if (op == OpCode::OP_DEFINE_GLOBAL) {
            uint16_t slot = readShort(chunk.code, offset + 1);
            definedSlots.push_back(slot);
        }
        if (op == OpCode::OP_CLOSURE && offset + 3 < chunk.code.size()) {
            uint8_t upvalueCount = chunk.code[offset + 3];
            offset += 4 + static_cast<size_t>(upvalueCount) * 2;
        } else {
            offset += BytecodeChunk::instructionSize(op);
        }
    }
    // 应有两个 OP_DEFINE_GLOBAL，槽位分别为 0 和 1
    ASSERT_EQ(definedSlots.size(), 2u);
    EXPECT_EQ(definedSlots[0], 0);
    EXPECT_EQ(definedSlots[1], 1);
}

// ============================================================
// 8. 类型注解强制与 TypeChecker 警告（2026-06-29）
// ------------------------------------------------------------
// 验证三件事：
//   (a) TypeChecker 在编译期对字面量初始化/赋值产生 DiagSource::TypeChecker 警告
//   (b) 编译器对带类型注解的 VarDecl/Assignment 发射 OP_TYPE_CHECK 指令
//   (c) 三后端运行时（Interpreter/VM/RegisterVM）在类型违反时统一抛错
// ============================================================

/// 辅助：以类型检查启用模式编译，返回 (result, diagnostics 引用)
struct TypeCheckCompile {
    CompileResult result;
    Compiler compiler;  // 保留所有权以访问 diagnostics
};
static TypeCheckCompile compileWithTypeCheck(const std::string& source) {
    TypeCheckCompile tc;
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return tc;
    tc.compiler.setEnableTypeCheck(true);
    tc.result = tc.compiler.compile(*ast);
    return tc;
}

/// 辅助：统计 DiagSource::TypeChecker 的警告数量
static int countTypeCheckerWarnings(const DiagnosticBag& bag) {
    int n = 0;
    for (const auto& d : bag.all()) {
        if (d.source == DiagSource::TypeChecker && d.isWarning()) ++n;
    }
    return n;
}

/// A1: `int a = "hello"` — 字面量初始化器类型不匹配 → 产生 TypeChecker 警告
TEST(CompilerTypeCheckTest, A1_LiteralMismatchOnVarDecl) {
    auto tc = compileWithTypeCheck("int a = \"hello\";");
    int warns = countTypeCheckerWarnings(tc.compiler.getDiagnostics());
    EXPECT_GE(warns, 1) << "TypeChecker 应对 int a = \"hello\" 产生至少一条警告";
}

/// A2: `int a = 5; a = "hello"` — 字面量赋值类型不匹配 → 产生 TypeChecker 警告
TEST(CompilerTypeCheckTest, A2_LiteralMismatchOnAssignment) {
    auto tc = compileWithTypeCheck("int a = 5; a = \"hello\";");
    int warns = countTypeCheckerWarnings(tc.compiler.getDiagnostics());
    EXPECT_GE(warns, 1) << "TypeChecker 应对 a = \"hello\" 产生警告";
}

/// A3: `float a = 5` — int 字面量可宽化为 float 注解 → 不应产生警告
TEST(CompilerTypeCheckTest, A3_IntWidensToFloat) {
    auto tc = compileWithTypeCheck("float a = 5;");
    EXPECT_EQ(countTypeCheckerWarnings(tc.compiler.getDiagnostics()), 0)
        << "int→float 宽化应为合法，无警告";
}

/// A4: `string a = 5` — 字面量 int 不匹配 string 注解 → 产生警告
TEST(CompilerTypeCheckTest, A4_IntToStringMismatch) {
    auto tc = compileWithTypeCheck("string a = 5;");
    EXPECT_GE(countTypeCheckerWarnings(tc.compiler.getDiagnostics()), 1);
}

/// A5: `bool a = "true"` — 字面量 string 不匹配 bool 注解 → 产生警告
TEST(CompilerTypeCheckTest, A5_StringToBoolMismatch) {
    auto tc = compileWithTypeCheck("bool a = \"true\";");
    EXPECT_GE(countTypeCheckerWarnings(tc.compiler.getDiagnostics()), 1);
}

/// A6: `int a = null` — null 兼容所有类型注解 → 不应产生警告
TEST(CompilerTypeCheckTest, A6_NullCompatibleWithAllAnnotations) {
    auto tc = compileWithTypeCheck("int a = null;");
    EXPECT_EQ(countTypeCheckerWarnings(tc.compiler.getDiagnostics()), 0)
        << "null 应兼容所有类型注解，无警告";
}

/// A7: `int a = 5` — 带类型注解的 VarDecl 应发射 OP_TYPE_CHECK 指令
TEST(CompilerTypeCheckTest, A7_EmitsOpTypeCheckForAnnotatedVarDecl) {
    auto tc = compileWithTypeCheck("int a = 5;");
    EXPECT_TRUE(containsOp(tc.result.mainChunk, OpCode::OP_TYPE_CHECK))
        << "带 int 类型注解的 VarDecl 应发射 OP_TYPE_CHECK";
}

/// A8: `var a = 5` — 无类型注解的 VarDecl 不应发射 OP_TYPE_CHECK
TEST(CompilerTypeCheckTest, A8_NoOpTypeCheckForUnannotatedVarDecl) {
    auto tc = compileWithTypeCheck("var a = 5;");
    EXPECT_FALSE(containsOp(tc.result.mainChunk, OpCode::OP_TYPE_CHECK))
        << "无类型注解的 VarDecl 不应发射 OP_TYPE_CHECK";
}

/// A9: `int a = 5; a = 10` — 后续字面量赋值仍应发射 OP_TYPE_CHECK
TEST(CompilerTypeCheckTest, A9_EmitsOpTypeCheckForAnnotatedAssignment) {
    auto tc = compileWithTypeCheck("int a = 5; a = 10;");
    int typeCheckCount = 0;
    auto ops = collectOps(tc.result.mainChunk);
    for (auto op : ops) {
        if (op == OpCode::OP_TYPE_CHECK) ++typeCheckCount;
    }
    EXPECT_GE(typeCheckCount, 2)
        << "VarDecl 和 Assignment 各应发射一次 OP_TYPE_CHECK";
}

/// A10: `int[] a = [1, 2, 3]` — 数组类型注解应发射 OP_TYPE_CHECK
TEST(CompilerTypeCheckTest, A10_ArrayTypeAnnotationEmitsCheck) {
    auto tc = compileWithTypeCheck("int[] a = [1, 2, 3];");
    EXPECT_TRUE(containsOp(tc.result.mainChunk, OpCode::OP_TYPE_CHECK));
    // 数组字面量在编译期不做元素级检查（运行时 OP_TYPE_CHECK 检查）
    EXPECT_EQ(countTypeCheckerWarnings(tc.compiler.getDiagnostics()), 0);
}
