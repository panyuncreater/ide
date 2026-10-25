// TestBacklogRegression.cpp — BACKLOG 开放项回归锁（B-001/B-002/B-003/B-006/B-008）
// B-001: 复合类型实参（dict[..]/fun(..)）显式类型实参解析
// B-002: enum 变体构造携带显式类型实参
// B-003: 数组/字典字面量默认参数（四后端绑定 + COW 隔离）
// B-006: JIT 方法调用 IC 多态调用点键稳定性
// B-008: finally 同帧 abrupt 替换 + 递归同 try 站点异常暂存

#include "common/ThreeBackends.h"
#include "cli/ProtocolUtils.h"
#include "cli/pkg_core.h"
#include "compiler/backend-reg/RegisterBytecode.h"
#include "common/RuntimeLimits.h"
#include "formatter/Formatter.h"
#include "interpreter/GcManager.h"
#include "lint/LintPass.h"
#include <gtest/gtest.h>

namespace {
bool parseClean(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    return ast != nullptr && !lx.getDiagnostics().hasErrors() && !p.hasErrors();
}

size_t parseErrorCount(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    p.parse(tk);
    return lx.getDiagnostics().all().size() + p.getDiagnostics().all().size();
}
} // namespace

// ============================================================
// B-001 复合类型实参
// ============================================================
TEST(BacklogRegression, B001DictTypeArg) {
    const std::string src = "fun f<T>(x: T) { return x; }"
                            "print(f<dict[string:int]>({\"a\": 1}));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "{\"a\": 1}");
}

TEST(BacklogRegression, B001FunTypeArg) {
    const std::string src = "fun id<T>(x: T) { return x; }"
                            "var g = fun(y: int) { return y + 1; };"
                            "print(id<fun(int):int>(g)(4));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "5");
}

TEST(BacklogRegression, B001NestedDictTypeArg) {
    const std::string src = "fun f<T>(x: T) { return x; }"
                            "print(f<dict[string:dict[str:int]]>({\"a\": {\"b\": 2}}));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "{\"a\": {\"b\": 2}}");
}

TEST(BacklogRegression, B001ComparisonStillParses) {
    const std::string src = "var a = 3; var b = 2; var c = 1;"
                            "print(a < b);"
                            "print(a < b + c);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "falsefalse");
}

// ============================================================
// B-002 enum 变体显式类型实参
// ============================================================
TEST(BacklogRegression, B002EnumVariantExplicitTypeArg) {
    const std::string src = "enum Box<T> { some(T) }"
                            "print(Box.some<int>(5));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "Box.some(5)");
}

TEST(BacklogRegression, B002GenericMethodExplicitTypeArg) {
    const std::string src = "class C { fun m<T>(x: T) { return x; } }"
                            "var c = C();"
                            "print(c.m<int>(7));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "7");
}

// ============================================================
// B-003 数组/字典字面量默认参数
// ============================================================
TEST(BacklogRegression, B003TopLevelFunArrayDefault) {
    const std::string src = "fun f(a: int[] = [1, 2]) { return a; }"
                            "print(f());"
                            "print(f([9]));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[1, 2][9]");
}

TEST(BacklogRegression, B003ClassInitArrayDefault) {
    const std::string src = "class C {"
                            "  var x: int[];"
                            "  init(a: int[] = [1, 2]) { this.x = a; }"
                            "}"
                            "var c = C();"
                            "print(c.x);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[1, 2]");
}

TEST(BacklogRegression, B003LambdaArrayDefault) {
    const std::string src = "var g = fun(a: int[] = [7]) { return a; };"
                            "print(g());";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[7]");
}

TEST(BacklogRegression, B003MixedScalarNestedArrayDefault) {
    const std::string src = "fun h(a: int = 3, b: array = [1, [2, 3]], c: string = \"x\") { return [a, b, c]; }"
                            "print(h());";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[3, [1, [2, 3]], \"x\"]");
}

TEST(BacklogRegression, B003DefaultArrayCowIsolation) {
    const std::string src = "fun m(a: int[] = [1, 2]) { return a; }"
                            "var x = m();"
                            "x.push(99);"
                            "print(x);"
                            "print(m());";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[1, 2, 99][1, 2]");
}

TEST(BacklogRegression, B003DictDefault) {
    const std::string src = "fun d(a: dict[string:int] = {\"k\": 1}) { return a; }"
                            "print(d());";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "{\"k\": 1}");
}

TEST(BacklogRegression, B003NonLiteralDefaultRejectedWithoutDerailing) {
    const std::string src = "fun k(a: int[] = b) { return 1; }"
                            "print(\"after\");";
    EXPECT_FALSE(parseClean(src));
    EXPECT_EQ(parseErrorCount(src), 1u); // 仅默认值诊断，无脱轨残留诊断
    // T6：顶层 print 正常解析执行
    EXPECT_ALL_BACKENDS(src, "after");
}

// ============================================================
// B-006 JIT 方法调用 IC 多态调用点
// ============================================================
#ifdef MINILANG_USE_JIT
TEST(BacklogRegression, B006JitMethodCallICPolymorphic) {
    const std::string src = "class A { fun tag() { return \"A\"; } }"
                            "class B { fun tag() { return \"B\"; } }"
                            "fun callTag(x) { return x.tag(); }"
                            "print(callTag(A()));"
                            "print(callTag(B()));"
                            "print(callTag(A()));"
                            "print(callTag(B()));";
    EXPECT_EQ(minilang_test::runJIT(src), "ABAB");
}
#endif

#ifdef MINILANG_USE_JIT
TEST(BacklogRegression, B003ArrayDefaultJIT) {
    EXPECT_EQ(minilang_test::runJIT("fun f(a: int[] = [1, 2]) { return a; } print(f()); print(f([9]));"),
              "[1, 2][9]");
}

TEST(BacklogRegression, B008FinallyAbruptReplacesAbruptJIT) {
    const std::string src = "var i = 0;"
                            "var out = \"\";"
                            "while (i < 3) {"
                            "  i = i + 1;"
                            "  try {"
                            "    if (i == 1) { break; }"
                            "    out = out + \"t\" + i;"
                            "  } finally {"
                            "    if (i == 1) { continue; }"
                            "  }"
                            "}"
                            "print(out);";
    EXPECT_EQ(minilang_test::runJIT(src), "t2t3");
    // 第二次执行命中进程内块缓存（同一字节码指纹），锁定新 opcode 的缓存路径
    EXPECT_EQ(minilang_test::runJIT(src), "t2t3");
}

TEST(BacklogRegression, B008RecursiveSameTrySiteExceptionStashJIT) {
    const std::string src = "fun f(n: int) {"
                            "  try {"
                            "    if (n == 1) { throw \"outer\"; }"
                            "    throw \"inner\";"
                            "  } finally {"
                            "    if (n == 1) {"
                            "      try { f(0); } catch (e) { print(\"caught \" + e); }"
                            "    }"
                            "  }"
                            "}"
                            "try { f(1); } catch (e) { print(\"outer caught \" + e); }";
    EXPECT_EQ(minilang_test::runJIT(src), "caught innerouter caught outer");
}
#endif

// ============================================================
// B-008 finally 边角
// ============================================================
TEST(BacklogRegression, B008FinallyAbruptReplacesAbrupt) {
    const std::string src = "var i = 0;"
                            "var out = \"\";"
                            "while (i < 3) {"
                            "  i = i + 1;"
                            "  try {"
                            "    if (i == 1) { break; }"
                            "    out = out + \"t\" + i;"
                            "  } finally {"
                            "    if (i == 1) { continue; }"
                            "  }"
                            "}"
                            "print(out);";
    EXPECT_ALL_BACKENDS(src, "t2t3");
}

TEST(BacklogRegression, B008RecursiveSameTrySiteExceptionStash) {
    const std::string src = "fun f(n: int) {"
                            "  try {"
                            "    if (n == 1) { throw \"outer\"; }"
                            "    throw \"inner\";"
                            "  } finally {"
                            "    if (n == 1) {"
                            "      try { f(0); } catch (e) { print(\"caught \" + e); }"
                            "    }"
                            "  }"
                            "}"
                            "try { f(1); } catch (e) { print(\"outer caught \" + e); }";
    EXPECT_ALL_BACKENDS(src, "caught innerouter caught outer");
}

// ============================================================
// OPT-01: 单节点 VarRef 原位变异快路径
// ============================================================
TEST(BacklogRegression, OPT01InPlaceMutationPreservesCowIsolation) {
    const std::string src = "class Box { var v = 0; init(x: int) { this.v = x; } }"
                            "var a = [1, 2, 3];"
                            "var b = a;"
                            "a[0] = 9;"
                            "a.push(4);"
                            "print(a); print(\"|\"); print(b); print(\"|\");"
                            "var d = {\"k\": 1};"
                            "var e = d;"
                            "d[\"k\"] = 2;"
                            "d.set(\"x\", 3);"
                            "print(d[\"k\"]); print(d[\"x\"]); print(e[\"k\"]); print(\"|\");"
                            "var o = Box(1);"
                            "var p = o;"
                            "o.v = 5;"
                            "print(o.v); print(p.v);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[9, 2, 3, 4]|[1, 2, 3]|231|51");
}

// ============================================================
// OPT-03: 字符串字面量 Value 缓存（循环 + COW 隔离）
// ============================================================
TEST(BacklogRegression, OPT03StringLiteralCacheIsolation) {
    const std::string src = "var a = [];"
                            "var i = 0;"
                            "while (i < 3) { a.push(\"x\"); i = i + 1; }"
                            "a[0] = a[0] + \"y\";"
                            "print(a[0]); print(\"x\");"
                            "var s = \"lit\";"
                            "var t = s;"
                            "s = s + \"!\";"
                            "print(s); print(t);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "xyxlit!lit");
}

// ============================================================
// OPT-18 B6: int-int 比较特化（GT/GTE/LTE）四后端一致
// ============================================================
TEST(BacklogRegression, OPT18IntComparisonSpecializations) {
    const std::string src = "var a: int = 5;"
                            "var b: int = 3;"
                            "print(a > b); print(\"|\");"
                            "print(a >= b); print(\"|\");"
                            "print(a <= b); print(\"|\");"
                            "print(b > a); print(\"|\");"
                            "print(a >= 5); print(\"|\");"
                            "print(\"x\" < \"y\");";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "true|true|false|false|true|true");
}

#ifdef MINILANG_USE_JIT
TEST(BacklogRegression, OPT18IntComparisonSpecializationsJIT) {
    const std::string src = "var a: int = 5;"
                            "var b: int = 3;"
                            "print(a > b); print(\"|\");"
                            "print(a >= b); print(\"|\");"
                            "print(a <= b); print(\"|\");"
                            "print(b > a); print(\"|\");"
                            "print(a >= 5); print(\"|\");"
                            "print(\"x\" < \"y\");";
    // 新特化操作码在 JIT 复用通用比较代码，覆盖 JIT 编译不再因未知 OpCode 降级
    EXPECT_EQ(minilang_test::runJIT(src), "true|true|false|false|true|true");
}
#endif

// ============================================================
// OPT-19: JIT OP_JUMP_IF_FALSE/OP_NOT 真值判定内联
// （INT/BOOL/NULL 原生路径 + jitTruthy helper 回退；kJitCodegenVersion 6）
// 锁定：真值语义与 Value::isTruthy 五后端一致，内联不得改变任何类型判定
// ============================================================
TEST(BacklogRegression, OPT19TruthyInlineScalarConditions) {
    // 内联路径全覆盖：INT 零/正/负、BOOL 真/假、NULL 经函数参数传递进 if 条件
    const std::string src = "fun probe(v) { if (v) { return 1; } return 0; }"
                            "print(probe(0)); print(\"|\");"
                            "print(probe(7)); print(\"|\");"
                            "print(probe(-3)); print(\"|\");"
                            "print(probe(true)); print(\"|\");"
                            "print(probe(false)); print(\"|\");"
                            "print(probe(null));";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "0|1|1|1|0|0");
}

TEST(BacklogRegression, OPT19TruthyInlineNotOperator) {
    // OP_NOT：内联标量 + helper 回退类型（空串/非空串/FLOAT/容器）逐项锁定
    const std::string src = "print(!0); print(\"|\"); print(!5); print(\"|\"); print(!-2); print(\"|\");"
                            "print(!true); print(\"|\"); print(!false); print(\"|\"); print(!null); print(\"|\");"
                            "print(!!7); print(\"|\"); print(!!0); print(\"|\");"
                            "print(!\"\"); print(\"|\"); print(!\"x\"); print(\"|\");"
                            "print(!0.0); print(\"|\"); print(!0.5); print(\"|\"); print(!-0.0); print(\"|\");"
                            "print(![]); print(\"|\"); print(!{\"k\": 1});";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src,
                         "true|false|false|false|true|true|true|false|true|false|true|false|true|false|false");
}

TEST(BacklogRegression, OPT19TruthyInlineHelperFallbackConditions) {
    // helper 回退路径：空串/非空串/FLOAT 0.0/0.5/数组/字典/闭包作 if 条件
    const std::string src = "var s = \"\";"
                            "if (s) { print(1); } else { print(0); }"
                            "s = \"x\";"
                            "if (s) { print(1); } else { print(0); }"
                            "if (0.0) { print(1); } else { print(0); }"
                            "if (0.5) { print(1); } else { print(0); }"
                            "if ([]) { print(1); } else { print(0); }"
                            "if ({\"k\": 1}) { print(1); } else { print(0); }"
                            "var f = fun() { return 2; };"
                            "if (f) { print(1); } else { print(0); }";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "0101111");
}

TEST(BacklogRegression, OPT19TruthyInlineWhileIntCondition) {
    // while 的每次回边都走 OP_JUMP_IF_FALSE：INT 内联热路径 + 数组元素真值混合
    const std::string src = "var i = 3;"
                            "var sum = 0;"
                            "while (i) { sum = sum + i; i = i - 1; }"
                            "print(sum); print(\"|\");"
                            "var items = [1, 0, 3];"
                            "var j = 0;"
                            "var hit = 0;"
                            "while (j < 3) { if (items[j]) { hit = hit + 1; } j = j + 1; }"
                            "print(hit);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "6|2");
}

// ============================================================
// 2026-10-25 OPT-19 轮发现：IR 常量池复制 -0.0/0.0 位模式失配
// IR 侧 scalarKey 按位模式区分 -0.0/0.0，而 StackVM lowering 曾经
// addConstant 去重复制（equals 判等把 IEEE 相等的两者合并），池长度缩短
// 导致其后所有 LOAD_CONST 索引越界（运行时报"常量池索引越界"）。
// 触发条件：-0.0 常量之后还需引用新常量（"tail"）。
// ============================================================
TEST(BacklogRegression, IRConstPoolCopyKeepsNegativeZeroDistinct) {
    const std::string src = "var z = -0.0;"
                            "var s = \"tail\";"
                            "print(s); print(\"|\"); print(!z);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "tail|true");
}

#ifdef MINILANG_USE_JIT
// ============================================================
// OPT-20: lazy/特化编译 funcTable 代缓存
// 同一 execute() 内 F 次 lazy 编译复用一份 funcTable（原为逐次全量重建，
// O(F²)）。锁定：多函数（含闭包/upvalue/互递归）lazy 编译语义不变。
// ============================================================
TEST(BacklogRegression, OPT20LazyFuncTableReuseMultipleChunks) {
    const std::string src =
        "fun makeCounter() { var n = 0; return fun() { n = n + 1; return n; }; }"
        "fun fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }"
        "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); }"
        "fun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); }"
        "var c = makeCounter();"
        "c(); c(); print(c()); print(\"|\");"
        "print(fib(12)); print(\"|\");"
        "print(isEven(10)); print(\"|\"); print(isOdd(10));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());
    JITBackend jit;
    jit.setLazyCompilation(true);
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "3|144|true|false");
    // 多 chunk 完成 lazy 编译（代缓存在其间被反复复用）
    EXPECT_GE(jit.getLazyCompiledChunks().size(), 4u);
}
#endif

// ============================================================
// OPT-21: IR 路径语句位赋值跳过结果重载
// 语句位 `x = v;`（全局/成员路径）原实现 STORE 消费值后重载再 POP；
// 现跳过重载+配对 POP（表达式位仍重载）。锁定：值语义、栈平衡、
// for update 热路径与 null 接收者报错行为五后端一致。
// ============================================================
TEST(BacklogRegression, OPT21StmtAssignmentValueSemantics) {
    const std::string src =
        "var g = 1;"
        "g = g + 1;"
        "print(g); print(\"|\");"
        "class C { var f = 0; init(x) { this.f = x; } }"
        "var o = C(5);"
        "o.f = o.f * 2;"
        "print(o.f); print(\"|\");"
        "var y = (g = g + 10);"
        "print(y); print(\"|\"); print(g); print(\"|\");"
        "var z = 0; z = y = g;"
        "print(z); print(y); print(\"|\");"
        "var sum = 0;"
        "for (var i = 0; i < 3; i = i + 1) { sum = sum + i; }"
        "print(sum);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "2|10|12|12|1212|3");
}

TEST(BacklogRegression, OPT21StmtAssignInBlocksAndLoops) {
    const std::string src =
        "var acc = 0;"
        "{ var b = 1; b = b + 1; acc = acc + b; }"
        "if (true) { acc = acc * 10; }"
        "var k = 0;"
        "while (k < 2) { k = k + 1; }"
        "print(acc); print(\"|\"); print(k);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "20|2");
}

TEST(BacklogRegression, OPT21NullReceiverMemberSetStillErrors) {
    // MEMBER_SET 自身对 null 接收者报错（kTypeNotMemberAssignable），
    // 语句位跳过重载不得吞掉该错误
    const std::string src = "var o = null; o.f = 1; print(\"unreachable\");";
    ASSERT_TRUE(parseClean(src));
    EXPECT_NE(minilang_test::runInterpreter(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(minilang_test::runStackVM(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(minilang_test::runStackVM_IR(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(minilang_test::runRegVM(src).find("<runtime:"), std::string::npos);
}

// ============================================================
// OPT-05/OPT-13: 方法返回接收者整体写回——COW 别名隔离
// ============================================================
TEST(BacklogRegression, OPT05ReceiverWholeWritebackAliasIsolation) {
    const std::string src = "class Box { var v = 0;"
                            "  init(x: int) { this.v = x; }"
                            "  fun set(x: int) { this.v = x; }"
                            "  fun bump(): int { this.v = this.v + 1; return this.v; }"
                            "}"
                            "var a = Box(1);"
                            "var b = a;"
                            "a.set(5);"
                            "print(a.v); print(\"|\"); print(b.v); print(\"|\");"
                            "print(a.bump()); print(\"|\"); print(a.v); print(b.v);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "5|1|6|61");
}

// ============================================================
// OPT-07: RegisterVM 名称版全局变量缓存——动态声明/读写路径
// ============================================================
TEST(BacklogRegression, OPT07NameVersionGlobalsAcrossBlocks) {
    const std::string src = "var xs = [];"
                            "var i = 0;"
                            "while (i < 3) {"
                            "  if (true) { var x = i; xs.push(x); }"
                            "  i = i + 1;"
                            "}"
                            "print(xs);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "[0, 1, 2]");
}

// ============================================================
// 2026-10-24 审计修复回归
// ============================================================

TEST(AuditRound2, ParserEnumBadVariantTerminates) {
    // enum 体内出现同步关键字时，synchronize() 不消费 token；必须强制前进而不是死循环。
    Lexer lx;
    auto tk = lx.scan("enum E { A, var x = 1; } print(1);");
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_NE(ast, nullptr);
    EXPECT_TRUE(p.hasErrors());
    EXPECT_LT(p.getDiagnostics().errorCount(), 1000);
}

TEST(AuditRound2, ParserReuseClearsMacroAndTraitTables) {
    const std::string src = "macro inc(x) { x + 1 } print(inc!(1));";
    Lexer lx1;
    auto tk1 = lx1.scan(src);
    Parser p;
    auto ast1 = p.parse(tk1);
    ASSERT_NE(ast1, nullptr);
    ASSERT_FALSE(p.hasErrors());

    Lexer lx2;
    auto tk2 = lx2.scan(src);
    auto ast2 = p.parse(tk2);
    EXPECT_NE(ast2, nullptr);
    EXPECT_FALSE(p.hasErrors()) << "第二次 parse 不应因旧 MacroDecl* 报重复定义";
}

TEST(AuditRound2, InterpreterGcKeepsClassClosureEnv) {
    auto& gc = GcManager::instance();
    const size_t savedThreshold = gc.gcAllocationThreshold();
    gc.setGcAllocationThreshold(1);

    const std::string src = "fun make() {"
                            "  var shared = [1, 2, 3];"
                            "  class C { fun f() { shared.push(4); return len(shared); } }"
                            "  return C();"
                            "}"
                            "var c = make();"
                            "var i = 0;"
                            "while (i < 50) { var t = [i]; i = i + 1; }"
                            "print(c.f());";
    EXPECT_EQ(minilang_test::runInterpreter(src), "4");
    gc.setGcAllocationThreshold(savedThreshold);
}

TEST(AuditRound2, IRDestructureShadowRestoresOuter) {
    // 块内解构绑定遮蔽外层同名变量：内层 10，块退出后外层恢复 1。
    const std::string src = "var a = 1; { var (a, b) = (10, 20); print(a); } print(a);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "101");
}

TEST(AuditRound2, RegisterVmClassMarker) {
    const std::string src = "class A { var x = 1; } print(A);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_ALL_BACKENDS(src, "class:A");
}

TEST(AuditRound2, LintFunctionValueCountsAsUsed) {
    Lexer lx;
    auto tk = lx.scan("fun d(x) { return x + 1; } var a = [1, 2]; var b = map(a, d); print(b);");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    ASSERT_FALSE(p.hasErrors());

    minilang::lint::LintPass pass;
    auto result = pass.analyze(*ast);
    bool unusedFunction = false;
    for (const auto& d : result.diagnostics.all()) {
        if (d.code == "lint-unused-function")
            unusedFunction = true;
    }
    EXPECT_FALSE(unusedFunction);
}

TEST(AuditRound2, FormatterKeepsConstFun) {
    Lexer lx;
    auto tk = lx.scan("const fun f(x) { return x + 1; } print(f(1));");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    ASSERT_FALSE(p.hasErrors());

    Formatter fmt;
    const std::string out = fmt.format(*ast);
    EXPECT_NE(out.find("const fun"), std::string::npos);
}

TEST(AuditRound2, PkgRejectsTraversalNames) {
    EXPECT_FALSE(minilang_pkg::isValidPackageName(".."));
    EXPECT_FALSE(minilang_pkg::isValidPackageName("../evil"));
    EXPECT_FALSE(minilang_pkg::isValidPackageName("a/b"));
    EXPECT_FALSE(minilang_pkg::isValidPackageName("a\\b"));
    EXPECT_FALSE(minilang_pkg::isValidPackageName(".hidden"));
    EXPECT_TRUE(minilang_pkg::isValidPackageName("math-utils"));
    EXPECT_TRUE(minilang_pkg::isValidPackageName("foo.bar"));

    minilang_pkg::PkgConfig cfg;
    cfg.packagesDir = "C:/tmp/mlpkgs";
    EXPECT_TRUE(minilang_pkg::getPackageInstallPath(cfg.packagesDir, "../evil").empty());
    EXPECT_FALSE(minilang_pkg::getPackageInstallPath(cfg.packagesDir, "math-utils").empty());
}

TEST(AuditRound2, RegBytecodeConstantHashDedup) {
    RegBytecodeChunk chunk;
    const uint16_t intA = chunk.addConstant(Value(static_cast<int64_t>(42)));
    const uint16_t intB = chunk.addConstant(Value(static_cast<int64_t>(42)));
    const uint16_t strA = chunk.addConstant(Value(std::string("hello")));
    const uint16_t strB = chunk.addConstant(Value(std::string("hello")));
    const uint16_t intZero = chunk.addConstant(Value(static_cast<int64_t>(0)));
    const uint16_t floatZero = chunk.addConstant(Value(0.0));
    EXPECT_EQ(intA, intB);
    EXPECT_EQ(strA, strB);
    EXPECT_NE(intZero, floatZero); // 类型严格匹配：int 0 与 float 0.0 不得合并
    EXPECT_EQ(chunk.constants.size(), 4u);
}

TEST(AuditRound2, ProtocolContentLengthBounds) {
    EXPECT_FALSE(minilang_cli::parseContentLength("").has_value());
    EXPECT_FALSE(minilang_cli::parseContentLength("abc").has_value());
    EXPECT_FALSE(minilang_cli::parseContentLength("999999999999999999999").has_value());
    EXPECT_FALSE(minilang_cli::parseContentLength("33554433").has_value());
    auto ok = minilang_cli::parseContentLength("42");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 42u);
}

#ifdef MINILANG_USE_JIT
TEST(AuditRound2, JitNaNEqualityUsesGenericPath) {
    const std::string src = "fun eq(x) { return x == x; }"
                            "var inf = 1e308 * 10;"
                            "var nan = inf - inf;"
                            "print(eq(nan));";
    EXPECT_EQ(minilang_test::runJIT(src), "false");
    EXPECT_EQ(minilang_test::runJIT(src), minilang_test::runStackVM(src));
}

TEST(AuditRound2, JitNonNumericArithmeticErrors) {
    const std::string src = "fun f(x) { return x - 1; } print(f(\"abc\"));";
    const std::string jit = minilang_test::runJIT(src);
    EXPECT_NE(jit.find("runtime:"), std::string::npos) << jit;
    EXPECT_EQ(jit.find("-1"), std::string::npos) << jit;
}

TEST(AuditRound2, JitTypeCheckEnforced) {
    const std::string src = "fun g(): int { return \"s\"; } print(g());";
    const std::string jit = minilang_test::runJIT(src);
    EXPECT_NE(jit.find("类型注解违反"), std::string::npos) << jit;
}

TEST(AuditRound2, JitOperandStackBoundsGraceful) {
    // 8 个局部槽 × 256 帧超过 2048 槽操作数栈；必须报递归深度错误而非越界覆盖。
    const std::string src = "fun f(n) {"
                            "  var a = 1; var b = 2; var c = 3; var d = 4;"
                            "  var e = 5; var g = 6; var h = 7;"
                            "  if (n == 0) { return 0; }"
                            "  return f(n - 1) + a + b + c + d + e + g + h;"
                            "}"
                            "print(f(300));";
    const std::string jit = minilang_test::runJIT(src);
    EXPECT_NE(jit.find("递归深度"), std::string::npos) << jit;
}

TEST(AuditRound2, JitSafepointKeepsBlockGlobalCycle) {
    const std::string src = "{"
                            "  var a = [1, 2, 3];"
                            "  var i = 0;"
                            "  while (i < 20000) { var t = [i]; i = i + 1; }"
                            "  print(a[0]);"
                            "}";
    // a 仅由 globals_ 名称表持有；JIT safepoint roots 漏掉它时会被当作孤岛清空。
    EXPECT_EQ(minilang_test::runJIT(src), "1");
}

TEST(AuditRound2, JitLoopLimitPerSiteAndPerCall) {
    auto& cfg = RuntimeLimits::RuntimeConfig::instance();
    const int64_t savedLimit = cfg.maxLoopIterations();
    cfg.setMaxLoopIterations(100);

    // 同一 chunk 内两个各 80 次的循环：独立计数，不应累计成 160 触发上限。
    const std::string twoLoops = "var s = 0; var i = 0;"
                                 "while (i < 80) { s = s + 1; i = i + 1; }"
                                 "i = 0;"
                                 "while (i < 80) { s = s + 1; i = i + 1; }"
                                 "print(s);";
    EXPECT_EQ(minilang_test::runJIT(twoLoops), "160");

    // 同一循环站点被函数调用两次：进入 chunk 时计数重置，不应跨调用累计。
    const std::string repeatedCall = "fun f() { var i = 0; while (i < 80) { i = i + 1; } return i; }"
                                     "print(f() + f());";
    EXPECT_EQ(minilang_test::runJIT(repeatedCall), "160");

    cfg.setMaxLoopIterations(savedLimit);
}
#endif

// ============================================================
// PERF: 语句级赋值结果丢弃 + 赋值表达式嵌套（栈平衡回归）
// ============================================================
TEST(BacklogRegression, PERFAssignmentStatementStackBalance) {
    const std::string src = "var a = 0; var b = 0;"
                            "var i = 0;"
                            "while (i < 3) { a = i; b = (a = a + 1); i = i + 1; }"
                            "print(a); print(\"|\"); print(b); print(\"|\");"
                            "var sum = 0;"
                            "for (var j = 0; j < 3; j = j + 1) { sum = sum + j; }"
                            "print(sum); print(\"|\");"
                            "fun f() { var x = 1; x = x + 2; return x; }"
                            "print(f());";
    ASSERT_TRUE(parseClean(src));
    EXPECT_FIVE_BACKENDS(src, "3|3|3|3");
}

// ============================================================
// PERF: 成员链内联存储——浅链（内联槽）与深链（溢出回退）读写一致
// 仅直译解释器支持深链写入（StackVM/JIT 编译器限制 3 层及以上成员赋值），
// 浅链读写已由既有四后端用例覆盖；此处同时锁定 ChainInfo 两条存储路径。
// ============================================================
TEST(BacklogRegression, PERFChainInlineStorageDeepAndShallow) {
    // 反向建链：新节点独占时设置 next 再赋给 root，避免 COW 别名写克隆。
    const std::string src = "class Node { var next = null; var v = 0;"
                            "  init(x: int) { this.v = x; } }"
                            "var root = Node(0);"
                            "var i = 0;"
                            "while (i < 10) {"
                            "  var n = Node(i + 1);"
                            "  n.next = root;"
                            "  root = n;"
                            "  i = i + 1;"
                            "}"
                            "print(root.next.v); print(\"|\");"
                            "root.next.next.next.next.next.next.next.next.next.next.v = 42;"
                            "print(root.next.next.next.next.next.next.next.next.next.next.v);";
    ASSERT_TRUE(parseClean(src));
    EXPECT_EQ(minilang_test::runInterpreter(src), "9|42");
}


// ============================================================
// B-010/B-011: 顶层块 fresh 全局变量的成员/索引赋值
// 根因：currentLocals_ 用 -1 哨兵标记块变量，visitMemberAssign/visitIndexAssign
// 把 -1 当成局部槽 255 发射 OP_MEMBER_SET_LOCAL/OP_INDEX_SET_LOCAL。
// ============================================================
TEST(BacklogRegression, B010BlockScopedGlobalMemberAssign) {
    const std::string src = "class Node { var next = null; var v = 0;"
                            "  init(x: int) { this.v = x; } }"
                            "var root = Node(0);"
                            "var i = 0;"
                            "while (i < 3) { var n = Node(i + 1); n.next = root; root = n; i = i + 1; }"
                            "print(root.v); print(\"|\"); print(root.next.v);";
    EXPECT_FIVE_BACKENDS(src, "3|2");
}

TEST(BacklogRegression, B010BlockScopedGlobalIndexAssign) {
    const std::string src = "var i = 0;"
                            "while (i < 2) { var a = [0, 0]; a[0] = i + 1; print(a[0]); i = i + 1; }";
    EXPECT_FIVE_BACKENDS(src, "12");
}

TEST(BacklogRegression, B010BlockScopedGlobalMethodCall) {
    const std::string src = "class Node { var v = 0;"
                            "  init(x: int) { this.v = x; }"
                            "  fun get(): int { return this.v; } }"
                            "var i = 0;"
                            "var sum = 0;"
                            "while (i < 3) { var n = Node(i + 1); sum = sum + n.get(); i = i + 1; }"
                            "print(sum);";
    EXPECT_FIVE_BACKENDS(src, "6");
}

// B-013: JIT 补齐 OP_MEMBER_SET——2 层嵌套成员赋值（全局/块作用域基变量）
TEST(BacklogRegression, B013NestedMemberAssign) {
    const std::string src = "class Inner { var v = 0; }"
                            "class Node { var next = null; }"
                            "var n = Node(); n.next = Inner(); n.next.v = 9;"
                            "print(n.next.v); print(\"|\");"
                            "var i = 0;"
                            "while (i < 2) { var m = Node(); m.next = Inner(); m.next.v = i + 1;"
                            "  print(m.next.v); i = i + 1; }"
                            // 压力段：100 轮块变量创建/删除，覆盖 PIC 地址复用与延迟释放
                            "var k = 0; var sum = 0;"
                            "while (k < 100) { var q = Node(); q.next = Inner(); q.next.v = k; sum = sum + q.next.v;"
                            "  k = k + 1; }"
                            "print(\"|\"); print(sum);";
    EXPECT_FIVE_BACKENDS(src, "9|12|4950");
}

#ifdef MINILANG_USE_JIT
// B-012: 同一 JITBackend 多次 execute——延迟释放表在下次执行前清空，
// 全局槽/名称全局覆盖与块变量删除后仍保持正确结果。
TEST(BacklogRegression, B012JitRepeatedExecuteRetiresValues) {
    const std::string src = "class Node { var next = null; var v = 0;"
                            "  init(x: int) { this.v = x; } }"
                            "var root = Node(0); var i = 0;"
                            "while (i < 20) { var n = Node(i + 1); n.next = root; root = n; i = i + 1; }"
                            "print(root.v); print(\"|\"); print(root.next.v);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser parser;
    auto ast = parser.parse(tk);
    ASSERT_TRUE(ast);
    Compiler compiler;
    auto compiled = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());
    JITBackend jit;
    for (int round = 0; round < 5; ++round) {
        std::string out;
        jit.setOutputCallback([&](const std::string& s) { out += s; });
        EXPECT_EQ(jit.execute(compiled), JitResult::OK) << "round " << round;
        EXPECT_EQ(out, "20|19") << "round " << round;
    }
}
#endif
