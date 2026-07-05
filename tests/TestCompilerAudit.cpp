// ============================================================
// Compiler + BytecodeChunk 审计回归测试
// ------------------------------------------------------------
// 覆盖审计发现的 4 个 Bug 的回归测试：
//   BUG-CP-1   (P1): addConstant 跨类型去重（int 0 与 float 0.0 误判相等）
//   BUG-PRES-1 (P2): compile() 预扫描未解包 ExportStmt，导出变量前向引用行为不一致
//   BUG-TRY-1  (P2): catch 块内 throw 跳过清理代码，导致 catch 变量泄漏
//   BUG-MOD-1  (P2): normalizeModulePath 未拒绝 Windows 驱动器相对路径 'C:foo'
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Value.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"

#include <string>
#include <vector>
#include <unordered_map>

// ============================================================
// 辅助函数
// ============================================================

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

static std::string runVMOutput(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";
    Compiler compiler;
    auto result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }
    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 执行源码，返回 VM 全局变量表（即使在运行时错误后也可检查状态）
static std::unordered_map<std::string, Value> runVMGlobals(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return {};
    Compiler compiler;
    auto result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) return {};
    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.execute(result);
    return vm.getGlobals();
}

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

static bool containsOp(const BytecodeChunk& chunk, OpCode op) {
    auto ops = collectOps(chunk);
    for (auto o : ops) {
        if (o == op) return true;
    }
    return false;
}

static uint16_t readShort(const std::vector<uint8_t>& code, size_t offset) {
    if (offset + 1 >= code.size()) return 0xFFFF;
    return code[offset] | (code[offset + 1] << 8);
}

// ============================================================
// BUG-CP-1: addConstant 跨类型去重
// ------------------------------------------------------------
// Value::equals() 对 int(0) 与 float(0.0) 返回 true（数值相等性）。
// 原实现仅依赖 equals() 去重，导致 int 0 与 float 0.0 共享同一常量索引。
// 编译器根据原始类型发射 OP_INT/OP_FLOAT，VM push 共享的常量值，
// float 变量会静默变为 int，破坏三后端一致性。
// 修复：addConstant 增加类型严格匹配 getType() == getType()。
// ============================================================

// 整数 0 和浮点 0.0 不应去重为同一常量
TEST(CompilerAuditCP1, IntAndFloatZeroNotDeduped) {
    auto result = compileSource("var i = 0; var f = 0.0;");
    auto& chunk = result.mainChunk;
    int intZeroCount = 0;
    int floatZeroCount = 0;
    for (auto& v : chunk.constants) {
        if (v.isInt() && v.intVal() == 0) ++intZeroCount;
        if (v.isFloat() && v.floatVal() == 0.0) ++floatZeroCount;
    }
    EXPECT_EQ(intZeroCount, 1) << "整数 0 应在常量池中存在一份";
    EXPECT_EQ(floatZeroCount, 1) << "浮点 0.0 应在常量池中独立存在一份（未被 int 0 去重）";
}

// 整数 1 和浮点 1.0 也不应去重
TEST(CompilerAuditCP1, IntAndFloatOneNotDeduped) {
    auto result = compileSource("var i = 1; var f = 1.0;");
    auto& chunk = result.mainChunk;
    int intOneCount = 0;
    int floatOneCount = 0;
    for (auto& v : chunk.constants) {
        if (v.isInt() && v.intVal() == 1) ++intOneCount;
        if (v.isFloat() && v.floatVal() == 1.0) ++floatOneCount;
    }
    EXPECT_EQ(intOneCount, 1);
    EXPECT_EQ(floatOneCount, 1) << "浮点 1.0 不应与整数 1 共享常量索引";
}

// 验证修复后浮点除法保持浮点类型（10.0 / 4 = 2.5，非整数截断 2）
TEST(CompilerAuditCP1, FloatDivisionPreservesType) {
    // 注意：如果常量池错误去重，0.0 可能被当作 int 0，
    // 但更直接的浮点保留验证是 10.0 / 4 == 2.5
    EXPECT_EQ(runVMOutput("print(10.0 / 4);"), "2.5");
    EXPECT_EQ(runVMOutput("print(10 / 4);"), "2");  // 整数除法截断
}

// 验证寄存器式后端常量池去重也类型严格匹配
TEST(CompilerAuditCP1, RegisterBackendAlsoTypeStrict) {
    Lexer lexer;
    auto tokens = lexer.scan("var i = 0; var f = 0.0;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    auto& regResult = compiler.getLastRegisterResult();
    int intZeroCount = 0;
    int floatZeroCount = 0;
    for (auto& v : regResult.mainChunk.constants) {
        if (v.isInt() && v.intVal() == 0) ++intZeroCount;
        if (v.isFloat() && v.floatVal() == 0.0) ++floatZeroCount;
    }
    EXPECT_EQ(intZeroCount, 1);
    EXPECT_EQ(floatZeroCount, 1) << "寄存器式后端常量池也应类型严格去重";
}

// ============================================================
// BUG-PRES-1: compile() 预扫描未解包 ExportStmt
// ------------------------------------------------------------
// 原实现 compile() 预扫描仅覆盖 VarDecl/ClassDecl，未解包 ExportStmt
// 内部声明，导致 `export var x` 使用慢路径 OP_DEFINE_VAR，
// 且前向引用行为不一致（普通 var 返回 null，导出 var 报运行时错误）。
// 修复：compile() 与 preScanModuleGlobals() 同步解包 ExportStmt(VarDecl/ClassDecl)。
// ============================================================

// export var 应分配全局槽位
TEST(CompilerAuditPRES1, ExportVarGetsGlobalSlot) {
    auto result = compileSource("export var x = 1;");
    EXPECT_GE(result.globalSlotCount, 1);
    bool foundX = false;
    for (auto& name : result.globalSlotNames) {
        if (name == "x") foundX = true;
    }
    EXPECT_TRUE(foundX) << "export var x 应分配全局槽位";
}

// export class 应分配全局槽位
TEST(CompilerAuditPRES1, ExportClassGetsGlobalSlot) {
    auto result = compileSource("export class Foo { var x = 1; }");
    EXPECT_GE(result.globalSlotCount, 1);
    bool foundFoo = false;
    for (auto& name : result.globalSlotNames) {
        if (name == "Foo") foundFoo = true;
    }
    EXPECT_TRUE(foundFoo) << "export class Foo 应分配全局槽位";
}

// export var 应使用 OP_DEFINE_GLOBAL（而非慢路径 OP_DEFINE_VAR）
TEST(CompilerAuditPRES1, ExportVarUsesDefineGlobalOp) {
    auto result = compileSource("export var x = 1;");
    auto& chunk = result.mainChunk;
    EXPECT_TRUE(containsOp(chunk, OpCode::OP_DEFINE_GLOBAL))
        << "export var 应通过预扫描使用 OP_DEFINE_GLOBAL 快路径";
    // OP_DEFINE_VAR 是慢路径，不应出现在 export var 的编译结果中
    // 注意：OP_DEFINE_VAR 可能在其他上下文出现，此处仅验证 OP_DEFINE_GLOBAL 存在
}

// 前向引用行为一致性：顶层 var f = funName（funName 未定义）应统一报错，
// 不应因预扫描差异而出现"静默 null"vs"运行时错误"的差异
TEST(CompilerAuditPRES1, ForwardRefBehaviorConsistent) {
    // 普通变量引用未定义的函数名——应统一报运行时错误（而非静默 null）
    std::string out = runVMOutput("var f = undefinedName; print(f);");
    // 期望运行时错误（而非打印 "null"）
    EXPECT_TRUE(out.find("runtime:") != std::string::npos)
        << "未定义变量应报运行时错误，实际输出: " << out;
}

// ============================================================
// BUG-TRY-1: catch 块内 throw 跳过清理代码
// ------------------------------------------------------------
// 原实现 catch 块编译后直接发射清理代码（OP_DELETE_VAR 等），
// 但 catch 块内若 throw，异常传播跳过清理代码，导致：
//   1. catch 变量未从全局表删除（泄漏到外层作用域）
//   2. 被遮蔽的全局变量未恢复（值丢失）
// 修复：用 OP_TRY_BEGIN 包装 catch 块，捕获内层 throw，
// 跳到 cleanupThrowIp 执行清理代码后 OP_THROW rethrow。
// ============================================================

// catch 块内 throw 后，被遮蔽的全局变量应恢复原值
// 使用顶层 catch（blockDepth_=0）使 shadowed global 保存/恢复机制生效。
// throw 999 未捕获会终止程序，但全局槽位状态仍可通过 runVMGlobals 检查。
TEST(CompilerAuditTRY1, CatchThrowRestoresShadowedGlobal) {
    std::string src =
        "var x = 100;"
        "try {"
        "  throw \"err\";"
        "} catch (x) {"
        "  print(x);"      // 打印异常值 "err"
        "  throw 999;"     // catch 块内再次 throw（应先执行清理恢复全局 x=100）
        "}";
    auto globals = runVMGlobals(src);
    // throw 999 未捕获，程序终止。但 BUG-TRY-1 修复确保清理代码在 throw 前执行，
    // 全局 x 应已恢复为 100（而非 null 或 "err"）。
    ASSERT_TRUE(globals.count("x") > 0);
    EXPECT_EQ(globals["x"].intVal(), 100)
        << "catch 块内 throw 后被遮蔽的全局变量应恢复原值 100";
}

// catch 块内 throw 后，catch 变量应被清理（不泄漏到外层）
TEST(CompilerAuditTRY1, CatchThrowCleansUpCatchVar) {
    std::string src =
        "try {"
        "  throw \"err\";"
        "} catch (e) {"
        "  throw 999;"     // catch 块内 throw
        "}"
        "print(e);";       // e 应已清理，此处应报运行时错误
    std::string out = runVMOutput(src);
    // 期望运行时错误（e 未定义），而非打印 catch 变量的值
    EXPECT_TRUE(out.find("runtime:") != std::string::npos)
        << "catch 变量应被清理，外层引用应报错，实际输出: " << out;
}

// catch 块正常退出（无 throw）时，清理代码仍应执行
TEST(CompilerAuditTRY1, CatchNormalExitStillCleansUp) {
    std::string src =
        "var x = 100;"
        "try {"
        "  throw \"err\";"
        "} catch (x) {"
        "  print(x);"      // 打印异常值
        "}"
        "print(x);";       // 应恢复为 100
    std::string out = runVMOutput(src);
    // 期望先打印 "err"（异常值），再打印 "100"（恢复后的全局值）
    EXPECT_TRUE(out.find("100") != std::string::npos)
        << "catch 正常退出时也应恢复被遮蔽的全局变量，实际输出: " << out;
}

// ============================================================
// BUG-MOD-1: normalizeModulePath 未拒绝 Windows 驱动器相对路径
// ------------------------------------------------------------
// 原实现仅检测 'C:/' 形式（path[1]==':' && path[2]=='/'），
// 未拒绝 'C:foo'（Windows 驱动器相对路径），且反斜杠检测分支为死代码
// （反斜杠在更早处已统一转为正斜杠）。
// 修复：拒绝所有 'X:' 开头形式（path.size()>=2 && path[1]==':'），
// 三后端（Compiler/Interpreter/IR）同步修改。
// ============================================================

// 'C:foo' 形式的 Windows 驱动器相对路径应被拒绝
TEST(CompilerAuditMOD1, WindowsDriveRelativePathRejected) {
    // 通过 Compiler::normalizeModulePath 间接验证：编译含 import 的源码，
    // 期望编译错误（模块路径非法）
    Lexer lexer;
    auto tokens = lexer.scan("import \"C:foo\";");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    Compiler compiler;
    // 设置一个空的 moduleLoader，确保即使路径校验通过也不会真正加载
    compiler.setModuleLoader([](const std::string&) { return std::string(); });
    compiler.compile(*ast);
    EXPECT_TRUE(compiler.getDiagnostics().hasErrors())
        << "Windows 驱动器相对路径 'C:foo' 应被拒绝";
    if (compiler.getDiagnostics().hasErrors()) {
        EXPECT_TRUE(compiler.getLastError().find("绝对路径") != std::string::npos
                 || compiler.getLastError().find("模块路径") != std::string::npos)
            << "错误消息应说明路径非法，实际: " << compiler.getLastError();
    }
}

// 'D:path' 形式也应被拒绝（非 C 盘）
TEST(CompilerAuditMOD1, OtherDriveRelativePathRejected) {
    Lexer lexer;
    auto tokens = lexer.scan("import \"D:somefile\";");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    Compiler compiler;
    compiler.setModuleLoader([](const std::string&) { return std::string(); });
    compiler.compile(*ast);
    EXPECT_TRUE(compiler.getDiagnostics().hasErrors())
        << "Windows 驱动器相对路径 'D:somefile' 应被拒绝";
}

// 合法相对路径不应被误拒
TEST(CompilerAuditMOD1, LegitRelativePathNotRejected) {
    Lexer lexer;
    auto tokens = lexer.scan("import \"mymodule\";");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    Compiler compiler;
    // 提供一个返回空字符串的 loader，模拟模块不存在
    // 期望"模块不存在"错误，而非"路径非法"错误
    compiler.setModuleLoader([](const std::string&) { return std::string(); });
    compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        EXPECT_TRUE(compiler.getLastError().find("无法加载") != std::string::npos
                 || compiler.getLastError().find("不存在") != std::string::npos)
            << "合法相对路径应尝试加载（而非路径非法错误），实际: " << compiler.getLastError();
    }
}

// Unix 绝对路径 '/etc/passwd' 应被拒绝
TEST(CompilerAuditMOD1, AbsolutePathStillRejected) {
    Lexer lexer;
    auto tokens = lexer.scan("import \"/etc/passwd\";");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    Compiler compiler;
    compiler.setModuleLoader([](const std::string&) { return std::string(); });
    compiler.compile(*ast);
    EXPECT_TRUE(compiler.getDiagnostics().hasErrors())
        << "Unix 绝对路径 '/etc/passwd' 应被拒绝";
    if (compiler.getDiagnostics().hasErrors()) {
        EXPECT_TRUE(compiler.getLastError().find("绝对路径") != std::string::npos
                 || compiler.getLastError().find("模块路径") != std::string::npos)
            << "错误消息应说明绝对路径非法，实际: " << compiler.getLastError();
    }
}
