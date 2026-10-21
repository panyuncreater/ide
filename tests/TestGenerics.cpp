// ============================================================
// R163 泛型 / 模板测试套件
// ------------------------------------------------------------
// 验证泛型函数与泛型类在四后端
// (Interpreter / StackVM / StackVM-IR / RegisterVM) 上的语义一致性。
//
// 覆盖点：
//   1. 泛型函数声明与调用（单/多类型参数）
//   2. 泛型函数体内类型参数注解擦除（var x: T = ...）
//   3. 泛型函数返回类型参数注解擦除（return x: T）
//   4. 泛型类声明与实例化
//   5. 泛型类方法体内类型参数注解擦除
//   6. 泛型类继承
//   7. 四后端语义一致性
//   8. Formatter 泛型类型参数输出
//   9. DocGenerator 泛型类型参数输出
// ============================================================

#include <gtest/gtest.h>

#include "compiler/backend-reg/RegisterVM.h"
#include "compiler/backend-stack/VM.h"
#include "compiler/core/Compiler.h"
#include "compiler/ir/IR.h"
#include "doc/DocGenerator.h"
#include "formatter/Formatter.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "TestJITHelper.h" // jit_test_helper::runJIT（MINILANG_USE_JIT 门控）

#include <string>

// ============================================================
// 辅助：四后端执行器
// ============================================================

static std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty())
            return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

// ============================================================
// 泛型函数测试
// ============================================================

TEST(GenericFunction, IdentityInt) {
    std::string src = "fun identity<T>(x: T): T { return x; }"
                      "print(identity(42));";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, IdentityString) {
    std::string src = "fun identity<T>(x: T): T { return x; }"
                      "print(identity(\"hello\"));";
    std::string expected = "hello";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, TwoTypeParams) {
    std::string src = "fun pair<K, V>(k: K, v: V): V { return v; }"
                      "print(pair(\"key\", 99));";
    std::string expected = "99";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, LocalVarWithTypeParam) {
    // var y: T = x; — 类型参数 T 应被擦除，不触发类型检查错误
    std::string src = "fun copy<T>(x: T): T { var y: T = x; return y; }"
                      "print(copy(7));";
    std::string expected = "7";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, AssignmentWithTypeParam) {
    // var y: T = x; y = 100; — 赋值时类型参数 T 应被擦除
    std::string src = "fun reassign<T>(x: T): T { var y: T = x; y = 100; return y; }"
                      "print(reassign(0));";
    std::string expected = "100";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, ReturnTypeErasure) {
    // return x; 其中函数返回类型为 T — 应被擦除
    std::string src = "fun get<T>(x: T): T { return x; }"
                      "var r = get(true);"
                      "print(r);";
    std::string expected = "true";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericFunction, GenericWithNonGenericParam) {
    // 混合泛型参数与具体类型参数
    std::string src = "fun wrap<T>(x: T, n: int): int { return n; }"
                      "print(wrap(\"data\", 55));";
    std::string expected = "55";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 泛型类测试
// ============================================================

TEST(GenericClass, BasicBox) {
    std::string src = "class Box<T> {"
                      "    var value: T = null;"
                      "    fun set(v: T): void { this.value = v; }"
                      "    fun get(): T { return this.value; }"
                      "}"
                      "var b = Box();"
                      "b.set(123);"
                      "print(b.get());";
    std::string expected = "123";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericClass, BoxWithString) {
    std::string src = "class Box<T> {"
                      "    var value: T = null;"
                      "    fun set(v: T): void { this.value = v; }"
                      "    fun get(): T { return this.value; }"
                      "}"
                      "var b = Box();"
                      "b.set(\"world\");"
                      "print(b.get());";
    std::string expected = "world";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericClass, TwoTypeParams) {
    std::string src = "class Pair<K, V> {"
                      "    var key: K = null;"
                      "    var val: V = null;"
                      "    fun setK(k: K): void { this.key = k; }"
                      "    fun setV(v: V): void { this.val = v; }"
                      "    fun getV(): V { return this.val; }"
                      "}"
                      "var p = Pair();"
                      "p.setK(\"name\");"
                      "p.setV(888);"
                      "print(p.getV());";
    std::string expected = "888";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericClass, MethodLocalVarWithTypeParam) {
    // 方法体内 var tmp: T = ... — 类型参数 T 应被擦除
    std::string src = "class Holder<T> {"
                      "    var data: T = null;"
                      "    fun store(v: T): void { var tmp: T = v; this.data = tmp; }"
                      "    fun get(): T { return this.data; }"
                      "}"
                      "var h = Holder();"
                      "h.store(42);"
                      "print(h.get());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericClass, GenericInheritance) {
    // 泛型类继承非泛型类
    std::string src = "class Base {"
                      "    fun greet(): string { return \"hello\"; }"
                      "}"
                      "class Ext<T> extends Base {"
                      "    var item: T = null;"
                      "    fun set(v: T): void { this.item = v; }"
                      "    fun get(): T { return this.item; }"
                      "}"
                      "var e = Ext();"
                      "e.set(77);"
                      "print(e.greet());"
                      "print(\";\");"
                      "print(e.get());";
    std::string expected = "hello;77";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 非泛型兼容性（确保非泛型代码不受影响）
// ============================================================

TEST(GenericCompat, NonGenericFunctionUnchanged) {
    std::string src = "fun add(a: int, b: int): int { return a + b; }"
                      "print(add(3, 4));";
    std::string expected = "7";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericCompat, NonGenericClassUnchanged) {
    std::string src = "class Counter {"
                      "    var count: int = 0;"
                      "    fun inc(): void { this.count = this.count + 1; }"
                      "    fun get(): int { return this.count; }"
                      "}"
                      "var c = Counter();"
                      "c.inc();"
                      "c.inc();"
                      "print(c.get());";
    std::string expected = "2";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// Formatter 泛型输出测试
// ============================================================

static std::string formatCode(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Formatter fmt;
    return fmt.format(*ast);
}

TEST(GenericFormatter, GenericFunctionOutput) {
    std::string src = "fun identity<T>(x: T): T { return x; }";
    std::string result = formatCode(src);
    EXPECT_NE(result.find("identity<T>"), std::string::npos);
    EXPECT_NE(result.find("(x: T)"), std::string::npos);
}

TEST(GenericFormatter, GenericFunctionTwoParams) {
    std::string src = "fun pair<K, V>(k: K, v: V): V { return v; }";
    std::string result = formatCode(src);
    EXPECT_NE(result.find("pair<K, V>"), std::string::npos);
}

TEST(GenericFormatter, GenericClassOutput) {
    std::string src = "class Box<T> { var value: T; fun get(): T { return value; } }";
    std::string result = formatCode(src);
    EXPECT_NE(result.find("Box<T>"), std::string::npos);
}

TEST(GenericFormatter, GenericClassTwoParams) {
    std::string src = "class Pair<K, V> { var key: K; }";
    std::string result = formatCode(src);
    EXPECT_NE(result.find("Pair<K, V>"), std::string::npos);
}

TEST(GenericFormatter, GenericClassWithExtends) {
    std::string src = "class Ext<T> extends Base { var item: T; }";
    std::string result = formatCode(src);
    EXPECT_NE(result.find("Ext<T>"), std::string::npos);
    EXPECT_NE(result.find("extends Base"), std::string::npos);
}

// ============================================================
// DocGenerator 泛型输出测试
// ============================================================

static minilang::doc::DocResult generateDocs(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    minilang::doc::DocGenerator gen(minilang::doc::DocFormat::Markdown);
    if (ast) {
        return gen.generate(*ast, lx.comments());
    }
    return {};
}

TEST(GenericDoc, GenericFunctionSignature) {
    std::string src = "fun identity<T>(x: T): T { return x; }";
    auto result = generateDocs(src);
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].name, "identity");
    ASSERT_EQ(result.entries[0].typeParams.size(), 1u);
    EXPECT_EQ(result.entries[0].typeParams[0], "T");
    EXPECT_NE(result.entries[0].signature.find("identity<T>"), std::string::npos);
}

TEST(GenericDoc, GenericFunctionTwoTypeParams) {
    std::string src = "fun pair<K, V>(k: K, v: V): V { return v; }";
    auto result = generateDocs(src);
    ASSERT_EQ(result.entries.size(), 1u);
    ASSERT_EQ(result.entries[0].typeParams.size(), 2u);
    EXPECT_EQ(result.entries[0].typeParams[0], "K");
    EXPECT_EQ(result.entries[0].typeParams[1], "V");
    EXPECT_NE(result.entries[0].signature.find("pair<K, V>"), std::string::npos);
}

TEST(GenericDoc, GenericClassSignature) {
    std::string src = "class Box<T> { var value: T; }";
    auto result = generateDocs(src);
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].name, "Box");
    ASSERT_EQ(result.entries[0].typeParams.size(), 1u);
    EXPECT_EQ(result.entries[0].typeParams[0], "T");
    EXPECT_NE(result.entries[0].signature.find("Box<T>"), std::string::npos);
}

TEST(GenericDoc, GenericClassTwoTypeParams) {
    std::string src = "class Pair<K, V> { var key: K; var val: V; }";
    auto result = generateDocs(src);
    ASSERT_EQ(result.entries.size(), 1u);
    ASSERT_EQ(result.entries[0].typeParams.size(), 2u);
    EXPECT_NE(result.entries[0].signature.find("Pair<K, V>"), std::string::npos);
}

TEST(GenericDoc, NonGenericHasEmptyTypeParams) {
    std::string src = "fun add(a: int, b: int): int { return a + b; }";
    auto result = generateDocs(src);
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_TRUE(result.entries[0].typeParams.empty());
}

// ============================================================
// 10. 泛型擦除补全回归锁定（2026-10-07）
// ------------------------------------------------------------
// R163 的擦除仅精确匹配裸类型名，复合注解（T[]/T?/dict[string:T]）漏擦导致
// 四后端不一致（Interpreter 拒绝 / 三 VM 接受，TestGenerics 探针实测）；
// 显式类型实参 id<int>(5) 被静默解析为比较运算（"未定义的变量: int"）。
// 本节锁定：① 复合注解四后端统一擦除；② 显式类型实参解析后擦除（C#/Java
// 同型消歧：平衡 <> 后紧跟 ( 才按类型实参，否则回退比较）。
// ============================================================

static void expectAllGenericBackends(const std::string& src, const std::string& expected) {
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(GenericErasureCompletion, ArrayOfTParamAnnotation) {
    // 修复前：Interpreter 报"类型注解违反: 期望类型 T[]，实际为 array"，三 VM 通过
    expectAllGenericBackends("fun first<T>(xs: T[]): T { return xs[0]; } print(first([7, 8]));", "7");
}

TEST(GenericErasureCompletion, OptionalTParamAnnotation) {
    // T? 注解：null 与具体值均放行（擦除）
    expectAllGenericBackends("fun isAbsent<T>(x: T?): string { if (x == null) { return \"y\"; } return \"n\"; }"
                             "print(isAbsent(null)); print(isAbsent(3));",
                             "yn");
}

TEST(GenericErasureCompletion, DictWithTPayloadAnnotation) {
    // dict[string:T] 参数注解：整体擦除（含键类型校验一并放弃，属记录的简化）
    expectAllGenericBackends("fun firstVal<T>(d: dict[string:T]): T { return d[\"k\"]; }"
                             "print(firstVal({\"k\": 42}));",
                             "42");
}

TEST(GenericErasureCompletion, DictNestedDictKeyAnnotationErasure) {
    // 键类型自带冒号形态（2026-10-22）：dict[dict[string:K]:int] —— 修复前取首个 ':'
    // 截断为 dict[string / K]:int，擦除漏判 → 运行时校验必拒合法程序；修复后按
    // parseTypeAnnotation 同构文法贪婪消费键类型后定位分隔符，K 被识别 → 整体擦除
    expectAllGenericBackends("fun f<K>(x: dict[dict[string:K]:int]): int { return 1; }"
                             "print(f(5));",
                             "1");
}

TEST(GenericErasureCompletion, DictFunKeyAnnotationErasure) {
    // 同族：fun 返回类型冒号与 dict 分隔冒号在括号外同深度，必须贪婪消费 fun():K
    // 后定位分隔符（括号深度感知不可区分，需文法同构切分）
    expectAllGenericBackends("fun f<K>(x: dict[fun():K:string]): int { return 2; }"
                             "print(f(5));",
                             "2");
}

TEST(GenericErasureCompletion, VarDeclTArrayAnnotation) {
    // 函数体内 var: T[] 注解——编译期不再发射 OP_TYPE_CHECK/TYPE_CHECK IR
    expectAllGenericBackends("fun wrap<T>(x: T): T { var copy: T[] = [x]; return copy[0]; } print(wrap(9));", "9");
}

TEST(GenericErasureCompletion, EnumPayloadCompositeTAnnotation) {
    // enum variant 载荷的复合类型参数注解（Cell(T[])）——Interpreter 侧载荷校验同步擦除
    expectAllGenericBackends("enum Row<T> { Cell(T[]) }"
                             "var r = Row.Cell([1, 2]);"
                             "print(r);",
                             "Row.Cell([1, 2])");
}

TEST(GenericErasureCompletion, ExplicitTypeArgsAtCall) {
    // 修复前：id<int>(5) 解析为比较链，报"未定义的变量: int"
    expectAllGenericBackends("fun id<T>(x: T): T { return x; } print(id<int>(5));", "5");
}

TEST(GenericErasureCompletion, ExplicitTypeArgsAtInstantiation) {
    expectAllGenericBackends("class Box<T> {"
                             "    var v: T = null;"
                             "    fun get(): T { return this.v; }"
                             "    fun set(x: T): void { this.v = x; }"
                             "}"
                             "var b = Box<int>();"
                             "b.set(5);"
                             "print(b.get());",
                             "5");
}

TEST(GenericErasureCompletion, NestedExplicitTypeArgs) {
    // 嵌套 Box<Box<int>>：>> 为两个独立 TK_GT（词法无移位运算符），平衡扫描天然支持
    expectAllGenericBackends("class Box<T> {"
                             "    var v: T = null;"
                             "    fun get(): T { return this.v; }"
                             "    fun set(x: T): void { this.v = x; }"
                             "}"
                             "var b = Box<Box<int>>();"
                             "var inner = Box();"
                             "inner.set(77);"
                             "b.set(inner);"
                             "print(b.get().get());",
                             "77");
}

TEST(GenericErasureCompletion, ComparisonPathStillWorksAfterScanBacktrack) {
    // 消歧回退路径：无 `> (` 结构的比较表达式不受探测影响
    expectAllGenericBackends("var a = 1; var b = 2; if (a < b) { print(\"lt\"); } print(a > b);", "ltfalse");
}

TEST(GenericErasureCompletion, ErasedAssignmentSemanticsDocumented) {
    // 记录既有擦除语义：泛型字段赋值不校验具体类型（b.set("s") 合法且打印 s）——
    // 若未来立项泛型运行时强检（需 ADR），本用例随语义变更同步修订
    expectAllGenericBackends("class Box<T> {"
                             "    var v: T = null;"
                             "    fun get(): T { return this.v; }"
                             "    fun set(x: T): void { this.v = x; }"
                             "}"
                             "var b = Box();"
                             "b.set(5);"
                             "b.set(\"s\");"
                             "print(b.get());",
                             "s");
}

// ============================================================
// 11. 泛型擦除补全第二轮（2026-10-21 审计修复回归）
// ------------------------------------------------------------
// 修复 1（P1 四后端不一致）：构造路径的 init 参数/默认值 checkType 发生在合并
//   类型参数 guard（runInitMethodBody 的 CallFrameGuard）之前——Box([1,2]) 在
//   Interpreter 被拒而 VM 无参数校验发射点直接通过。修复 = guard 前移至
//   setupInitEnvironment / visitVarDecl 自动构造的参数循环之前。
// 修复 2（P1 四后端不一致）：JIT jitBuildEnumVariant 载荷校验仍用 isTypeParameter
//   精确匹配（第七处擦除点漏改），复合注解载荷 + 非空数组在 JIT 报类型不匹配。
// ============================================================

TEST(GenericErasureCompletion, GenericClassInitParamCompositeAnnotation) {
    // 修复前：Interpreter 报"类型注解违反: 期望类型 T[]，实际为 array"，三 VM 通过
    expectAllGenericBackends("class Box<T> {"
                             "    init(v: T[]) { }"
                             "}"
                             "var b = Box([1, 2]);"
                             "print(\"ok\");",
                             "ok");
}

TEST(GenericErasureCompletion, GenericClassInitDefaultTParamAnnotation) {
    // 自动构造路径（constructClassInstance 零实参分支）：默认值的 checkType 先于旧
    // guard——修复前 checkType(3, "T") 在空 currentTypeParams_ 下按具体类型校验被拒
    // （"类型注解违反: 期望类型 T"），VM 无参数校验发射点通过——四后端不一致。
    // 注：数组字面量默认值（= [1, 2]）被 Parser BUG-DEF-1 策略拒绝（仅字面量），
    // 故用标量默认值；复合注解的默认值场景待 BUG-DEF-1 扩展后补充。
    expectAllGenericBackends("class Box<T> {"
                             "    var v = null;"
                             "    init(x: T = 3) { this.v = x; }"
                             "}"
                             "var b = Box();"
                             "print(b.v);",
                             "3");
}

#ifdef MINILANG_USE_JIT
TEST(GenericErasureCompletion, EnumPayloadCompositeTAnnotationJIT) {
    // 修复前：jitBuildEnumVariant 载荷校验用 isTypeParameter 精确匹配——
    // Some(T[]) 复合注解 + 非空数组在 JIT 报"类型不匹配"中止，其余三后端
    // （已递归擦除）构造成功——四后端分歧
    EXPECT_EQ(jit_test_helper::runJIT("enum Row<T> { Cell(T[]) }"
                                      "var r = Row.Cell([1, 2]);"
                                      "print(r);"),
              "Row.Cell([1, 2])");
}
#endif
