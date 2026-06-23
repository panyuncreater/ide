// ============================================================
// BuiltinMethods 单元测试
// ------------------------------------------------------------
// 覆盖 BuiltinMethods 类的所有功能点：
//   1. 数组内置方法（push / pop / len / remove / contains / join）
//   2. 字典内置方法（len / keys / values / has / contains / get / remove）
//   3. 字符串内置方法（len / upper / lower / split / replace / trim）
//   4. 错误处理（未知方法名 / 参数数量不匹配 / 参数类型不匹配 /
//               空数组 pop / 数组索引越界 / 空分隔符 split 等）
//   5. objectModified 标志位正确性（push/pop/remove 应为 true，
//               len/contains/join/keys/values/has/get/upper/lower/split/replace/trim 应为 false）
// ============================================================

#include <gtest/gtest.h>

#include "interpreter/BuiltinMethods.h"
#include "interpreter/Value.h"
#include "interpreter/Interpreter.h"  // RuntimeError 定义

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================
// 辅助函数：构造 int 值
// ============================================================
static Value makeInt(int64_t v) { return Value(v); }
static Value makeStr(const std::string& s) { return Value(s); }
static Value makeBool(bool b) { return Value(b); }

// ============================================================
// 1. 数组内置方法 - push
// ============================================================

TEST(ArrayBuiltinPushTest, PushSingleElement) {
    // push 一个元素到数组末尾，返回 null，且标记 objectModified
    std::vector<Value> elems = {makeInt(1), makeInt(2)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(3)};

    auto result = BuiltinMethods::handleArrayMethod("push", arr, args, 1, 1);

    EXPECT_TRUE(result.result.isNull());
    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(arr.arrayVal().size(), 3u);
    EXPECT_EQ(arr.arrayVal()[2].intVal(), 3);
}

TEST(ArrayBuiltinPushTest, PushStringElement) {
    // push 字符串元素
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeStr("hello")};

    auto result = BuiltinMethods::handleArrayMethod("push", arr, args, 1, 1);

    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(arr.arrayVal().size(), 2u);
    EXPECT_EQ(arr.arrayVal()[1].stringVal(), "hello");
}

TEST(ArrayBuiltinPushTest, PushToEmptyArray) {
    // push 到空数组
    std::vector<Value> elems;
    Value arr(elems);
    std::vector<Value> args = {makeInt(42)};

    auto result = BuiltinMethods::handleArrayMethod("push", arr, args, 1, 1);

    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(arr.arrayVal().size(), 1u);
    EXPECT_EQ(arr.arrayVal()[0].intVal(), 42);
}

TEST(ArrayBuiltinPushTest, PushWrongArgCount) {
    // push 期望 1 个参数，传入 0 个应抛出异常
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args;

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("push", arr, args, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinPushTest, PushTooManyArgs) {
    // push 期望 1 个参数，传入 2 个应抛出异常
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(2), makeInt(3)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("push", arr, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - pop
// ============================================================

TEST(ArrayBuiltinPopTest, PopLastElement) {
    // pop 返回末尾元素并从数组中移除
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr(elems);

    auto result = BuiltinMethods::handleArrayMethod("pop", arr, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 3);
    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(arr.arrayVal().size(), 2u);
    EXPECT_EQ(arr.arrayVal()[1].intVal(), 2);
}

TEST(ArrayBuiltinPopTest, PopFromStringArray) {
    // pop 字符串元素
    std::vector<Value> elems = {makeStr("a"), makeStr("b")};
    Value arr(elems);

    auto result = BuiltinMethods::handleArrayMethod("pop", arr, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "b");
    EXPECT_EQ(arr.arrayVal().size(), 1u);
}

TEST(ArrayBuiltinPopTest, PopUntilEmpty) {
    // 连续 pop 直到数组为空
    std::vector<Value> elems = {makeInt(10), makeInt(20)};
    Value arr(elems);

    auto r1 = BuiltinMethods::handleArrayMethod("pop", arr, {}, 1, 1);
    auto r2 = BuiltinMethods::handleArrayMethod("pop", arr, {}, 1, 1);

    EXPECT_EQ(r1.result.intVal(), 20);
    EXPECT_EQ(r2.result.intVal(), 10);
    EXPECT_EQ(arr.arrayVal().size(), 0u);
}

TEST(ArrayBuiltinPopTest, PopFromEmptyArrayThrows) {
    // 对空数组调用 pop 应抛出异常
    std::vector<Value> elems;
    Value arr(elems);

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("pop", arr, {}, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinPopTest, PopWithArgsThrows) {
    // pop 期望 0 个参数，传入参数应抛出异常
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(99)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("pop", arr, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - len
// ============================================================

TEST(ArrayBuiltinLenTest, LenOfNonEmptyArray) {
    // len 返回数组长度
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr(elems);

    auto result = BuiltinMethods::handleArrayMethod("len", arr, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 3);
    EXPECT_FALSE(result.objectModified);  // len 不修改对象
}

TEST(ArrayBuiltinLenTest, LenOfEmptyArray) {
    // 空数组 len 返回 0
    std::vector<Value> elems;
    Value arr(elems);

    auto result = BuiltinMethods::handleArrayMethod("len", arr, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 0);
}

TEST(ArrayBuiltinLenTest, LenWithArgsThrows) {
    // len 期望 0 个参数
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("len", arr, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - remove
// ============================================================

TEST(ArrayBuiltinRemoveTest, RemoveByIndex) {
    // remove 按索引移除元素
    std::vector<Value> elems = {makeInt(10), makeInt(20), makeInt(30)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(1)};

    auto result = BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1);

    EXPECT_TRUE(result.result.isNull());
    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(arr.arrayVal().size(), 2u);
    EXPECT_EQ(arr.arrayVal()[0].intVal(), 10);
    EXPECT_EQ(arr.arrayVal()[1].intVal(), 30);
}

TEST(ArrayBuiltinRemoveTest, RemoveFirstElement) {
    // 移除首元素
    std::vector<Value> elems = {makeInt(1), makeInt(2)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(0)};

    auto result = BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1);

    EXPECT_EQ(arr.arrayVal().size(), 1u);
    EXPECT_EQ(arr.arrayVal()[0].intVal(), 2);
}

TEST(ArrayBuiltinRemoveTest, RemoveLastElement) {
    // 移除末尾元素
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(2)};

    auto result = BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1);

    EXPECT_EQ(arr.arrayVal().size(), 2u);
    EXPECT_EQ(arr.arrayVal()[1].intVal(), 2);
}

TEST(ArrayBuiltinRemoveTest, RemoveNegativeIndexThrows) {
    // 负索引应抛出异常
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(-1)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinRemoveTest, RemoveOutOfRangeIndexThrows) {
    // 越界索引应抛出异常
    std::vector<Value> elems = {makeInt(1), makeInt(2)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(5)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinRemoveTest, RemoveFromEmptyArrayThrows) {
    // 对空数组 remove 任何索引都应抛出异常
    std::vector<Value> elems;
    Value arr(elems);
    std::vector<Value> args = {makeInt(0)};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinRemoveTest, RemoveNonIntArgThrows) {
    // remove 参数必须是整数索引
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeStr("0")};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("remove", arr, args, 1, 1),
        RuntimeError);
}

TEST(ArrayBuiltinRemoveTest, RemoveWrongArgCount) {
    // remove 期望 1 个参数
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("remove", arr, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - contains
// ============================================================

TEST(ArrayBuiltinContainsTest, ContainsExistingInt) {
    // contains 找到元素返回 true
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(2)};

    auto result = BuiltinMethods::handleArrayMethod("contains", arr, args, 1, 1);

    EXPECT_TRUE(result.result.boolVal());
    EXPECT_FALSE(result.objectModified);
}

TEST(ArrayBuiltinContainsTest, ContainsMissingInt) {
    // contains 未找到返回 false
    std::vector<Value> elems = {makeInt(1), makeInt(2)};
    Value arr(elems);
    std::vector<Value> args = {makeInt(99)};

    auto result = BuiltinMethods::handleArrayMethod("contains", arr, args, 1, 1);

    EXPECT_FALSE(result.result.boolVal());
}

TEST(ArrayBuiltinContainsTest, ContainsString) {
    // contains 字符串元素
    std::vector<Value> elems = {makeStr("apple"), makeStr("banana")};
    Value arr(elems);
    std::vector<Value> args = {makeStr("banana")};

    auto result = BuiltinMethods::handleArrayMethod("contains", arr, args, 1, 1);

    EXPECT_TRUE(result.result.boolVal());
}

TEST(ArrayBuiltinContainsTest, ContainsEmptyArray) {
    // 空数组 contains 任何值都返回 false
    std::vector<Value> elems;
    Value arr(elems);
    std::vector<Value> args = {makeInt(1)};

    auto result = BuiltinMethods::handleArrayMethod("contains", arr, args, 1, 1);

    EXPECT_FALSE(result.result.boolVal());
}

TEST(ArrayBuiltinContainsTest, ContainsWrongArgCount) {
    // contains 期望 1 个参数
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("contains", arr, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - join
// ============================================================

TEST(ArrayBuiltinJoinTest, JoinWithSeparator) {
    // join 用分隔符连接元素
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr(elems);
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleArrayMethod("join", arr, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "1,2,3");
    EXPECT_FALSE(result.objectModified);
}

TEST(ArrayBuiltinJoinTest, JoinWithoutSeparator) {
    // join 不传分隔符时直接连接
    std::vector<Value> elems = {makeStr("a"), makeStr("b"), makeStr("c")};
    Value arr(elems);

    auto result = BuiltinMethods::handleArrayMethod("join", arr, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "abc");
}

TEST(ArrayBuiltinJoinTest, JoinEmptyArray) {
    // 空数组 join 返回空字符串
    std::vector<Value> elems;
    Value arr(elems);
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleArrayMethod("join", arr, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(ArrayBuiltinJoinTest, JoinSingleElement) {
    // 单元素数组 join
    std::vector<Value> elems = {makeInt(42)};
    Value arr(elems);
    std::vector<Value> args = {makeStr("-")};

    auto result = BuiltinMethods::handleArrayMethod("join", arr, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "42");
}

TEST(ArrayBuiltinJoinTest, JoinMixedTypes) {
    // 混合类型数组 join（使用 toString 转换）
    std::vector<Value> elems = {makeInt(1), makeStr("two"), makeBool(true)};
    Value arr(elems);
    std::vector<Value> args = {makeStr("|")};

    auto result = BuiltinMethods::handleArrayMethod("join", arr, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "1|two|true");
}

TEST(ArrayBuiltinJoinTest, JoinTooManyArgs) {
    // join 最多 1 个参数，传入 2 个应抛出异常
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);
    std::vector<Value> args = {makeStr(","), makeStr("-")};

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("join", arr, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 1. 数组内置方法 - 未知方法
// ============================================================

TEST(ArrayBuiltinUnknownMethodTest, UnknownMethodThrows) {
    // 数组没有 shift/unshift/sort/reverse/map/filter/reduce/indexOf/slice/concat 等方法
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);

    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("shift", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("unshift", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("sort", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("reverse", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("map", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("filter", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("reduce", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("indexOf", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("slice", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("concat", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("size", arr, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleArrayMethod("nonExistentMethod", arr, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - len
// ============================================================

TEST(DictBuiltinLenTest, LenOfNonEmptyDict) {
    // len 返回键值对数量
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    m["b"] = makeInt(2);
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("len", d, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 2);
    EXPECT_FALSE(result.objectModified);
}

TEST(DictBuiltinLenTest, LenOfEmptyDict) {
    // 空字典 len 返回 0
    std::unordered_map<std::string, Value> m;
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("len", d, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 0);
}

TEST(DictBuiltinLenTest, LenWithArgsThrows) {
    // len 期望 0 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("len", d, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - keys
// ============================================================

TEST(DictBuiltinKeysTest, KeysReturnsAllKeys) {
    // keys 返回所有键组成的数组
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    m["b"] = makeInt(2);
    m["c"] = makeInt(3);
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("keys", d, {}, 1, 1);

    EXPECT_TRUE(result.result.isArray());
    EXPECT_EQ(result.result.arrayVal().size(), 3u);
    EXPECT_FALSE(result.objectModified);

    // 收集所有键值（unordered_map 顺序不确定）
    std::vector<std::string> keys;
    for (const auto& k : result.result.arrayVal()) {
        keys.push_back(k.stringVal());
    }
    std::sort(keys.begin(), keys.end());
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
    EXPECT_EQ(keys[2], "c");
}

TEST(DictBuiltinKeysTest, KeysOfEmptyDict) {
    // 空字典 keys 返回空数组
    std::unordered_map<std::string, Value> m;
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("keys", d, {}, 1, 1);

    EXPECT_TRUE(result.result.isArray());
    EXPECT_EQ(result.result.arrayVal().size(), 0u);
}

TEST(DictBuiltinKeysTest, KeysWithArgsThrows) {
    // keys 期望 0 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("keys", d, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - values
// ============================================================

TEST(DictBuiltinValuesTest, ValuesReturnsAllValues) {
    // values 返回所有值组成的数组
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(10);
    m["b"] = makeInt(20);
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("values", d, {}, 1, 1);

    EXPECT_TRUE(result.result.isArray());
    EXPECT_EQ(result.result.arrayVal().size(), 2u);
    EXPECT_FALSE(result.objectModified);

    // 收集所有值（unordered_map 顺序不确定）
    std::vector<int64_t> vals;
    for (const auto& v : result.result.arrayVal()) {
        vals.push_back(v.intVal());
    }
    std::sort(vals.begin(), vals.end());
    ASSERT_EQ(vals.size(), 2u);
    EXPECT_EQ(vals[0], 10);
    EXPECT_EQ(vals[1], 20);
}

TEST(DictBuiltinValuesTest, ValuesOfEmptyDict) {
    // 空字典 values 返回空数组
    std::unordered_map<std::string, Value> m;
    Value d(m);

    auto result = BuiltinMethods::handleDictMethod("values", d, {}, 1, 1);

    EXPECT_TRUE(result.result.isArray());
    EXPECT_EQ(result.result.arrayVal().size(), 0u);
}

TEST(DictBuiltinValuesTest, ValuesWithArgsThrows) {
    // values 期望 0 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("values", d, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - has / contains
// ============================================================

TEST(DictBuiltinHasTest, HasExistingKey) {
    // has 找到键返回 true
    std::unordered_map<std::string, Value> m;
    m["name"] = makeStr("Alice");
    Value d(m);
    std::vector<Value> args = {makeStr("name")};

    auto result = BuiltinMethods::handleDictMethod("has", d, args, 1, 1);

    EXPECT_TRUE(result.result.boolVal());
    EXPECT_FALSE(result.objectModified);
}

TEST(DictBuiltinHasTest, HasMissingKey) {
    // has 未找到键返回 false
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("missing")};

    auto result = BuiltinMethods::handleDictMethod("has", d, args, 1, 1);

    EXPECT_FALSE(result.result.boolVal());
}

TEST(DictBuiltinHasTest, HasOnEmptyDict) {
    // 空字典 has 任何键都返回 false
    std::unordered_map<std::string, Value> m;
    Value d(m);
    std::vector<Value> args = {makeStr("any")};

    auto result = BuiltinMethods::handleDictMethod("has", d, args, 1, 1);

    EXPECT_FALSE(result.result.boolVal());
}

TEST(DictBuiltinContainsTest, ContainsAliasOfHas) {
    // contains 是 has 的别名
    std::unordered_map<std::string, Value> m;
    m["key"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("key")};

    auto result = BuiltinMethods::handleDictMethod("contains", d, args, 1, 1);

    EXPECT_TRUE(result.result.boolVal());
}

TEST(DictBuiltinHasTest, HasWrongArgCount) {
    // has 期望 1 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("has", d, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - get
// ============================================================

TEST(DictBuiltinGetTest, GetExistingKey) {
    // get 获取已存在键的值
    std::unordered_map<std::string, Value> m;
    m["name"] = makeStr("Alice");
    m["age"] = makeInt(30);
    Value d(m);
    std::vector<Value> args = {makeStr("name")};

    auto result = BuiltinMethods::handleDictMethod("get", d, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "Alice");
    EXPECT_FALSE(result.objectModified);
}

TEST(DictBuiltinGetTest, GetMissingKeyReturnsNull) {
    // get 不存在的键返回 null
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("missing")};

    auto result = BuiltinMethods::handleDictMethod("get", d, args, 1, 1);

    EXPECT_TRUE(result.result.isNull());
}

TEST(DictBuiltinGetTest, GetMissingKeyWithDefault) {
    // get 不存在的键返回默认值
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("missing"), makeStr("default")};

    auto result = BuiltinMethods::handleDictMethod("get", d, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "default");
}

TEST(DictBuiltinGetTest, GetMissingKeyWithIntDefault) {
    // 默认值可以是任意类型
    std::unordered_map<std::string, Value> m;
    Value d(m);
    std::vector<Value> args = {makeStr("x"), makeInt(42)};

    auto result = BuiltinMethods::handleDictMethod("get", d, args, 1, 1);

    EXPECT_EQ(result.result.intVal(), 42);
}

TEST(DictBuiltinGetTest, GetExistingKeyIgnoresDefault) {
    // 键存在时忽略默认值
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("a"), makeInt(999)};

    auto result = BuiltinMethods::handleDictMethod("get", d, args, 1, 1);

    EXPECT_EQ(result.result.intVal(), 1);
}

TEST(DictBuiltinGetTest, GetNoArgsThrows) {
    // get 至少需要 1 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("get", d, {}, 1, 1),
        RuntimeError);
}

TEST(DictBuiltinGetTest, GetTooManyArgsThrows) {
    // get 最多 2 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("a"), makeInt(1), makeInt(2)};

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("get", d, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - remove
// ============================================================

TEST(DictBuiltinRemoveTest, RemoveExistingKey) {
    // remove 移除已存在的键
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    m["b"] = makeInt(2);
    Value d(m);
    std::vector<Value> args = {makeStr("a")};

    auto result = BuiltinMethods::handleDictMethod("remove", d, args, 1, 1);

    EXPECT_TRUE(result.result.isNull());
    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(d.dictVal().size(), 1u);
    EXPECT_EQ(d.dictVal().find("a"), d.dictVal().end());
    EXPECT_NE(d.dictVal().find("b"), d.dictVal().end());
}

TEST(DictBuiltinRemoveTest, RemoveMissingKey) {
    // remove 不存在的键不报错（与 std::unordered_map::erase 一致）
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);
    std::vector<Value> args = {makeStr("missing")};

    auto result = BuiltinMethods::handleDictMethod("remove", d, args, 1, 1);

    EXPECT_TRUE(result.objectModified);
    EXPECT_EQ(d.dictVal().size(), 1u);
}

TEST(DictBuiltinRemoveTest, RemoveFromEmptyDict) {
    // 对空字典 remove 不报错
    std::unordered_map<std::string, Value> m;
    Value d(m);
    std::vector<Value> args = {makeStr("any")};

    auto result = BuiltinMethods::handleDictMethod("remove", d, args, 1, 1);

    EXPECT_EQ(d.dictVal().size(), 0u);
}

TEST(DictBuiltinRemoveTest, RemoveWrongArgCount) {
    // remove 期望 1 个参数
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("remove", d, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 2. 字典内置方法 - 未知方法
// ============================================================

TEST(DictBuiltinUnknownMethodTest, UnknownMethodThrows) {
    // 字典没有 set/delete/merge/size 等方法
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);

    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("set", d, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("delete", d, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("merge", d, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("size", d, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleDictMethod("nonExistentMethod", d, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 3. 字符串内置方法 - len
// ============================================================

TEST(StringBuiltinLenTest, LenAsciiString) {
    // len 返回 ASCII 字符串长度
    Value s(makeStr("hello"));

    auto result = BuiltinMethods::handleStringMethod("len", s, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 5);
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinLenTest, LenEmptyString) {
    // 空字符串 len 返回 0
    Value s(makeStr(""));

    auto result = BuiltinMethods::handleStringMethod("len", s, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 0);
}

TEST(StringBuiltinLenTest, LenUtf8String) {
    // len 按 UTF-8 码位计数（非字节数）
    // "你好" 共 2 个码位，6 个字节
    Value s(makeStr("\xe4\xbd\xa0\xe5\xa5\xbd"));  // "你好" UTF-8

    auto result = BuiltinMethods::handleStringMethod("len", s, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 2);
}

TEST(StringBuiltinLenTest, LenMixedAsciiAndUtf8) {
    // 混合 ASCII 与 UTF-8：a你b好c → 5 个码位
    Value s(makeStr("a\xe4\xbd\xa0""b\xe5\xa5\xbd""c"));  // "a你b好c"

    auto result = BuiltinMethods::handleStringMethod("len", s, {}, 1, 1);

    EXPECT_EQ(result.result.intVal(), 5);
}

TEST(StringBuiltinLenTest, LenWithArgsThrows) {
    // len 期望 0 个参数
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("len", s, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 3. 字符串内置方法 - upper
// ============================================================

TEST(StringBuiltinUpperTest, UpperAscii) {
    // upper 转大写
    Value s(makeStr("hello"));

    auto result = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "HELLO");
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinUpperTest, UpperMixedCase) {
    // 混合大小写转大写
    Value s(makeStr("Hello World"));

    auto result = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "HELLO WORLD");
}

TEST(StringBuiltinUpperTest, UpperAlreadyUpper) {
    // 已是大写不变
    Value s(makeStr("ABC"));

    auto result = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "ABC");
}

TEST(StringBuiltinUpperTest, UpperEmptyString) {
    // 空字符串 upper 返回空字符串
    Value s(makeStr(""));

    auto result = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(StringBuiltinUpperTest, UpperDoesNotModifyOriginal) {
    // 字符串不可变，原对象不应被修改
    Value s(makeStr("hello"));

    auto result = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);

    EXPECT_EQ(s.stringVal(), "hello");
    EXPECT_EQ(result.result.stringVal(), "HELLO");
}

TEST(StringBuiltinUpperTest, UpperWithArgsThrows) {
    // upper 期望 0 个参数
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("upper", s, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 3. 字符串内置方法 - lower
// ============================================================

TEST(StringBuiltinLowerTest, LowerAscii) {
    // lower 转小写
    Value s(makeStr("HELLO"));

    auto result = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinLowerTest, LowerMixedCase) {
    // 混合大小写转小写
    Value s(makeStr("Hello World"));

    auto result = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello world");
}

TEST(StringBuiltinLowerTest, LowerAlreadyLower) {
    // 已是小写不变
    Value s(makeStr("abc"));

    auto result = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "abc");
}

TEST(StringBuiltinLowerTest, LowerEmptyString) {
    // 空字符串 lower 返回空字符串
    Value s(makeStr(""));

    auto result = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(StringBuiltinLowerTest, LowerDoesNotModifyOriginal) {
    // 字符串不可变，原对象不应被修改
    Value s(makeStr("HELLO"));

    auto result = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);

    EXPECT_EQ(s.stringVal(), "HELLO");
    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinLowerTest, LowerWithArgsThrows) {
    // lower 期望 0 个参数
    Value s(makeStr("HELLO"));
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("lower", s, args, 1, 1),
        RuntimeError);
}

// ============================================================
// 3. 字符串内置方法 - split
// ============================================================

TEST(StringBuiltinSplitTest, SplitWithSeparator) {
    // split 按分隔符分割
    Value s(makeStr("a,b,c"));
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_TRUE(result.result.isArray());
    EXPECT_EQ(result.result.arrayVal().size(), 3u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "a");
    EXPECT_EQ(result.result.arrayVal()[1].stringVal(), "b");
    EXPECT_EQ(result.result.arrayVal()[2].stringVal(), "c");
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinSplitTest, SplitDefaultSeparator) {
    // split 不传分隔符时默认按空格分割
    Value s(makeStr("hello world foo"));
    std::vector<Value> args;

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(result.result.arrayVal().size(), 3u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "hello");
    EXPECT_EQ(result.result.arrayVal()[1].stringVal(), "world");
    EXPECT_EQ(result.result.arrayVal()[2].stringVal(), "foo");
}

TEST(StringBuiltinSplitTest, SplitNoMatch) {
    // 分隔符不存在时返回单元素数组（原字符串）
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(result.result.arrayVal().size(), 1u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "hello");
}

TEST(StringBuiltinSplitTest, SplitEmptyString) {
    // 空字符串 split 返回单元素空字符串数组
    Value s(makeStr(""));
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(result.result.arrayVal().size(), 1u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "");
}

TEST(StringBuiltinSplitTest, SplitConsecutiveSeparators) {
    // 连续分隔符产生空字符串元素
    Value s(makeStr("a,,b"));
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(result.result.arrayVal().size(), 3u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "a");
    EXPECT_EQ(result.result.arrayVal()[1].stringVal(), "");
    EXPECT_EQ(result.result.arrayVal()[2].stringVal(), "b");
}

TEST(StringBuiltinSplitTest, SplitMultiCharSeparator) {
    // 多字符分隔符
    Value s(makeStr("a::b::c"));
    std::vector<Value> args = {makeStr("::")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(result.result.arrayVal().size(), 3u);
    EXPECT_EQ(result.result.arrayVal()[0].stringVal(), "a");
    EXPECT_EQ(result.result.arrayVal()[1].stringVal(), "b");
    EXPECT_EQ(result.result.arrayVal()[2].stringVal(), "c");
}

TEST(StringBuiltinSplitTest, SplitEmptySeparatorThrows) {
    // 空分隔符应抛出异常
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeStr("")};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("split", s, args, 1, 1),
        RuntimeError);
}

TEST(StringBuiltinSplitTest, SplitDoesNotModifyOriginal) {
    // 字符串不可变，原对象不应被修改
    Value s(makeStr("a,b,c"));
    std::vector<Value> args = {makeStr(",")};

    auto result = BuiltinMethods::handleStringMethod("split", s, args, 1, 1);

    EXPECT_EQ(s.stringVal(), "a,b,c");
}

// ============================================================
// 3. 字符串内置方法 - replace
// ============================================================

TEST(StringBuiltinReplaceTest, ReplaceSingleOccurrence) {
    // replace 替换单次出现
    Value s(makeStr("hello world"));
    std::vector<Value> args = {makeStr("world"), makeStr("MiniLang")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello MiniLang");
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinReplaceTest, ReplaceAllOccurrences) {
    // replace 替换所有出现
    Value s(makeStr("a-b-c-d"));
    std::vector<Value> args = {makeStr("-"), makeStr("+")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "a+b+c+d");
}

TEST(StringBuiltinReplaceTest, ReplaceNoMatch) {
    // 无匹配时返回原字符串
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeStr("xyz"), makeStr("abc")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinReplaceTest, ReplaceWithLongerString) {
    // 替换为更长的字符串
    Value s(makeStr("a.c"));
    std::vector<Value> args = {makeStr("."), makeStr("...")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "a...c");
}

TEST(StringBuiltinReplaceTest, ReplaceWithShorterString) {
    // 替换为更短的字符串
    Value s(makeStr("a...c"));
    std::vector<Value> args = {makeStr("..."), makeStr(".")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "a.c");
}

TEST(StringBuiltinReplaceTest, ReplaceWithEmptyString) {
    // 替换为空字符串（即删除）
    Value s(makeStr("hello world"));
    std::vector<Value> args = {makeStr(" "), makeStr("")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "helloworld");
}

TEST(StringBuiltinReplaceTest, ReplaceEmptyFromReturnsOriginal) {
    // 空 from 字符串返回原字符串
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeStr(""), makeStr("x")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinReplaceTest, ReplaceEmptyString) {
    // 对空字符串 replace 返回空字符串
    Value s(makeStr(""));
    std::vector<Value> args = {makeStr("a"), makeStr("b")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(StringBuiltinReplaceTest, ReplaceInsufficientArgs) {
    // replace 需要 2 个参数
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeStr("a")};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("replace", s, args, 1, 1),
        RuntimeError);
}

TEST(StringBuiltinReplaceTest, ReplaceNoArgsThrows) {
    // replace 至少需要 2 个参数
    Value s(makeStr("hello"));

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("replace", s, {}, 1, 1),
        RuntimeError);
}

TEST(StringBuiltinReplaceTest, ReplaceDoesNotModifyOriginal) {
    // 字符串不可变，原对象不应被修改
    Value s(makeStr("hello world"));
    std::vector<Value> args = {makeStr("world"), makeStr("MiniLang")};

    auto result = BuiltinMethods::handleStringMethod("replace", s, args, 1, 1);

    EXPECT_EQ(s.stringVal(), "hello world");
}

// ============================================================
// 3. 字符串内置方法 - trim
// ============================================================

TEST(StringBuiltinTrimTest, TrimLeadingSpaces) {
    // trim 去除前导空白
    Value s(makeStr("   hello"));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
    EXPECT_FALSE(result.objectModified);
}

TEST(StringBuiltinTrimTest, TrimTrailingSpaces) {
    // trim 去除尾部空白
    Value s(makeStr("hello   "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinTrimTest, TrimBothSides) {
    // trim 去除首尾空白
    Value s(makeStr("   hello world   "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello world");
}

TEST(StringBuiltinTrimTest, TrimTabs) {
    // trim 去除制表符
    Value s(makeStr("\t\thello\t"));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinTrimTest, TrimNewlines) {
    // trim 去除换行符
    Value s(makeStr("\n\rhello\n\r"));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinTrimTest, TrimMixedWhitespace) {
    // trim 去除混合空白
    Value s(makeStr(" \t\n hello \r\n\t "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinTrimTest, TrimNoWhitespace) {
    // 无空白时不变
    Value s(makeStr("hello"));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello");
}

TEST(StringBuiltinTrimTest, TrimEmptyString) {
    // 空字符串 trim 返回空字符串
    Value s(makeStr(""));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(StringBuiltinTrimTest, TrimAllWhitespace) {
    // 全空白字符串 trim 返回空字符串
    Value s(makeStr("   \t\n  "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "");
}

TEST(StringBuiltinTrimTest, TrimInternalSpacesPreserved) {
    // trim 保留内部空白
    Value s(makeStr("  hello   world  "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(result.result.stringVal(), "hello   world");
}

TEST(StringBuiltinTrimTest, TrimWithArgsThrows) {
    // trim 期望 0 个参数
    Value s(makeStr("hello"));
    std::vector<Value> args = {makeInt(1)};

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("trim", s, args, 1, 1),
        RuntimeError);
}

TEST(StringBuiltinTrimTest, TrimDoesNotModifyOriginal) {
    // 字符串不可变，原对象不应被修改
    Value s(makeStr("  hello  "));

    auto result = BuiltinMethods::handleStringMethod("trim", s, {}, 1, 1);

    EXPECT_EQ(s.stringVal(), "  hello  ");
    EXPECT_EQ(result.result.stringVal(), "hello");
}

// ============================================================
// 3. 字符串内置方法 - 未知方法
// ============================================================

TEST(StringBuiltinUnknownMethodTest, UnknownMethodThrows) {
    // 字符串没有 size/length/substring/slice/indexOf/contains/startsWith/endsWith 等方法
    Value s(makeStr("hello"));

    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("size", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("length", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("substring", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("slice", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("indexOf", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("contains", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("startsWith", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("endsWith", s, {}, 1, 1),
        RuntimeError);
    EXPECT_THROW(
        BuiltinMethods::handleStringMethod("nonExistentMethod", s, {}, 1, 1),
        RuntimeError);
}

// ============================================================
// 4. 错误处理 - RuntimeError 携带行号列号
// ============================================================

TEST(BuiltinErrorHandlingTest, ArrayErrorPreservesLineCol) {
    // 数组方法错误应携带调用位置信息
    std::vector<Value> elems;
    Value arr(elems);

    try {
        BuiltinMethods::handleArrayMethod("pop", arr, {}, 42, 7);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        EXPECT_EQ(e.line, 42);
        EXPECT_EQ(e.column, 7);
    }
}

TEST(BuiltinErrorHandlingTest, DictErrorPreservesLineCol) {
    // 字典方法错误应携带调用位置信息
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    Value d(m);

    try {
        BuiltinMethods::handleDictMethod("len", d, {makeInt(1)}, 100, 50);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        EXPECT_EQ(e.line, 100);
        EXPECT_EQ(e.column, 50);
    }
}

TEST(BuiltinErrorHandlingTest, StringErrorPreservesLineCol) {
    // 字符串方法错误应携带调用位置信息
    Value s(makeStr("hello"));

    try {
        BuiltinMethods::handleStringMethod("len", s, {makeInt(1)}, 10, 20);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        EXPECT_EQ(e.line, 10);
        EXPECT_EQ(e.column, 20);
    }
}

TEST(BuiltinErrorHandlingTest, ArrayUnknownMethodErrorMessage) {
    // 错误消息应包含未知方法名
    std::vector<Value> elems = {makeInt(1)};
    Value arr(elems);

    try {
        BuiltinMethods::handleArrayMethod("fooBar", arr, {}, 1, 1);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("fooBar"), std::string::npos);
        EXPECT_NE(msg.find("数组"), std::string::npos);
    }
}

TEST(BuiltinErrorHandlingTest, DictUnknownMethodErrorMessage) {
    // 错误消息应包含未知方法名
    std::unordered_map<std::string, Value> m;
    Value d(m);

    try {
        BuiltinMethods::handleDictMethod("fooBar", d, {}, 1, 1);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("fooBar"), std::string::npos);
        EXPECT_NE(msg.find("字典"), std::string::npos);
    }
}

TEST(BuiltinErrorHandlingTest, StringUnknownMethodErrorMessage) {
    // 错误消息应包含未知方法名
    Value s(makeStr("hello"));

    try {
        BuiltinMethods::handleStringMethod("fooBar", s, {}, 1, 1);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("fooBar"), std::string::npos);
        EXPECT_NE(msg.find("字符串"), std::string::npos);
    }
}

// ============================================================
// 5. objectModified 标志位综合测试
// ============================================================

TEST(BuiltinObjectModifiedTest, ArrayMutatingMethodsSetModified) {
    // push/pop/remove 应设置 objectModified=true
    std::vector<Value> elems = {makeInt(1), makeInt(2), makeInt(3)};
    Value arr1(elems);
    Value arr2(elems);
    Value arr3(elems);

    auto r1 = BuiltinMethods::handleArrayMethod("push", arr1, {makeInt(4)}, 1, 1);
    auto r2 = BuiltinMethods::handleArrayMethod("pop", arr2, {}, 1, 1);
    auto r3 = BuiltinMethods::handleArrayMethod("remove", arr3, {makeInt(0)}, 1, 1);

    EXPECT_TRUE(r1.objectModified);
    EXPECT_TRUE(r2.objectModified);
    EXPECT_TRUE(r3.objectModified);
}

TEST(BuiltinObjectModifiedTest, ArrayNonMutatingMethodsNotModified) {
    // len/contains/join 不应设置 objectModified
    std::vector<Value> elems = {makeInt(1), makeInt(2)};
    Value arr1(elems);
    Value arr2(elems);
    Value arr3(elems);

    auto r1 = BuiltinMethods::handleArrayMethod("len", arr1, {}, 1, 1);
    auto r2 = BuiltinMethods::handleArrayMethod("contains", arr2, {makeInt(1)}, 1, 1);
    auto r3 = BuiltinMethods::handleArrayMethod("join", arr3, {makeStr(",")}, 1, 1);

    EXPECT_FALSE(r1.objectModified);
    EXPECT_FALSE(r2.objectModified);
    EXPECT_FALSE(r3.objectModified);
}

TEST(BuiltinObjectModifiedTest, DictMutatingMethodsSetModified) {
    // remove 应设置 objectModified=true
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    m["b"] = makeInt(2);
    Value d(m);

    auto r = BuiltinMethods::handleDictMethod("remove", d, {makeStr("a")}, 1, 1);

    EXPECT_TRUE(r.objectModified);
}

TEST(BuiltinObjectModifiedTest, DictNonMutatingMethodsNotModified) {
    // len/keys/values/has/contains/get 不应设置 objectModified
    std::unordered_map<std::string, Value> m;
    m["a"] = makeInt(1);
    m["b"] = makeInt(2);
    Value d1(m);
    Value d2(m);
    Value d3(m);
    Value d4(m);
    Value d5(m);
    Value d6(m);

    auto r1 = BuiltinMethods::handleDictMethod("len", d1, {}, 1, 1);
    auto r2 = BuiltinMethods::handleDictMethod("keys", d2, {}, 1, 1);
    auto r3 = BuiltinMethods::handleDictMethod("values", d3, {}, 1, 1);
    auto r4 = BuiltinMethods::handleDictMethod("has", d4, {makeStr("a")}, 1, 1);
    auto r5 = BuiltinMethods::handleDictMethod("contains", d5, {makeStr("a")}, 1, 1);
    auto r6 = BuiltinMethods::handleDictMethod("get", d6, {makeStr("a")}, 1, 1);

    EXPECT_FALSE(r1.objectModified);
    EXPECT_FALSE(r2.objectModified);
    EXPECT_FALSE(r3.objectModified);
    EXPECT_FALSE(r4.objectModified);
    EXPECT_FALSE(r5.objectModified);
    EXPECT_FALSE(r6.objectModified);
}

TEST(BuiltinObjectModifiedTest, StringMethodsNeverModified) {
    // 字符串不可变，所有方法都不应设置 objectModified
    Value s(makeStr("hello"));
    Value s2(makeStr("a,b,c"));
    Value s3(makeStr("hello world"));
    Value s4(makeStr("  hi  "));

    auto r1 = BuiltinMethods::handleStringMethod("len", s, {}, 1, 1);
    auto r2 = BuiltinMethods::handleStringMethod("upper", s, {}, 1, 1);
    auto r3 = BuiltinMethods::handleStringMethod("lower", s, {}, 1, 1);
    auto r4 = BuiltinMethods::handleStringMethod("split", s2, {makeStr(",")}, 1, 1);
    auto r5 = BuiltinMethods::handleStringMethod("replace", s3, {makeStr("world"), makeStr("x")}, 1, 1);
    auto r6 = BuiltinMethods::handleStringMethod("trim", s4, {}, 1, 1);

    EXPECT_FALSE(r1.objectModified);
    EXPECT_FALSE(r2.objectModified);
    EXPECT_FALSE(r3.objectModified);
    EXPECT_FALSE(r4.objectModified);
    EXPECT_FALSE(r5.objectModified);
    EXPECT_FALSE(r6.objectModified);
}
