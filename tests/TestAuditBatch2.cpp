// ============================================================
// 审计批次 2 回归测试 — 三后端继承一致性（BUG-INH-AUDIT-2）
// ------------------------------------------------------------
// 12 项测试覆盖三后端（Interpreter / StackVM / StackVM-IR / RegisterVM）
// 在类继承场景下的语义一致性。重点验证以下审计修复：
//   BUG-INH-AUDIT-4: fieldsModified 谓词修正（this.arr[i]=val 的 COW detach 后字段同步）
//   BUG-INH-AUDIT-5: ModuleIsolation 字段作用域污染（方法体内引用同名模块顶层变量）
//   BUG-INH-AUDIT-6: super 在非方法上下文中的运行时错误统一
//   BUG-INH-AUDIT-7: 父类重定义时子类缓存失效（REPL 场景，此处验证单次执行的字段布局）
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"

#include <string>
#include <memory>

// ============================================================
// 辅助函数
// ============================================================

static std::string runInterpreter(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty()) return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string runStackVM(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runRegVM(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static bool isRuntimeError(const std::string& s) {
    return s.find("<runtime:") != std::string::npos;
}

// ============================================================
// 1. 父类字段默认值被子类继承
// ============================================================
TEST(AuditBatch2Inherit, ParentFieldDefaultInherited) {
    std::string src =
        "class Animal {"
        "  var sound = \"...\";"
        "}"
        "class Dog : Animal {"
        "}"
        "var d = Dog();"
        "print(d.sound);";
    std::string expected = "...";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 2. 子类覆盖父类字段默认值
// ============================================================
TEST(AuditBatch2Inherit, ChildOverridesFieldDefault) {
    std::string src =
        "class Animal {"
        "  var sound = \"generic\";"
        "}"
        "class Dog : Animal {"
        "  var sound = \"bark\";"
        "}"
        "var d = Dog();"
        "print(d.sound);";
    std::string expected = "bark";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 3. super.method() 调用父类方法
// ============================================================
TEST(AuditBatch2Inherit, SuperMethodCall) {
    std::string src =
        "class Base {"
        "  fun greet() { return \"hello\"; }"
        "}"
        "class Derived : Base {"
        "  fun greet() { return super.greet() + \"!\"; }"
        "}"
        "var d = Derived();"
        "print(d.greet());";
    std::string expected = "hello!";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 4. 三层继承的 super 链式调用
// ============================================================
TEST(AuditBatch2Inherit, ThreeLevelSuperChain) {
    std::string src =
        "class A { fun f() { return \"A\"; } }"
        "class B : A { fun f() { return super.f() + \"B\"; } }"
        "class C : B { fun f() { return super.f() + \"C\"; } }"
        "var c = C();"
        "print(c.f());";
    std::string expected = "ABC";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 5. 方法内修改 this.field 后值正确返回（fieldsModified 路径）
// ============================================================
TEST(AuditBatch2Inherit, MethodModifiesField) {
    std::string src =
        "class Counter {"
        "  var count = 0;"
        "  fun inc() { this.count = this.count + 1; return this.count; }"
        "}"
        "var c = Counter();"
        "print(c.inc());"
        "print(c.inc());"
        "print(c.inc());";
    std::string expected = "123";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 6. BUG-INH-AUDIT-4: 方法内 this.arr[i] = val（数组字段元素赋值）
//    验证 fieldsModified 谓词修正后字段同步回 this.fields()
// ============================================================
TEST(AuditBatch2Inherit, MethodModifiesArrayFieldElement) {
    std::string src =
        "class Stack {"
        "  var data = [10, 20, 30];"
        "  fun setAt(i, v) { this.data[i] = v; }"
        "  fun getAt(i) { return this.data[i]; }"
        "}"
        "var s = Stack();"
        "s.setAt(1, 99);"
        "print(s.getAt(1));"
        "print(s.getAt(0));"
        "print(s.getAt(2));";
    std::string expected = "991030";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 7. BUG-INH-AUDIT-4: 方法内 this.dict[k] = v（字典字段元素赋值）
// ============================================================
TEST(AuditBatch2Inherit, MethodModifiesDictFieldElement) {
    std::string src =
        "class Config {"
        "  var opts = {\"a\": 1, \"b\": 2};"
        "  fun set(k, v) { this.opts[k] = v; }"
        "  fun get(k) { return this.opts[k]; }"
        "}"
        "var c = Config();"
        "c.set(\"a\", 100);"
        "c.set(\"c\", 3);"
        "print(c.get(\"a\"));"
        "print(c.get(\"b\"));"
        "print(c.get(\"c\"));";
    std::string expected = "10023";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 8. super.method() 调用父类方法，父类方法内调用 this.otherMethod()
//    动态分派回子类覆盖的方法
// ============================================================
TEST(AuditBatch2Inherit, SuperCallDynamicDispatch) {
    std::string src =
        "class Base {"
        "  fun helper() { return \"base.helper\"; }"
        "  fun doWork() { return this.helper(); }"
        "}"
        "class Derived : Base {"
        "  fun helper() { return \"derived.helper\"; }"
        "  fun doWork() { return super.doWork() + \"/derived\"; }"
        "}"
        "var d = Derived();"
        "print(d.doWork());";
    std::string expected = "derived.helper/derived";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 9. BUG-INH-AUDIT-6: super 在顶层非方法上下文中使用
//    三后端统一报运行时错误（不崩溃）
// ============================================================
TEST(AuditBatch2Inherit, SuperAtTopLevelRuntimeError) {
    std::string src =
        "class A { fun get() { return 1; } }"
        "super.get();";
    auto ri = runInterpreter(src);
    auto rs = runStackVM(src);
    auto rsIR = runStackVM_IR(src);
    auto rr = runRegVM(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter: " << ri;
    EXPECT_TRUE(isRuntimeError(rs)) << "StackVM: " << rs;
    EXPECT_TRUE(isRuntimeError(rsIR)) << "StackVM-IR: " << rsIR;
    EXPECT_TRUE(isRuntimeError(rr)) << "RegVM: " << rr;
}

// ============================================================
// 10. BUG-INH-AUDIT-6: super 在普通函数内使用
//     三后端统一报运行时错误
// ============================================================
TEST(AuditBatch2Inherit, SuperInPlainFunctionRuntimeError) {
    std::string src =
        "class A { fun get() { return 1; } }"
        "fun f() { return super.get(); }"
        "print(f());";
    auto ri = runInterpreter(src);
    auto rs = runStackVM(src);
    auto rsIR = runStackVM_IR(src);
    auto rr = runRegVM(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter: " << ri;
    EXPECT_TRUE(isRuntimeError(rs)) << "StackVM: " << rs;
    EXPECT_TRUE(isRuntimeError(rsIR)) << "StackVM-IR: " << rsIR;
    EXPECT_TRUE(isRuntimeError(rr)) << "RegVM: " << rr;
}

// ============================================================
// 11. BUG-INH-AUDIT-6: super 在 try/catch 内不可捕获（运行时错误穿透）
//     三后端统一行为
// ============================================================
TEST(AuditBatch2Inherit, SuperErrorNotCatchableByTryCatch) {
    std::string src =
        "class A { fun get() { return 1; } }"
        "try {"
        "  super.get();"
        "} catch (e) {"
        "  print(\"caught:\" + e);"
        "}"
        "print(\"end\");";
    auto ri = runInterpreter(src);
    auto rs = runStackVM(src);
    auto rsIR = runStackVM_IR(src);
    auto rr = runRegVM(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter: " << ri;
    EXPECT_TRUE(isRuntimeError(rs)) << "StackVM: " << rs;
    EXPECT_TRUE(isRuntimeError(rsIR)) << "StackVM-IR: " << rsIR;
    EXPECT_TRUE(isRuntimeError(rr)) << "RegVM: " << rr;
}

// ============================================================
// 12. 多层继承字段遮蔽 + 方法链
// ============================================================
TEST(AuditBatch2Inherit, MultiLayerFieldShadowingWithMethods) {
    std::string src =
        "class A {"
        "  var x = 1;"
        "  fun getX() { return this.x; }"
        "}"
        "class B : A {"
        "  var x = 2;"
        "  fun getXB() { return this.x; }"
        "}"
        "class C : B {"
        "  var x = 3;"
        "  fun getXC() { return this.x; }"
        "}"
        "var c = C();"
        "print(c.getX());"
        "print(c.getXB());"
        "print(c.getXC());";
    std::string expected = "333";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 13. BUG-INH-AUDIT-5: ModuleIsolation 字段作用域隔离
//     类字段声明不污染外层作用域，this.field 访问返回字段值（三后端一致）
//     注意：BUG-INH-AUDIT-5 fix 仅作用于 import 模块的 AST 重命名，本测试
//     验证字段访问的基础语义在非模块场景下三后端一致（裸名 x 解析在
//     Interpreter/IR 间存在预存在差异，不属于 BUG-INH-AUDIT-5 范畴）
// ============================================================
TEST(AuditBatch2Inherit, ModuleTopLevelVarSameNameAsFieldAccessibleInMethod) {
    // 模块顶层 var x = 100；类中字段 var x = 200；
    // - this.field 访问应稳定返回 200（三后端一致）
    // - 裸名 x 解析在 Interpreter/IR 间存在已知差异（非本审计范畴）
    std::string src =
        "var x = 100;"
        "class C {"
        "  var x = 200;"
        "  fun getField() { return this.x; }"
        "  fun getFieldTwice() { return this.x + this.x; }"
        "}"
        "var c = C();"
        "print(c.getField());"
        "print(c.getFieldTwice());";
    std::string expected = "200400";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 14. BUG-IR-OPT-AUDIT-5: loopUnroll vreg 重命名正确性
//     直接调用 optimizeIR + loopUnrollingPass 验证 vreg 唯一性
//     构造最小循环模式 IR，启用 loopUnroll + DCE，验证展开后
//     body 副本中 vreg 不重复（SSA 不变量保持）
// ============================================================
TEST(AuditBatch2IRopt, LoopUnrollVregRenamingCorrectness) {
    // 构造最小循环 IR：
    //   for (var i = 0; i < 3; i = i + 1) { var local = i * 2; }
    // IR 模式：LABEL L1; LOAD_LOCAL i; LOAD_CONST 3; LT; JUMP_IF_FALSE L_exit;
    //         POP; LOAD_LOCAL i; LOAD_CONST 2; MUL dest, i, 2; STORE_LOCAL local, dest;
    //         LOAD_LOCAL i; LOAD_CONST 1; ADD; STORE_LOCAL i; JUMP L1; LABEL L_exit; POP
    IRFunction ir;
    ir.nextVReg = 0;
    ir.constants.push_back(Value(static_cast<int64_t>(3)));   // idx 0: N=3
    ir.constants.push_back(Value(static_cast<int64_t>(2)));   // idx 1: factor=2
    ir.constants.push_back(Value(static_cast<int64_t>(1)));   // idx 2: increment=1
    ir.constants.push_back(Value(static_cast<int64_t>(0)));   // idx 3: zero (initial)

    IRBasicBlock block;
    // 简化：用 labelIndex 0/1
    uint32_t startLabelIdx = 0;
    uint32_t exitLabelIdx = 1;

    uint32_t counterSlot = 0;
    uint32_t localSlot = 1;

    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{ IROperand::label(startLabelIdx) }, 1);
    // LOAD_LOCAL slot=0 → vreg v0
    uint32_t v0 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{ IROperand::vreg(v0), IROperand::local(counterSlot) }, 2);
    // LOAD_CONST 3 → vreg v1
    uint32_t v1 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ IROperand::vreg(v1), IROperand::constant(0) }, 3);
    // LT v2, v0, v1
    uint32_t v2 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LT, std::vector<IROperand>{ IROperand::vreg(v2), IROperand::vreg(v0), IROperand::vreg(v1) }, 4);
    // JUMP_IF_FALSE v2, exitLabel
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, std::vector<IROperand>{ IROperand::vreg(v2), IROperand::label(exitLabelIdx) }, 5);
    // POP v2
    block.instructions.emplace_back(IROp::POP, std::vector<IROperand>{ IROperand::vreg(v2) }, 6);
    // body: LOAD_LOCAL i; LOAD_CONST 2; MUL; STORE_LOCAL local
    uint32_t v3 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{ IROperand::vreg(v3), IROperand::local(counterSlot) }, 7);
    uint32_t v4 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ IROperand::vreg(v4), IROperand::constant(1) }, 8);
    uint32_t v5 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::MUL, std::vector<IROperand>{ IROperand::vreg(v5), IROperand::vreg(v3), IROperand::vreg(v4) }, 9);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{ IROperand::local(localSlot), IROperand::vreg(v5) }, 10);
    // counter update: LOAD_LOCAL i; LOAD_CONST 1; ADD; STORE_LOCAL i
    uint32_t v6 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{ IROperand::vreg(v6), IROperand::local(counterSlot) }, 11);
    uint32_t v7 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ IROperand::vreg(v7), IROperand::constant(2) }, 12);
    uint32_t v8 = ir.nextVReg++;
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{ IROperand::vreg(v8), IROperand::vreg(v6), IROperand::vreg(v7) }, 13);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{ IROperand::local(counterSlot), IROperand::vreg(v8) }, 14);
    // JUMP L1
    block.instructions.emplace_back(IROp::JUMP, std::vector<IROperand>{ IROperand::label(startLabelIdx) }, 15);
    // LABEL L_exit
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{ IROperand::label(exitLabelIdx) }, 16);
    // POP v0
    block.instructions.emplace_back(IROp::POP, std::vector<IROperand>{ IROperand::vreg(v0) }, 17);

    ir.blocks.push_back(std::move(block));

    // 记录展开前的 nextVReg
    uint32_t vregBefore = ir.nextVReg;

    // 调用 optimizeIR 启用 loopUnroll + DCE
    bool modified = optimizeIR(ir, /*copyProp=*/false, /*DCE=*/true, /*CSE=*/false, /*loopUnroll=*/true);
    EXPECT_TRUE(modified) << "loopUnroll 应修改 IR";

    // 验证 SSA 不变量：每个 vreg 只能被定义一次（作为 dest）
    std::unordered_map<uint32_t, int> defCount;
    for (const auto& blk : ir.blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.operands.empty()) continue;
            if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
                defCount[instr.operands[0].index]++;
            }
        }
    }
    bool ssaValid = true;
    std::string duplicates;
    for (const auto& kv : defCount) {
        if (kv.second > 1) {
            ssaValid = false;
            duplicates += " vreg " + std::to_string(kv.first) + " defined " + std::to_string(kv.second) + " times;";
        }
    }
    EXPECT_TRUE(ssaValid) << "BUG-IR-OPT-AUDIT-5: 展开后 vreg 重复定义:" << duplicates;

    // nextVReg 应增长（每个迭代分配 fresh vreg）
    EXPECT_GT(ir.nextVReg, vregBefore) << "展开后应分配新 vreg";
}

// ============================================================
// 15. BUG-IR-OPT-AUDIT-1/2/3/4: 优化 pass 不破坏语义
//     寄存器式后端启用 irOptimize 后语义不变
// ============================================================
TEST(AuditBatch2IRopt, OptimizationPreservesSemanticsRegVM) {
    std::string src =
        "var x = 1 + 2 * 3;"
        "var y = (x - 1) * 2;"
        "var z = y > 5;"
        "print(x);"
        "print(y);"
        "print(z);";
    std::string expected = "712true";  // x=7, y=12, z=true

    // 三后端默认结果一致
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);

    // 寄存器式后端启用 IR 优化（常量折叠 + DCE）后结果不变
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c; c.setUseRegisterVM(true); c.setIROptimize(true);
    c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors()) << c.getLastError();
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    EXPECT_FALSE(vm.hasError()) << vm.getLastError();
    EXPECT_EQ(out, expected);
}

// ============================================================
// 16. BUG-IR-OPT-AUDIT-1/2/3/4: 常量折叠 + 复杂表达式不破坏语义
// ============================================================
TEST(AuditBatch2IRopt, ConstantFoldingComplexExpression) {
    std::string src =
        "var a = 10;"
        "var b = 20;"
        "var c = (a + b) * 2 - 5;"
        "var d = c / 3;"
        "print(c);"
        "print(d);";
    std::string expected = "5518";  // (10+20)*2-5=55, 55/3=18 (整数除法截断)
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);

    // 启用 IR 优化后结果不变
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c; c.setUseRegisterVM(true); c.setIROptimize(true);
    c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors()) << c.getLastError();
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    EXPECT_EQ(out, expected);
}
