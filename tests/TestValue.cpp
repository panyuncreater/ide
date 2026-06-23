// ============================================================
// Value 单元测试
// ------------------------------------------------------------
// 覆盖 Value 类的所有功能点：
//   1.  各类型构造与访问器（int/float/bool/string/array/dict/null/closure/instance）
//   2.  类型查询方法（isXxx / getType / isNumber）及互斥性
//   3.  COW（Copy-On-Write）语义：拷贝共享、ensureUnique detach、tryGetMutableXxx
//   4.  深拷贝 clone() 及嵌套结构独立性
//   5.  toString 各类型字符串表示
//   6.  equals 相等比较（含 int/float 跨类型比较）
//   7.  isTruthy 真值判断
//   8.  数组操作（访问/大小/push/嵌套）
//   9.  字典操作（访问/大小/键存在性/插入）
//   10. 工具方法（toDouble / typeName / getType / type 别名）
//   11. 实例字段操作
//   12. 闭包操作（capturedVars / params / env）
// ============================================================

#include <gtest/gtest.h>

#include "interpreter/Value.h"
#include "interpreter/Environment.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// ============================================================
// 1. 各类型构造与访问器
// ============================================================

TEST(ValueConstructionTest, DefaultConstructIsNull) {
    // 默认构造应为 null
    Value v;
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(v.getType(), ValueType::VAL_NULL);
}

TEST(ValueConstructionTest, Int64Construct) {
    Value v(int64_t(42));
    EXPECT_TRUE(v.isInt());
    EXPECT_EQ(v.intVal(), 42);
    EXPECT_EQ(v.getType(), ValueType::VAL_INT);
}

TEST(ValueConstructionTest, Int64Negative) {
    Value v(int64_t(-100));
    EXPECT_TRUE(v.isInt());
    EXPECT_EQ(v.intVal(), -100);
}

TEST(ValueConstructionTest, IntConstruct) {
    // int 便捷构造重载
    Value v(int(7));
    EXPECT_TRUE(v.isInt());
    EXPECT_EQ(v.intVal(), 7);
}

TEST(ValueConstructionTest, FloatConstruct) {
    Value v(3.14);
    EXPECT_TRUE(v.isFloat());
    EXPECT_DOUBLE_EQ(v.floatVal(), 3.14);
    EXPECT_EQ(v.getType(), ValueType::VAL_FLOAT);
}

TEST(ValueConstructionTest, BoolConstruct) {
    Value v(true);
    EXPECT_TRUE(v.isBool());
    EXPECT_EQ(v.boolVal(), true);
    EXPECT_EQ(v.getType(), ValueType::VAL_BOOL);
}

TEST(ValueConstructionTest, BoolFalse) {
    Value v(false);
    EXPECT_TRUE(v.isBool());
    EXPECT_EQ(v.boolVal(), false);
}

TEST(ValueConstructionTest, StringConstruct) {
    Value v(std::string("hello"));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "hello");
    EXPECT_EQ(v.getType(), ValueType::VAL_STRING);
}

TEST(ValueConstructionTest, StringMoveConstruct) {
    std::string s = "world";
    Value v(std::move(s));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "world");
}

TEST(ValueConstructionTest, ConstCharConstruct) {
    Value v("literal");
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "literal");
}

TEST(ValueConstructionTest, EmptyString) {
    Value v(std::string(""));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal().size(), 0u);
}

TEST(ValueConstructionTest, ArrayConstruct) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value v(elems);
    EXPECT_TRUE(v.isArray());
    EXPECT_EQ(v.arrayVal().size(), 3u);
    EXPECT_EQ(v.arrayVal()[0].intVal(), 1);
    EXPECT_EQ(v.arrayVal()[1].intVal(), 2);
    EXPECT_EQ(v.arrayVal()[2].intVal(), 3);
    EXPECT_EQ(v.getType(), ValueType::VAL_ARRAY);
}

TEST(ValueConstructionTest, ArrayMoveConstruct) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value v(std::move(elems));
    EXPECT_TRUE(v.isArray());
    EXPECT_EQ(v.arrayVal().size(), 2u);
}

TEST(ValueConstructionTest, EmptyArray) {
    std::vector<Value> elems;
    Value v(elems);
    EXPECT_TRUE(v.isArray());
    EXPECT_EQ(v.arrayVal().size(), 0u);
}

TEST(ValueConstructionTest, DictConstruct) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    m["b"] = Value(std::string("two"));
    Value v(m);
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at("a").intVal(), 1);
    EXPECT_EQ(v.dictVal().at("b").stringVal(), "two");
    EXPECT_EQ(v.getType(), ValueType::VAL_DICT);
}

TEST(ValueConstructionTest, DictMoveConstruct) {
    std::unordered_map<std::string, Value> m;
    m["x"] = Value(int64_t(10));
    Value v(std::move(m));
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 1u);
    EXPECT_EQ(v.dictVal().at("x").intVal(), 10);
}

TEST(ValueConstructionTest, EmptyDict) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 0u);
}

TEST(ValueConstructionTest, NullValueFactory) {
    Value v = Value::nullValue();
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(v.getType(), ValueType::VAL_NULL);
}

TEST(ValueConstructionTest, MakeInstance) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_TRUE(v.isInstance());
    EXPECT_EQ(v.className(), "MyClass");
    EXPECT_EQ(v.fields().size(), 0u);
    EXPECT_EQ(v.getType(), ValueType::VAL_INSTANCE);
}

TEST(ValueConstructionTest, MakeInstanceEmptyName) {
    Value v = Value::makeInstance("");
    EXPECT_TRUE(v.isInstance());
    EXPECT_EQ(v.className(), "");
}

TEST(ValueConstructionTest, MakeClosure) {
    auto env = std::make_shared<Environment>();
    std::vector<std::string> params = {"x", "y"};
    Value v = Value::makeClosure("myFunc", env, params, nullptr);
    EXPECT_TRUE(v.isClosure());
    EXPECT_EQ(v.closureName(), "myFunc");
    EXPECT_EQ(v.closureParams().size(), 2u);
    EXPECT_EQ(v.closureParams()[0], "x");
    EXPECT_EQ(v.closureParams()[1], "y");
    EXPECT_EQ(v.closureBody(), nullptr);
    EXPECT_EQ(v.closureEnv(), env);
    EXPECT_EQ(v.getType(), ValueType::VAL_CLOSURE);
}

TEST(ValueConstructionTest, MakeClosureEmptyParams) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_TRUE(v.isClosure());
    EXPECT_EQ(v.closureParams().size(), 0u);
}

TEST(ValueConstructionTest, CopyConstruction) {
    Value original(std::string("hello"));
    Value copy = original;
    EXPECT_EQ(copy.stringVal(), "hello");
    EXPECT_EQ(original.stringVal(), "hello");
}

TEST(ValueConstructionTest, MoveConstruction) {
    Value original(std::string("hello"));
    Value moved = std::move(original);
    EXPECT_EQ(moved.stringVal(), "hello");
    EXPECT_TRUE(moved.isUniquelyOwned());
}

// ============================================================
// 2. 类型查询方法
// ============================================================

TEST(ValueTypeQueryTest, IsInt) {
    Value v(int64_t(42));
    EXPECT_TRUE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_TRUE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsFloat) {
    Value v(3.14);
    EXPECT_FALSE(v.isInt());
    EXPECT_TRUE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_TRUE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsBool) {
    Value v(true);
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_TRUE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsString) {
    Value v(std::string("hello"));
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_TRUE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsNull) {
    Value v;
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_TRUE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsArray) {
    Value v(std::vector<Value>{Value(int64_t(1))});
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_TRUE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsDict) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_TRUE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsInstance) {
    Value v = Value::makeInstance("X");
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_TRUE(v.isInstance());
    EXPECT_FALSE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, IsClosure) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_FALSE(v.isInt());
    EXPECT_FALSE(v.isFloat());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isNull());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isDict());
    EXPECT_FALSE(v.isInstance());
    EXPECT_TRUE(v.isClosure());
    EXPECT_FALSE(v.isNumber());
}

TEST(ValueTypeQueryTest, GetTypeReturnsCorrectEnum) {
    EXPECT_EQ(Value().getType(), ValueType::VAL_NULL);
    EXPECT_EQ(Value(int64_t(1)).getType(), ValueType::VAL_INT);
    EXPECT_EQ(Value(1.0).getType(), ValueType::VAL_FLOAT);
    EXPECT_EQ(Value(true).getType(), ValueType::VAL_BOOL);
    EXPECT_EQ(Value(std::string("x")).getType(), ValueType::VAL_STRING);
    EXPECT_EQ(Value(std::vector<Value>{}).getType(), ValueType::VAL_ARRAY);
    EXPECT_EQ(Value(std::unordered_map<std::string, Value>{}).getType(), ValueType::VAL_DICT);
    EXPECT_EQ(Value::makeInstance("X").getType(), ValueType::VAL_INSTANCE);
    auto env = std::make_shared<Environment>();
    EXPECT_EQ(Value::makeClosure("f", env, {}, nullptr).getType(), ValueType::VAL_CLOSURE);
}

TEST(ValueTypeQueryTest, TypeMethodAlias) {
    // type() 是 getType() 的别名，返回值应一致
    Value v(int64_t(42));
    EXPECT_EQ(v.type(), v.getType());
}

// ============================================================
// 3. COW（Copy-On-Write）语义
// ============================================================

TEST(ValueCowTest, CopyConstructionSharesStringData) {
    Value original(std::string("hello"));
    Value copy = original;
    // 共享后两者都非独占
    EXPECT_FALSE(original.isUniquelyOwned());
    EXPECT_FALSE(copy.isUniquelyOwned());
    EXPECT_EQ(original.stringVal(), "hello");
    EXPECT_EQ(copy.stringVal(), "hello");
}

TEST(ValueCowTest, CopyAssignmentSharesArrayData) {
    Value original(std::vector<Value>{Value(int64_t(1))});
    Value assigned;
    assigned = original;
    EXPECT_FALSE(original.isUniquelyOwned());
    EXPECT_FALSE(assigned.isUniquelyOwned());
    EXPECT_EQ(assigned.arrayVal().size(), 1u);
}

TEST(ValueCowTest, EnsureUniqueDetachesStringOnWrite) {
    Value original(std::string("hello"));
    Value copy = original;
    // 修改 copy 应触发 detach
    copy.stringVal() = "world";
    EXPECT_EQ(original.stringVal(), "hello");
    EXPECT_EQ(copy.stringVal(), "world");
    EXPECT_TRUE(original.isUniquelyOwned());
    EXPECT_TRUE(copy.isUniquelyOwned());
}

TEST(ValueCowTest, EnsureUniqueDetachesArrayOnWrite) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value original(elems);
    Value copy = original;
    copy.arrayVal().push_back(Value(int64_t(3)));
    EXPECT_EQ(original.arrayVal().size(), 2u);
    EXPECT_EQ(copy.arrayVal().size(), 3u);
}

TEST(ValueCowTest, EnsureUniqueDetachesDictOnWrite) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value copy = original;
    copy.dictVal()["b"] = Value(int64_t(2));
    EXPECT_EQ(original.dictVal().size(), 1u);
    EXPECT_EQ(copy.dictVal().size(), 2u);
}

TEST(ValueCowTest, EnsureUniqueDetachesInstanceFieldsOnWrite) {
    Value original = Value::makeInstance("C");
    original.fields()["x"] = Value(int64_t(1));
    Value copy = original;
    copy.fields()["y"] = Value(int64_t(2));
    EXPECT_EQ(original.fields().size(), 1u);
    EXPECT_EQ(copy.fields().size(), 2u);
}

TEST(ValueCowTest, TryGetMutableArrayWhenUnique) {
    Value v(std::vector<Value>{Value(int64_t(1))});
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableArray();
    ASSERT_NE(mut, nullptr);
    mut->push_back(Value(int64_t(2)));
    EXPECT_EQ(v.arrayVal().size(), 2u);
}

TEST(ValueCowTest, TryGetMutableArrayWhenShared) {
    Value original(std::vector<Value>{Value(int64_t(1))});
    Value copy = original;  // 共享，refcount==2
    auto* mut = original.tryGetMutableArray();
    EXPECT_EQ(mut, nullptr);  // 非独占时返回 nullptr
}

TEST(ValueCowTest, TryGetMutableArrayOnNonArray) {
    Value v(int64_t(42));
    auto* mut = v.tryGetMutableArray();
    EXPECT_EQ(mut, nullptr);
}

TEST(ValueCowTest, TryGetMutableDictWhenUnique) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableDict();
    ASSERT_NE(mut, nullptr);
    (*mut)["b"] = Value(int64_t(2));
    EXPECT_EQ(v.dictVal().size(), 2u);
}

TEST(ValueCowTest, TryGetMutableDictWhenShared) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value copy = original;
    auto* mut = original.tryGetMutableDict();
    EXPECT_EQ(mut, nullptr);
}

TEST(ValueCowTest, TryGetMutableFieldsWhenUnique) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableFields();
    ASSERT_NE(mut, nullptr);
    (*mut)["x"] = Value(int64_t(1));
    EXPECT_EQ(v.fields().size(), 1u);
}

TEST(ValueCowTest, TryGetMutableFieldsWhenShared) {
    Value original = Value::makeInstance("MyClass");
    Value copy = original;
    auto* mut = original.tryGetMutableFields();
    EXPECT_EQ(mut, nullptr);
}

TEST(ValueCowTest, ScalarIsAlwaysUniquelyOwned) {
    // 标量类型始终独占
    EXPECT_TRUE(Value(int64_t(1)).isUniquelyOwned());
    EXPECT_TRUE(Value(2.0).isUniquelyOwned());
    EXPECT_TRUE(Value(true).isUniquelyOwned());
    EXPECT_TRUE(Value().isUniquelyOwned());
    // 拷贝后标量仍独占
    Value intV(int64_t(42));
    Value intCopy = intV;
    EXPECT_TRUE(intV.isUniquelyOwned());
    EXPECT_TRUE(intCopy.isUniquelyOwned());
}

TEST(ValueCowTest, ClosureCowDetach) {
    auto env = std::make_shared<Environment>();
    Value original = Value::makeClosure("f", env, {"x"}, nullptr);
    Value copy = original;
    EXPECT_FALSE(original.isUniquelyOwned());
    // 修改 copy 触发 detach
    copy.closureName() = "g";
    EXPECT_EQ(original.closureName(), "f");
    EXPECT_EQ(copy.closureName(), "g");
    EXPECT_TRUE(original.isUniquelyOwned());
    EXPECT_TRUE(copy.isUniquelyOwned());
}

// ============================================================
// 4. 深拷贝
// ============================================================

TEST(ValueDeepCloneTest, StringClone) {
    Value original(std::string("hello"));
    Value cloned = original.clone();
    EXPECT_EQ(cloned.stringVal(), "hello");
    EXPECT_TRUE(original.isUniquelyOwned());
    EXPECT_TRUE(cloned.isUniquelyOwned());
}

TEST(ValueDeepCloneTest, ArrayCloneIndependence) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value original(elems);
    Value cloned = original.clone();
    cloned.arrayVal().push_back(Value(int64_t(3)));
    EXPECT_EQ(original.arrayVal().size(), 2u);
    EXPECT_EQ(cloned.arrayVal().size(), 3u);
}

TEST(ValueDeepCloneTest, DictCloneIndependence) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value cloned = original.clone();
    cloned.dictVal()["b"] = Value(int64_t(2));
    EXPECT_EQ(original.dictVal().size(), 1u);
    EXPECT_EQ(cloned.dictVal().size(), 2u);
}

TEST(ValueDeepCloneTest, NestedArrayCloneIndependence) {
    // 嵌套结构：[[1, 2], [3, 4]]
    std::vector<Value> inner1 = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> inner2 = {Value(int64_t(3)), Value(int64_t(4))};
    std::vector<Value> outer = {Value(inner1), Value(inner2)};
    Value original(outer);
    Value cloned = original.clone();
    // 修改 cloned 的内部数组，不影响 original
    cloned.arrayVal()[0].arrayVal().push_back(Value(int64_t(99)));
    EXPECT_EQ(original.arrayVal()[0].arrayVal().size(), 2u);
    EXPECT_EQ(cloned.arrayVal()[0].arrayVal().size(), 3u);
}

TEST(ValueDeepCloneTest, InstanceCloneIndependence) {
    Value original = Value::makeInstance("MyClass");
    original.fields()["x"] = Value(int64_t(1));
    Value cloned = original.clone();
    cloned.fields()["y"] = Value(int64_t(2));
    EXPECT_EQ(original.fields().size(), 1u);
    EXPECT_EQ(cloned.fields().size(), 2u);
    EXPECT_EQ(original.className(), "MyClass");
    EXPECT_EQ(cloned.className(), "MyClass");
}

TEST(ValueDeepCloneTest, ScalarClone) {
    Value intV(int64_t(42));
    Value cloned = intV.clone();
    EXPECT_EQ(cloned.intVal(), 42);

    Value nullV;
    Value clonedNull = nullV.clone();
    EXPECT_TRUE(clonedNull.isNull());

    Value boolV(true);
    Value clonedBool = boolV.clone();
    EXPECT_EQ(clonedBool.boolVal(), true);
}

TEST(ValueDeepCloneTest, ClosureClone) {
    auto env = std::make_shared<Environment>();
    Value original = Value::makeClosure("f", env, {"x"}, nullptr);
    Value cloned = original.clone();
    EXPECT_EQ(cloned.closureName(), "f");
    EXPECT_EQ(cloned.closureParams().size(), 1u);
    EXPECT_TRUE(original.isUniquelyOwned());
    EXPECT_TRUE(cloned.isUniquelyOwned());
}

// ============================================================
// 5. toString 方法
// ============================================================

TEST(ValueToStringTest, IntToString) {
    EXPECT_EQ(Value(int64_t(42)).toString(), "42");
}

TEST(ValueToStringTest, NegativeIntToString) {
    EXPECT_EQ(Value(int64_t(-7)).toString(), "-7");
}

TEST(ValueToStringTest, ZeroIntToString) {
    EXPECT_EQ(Value(int64_t(0)).toString(), "0");
}

TEST(ValueToStringTest, FloatToString) {
    // %.17g 格式：1.0 -> "1"
    EXPECT_EQ(Value(1.0).toString(), "1");
}

TEST(ValueToStringTest, FloatWithFractionToString) {
    // 2.5 可精确表示
    EXPECT_EQ(Value(2.5).toString(), "2.5");
}

TEST(ValueToStringTest, BoolToString) {
    EXPECT_EQ(Value(true).toString(), "true");
    EXPECT_EQ(Value(false).toString(), "false");
}

TEST(ValueToStringTest, StringToString) {
    EXPECT_EQ(Value(std::string("hello")).toString(), "hello");
}

TEST(ValueToStringTest, EmptyStringToString) {
    EXPECT_EQ(Value(std::string("")).toString(), "");
}

TEST(ValueToStringTest, NullToString) {
    EXPECT_EQ(Value().toString(), "null");
}

TEST(ValueToStringTest, ArrayToString) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value v(elems);
    EXPECT_EQ(v.toString(), "[1, 2, 3]");
}

TEST(ValueToStringTest, EmptyArrayToString) {
    std::vector<Value> elems;
    Value v(elems);
    EXPECT_EQ(v.toString(), "[]");
}

TEST(ValueToStringTest, ArrayWithStringElements) {
    // 数组中的字符串元素会被双引号包裹
    std::vector<Value> elems = {Value(std::string("a")), Value(std::string("b"))};
    Value v(elems);
    EXPECT_EQ(v.toString(), "[\"a\", \"b\"]");
}

TEST(ValueToStringTest, ArrayWithMixedTypes) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(std::string("x")), Value(true)};
    Value v(elems);
    EXPECT_EQ(v.toString(), "[1, \"x\", true]");
}

TEST(ValueToStringTest, NestedArrayToString) {
    std::vector<Value> inner = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> outer = {Value(inner), Value(int64_t(3))};
    Value v(outer);
    EXPECT_EQ(v.toString(), "[[1, 2], 3]");
}

TEST(ValueToStringTest, DictToString) {
    // unordered_map 迭代顺序不确定，检查子串存在性
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    m["b"] = Value(int64_t(2));
    Value v(m);
    std::string s = v.toString();
    EXPECT_NE(s.find("\"a\": 1"), std::string::npos);
    EXPECT_NE(s.find("\"b\": 2"), std::string::npos);
}

TEST(ValueToStringTest, EmptyDictToString) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_EQ(v.toString(), "{}");
}

TEST(ValueToStringTest, DictWithStringValue) {
    std::unordered_map<std::string, Value> m;
    m["key"] = Value(std::string("value"));
    Value v(m);
    std::string s = v.toString();
    EXPECT_NE(s.find("\"key\": \"value\""), std::string::npos);
}

TEST(ValueToStringTest, InstanceToString) {
    Value v = Value::makeInstance("Point");
    v.fields()["x"] = Value(int64_t(1));
    v.fields()["y"] = Value(int64_t(2));
    std::string s = v.toString();
    EXPECT_NE(s.find("Point{"), std::string::npos);
    EXPECT_NE(s.find("x: 1"), std::string::npos);
    EXPECT_NE(s.find("y: 2"), std::string::npos);
}

TEST(ValueToStringTest, EmptyInstanceToString) {
    Value v = Value::makeInstance("Empty");
    EXPECT_EQ(v.toString(), "Empty{}");
}

TEST(ValueToStringTest, ClosureToString) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("myFunc", env, {}, nullptr);
    EXPECT_EQ(v.toString(), "<fun:myFunc>");
}

// ============================================================
// 6. equals 方法
// ============================================================

TEST(ValueEqualsTest, IntEquals) {
    EXPECT_TRUE(Value(int64_t(42)).equals(Value(int64_t(42))));
    EXPECT_FALSE(Value(int64_t(42)).equals(Value(int64_t(43))));
}

TEST(ValueEqualsTest, FloatEquals) {
    EXPECT_TRUE(Value(3.14).equals(Value(3.14)));
    EXPECT_FALSE(Value(3.14).equals(Value(2.71)));
}

TEST(ValueEqualsTest, BoolEquals) {
    EXPECT_TRUE(Value(true).equals(Value(true)));
    EXPECT_FALSE(Value(true).equals(Value(false)));
}

TEST(ValueEqualsTest, StringEquals) {
    EXPECT_TRUE(Value(std::string("hello")).equals(Value(std::string("hello"))));
    EXPECT_FALSE(Value(std::string("hello")).equals(Value(std::string("world"))));
}

TEST(ValueEqualsTest, NullEquals) {
    EXPECT_TRUE(Value().equals(Value()));
    EXPECT_TRUE(Value::nullValue().equals(Value::nullValue()));
}

TEST(ValueEqualsTest, DifferentTypesNotEqual) {
    EXPECT_FALSE(Value(int64_t(1)).equals(Value(std::string("1"))));
    EXPECT_FALSE(Value(int64_t(1)).equals(Value(true)));
    EXPECT_FALSE(Value(int64_t(1)).equals(Value()));
    EXPECT_FALSE(Value(std::string("x")).equals(Value(std::vector<Value>{})));
}

TEST(ValueEqualsTest, IntFloatCrossCompareEqual) {
    // int 1 == float 1.0（1.0 是整数值，在 int64_t 范围内）
    EXPECT_TRUE(Value(int64_t(1)).equals(Value(1.0)));
    EXPECT_TRUE(Value(1.0).equals(Value(int64_t(1))));
}

TEST(ValueEqualsTest, IntFloatCrossCompareNotEqual) {
    // int 1 != float 2.5（有小数部分）
    EXPECT_FALSE(Value(int64_t(1)).equals(Value(2.5)));
    EXPECT_FALSE(Value(2.5).equals(Value(int64_t(1))));
}

TEST(ValueEqualsTest, IntFloatCrossCompareDifferentInt) {
    // int 2 != float 1.0
    EXPECT_FALSE(Value(int64_t(2)).equals(Value(1.0)));
}

TEST(ValueEqualsTest, ArrayEquals) {
    std::vector<Value> a1 = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> a2 = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> a3 = {Value(int64_t(1)), Value(int64_t(3))};
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
    EXPECT_FALSE(Value(a1).equals(Value(a3)));
}

TEST(ValueEqualsTest, ArrayDifferentSizeNotEqual) {
    std::vector<Value> a1 = {Value(int64_t(1))};
    std::vector<Value> a2 = {Value(int64_t(1)), Value(int64_t(2))};
    EXPECT_FALSE(Value(a1).equals(Value(a2)));
}

TEST(ValueEqualsTest, EmptyArrayEquals) {
    std::vector<Value> a1, a2;
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
}

TEST(ValueEqualsTest, NestedArrayEquals) {
    std::vector<Value> a1 = {Value(std::vector<Value>{Value(int64_t(1))})};
    std::vector<Value> a2 = {Value(std::vector<Value>{Value(int64_t(1))})};
    std::vector<Value> a3 = {Value(std::vector<Value>{Value(int64_t(2))})};
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
    EXPECT_FALSE(Value(a1).equals(Value(a3)));
}

TEST(ValueEqualsTest, DictEquals) {
    std::unordered_map<std::string, Value> m1;
    m1["a"] = Value(int64_t(1));
    m1["b"] = Value(int64_t(2));
    std::unordered_map<std::string, Value> m2;
    m2["a"] = Value(int64_t(1));
    m2["b"] = Value(int64_t(2));
    std::unordered_map<std::string, Value> m3;
    m3["a"] = Value(int64_t(1));
    m3["b"] = Value(int64_t(99));
    EXPECT_TRUE(Value(m1).equals(Value(m2)));
    EXPECT_FALSE(Value(m1).equals(Value(m3)));
}

TEST(ValueEqualsTest, DictDifferentSizeNotEqual) {
    std::unordered_map<std::string, Value> m1;
    m1["a"] = Value(int64_t(1));
    std::unordered_map<std::string, Value> m2;
    m2["a"] = Value(int64_t(1));
    m2["b"] = Value(int64_t(2));
    EXPECT_FALSE(Value(m1).equals(Value(m2)));
}

TEST(ValueEqualsTest, DictMissingKeyNotEqual) {
    std::unordered_map<std::string, Value> m1;
    m1["a"] = Value(int64_t(1));
    m1["b"] = Value(int64_t(2));
    std::unordered_map<std::string, Value> m2;
    m2["a"] = Value(int64_t(1));
    m2["c"] = Value(int64_t(2));  // 键名不同
    EXPECT_FALSE(Value(m1).equals(Value(m2)));
}

TEST(ValueEqualsTest, InstanceEquals) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("Point");
    v2.fields()["x"] = Value(int64_t(1));
    EXPECT_TRUE(v1.equals(v2));
}

TEST(ValueEqualsTest, InstanceDifferentClassNotEqual) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("OtherClass");
    v2.fields()["x"] = Value(int64_t(1));
    EXPECT_FALSE(v1.equals(v2));
}

TEST(ValueEqualsTest, InstanceDifferentFieldsNotEqual) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("Point");
    v2.fields()["x"] = Value(int64_t(99));
    EXPECT_FALSE(v1.equals(v2));
}

TEST(ValueEqualsTest, ClosureEqualsSameNameAndEnv) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_TRUE(c1.equals(c2));
}

TEST(ValueEqualsTest, ClosureDifferentNameNotEqual) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = Value::makeClosure("g", env, {}, nullptr);
    EXPECT_FALSE(c1.equals(c2));
}

TEST(ValueEqualsTest, ClosureDifferentEnvNotEqual) {
    auto env1 = std::make_shared<Environment>();
    auto env2 = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env1, {}, nullptr);
    Value c2 = Value::makeClosure("f", env2, {}, nullptr);
    EXPECT_FALSE(c1.equals(c2));
}

// ============================================================
// 7. isTruthy 方法
// ============================================================

TEST(ValueIsTruthyTest, NonZeroIntIsTrue) {
    EXPECT_TRUE(Value(int64_t(1)).isTruthy());
    EXPECT_TRUE(Value(int64_t(-1)).isTruthy());
    EXPECT_TRUE(Value(int64_t(100)).isTruthy());
}

TEST(ValueIsTruthyTest, ZeroIntIsFalse) {
    EXPECT_FALSE(Value(int64_t(0)).isTruthy());
}

TEST(ValueIsTruthyTest, NonZeroFloatIsTrue) {
    EXPECT_TRUE(Value(3.14).isTruthy());
    EXPECT_TRUE(Value(-0.5).isTruthy());
}

TEST(ValueIsTruthyTest, ZeroFloatIsFalse) {
    EXPECT_FALSE(Value(0.0).isTruthy());
}

TEST(ValueIsTruthyTest, BoolTruthy) {
    EXPECT_TRUE(Value(true).isTruthy());
    EXPECT_FALSE(Value(false).isTruthy());
}

TEST(ValueIsTruthyTest, NonEmptyStringIsTrue) {
    EXPECT_TRUE(Value(std::string("hello")).isTruthy());
    EXPECT_TRUE(Value(std::string(" ")).isTruthy());
}

TEST(ValueIsTruthyTest, EmptyStringIsFalse) {
    EXPECT_FALSE(Value(std::string("")).isTruthy());
}

TEST(ValueIsTruthyTest, NullIsFalse) {
    EXPECT_FALSE(Value().isTruthy());
    EXPECT_FALSE(Value::nullValue().isTruthy());
}

TEST(ValueIsTruthyTest, ArrayAlwaysTrue) {
    // 数组无论是否为空，isTruthy 均为 true
    EXPECT_TRUE(Value(std::vector<Value>{}).isTruthy());
    std::vector<Value> nonEmpty = {Value(int64_t(1))};
    EXPECT_TRUE(Value(nonEmpty).isTruthy());
}

TEST(ValueIsTruthyTest, DictAlwaysTrue) {
    // 字典无论是否为空，isTruthy 均为 true
    EXPECT_TRUE(Value(std::unordered_map<std::string, Value>{}).isTruthy());
    std::unordered_map<std::string, Value> nonEmpty;
    nonEmpty["a"] = Value(int64_t(1));
    EXPECT_TRUE(Value(nonEmpty).isTruthy());
}

TEST(ValueIsTruthyTest, InstanceAlwaysTrue) {
    Value v = Value::makeInstance("X");
    EXPECT_TRUE(v.isTruthy());
}

TEST(ValueIsTruthyTest, ClosureAlwaysTrue) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_TRUE(v.isTruthy());
}

// ============================================================
// 8. 数组操作
// ============================================================

TEST(ValueArrayTest, ElementAccess) {
    std::vector<Value> elems = {
        Value(int64_t(10)),
        Value(std::string("hello")),
        Value(true)
    };
    Value v(elems);
    EXPECT_EQ(v.arrayVal()[0].intVal(), 10);
    EXPECT_EQ(v.arrayVal()[1].stringVal(), "hello");
    EXPECT_EQ(v.arrayVal()[2].boolVal(), true);
}

TEST(ValueArrayTest, Size) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value v(elems);
    EXPECT_EQ(v.arrayVal().size(), 3u);
}

TEST(ValueArrayTest, EmptyArraySize) {
    std::vector<Value> elems;
    Value v(elems);
    EXPECT_EQ(v.arrayVal().size(), 0u);
}

TEST(ValueArrayTest, PushBack) {
    std::vector<Value> elems = {Value(int64_t(1))};
    Value v(elems);
    v.arrayVal().push_back(Value(int64_t(2)));
    EXPECT_EQ(v.arrayVal().size(), 2u);
    EXPECT_EQ(v.arrayVal()[1].intVal(), 2);
}

TEST(ValueArrayTest, NestedArrayAccess) {
    std::vector<Value> inner = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> outer = {Value(inner)};
    Value v(outer);
    EXPECT_EQ(v.arrayVal()[0].arrayVal()[0].intVal(), 1);
    EXPECT_EQ(v.arrayVal()[0].arrayVal()[1].intVal(), 2);
}

TEST(ValueArrayTest, MixedTypesArray) {
    std::vector<Value> elems = {
        Value(int64_t(1)),
        Value(2.5),
        Value(std::string("three")),
        Value(true),
        Value()
    };
    Value v(elems);
    EXPECT_EQ(v.arrayVal().size(), 5u);
    EXPECT_TRUE(v.arrayVal()[0].isInt());
    EXPECT_TRUE(v.arrayVal()[1].isFloat());
    EXPECT_TRUE(v.arrayVal()[2].isString());
    EXPECT_TRUE(v.arrayVal()[3].isBool());
    EXPECT_TRUE(v.arrayVal()[4].isNull());
}

TEST(ValueArrayTest, ConstAccessDoesNotDetach) {
    // const 访问不应触发 COW detach
    Value original(std::vector<Value>{Value(int64_t(1))});
    Value copy = original;
    const Value& constCopy = copy;
    // const 访问
    EXPECT_EQ(constCopy.arrayVal().size(), 1u);
    // 仍然共享（未 detach）
    EXPECT_FALSE(copy.isUniquelyOwned());
}

// ============================================================
// 9. 字典操作
// ============================================================

TEST(ValueDictTest, KeyValueAccess) {
    std::unordered_map<std::string, Value> m;
    m["name"] = Value(std::string("Alice"));
    m["age"] = Value(int64_t(30));
    Value v(m);
    EXPECT_EQ(v.dictVal().at("name").stringVal(), "Alice");
    EXPECT_EQ(v.dictVal().at("age").intVal(), 30);
}

TEST(ValueDictTest, Size) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    m["b"] = Value(int64_t(2));
    m["c"] = Value(int64_t(3));
    Value v(m);
    EXPECT_EQ(v.dictVal().size(), 3u);
}

TEST(ValueDictTest, EmptyDictSize) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_EQ(v.dictVal().size(), 0u);
}

TEST(ValueDictTest, KeyExists) {
    std::unordered_map<std::string, Value> m;
    m["exists"] = Value(int64_t(1));
    Value v(m);
    EXPECT_NE(v.dictVal().find("exists"), v.dictVal().end());
    EXPECT_EQ(v.dictVal().find("missing"), v.dictVal().end());
}

TEST(ValueDictTest, InsertNewKey) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    v.dictVal()["b"] = Value(std::string("new"));
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at("b").stringVal(), "new");
}

TEST(ValueDictTest, OverwriteExistingKey) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    v.dictVal()["a"] = Value(int64_t(99));
    EXPECT_EQ(v.dictVal().size(), 1u);
    EXPECT_EQ(v.dictVal().at("a").intVal(), 99);
}

TEST(ValueDictTest, MixedValueTypes) {
    std::unordered_map<std::string, Value> m;
    m["int"] = Value(int64_t(1));
    m["float"] = Value(2.5);
    m["str"] = Value(std::string("hello"));
    m["bool"] = Value(true);
    m["null"] = Value();
    m["arr"] = Value(std::vector<Value>{Value(int64_t(1))});
    Value v(m);
    EXPECT_EQ(v.dictVal().size(), 6u);
    EXPECT_TRUE(v.dictVal().at("int").isInt());
    EXPECT_TRUE(v.dictVal().at("float").isFloat());
    EXPECT_TRUE(v.dictVal().at("str").isString());
    EXPECT_TRUE(v.dictVal().at("bool").isBool());
    EXPECT_TRUE(v.dictVal().at("null").isNull());
    EXPECT_TRUE(v.dictVal().at("arr").isArray());
}

TEST(ValueDictTest, NestedDictInDict) {
    std::unordered_map<std::string, Value> inner;
    inner["x"] = Value(int64_t(1));
    std::unordered_map<std::string, Value> outer;
    outer["nested"] = Value(inner);
    Value v(outer);
    EXPECT_EQ(v.dictVal().at("nested").dictVal().at("x").intVal(), 1);
}

// ============================================================
// 10. 工具方法（toDouble / typeName）
// ============================================================

TEST(ValueUtilityTest, IntToDouble) {
    EXPECT_DOUBLE_EQ(Value(int64_t(42)).toDouble(), 42.0);
    EXPECT_DOUBLE_EQ(Value(int64_t(-7)).toDouble(), -7.0);
}

TEST(ValueUtilityTest, FloatToDouble) {
    EXPECT_DOUBLE_EQ(Value(3.14).toDouble(), 3.14);
}

TEST(ValueUtilityTest, NonNumberToDoubleReturnsZero) {
    EXPECT_DOUBLE_EQ(Value(std::string("hello")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(Value(true).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(Value().toDouble(), 0.0);
}

TEST(ValueUtilityTest, TypeName) {
    EXPECT_EQ(Value(int64_t(1)).typeName(), "int");
    EXPECT_EQ(Value(1.0).typeName(), "float");
    EXPECT_EQ(Value(true).typeName(), "bool");
    EXPECT_EQ(Value(std::string("x")).typeName(), "string");
    EXPECT_EQ(Value().typeName(), "null");
    EXPECT_EQ(Value(std::vector<Value>{}).typeName(), "array");
    EXPECT_EQ(Value(std::unordered_map<std::string, Value>{}).typeName(), "dict");
    EXPECT_EQ(Value::makeInstance("MyClass").typeName(), "MyClass");
    // 空类名时返回 "instance"
    EXPECT_EQ(Value::makeInstance("").typeName(), "instance");
    auto env = std::make_shared<Environment>();
    EXPECT_EQ(Value::makeClosure("f", env, {}, nullptr).typeName(), "closure");
}

// ============================================================
// 11. 实例字段操作
// ============================================================

TEST(ValueInstanceTest, SetAndGetField) {
    Value v = Value::makeInstance("Point");
    v.fields()["x"] = Value(int64_t(10));
    v.fields()["y"] = Value(int64_t(20));
    EXPECT_EQ(v.fields().size(), 2u);
    EXPECT_EQ(v.fields().at("x").intVal(), 10);
    EXPECT_EQ(v.fields().at("y").intVal(), 20);
}

TEST(ValueInstanceTest, ModifyField) {
    Value v = Value::makeInstance("Counter");
    v.fields()["count"] = Value(int64_t(0));
    v.fields()["count"] = Value(int64_t(1));
    v.fields()["count"] = Value(int64_t(2));
    EXPECT_EQ(v.fields().at("count").intVal(), 2);
    EXPECT_EQ(v.fields().size(), 1u);
}

TEST(ValueInstanceTest, FieldWithComplexValue) {
    Value v = Value::makeInstance("Container");
    v.fields()["items"] = Value(std::vector<Value>{Value(int64_t(1)), Value(int64_t(2))});
    EXPECT_EQ(v.fields().at("items").arrayVal().size(), 2u);
}

TEST(ValueInstanceTest, ClassNameAccess) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_EQ(v.className(), "MyClass");
}

// ============================================================
// 12. 闭包操作
// ============================================================

TEST(ValueClosureTest, CapturedVars) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {"x"}, nullptr);
    v.capturedVars()["y"] = Value(int64_t(42));
    EXPECT_EQ(v.capturedVars().size(), 1u);
    EXPECT_EQ(v.capturedVars().at("y").intVal(), 42);
}

TEST(ValueClosureTest, ParamsAccess) {
    auto env = std::make_shared<Environment>();
    std::vector<std::string> params = {"a", "b", "c"};
    Value v = Value::makeClosure("f", env, params, nullptr);
    EXPECT_EQ(v.closureParams().size(), 3u);
    EXPECT_EQ(v.closureParams()[0], "a");
    EXPECT_EQ(v.closureParams()[1], "b");
    EXPECT_EQ(v.closureParams()[2], "c");
}

TEST(ValueClosureTest, ClosureEnvShared) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    // closureEnv() 返回 weak_ptr.lock() 的 shared_ptr，应指向同一对象
    EXPECT_EQ(v.closureEnv().get(), env.get());
}

TEST(ValueClosureTest, ClosureBodyIsNullByDefault) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_EQ(v.closureBody(), nullptr);
}

TEST(ValueClosureTest, CapturedVarsEmptyByDefault) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_EQ(v.capturedVars().size(), 0u);
}
