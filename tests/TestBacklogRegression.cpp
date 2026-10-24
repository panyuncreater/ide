// TestBacklogRegression.cpp — BACKLOG 开放项回归锁（B-001/B-002/B-003/B-006/B-008）
// B-001: 复合类型实参（dict[..]/fun(..)）显式类型实参解析
// B-002: enum 变体构造携带显式类型实参
// B-003: 数组/字典字面量默认参数（四后端绑定 + COW 隔离）
// B-006: JIT 方法调用 IC 多态调用点键稳定性
// B-008: finally 同帧 abrupt 替换 + 递归同 try 站点异常暂存

#include "common/ThreeBackends.h"
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
