// ============================================================
// TestResultPropagation.cpp — 七特性 MVP 阶段 1：? 传播运算符 × 泛型 enum
// ------------------------------------------------------------
// 既有 TestBuiltinModules.cpp 的 QmarkOperator 测试组锁定了 std/result
// dict 协议路径（{"tag": ...}）；本文件锁定阶段 1 新增的泛型 enum variant
// 路径：`enum Result<T, E> { Ok(T), Err(E) }` 的 variant 值直接支持 `?`。
// 语义（与 dict 路径一致的异常式传播）：
//   - Ok(v)/Some(v) → 解包 v；Ok()/Some() 无 payload → null
//   - Err(e) → 运行时错误"? 传播 Err: <e>"（try/catch 可捕获）
//   - None → 运行时错误"? 传播 None"
//   - 其他 variant / 非 Result 值 → 类型约束错误
// 四后端一致性由 EXPECT_ALL_BACKENDS 锁定。
// ============================================================

#include "common/ThreeBackends.h"
#include <gtest/gtest.h>
#include <string>

// ============================================================
// 1. Ok/Some 解包
// ============================================================

TEST(ResultPropagationEnum, UnwrapsOkVariant) {
    std::string src = R"(
enum Result<T, E> { Ok(T), Err(E) }
var r = Result.Ok(42);
print(r?);
)";
    EXPECT_ALL_BACKENDS(src, "42");
}

TEST(ResultPropagationEnum, UnwrapsSomeVariant) {
    std::string src = R"(
enum Option<T> { Some(T), None }
var o = Option.Some("hello");
print(o?);
)";
    EXPECT_ALL_BACKENDS(src, "hello");
}

// ============================================================
// 2. Err/None 传播（异常式，可被 try/catch 捕获）
// ============================================================

TEST(ResultPropagationEnum, ErrVariantPropagatesCatchable) {
    std::string src = R"(
enum Result<T, E> { Ok(T), Err(E) }
try {
    var x = Result.Err("boom")?;
    print("unreached");
} catch (e) {
    print("caught");
}
)";
    EXPECT_ALL_BACKENDS(src, "caught");
}

TEST(ResultPropagationEnum, NoneVariantPropagatesCatchable) {
    std::string src = R"(
enum Option<T> { Some(T), None }
try {
    var x = Option.None?;
    print("unreached");
} catch (e) {
    print("caught-none");
}
)";
    EXPECT_ALL_BACKENDS(src, "caught-none");
}

// ============================================================
// 3. 传播链：函数内多级 ? 由外层 try/catch 收口
// ============================================================

TEST(ResultPropagationEnum, PropagationChainThroughFunctions) {
    std::string src = R"(
enum Result<T, E> { Ok(T), Err(E) }
fun safe_div(a, b) {
    if (b == 0) { return Result.Err("div by zero"); }
    return Result.Ok(a / b);
}
fun compute(x) {
    var v1 = safe_div(100, x)?;
    var v2 = safe_div(v1, 2)?;
    return v2 + 1;
}
print(compute(5));
try { print(compute(0)); } catch (e) { print("propagated"); }
)";
    // 100/5=20, 20/2=10, 10+1=11
    EXPECT_ALL_BACKENDS(src, "11propagated");
}

// ============================================================
// 4. 边界：无 payload 的 Ok / 用户自定义 enum 名 / 非法 variant
// ============================================================

TEST(ResultPropagationEnum, OkWithoutPayloadUnwrapsToNull) {
    std::string src = R"(
enum Result<T, E> { Ok(T), Err(E) }
enum Signal { Ok }
var s = Signal.Ok;
print(s? == null);
)";
    EXPECT_ALL_BACKENDS(src, "true");
}

TEST(ResultPropagationEnum, CustomEnumNameWithOkErrVariantsWorks) {
    // ? 按 variantName 分派，enum 名不限定为 Result/Option
    std::string src = R"(
enum MyOutcome<T, E> { Ok(T), Err(E) }
print(MyOutcome.Ok(7)?);
)";
    EXPECT_ALL_BACKENDS(src, "7");
}

TEST(ResultPropagationEnum, NonResultVariantRejected) {
    std::string src = R"(
enum Color { Red, Green }
try {
    var x = Color.Red?;
    print("unreached");
} catch (e) {
    print("type-err");
}
)";
    EXPECT_ALL_BACKENDS(src, "type-err");
}

// ============================================================
// 5. 与 match 表达式协同（? 解包后的值参与后续模式匹配）
// ============================================================

TEST(ResultPropagationEnum, UnwrappedValueUsableInMatch) {
    std::string src = R"(
enum Result<T, E> { Ok(T), Err(E) }
enum Option<T> { Some(T), None }
var inner = Result.Ok(Option.Some(9));
var opt = inner?;
var n = match (opt) {
    case Option.Some(v) => v
    case Option.None => -1
};
print(n);
)";
    EXPECT_ALL_BACKENDS(src, "9");
}
