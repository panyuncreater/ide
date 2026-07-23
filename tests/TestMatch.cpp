// ============================================================
// R134 模式匹配扩展测试套件
// ------------------------------------------------------------
// 验证 6 种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）+ guard 表达式
// 在四后端 (Interpreter / StackVM / StackVM-IR / RegisterVM) 上的语义一致性。
//
// 覆盖点：
//   1. VARIABLE pattern：绑定整个 scrut 到变量
//   2. TUPLE pattern：元组解构（类型检查 + 元素数检查 + 递归匹配）
//   3. OR pattern：任一子 pattern 匹配即成功
//   4. 嵌套 pattern：VARIANT 内 TUPLE、TUPLE 内 OR 等
//   5. Guard 表达式：pattern 匹配后求值 guard，false 则 fall through
//   6. Guard with binding：guard 引用 pattern 绑定的变量
//   7. 三后端语义一致性（四路径输出对比）
//   8. 边界条件：类型不匹配、元素数不匹配、guard false、OR 全失败
// ============================================================

#include "common/ThreeBackends.h"

#include <string>

using namespace minilang_test;

// ============================================================
// VARIABLE pattern 测试
// ============================================================

// S1: VARIABLE pattern 绑定整个 scrut（基础场景）
TEST(MatchR134, VariablePatternBasic) {
    std::string src = "var x = 42;"
                      "var r = match (x) {"
                      "  case v => v + 1"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "43");
}

// S2: VARIABLE pattern 在 default 前（多 case 选择）
TEST(MatchR134, VariablePatternBeforeDefault) {
    std::string src = "var x = 99;"
                      "var r = match (x) {"
                      "  case 1 => 100"
                      "  case v => v"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "99");
}

// ============================================================
// TUPLE pattern 测试
// ============================================================

// S3: TUPLE pattern 基础元组解构
TEST(MatchR134, TuplePatternBasic) {
    std::string src = "var t = (10, 20);"
                      "var r = match (t) {"
                      "  case (a, b) => a + b"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "30");
}

// S4: TUPLE pattern 嵌套（元组中的元组）
TEST(MatchR134, TuplePatternNested) {
    std::string src = "var t = ((1, 2), 3);"
                      "var r = match (t) {"
                      "  case ((a, b), c) => a + b + c"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "6");
}

// S5: TUPLE pattern 类型不匹配（scrut 非 tuple）
TEST(MatchR134, TuplePatternTypeMismatch) {
    std::string src = "var x = 42;"
                      "var r = match (x) {"
                      "  case (a, b) => a + b"
                      "  default => -1"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "-1");
}

// S6: TUPLE pattern 元素数不匹配
TEST(MatchR134, TuplePatternSizeMismatch) {
    std::string src = "var t = (1, 2, 3);"
                      "var r = match (t) {"
                      "  case (a, b) => a + b"
                      "  default => -1"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "-1");
}

// S7: TUPLE pattern with WILDCARD 元素
TEST(MatchR134, TuplePatternWithWildcard) {
    std::string src = "var t = (1, 2, 3);"
                      "var r = match (t) {"
                      "  case (_, _, c) => c"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "3");
}

// ============================================================
// OR pattern 测试
// ============================================================

// S8: OR pattern 基础（字面量 OR）
TEST(MatchR134, OrPatternLiteral) {
    std::string src = "var r = match (2) {"
                      "  case 1 or 2 or 3 => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S9: OR pattern 全部不匹配 fall through
TEST(MatchR134, OrPatternNoMatch) {
    std::string src = "var r = match (5) {"
                      "  case 1 or 2 or 3 => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "0");
}

// S10: OR pattern with VARIABLE（每个分支绑定不同变量）
TEST(MatchR134, OrPatternWithVariable) {
    std::string src = "var r = match (42) {"
                      "  case 1 or 2 or v => v"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S11: OR pattern with VARIANT（enum variant OR）
TEST(MatchR134, OrPatternVariant) {
    std::string src = "enum Option { Some, None }"
                      "var r = match (Option.None) {"
                      "  case Option.Some => 1"
                      "  case Option.None or Option.Some => 2"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "2");
}

// ============================================================
// Guard 表达式测试
// ============================================================

// S12: Guard 基础（pattern 匹配后求值 guard）
TEST(MatchR134, GuardBasic) {
    std::string src = "var r = match (5) {"
                      "  case x if x > 0 => 1"
                      "  case x if x < 0 => -1"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "1");
}

// S13: Guard false 时 fall through 到下一 case
TEST(MatchR134, GuardFallsThrough) {
    std::string src = "var r = match (-5) {"
                      "  case x if x > 0 => 1"
                      "  case x if x < 0 => -1"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "-1");
}

// S14: Guard with TUPLE pattern binding（guard 引用 pattern 绑定变量）
TEST(MatchR134, GuardWithTupleBinding) {
    std::string src = "var t = (10, 5);"
                      "var r = match (t) {"
                      "  case (a, b) if a > b => 1"
                      "  case (a, b) if a < b => -1"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "1");
}

// S15: Guard with VARIANT binding（guard 引用 variant 字段绑定）
TEST(MatchR134, GuardWithVariantBinding) {
    // R133 fix: enum variant 必须声明参数类型才能接受参数（Some(int) 而非 Some）
    std::string src = "enum Option { Some(int), None }"
                      "var r = match (Option.Some(42)) {"
                      "  case Option.Some(x) if x > 0 => x"
                      "  case Option.Some(x) if x <= 0 => 0"
                      "  case Option.None => -1"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "42");
}

// ============================================================
// 嵌套与综合场景
// ============================================================

// S16: 嵌套 VARIANT pattern（VARIANT 内 LITERAL pattern）
// 注：MiniLang enum 不支持自引用类型（Option 内嵌 Option），改为测 VARIANT 内 LITERAL 嵌套。
TEST(MatchR134, NestedVariantPattern) {
    // R133 fix: enum variant 必须声明参数类型（Some(int)），VARIANT 内嵌套 LITERAL pattern
    std::string src = "enum Option { Some(int), None }"
                      "var r = match (Option.Some(99)) {"
                      "  case Option.Some(99) => 100"
                      "  case Option.Some(x) => x"
                      "  case Option.None => -1"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S17: OR pattern with TUPLE（OR 子 pattern 是 TUPLE）
TEST(MatchR134, OrPatternWithTuple) {
    // R133 fix: 元组字面量在 expression() 中需双层括号（外层 match scrutinee 的 '('，内层元组字面量）
    std::string src = "var r = match ((3, 4)) {"
                      "  case (1, 2) or (3, 4) => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S17a: TUPLE pattern with literal tuple scrutinee (no OR)
TEST(MatchR134, TuplePatternWithLiteralScrutinee) {
    std::string src = "var r = match ((3, 4)) {"
                      "  case (3, 4) => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S17c: var scrutinee + TUPLE pattern with LITERAL sub-patterns
TEST(MatchR134, TuplePatternVarScrutineeLiteralSub) {
    std::string src = "var t = (3, 4);"
                      "var r = match (t) {"
                      "  case (3, 4) => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S17b: OR pattern with LITERAL sub-patterns and tuple scrutinee
TEST(MatchR134, OrPatternWithTupleLiteralScrutinee) {
    std::string src = "var r = match ((3, 4)) {"
                      "  case (1, 2) or (3, 4) => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S18: 嵌套 OR in TUPLE（TUPLE 元素是 OR）
TEST(MatchR134, NestedOrInTuple) {
    std::string src = "var r = match ((2, 3)) {"
                      "  case (1 or 2, 3 or 4) => 100"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "100");
}

// S19: 综合场景：VARIANT + TUPLE + OR + Guard
TEST(MatchR134, ComplexCombination) {
    // R133 fix: enum variant 必须声明参数类型（Circle(int)/Rect(int,int)/Point 无参）
    std::string src = "enum Shape { Circle(int), Rect(int, int), Point }"
                      "var r = match (Shape.Rect(10, 20)) {"
                      "  case Shape.Circle(r) if r > 0 => 1"
                      "  case Shape.Rect(w, h) or Shape.Point => 2"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "2");
}

// S20: 综合场景：多 case 全匹配验证
TEST(MatchR134, MultipleCaseSelection) {
    std::string src = "var r = match (5) {"
                      "  case x if x < 0 => -1"
                      "  case 1 or 2 => 100"
                      "  case x if x > 2 => 200"
                      "  default => 0"
                      "};"
                      "print(r);";
    EXPECT_ALL_BACKENDS(src, "200");
}
