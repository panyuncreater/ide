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

#include "interpreter/Environment.h"
#include "interpreter/Value.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// 1. 各类型构造与访问器
// ============================================================

TEST(ValueConstructionTest, DefaultConstructIsNull) {
    // 默认构造应为 null
    Value v;
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(v.getType(), ValueType::VAL_NULL);
}

// 验证 int64 构造后能正确识别为整型，且 intVal()/getType() 返回期望值
TEST(ValueConstructionTest, Int64Construct) {
    Value v(int64_t(42));
    EXPECT_TRUE(v.isInt());
    EXPECT_EQ(v.intVal(), 42);
    EXPECT_EQ(v.getType(), ValueType::VAL_INT);
}

// 验证负整数构造与访问（边界：负号值）
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

// 验证 float 构造、访问器与类型枚举正确
TEST(ValueConstructionTest, FloatConstruct) {
    Value v(3.14);
    EXPECT_TRUE(v.isFloat());
    EXPECT_DOUBLE_EQ(v.floatVal(), 3.14);
    EXPECT_EQ(v.getType(), ValueType::VAL_FLOAT);
}

// 验证 bool(true) 构造与访问器
TEST(ValueConstructionTest, BoolConstruct) {
    Value v(true);
    EXPECT_TRUE(v.isBool());
    EXPECT_EQ(v.boolVal(), true);
    EXPECT_EQ(v.getType(), ValueType::VAL_BOOL);
}

// 验证 bool(false) 构造
TEST(ValueConstructionTest, BoolFalse) {
    Value v(false);
    EXPECT_TRUE(v.isBool());
    EXPECT_EQ(v.boolVal(), false);
}

// 验证 string 构造与按值访问
TEST(ValueConstructionTest, StringConstruct) {
    Value v(std::string("hello"));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "hello");
    EXPECT_EQ(v.getType(), ValueType::VAL_STRING);
}

// 验证从 std::string 移动构造，移动后源内容被取走
TEST(ValueConstructionTest, StringMoveConstruct) {
    std::string s = "world";
    Value v(std::move(s));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "world");
}

// 验证 const char* 字面量构造为字符串
TEST(ValueConstructionTest, ConstCharConstruct) {
    Value v("literal");
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal(), "literal");
}

// 验证空字符串构造（边界：零长度字符串）
TEST(ValueConstructionTest, EmptyString) {
    Value v(std::string(""));
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.stringVal().size(), 0u);
}

// 验证数组构造、逐元素访问与 VAL_ARRAY 类型
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

// 验证从 vector<Value> 移动构造数组
TEST(ValueConstructionTest, ArrayMoveConstruct) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value v(std::move(elems));
    EXPECT_TRUE(v.isArray());
    EXPECT_EQ(v.arrayVal().size(), 2u);
}

// 验证空数组构造（边界：零元素）
TEST(ValueConstructionTest, EmptyArray) {
    std::vector<Value> elems;
    Value v(elems);
    EXPECT_TRUE(v.isArray());
    EXPECT_EQ(v.arrayVal().size(), 0u);
}

// 验证字典构造、按 key 访问与 VAL_DICT 类型
TEST(ValueConstructionTest, DictConstruct) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    m["b"] = Value(std::string("two"));
    Value v(m);
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("a")}).intVal(), 1);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("b")}).stringVal(), "two");
    EXPECT_EQ(v.getType(), ValueType::VAL_DICT);
}

// 验证从 map 移动构造字典
TEST(ValueConstructionTest, DictMoveConstruct) {
    std::unordered_map<std::string, Value> m;
    m["x"] = Value(int64_t(10));
    Value v(std::move(m));
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 1u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("x")}).intVal(), 10);
}

// 验证空字典构造（边界：零键值对）
TEST(ValueConstructionTest, EmptyDict) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_TRUE(v.isDict());
    EXPECT_EQ(v.dictVal().size(), 0u);
}

// 验证 Value::nullValue() 工厂返回 null 值
TEST(ValueConstructionTest, NullValueFactory) {
    Value v = Value::nullValue();
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(v.getType(), ValueType::VAL_NULL);
}

// 验证 makeInstance 创建实例、类名正确且字段初始为空
TEST(ValueConstructionTest, MakeInstance) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_TRUE(v.isInstance());
    EXPECT_EQ(v.className(), "MyClass");
    EXPECT_EQ(v.fields().size(), 0u);
    EXPECT_EQ(v.getType(), ValueType::VAL_INSTANCE);
}

// 验证空类名实例（边界：空字符串类名）
TEST(ValueConstructionTest, MakeInstanceEmptyName) {
    Value v = Value::makeInstance("");
    EXPECT_TRUE(v.isInstance());
    EXPECT_EQ(v.className(), "");
}

// 验证 makeClosure 构造闭包及 closureName/params/env/body 等访问器
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

// 验证空参数列表闭包（边界：零参数）
TEST(ValueConstructionTest, MakeClosureEmptyParams) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_TRUE(v.isClosure());
    EXPECT_EQ(v.closureParams().size(), 0u);
}

// 验证拷贝构造后副本与原值相等（COW 共享、不分离）
TEST(ValueConstructionTest, CopyConstruction) {
    Value original(std::string("hello"));
    Value copy = original;
    EXPECT_EQ(copy.stringVal(), "hello");
    EXPECT_EQ(original.stringVal(), "hello");
}

// 验证移动构造后源失效、目标独占（isUniquelyOwned）
TEST(ValueConstructionTest, MoveConstruction) {
    Value original(std::string("hello"));
    Value moved = std::move(original);
    EXPECT_EQ(moved.stringVal(), "hello");
    EXPECT_TRUE(moved.isUniquelyOwned());
}

// ============================================================
// 2. 类型查询方法
// ============================================================

// 验证 int 值 isInt 为真、其余类型判断全为假，并确认 isNumber() 为真
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

// 验证 float 值 isFloat 为真、其余为假、isNumber() 为真
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

// 验证 bool 值 isBool 为真、其余为假、isNumber() 为假
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

// 验证 string 值 isString 为真、其余类型判断全为假
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

// 验证 null 值 isNull 为真、其余类型判断全为假
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

// 验证 array 值 isArray 为真、其余类型判断全为假
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

// 验证 dict 值 isDict 为真、其余类型判断全为假
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

// 验证 instance 值 isInstance 为真、其余类型判断全为假
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

// 验证 closure 值 isClosure 为真、其余类型判断全为假
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

// 验证 getType() 对全部 9 种类型（含 null）返回正确的 ValueType 枚举
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

// 验证 type() 是 getType() 的别名，二者返回值一致
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

// 验证拷贝赋值后数组数据共享、双方均非独占
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

// 验证对共享数组做 push_back 触发 COW 分离，原数组元素数保持不变
TEST(ValueCowTest, EnsureUniqueDetachesArrayOnWrite) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value original(elems);
    Value copy = original;
    copy.arrayVal().push_back(Value(int64_t(3)));
    EXPECT_EQ(original.arrayVal().size(), 2u);
    EXPECT_EQ(copy.arrayVal().size(), 3u);
}

// 验证对共享字典插入新键触发 COW 分离，原字典保持不变
TEST(ValueCowTest, EnsureUniqueDetachesDictOnWrite) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value copy = original;
    copy.dictVal()[Value::DictKey{std::string("b")}] = Value(int64_t(2));
    EXPECT_EQ(original.dictVal().size(), 1u);
    EXPECT_EQ(copy.dictVal().size(), 2u);
}

// 验证对共享实例写字段触发 COW 分离，原实例字段集合不变
TEST(ValueCowTest, EnsureUniqueDetachesInstanceFieldsOnWrite) {
    Value original = Value::makeInstance("C");
    original.fields()["x"] = Value(int64_t(1));
    Value copy = original;
    copy.fields()["y"] = Value(int64_t(2));
    EXPECT_EQ(original.fields().size(), 1u);
    EXPECT_EQ(copy.fields().size(), 2u);
}

// 验证独占数组 tryGetMutableArray 返回非空指针且可原地写入
TEST(ValueCowTest, TryGetMutableArrayWhenUnique) {
    Value v(std::vector<Value>{Value(int64_t(1))});
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableArray();
    ASSERT_NE(mut, nullptr);
    mut->push_back(Value(int64_t(2)));
    EXPECT_EQ(v.arrayVal().size(), 2u);
}

// 验证共享（引用计数>1）数组 tryGetMutableArray 返回 nullptr（禁止原地修改）
TEST(ValueCowTest, TryGetMutableArrayWhenShared) {
    Value original(std::vector<Value>{Value(int64_t(1))});
    Value copy = original; // 共享，refcount==2
    auto* mut = original.tryGetMutableArray();
    EXPECT_EQ(mut, nullptr); // 非独占时返回 nullptr
}

// 验证对非数组类型调用 tryGetMutableArray 返回 nullptr
TEST(ValueCowTest, TryGetMutableArrayOnNonArray) {
    Value v(int64_t(42));
    auto* mut = v.tryGetMutableArray();
    EXPECT_EQ(mut, nullptr);
}

// 验证独占字典 tryGetMutableDict 返回非空指针且可原地写入
TEST(ValueCowTest, TryGetMutableDictWhenUnique) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableDict();
    ASSERT_NE(mut, nullptr);
    (*mut)[Value::DictKey{std::string("b")}] = Value(int64_t(2));
    EXPECT_EQ(v.dictVal().size(), 2u);
}

// 验证共享字典 tryGetMutableDict 返回 nullptr
TEST(ValueCowTest, TryGetMutableDictWhenShared) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value copy = original;
    auto* mut = original.tryGetMutableDict();
    EXPECT_EQ(mut, nullptr);
}

// 验证独占实例 tryGetMutableFields 返回非空指针且可原地写入
TEST(ValueCowTest, TryGetMutableFieldsWhenUnique) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_TRUE(v.isUniquelyOwned());
    auto* mut = v.tryGetMutableFields();
    ASSERT_NE(mut, nullptr);
    (*mut)["x"] = Value(int64_t(1));
    EXPECT_EQ(v.fields().size(), 1u);
}

// 验证共享实例 tryGetMutableFields 返回 nullptr
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

// 验证字符串 clone 产生独立副本且双方均变为独占
TEST(ValueDeepCloneTest, StringClone) {
    Value original(std::string("hello"));
    Value cloned = original.clone();
    EXPECT_EQ(cloned.stringVal(), "hello");
    EXPECT_TRUE(original.isUniquelyOwned());
    EXPECT_TRUE(cloned.isUniquelyOwned());
}

// 验证数组 clone 后修改副本不影响原数组（深独立）
TEST(ValueDeepCloneTest, ArrayCloneIndependence) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2))};
    Value original(elems);
    Value cloned = original.clone();
    cloned.arrayVal().push_back(Value(int64_t(3)));
    EXPECT_EQ(original.arrayVal().size(), 2u);
    EXPECT_EQ(cloned.arrayVal().size(), 3u);
}

// 验证字典 clone 后修改副本不影响原字典
TEST(ValueDeepCloneTest, DictCloneIndependence) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value original(m);
    Value cloned = original.clone();
    cloned.dictVal()[Value::DictKey{std::string("b")}] = Value(int64_t(2));
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

// 验证实例 clone 后修改副本字段不影响原实例，且类名保持不变
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

// 验证标量（int / null / bool）clone 值不变且各自独占
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

// 验证闭包 clone 后名称与参数独立，双方均变为独占
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

// 验证整数 42 的 toString 为 "42"
TEST(ValueToStringTest, IntToString) {
    EXPECT_EQ(Value(int64_t(42)).toString(), "42");
}

// 验证负整数 toString 带负号 "-7"
TEST(ValueToStringTest, NegativeIntToString) {
    EXPECT_EQ(Value(int64_t(-7)).toString(), "-7");
}

// 验证零的 toString 为 "0"
TEST(ValueToStringTest, ZeroIntToString) {
    EXPECT_EQ(Value(int64_t(0)).toString(), "0");
}

TEST(ValueToStringTest, FloatToString) {
    // %.17g 格式：1.0 -> "1"
    EXPECT_EQ(Value(1.0).toString(), "1");
}

// 验证带小数 float 的 toString 精确表示为 "2.5"
TEST(ValueToStringTest, FloatWithFractionToString) {
    // 2.5 可精确表示
    EXPECT_EQ(Value(2.5).toString(), "2.5");
}

// 验证 bool 的 toString 为 "true"/"false"
TEST(ValueToStringTest, BoolToString) {
    EXPECT_EQ(Value(true).toString(), "true");
    EXPECT_EQ(Value(false).toString(), "false");
}

// 验证字符串 toString 等于其字面内容
TEST(ValueToStringTest, StringToString) {
    EXPECT_EQ(Value(std::string("hello")).toString(), "hello");
}

// 验证空字符串 toString 为空串
TEST(ValueToStringTest, EmptyStringToString) {
    EXPECT_EQ(Value(std::string("")).toString(), "");
}

// 验证 null 的 toString 为 "null"
TEST(ValueToStringTest, NullToString) {
    EXPECT_EQ(Value().toString(), "null");
}

// 验证数组 toString 为逗号分隔的 "[1, 2, 3]"
TEST(ValueToStringTest, ArrayToString) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value v(elems);
    EXPECT_EQ(v.toString(), "[1, 2, 3]");
}

// 验证空数组 toString 为 "[]"
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

// 验证混合类型数组 toString 中元素按各自类型格式化
TEST(ValueToStringTest, ArrayWithMixedTypes) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(std::string("x")), Value(true)};
    Value v(elems);
    EXPECT_EQ(v.toString(), "[1, \"x\", true]");
}

// 验证嵌套数组 toString 为 "[[1, 2], 3]"
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

// 验证空字典 toString 为 "{}"
TEST(ValueToStringTest, EmptyDictToString) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_EQ(v.toString(), "{}");
}

// 验证字典字符串值 toString 带双引号
TEST(ValueToStringTest, DictWithStringValue) {
    std::unordered_map<std::string, Value> m;
    m["key"] = Value(std::string("value"));
    Value v(m);
    std::string s = v.toString();
    EXPECT_NE(s.find("\"key\": \"value\""), std::string::npos);
}

// 验证实例 toString 形如 "Point{x: 1, y: 2}"
TEST(ValueToStringTest, InstanceToString) {
    Value v = Value::makeInstance("Point");
    v.fields()["x"] = Value(int64_t(1));
    v.fields()["y"] = Value(int64_t(2));
    std::string s = v.toString();
    EXPECT_NE(s.find("Point{"), std::string::npos);
    EXPECT_NE(s.find("x: 1"), std::string::npos);
    EXPECT_NE(s.find("y: 2"), std::string::npos);
}

// 验证空实例 toString 为 "Empty{}"
TEST(ValueToStringTest, EmptyInstanceToString) {
    Value v = Value::makeInstance("Empty");
    EXPECT_EQ(v.toString(), "Empty{}");
}

// 验证闭包 toString 为 "<fun:myFunc>" 形式
TEST(ValueToStringTest, ClosureToString) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("myFunc", env, {}, nullptr);
    EXPECT_EQ(v.toString(), "<fun:myFunc>");
}

// ============================================================
// 6. equals 方法
// ============================================================

// 验证同值 int 相等、异值不相等
TEST(ValueEqualsTest, IntEquals) {
    EXPECT_TRUE(Value(int64_t(42)).equals(Value(int64_t(42))));
    EXPECT_FALSE(Value(int64_t(42)).equals(Value(int64_t(43))));
}

// 验证同值 float 相等、异值不相等
TEST(ValueEqualsTest, FloatEquals) {
    EXPECT_TRUE(Value(3.14).equals(Value(3.14)));
    EXPECT_FALSE(Value(3.14).equals(Value(2.71)));
}

// 验证 bool 相等/不相等
TEST(ValueEqualsTest, BoolEquals) {
    EXPECT_TRUE(Value(true).equals(Value(true)));
    EXPECT_FALSE(Value(true).equals(Value(false)));
}

// 验证字符串相等/不相等
TEST(ValueEqualsTest, StringEquals) {
    EXPECT_TRUE(Value(std::string("hello")).equals(Value(std::string("hello"))));
    EXPECT_FALSE(Value(std::string("hello")).equals(Value(std::string("world"))));
}

// 验证 null 值与 null 工厂值彼此相等
TEST(ValueEqualsTest, NullEquals) {
    EXPECT_TRUE(Value().equals(Value()));
    EXPECT_TRUE(Value::nullValue().equals(Value::nullValue()));
}

// 验证不同类型（int vs string、int vs bool、string vs array 等）互不相等
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

// 验证同元素数组相等、单个元素不同则不等
TEST(ValueEqualsTest, ArrayEquals) {
    std::vector<Value> a1 = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> a2 = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> a3 = {Value(int64_t(1)), Value(int64_t(3))};
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
    EXPECT_FALSE(Value(a1).equals(Value(a3)));
}

// 验证长度不同的数组不相等
TEST(ValueEqualsTest, ArrayDifferentSizeNotEqual) {
    std::vector<Value> a1 = {Value(int64_t(1))};
    std::vector<Value> a2 = {Value(int64_t(1)), Value(int64_t(2))};
    EXPECT_FALSE(Value(a1).equals(Value(a2)));
}

// 验证两个空数组相等
TEST(ValueEqualsTest, EmptyArrayEquals) {
    std::vector<Value> a1, a2;
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
}

// 验证嵌套数组按元素递归比较相等/不等
TEST(ValueEqualsTest, NestedArrayEquals) {
    std::vector<Value> a1 = {Value(std::vector<Value>{Value(int64_t(1))})};
    std::vector<Value> a2 = {Value(std::vector<Value>{Value(int64_t(1))})};
    std::vector<Value> a3 = {Value(std::vector<Value>{Value(int64_t(2))})};
    EXPECT_TRUE(Value(a1).equals(Value(a2)));
    EXPECT_FALSE(Value(a1).equals(Value(a3)));
}

// 验证同键值字典相等、某个值不同则不等
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

// 验证键数量不同的字典不相等
TEST(ValueEqualsTest, DictDifferentSizeNotEqual) {
    std::unordered_map<std::string, Value> m1;
    m1["a"] = Value(int64_t(1));
    std::unordered_map<std::string, Value> m2;
    m2["a"] = Value(int64_t(1));
    m2["b"] = Value(int64_t(2));
    EXPECT_FALSE(Value(m1).equals(Value(m2)));
}

// 验证键名不同（缺失键）的字典不相等
TEST(ValueEqualsTest, DictMissingKeyNotEqual) {
    std::unordered_map<std::string, Value> m1;
    m1["a"] = Value(int64_t(1));
    m1["b"] = Value(int64_t(2));
    std::unordered_map<std::string, Value> m2;
    m2["a"] = Value(int64_t(1));
    m2["c"] = Value(int64_t(2)); // 键名不同
    EXPECT_FALSE(Value(m1).equals(Value(m2)));
}

// 验证同类同字段实例相等
TEST(ValueEqualsTest, InstanceEquals) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("Point");
    v2.fields()["x"] = Value(int64_t(1));
    EXPECT_TRUE(v1.equals(v2));
}

// 验证类名不同的实例不相等
TEST(ValueEqualsTest, InstanceDifferentClassNotEqual) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("OtherClass");
    v2.fields()["x"] = Value(int64_t(1));
    EXPECT_FALSE(v1.equals(v2));
}

// 验证字段值不同的实例不相等
TEST(ValueEqualsTest, InstanceDifferentFieldsNotEqual) {
    Value v1 = Value::makeInstance("Point");
    v1.fields()["x"] = Value(int64_t(1));
    Value v2 = Value::makeInstance("Point");
    v2.fields()["x"] = Value(int64_t(99));
    EXPECT_FALSE(v1.equals(v2));
}

// L3 fix（2026-07-19）: 闭包相等性契约改为按 functionId 判断。
// 独立 makeClosure 调用创建的闭包不判等（即使 name/env 相同），
// 因为每次 makeClosure 分配全局唯一的 functionId。
TEST(ValueEqualsTest, ClosureIndependentMakeNotEqual) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_FALSE(c1.equals(c2));
}

// L3 fix: 同一闭包值的拷贝判等（COW detach 保留 functionId）。
TEST(ValueEqualsTest, ClosureCopyEqual) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = c1; // 拷贝构造（共享 ClosureData，functionId 相同）
    EXPECT_TRUE(c1.equals(c2));
}

// L3 fix: COW detach 后的拷贝仍判等（拷贝构造保留 functionId）。
TEST(ValueEqualsTest, ClosureCOWDetachEqual) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = c1;
    // 触发 COW detach：修改 c2 的 params 会复制 ClosureData
    c2.closureParams().push_back("x");
    EXPECT_TRUE(c1.equals(c2));
}

// 验证名称不同的闭包不相等（functionId 也不同）
TEST(ValueEqualsTest, ClosureDifferentNameNotEqual) {
    auto env = std::make_shared<Environment>();
    Value c1 = Value::makeClosure("f", env, {}, nullptr);
    Value c2 = Value::makeClosure("g", env, {}, nullptr);
    EXPECT_FALSE(c1.equals(c2));
}

// 验证捕获环境不同的闭包不相等（functionId 也不同）
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

// 验证非零整数（含正、负、大数）为真
TEST(ValueIsTruthyTest, NonZeroIntIsTrue) {
    EXPECT_TRUE(Value(int64_t(1)).isTruthy());
    EXPECT_TRUE(Value(int64_t(-1)).isTruthy());
    EXPECT_TRUE(Value(int64_t(100)).isTruthy());
}

// 验证整数 0 为假
TEST(ValueIsTruthyTest, ZeroIntIsFalse) {
    EXPECT_FALSE(Value(int64_t(0)).isTruthy());
}

// 验证非零 float（含负）为真
TEST(ValueIsTruthyTest, NonZeroFloatIsTrue) {
    EXPECT_TRUE(Value(3.14).isTruthy());
    EXPECT_TRUE(Value(-0.5).isTruthy());
}

// 验证 float 0.0 为假
TEST(ValueIsTruthyTest, ZeroFloatIsFalse) {
    EXPECT_FALSE(Value(0.0).isTruthy());
}

// 验证 true 为真、false 为假
TEST(ValueIsTruthyTest, BoolTruthy) {
    EXPECT_TRUE(Value(true).isTruthy());
    EXPECT_FALSE(Value(false).isTruthy());
}

// 验证非空字符串（含仅空格串）为真
TEST(ValueIsTruthyTest, NonEmptyStringIsTrue) {
    EXPECT_TRUE(Value(std::string("hello")).isTruthy());
    EXPECT_TRUE(Value(std::string(" ")).isTruthy());
}

// 验证空字符串为假
TEST(ValueIsTruthyTest, EmptyStringIsFalse) {
    EXPECT_FALSE(Value(std::string("")).isTruthy());
}

// 验证 null 为假
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

// 验证实例恒为真（与字段内容无关）
TEST(ValueIsTruthyTest, InstanceAlwaysTrue) {
    Value v = Value::makeInstance("X");
    EXPECT_TRUE(v.isTruthy());
}

// 验证闭包恒为真
TEST(ValueIsTruthyTest, ClosureAlwaysTrue) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_TRUE(v.isTruthy());
}

// ============================================================
// 8. 数组操作
// ============================================================

// 验证数组按索引访问混合类型（int/string/bool）元素
TEST(ValueArrayTest, ElementAccess) {
    std::vector<Value> elems = {Value(int64_t(10)), Value(std::string("hello")), Value(true)};
    Value v(elems);
    EXPECT_EQ(v.arrayVal()[0].intVal(), 10);
    EXPECT_EQ(v.arrayVal()[1].stringVal(), "hello");
    EXPECT_EQ(v.arrayVal()[2].boolVal(), true);
}

// 验证数组 size() 返回元素个数
TEST(ValueArrayTest, Size) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value v(elems);
    EXPECT_EQ(v.arrayVal().size(), 3u);
}

// 验证空数组 size() 为 0
TEST(ValueArrayTest, EmptyArraySize) {
    std::vector<Value> elems;
    Value v(elems);
    EXPECT_EQ(v.arrayVal().size(), 0u);
}

// 验证 push_back 追加元素并改变数组大小
TEST(ValueArrayTest, PushBack) {
    std::vector<Value> elems = {Value(int64_t(1))};
    Value v(elems);
    v.arrayVal().push_back(Value(int64_t(2)));
    EXPECT_EQ(v.arrayVal().size(), 2u);
    EXPECT_EQ(v.arrayVal()[1].intVal(), 2);
}

// 验证嵌套数组的二级索引访问
TEST(ValueArrayTest, NestedArrayAccess) {
    std::vector<Value> inner = {Value(int64_t(1)), Value(int64_t(2))};
    std::vector<Value> outer = {Value(inner)};
    Value v(outer);
    EXPECT_EQ(v.arrayVal()[0].arrayVal()[0].intVal(), 1);
    EXPECT_EQ(v.arrayVal()[0].arrayVal()[1].intVal(), 2);
}

// 验证混合类型数组各元素的类型识别（int/float/string/bool/null）
TEST(ValueArrayTest, MixedTypesArray) {
    std::vector<Value> elems = {Value(int64_t(1)), Value(2.5), Value(std::string("three")), Value(true), Value()};
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

// 验证字典按 key 访问字符串与整型值
TEST(ValueDictTest, KeyValueAccess) {
    std::unordered_map<std::string, Value> m;
    m["name"] = Value(std::string("Alice"));
    m["age"] = Value(int64_t(30));
    Value v(m);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("name")}).stringVal(), "Alice");
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("age")}).intVal(), 30);
}

// 验证字典 size() 返回键值对数量
TEST(ValueDictTest, Size) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    m["b"] = Value(int64_t(2));
    m["c"] = Value(int64_t(3));
    Value v(m);
    EXPECT_EQ(v.dictVal().size(), 3u);
}

// 验证空字典 size() 为 0
TEST(ValueDictTest, EmptyDictSize) {
    std::unordered_map<std::string, Value> m;
    Value v(m);
    EXPECT_EQ(v.dictVal().size(), 0u);
}

// 验证 find 能区分存在的键与缺失的键
TEST(ValueDictTest, KeyExists) {
    std::unordered_map<std::string, Value> m;
    m["exists"] = Value(int64_t(1));
    Value v(m);
    EXPECT_NE(v.dictVal().find(Value::DictKey{std::string("exists")}), v.dictVal().end());
    EXPECT_EQ(v.dictVal().find(Value::DictKey{std::string("missing")}), v.dictVal().end());
}

// 验证向字典插入新键后 size 增加且可访问
TEST(ValueDictTest, InsertNewKey) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    v.dictVal()[Value::DictKey{std::string("b")}] = Value(std::string("new"));
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("b")}).stringVal(), "new");
}

// 验证覆盖已有键不改变 size，仅更新值
TEST(ValueDictTest, OverwriteExistingKey) {
    std::unordered_map<std::string, Value> m;
    m["a"] = Value(int64_t(1));
    Value v(m);
    v.dictVal()[Value::DictKey{std::string("a")}] = Value(int64_t(99));
    EXPECT_EQ(v.dictVal().size(), 1u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("a")}).intVal(), 99);
}

// 验证字典可保存混合类型值（int/float/string/bool/null/array）
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
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("int")}).isInt());
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("float")}).isFloat());
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("str")}).isString());
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("bool")}).isBool());
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("null")}).isNull());
    EXPECT_TRUE(v.dictVal().at(Value::DictKey{std::string("arr")}).isArray());
}

// 验证嵌套字典的二级键访问
TEST(ValueDictTest, NestedDictInDict) {
    std::unordered_map<std::string, Value> inner;
    inner["x"] = Value(int64_t(1));
    std::unordered_map<std::string, Value> outer;
    outer["nested"] = Value(inner);
    Value v(outer);
    EXPECT_EQ(
        v.dictVal().at(Value::DictKey{std::string("nested")}).dictVal().at(Value::DictKey{std::string("x")}).intVal(),
        1);
}

// ============================================================
// L4 fix（2026-07-19）: 字典键支持 string/int/bool/float 四种类型
// ============================================================

// 验证 int 键与 string 键不冲突（d[1] 与 d["1"] 是不同的键）
TEST(ValueDictTest, L4_IntKeyDistinctFromStringKey) {
    Value::DictMap m; // R97 #2: 改用 DictMap（含 DictKeyEqual 透明比较器）
    m[Value::DictKey{int64_t(1)}] = Value(int64_t(100));
    m[Value::DictKey{std::string("1")}] = Value(int64_t(200));
    Value v(std::move(m));
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{int64_t(1)}).intVal(), 100);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{std::string("1")}).intVal(), 200);
}

// 验证 bool 键与 int 键不冲突（d[true] 与 d[1] 是不同的键）
TEST(ValueDictTest, L4_BoolKeyDistinctFromIntKey) {
    Value::DictMap m; // R97 #2: 改用 DictMap（含 DictKeyEqual 透明比较器）
    m[Value::DictKey{true}] = Value(int64_t(1));
    m[Value::DictKey{int64_t(1)}] = Value(int64_t(2));
    Value v(std::move(m));
    EXPECT_EQ(v.dictVal().size(), 2u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{true}).intVal(), 1);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{int64_t(1)}).intVal(), 2);
}

// 验证 float 键
TEST(ValueDictTest, L4_FloatKey) {
    Value::DictMap m; // R97 #2: 改用 DictMap（含 DictKeyEqual 透明比较器）
    m[Value::DictKey{3.14}] = Value(std::string("pi"));
    Value v(std::move(m));
    EXPECT_EQ(v.dictVal().size(), 1u);
    EXPECT_EQ(v.dictVal().at(Value::DictKey{3.14}).stringVal(), "pi");
}

// 验证 dictKeyFromValue 对非法键类型返回 nullopt
TEST(ValueDictTest, L4_DictKeyFromValueRejectsInvalidTypes) {
    EXPECT_FALSE(Value::dictKeyFromValue(Value::nullValue()).has_value());
    EXPECT_FALSE(Value::dictKeyFromValue(Value(std::vector<Value>{})).has_value());
    EXPECT_FALSE(Value::dictKeyFromValue(Value(std::unordered_map<std::string, Value>{})).has_value());
    EXPECT_FALSE(Value::dictKeyFromValue(Value::makeInstance("C")).has_value());
    auto env = std::make_shared<Environment>();
    EXPECT_FALSE(Value::dictKeyFromValue(Value::makeClosure("f", env, {}, nullptr)).has_value());
}

// 验证 dictKeyToValue 往返一致性
TEST(ValueDictTest, L4_DictKeyRoundTrip) {
    EXPECT_EQ(Value::dictKeyToValue(Value::DictKey{int64_t(42)}).intVal(), 42);
    EXPECT_EQ(Value::dictKeyToValue(Value::DictKey{std::string("k")}).stringVal(), "k");
    EXPECT_EQ(Value::dictKeyToValue(Value::DictKey{true}).boolVal(), true);
    EXPECT_EQ(Value::dictKeyToValue(Value::DictKey{2.5}).floatVal(), 2.5);
}

// 验证 dictKeyToString 显示转换
TEST(ValueDictTest, L4_DictKeyToString) {
    EXPECT_EQ(Value::dictKeyToString(Value::DictKey{int64_t(42)}), "42");
    EXPECT_EQ(Value::dictKeyToString(Value::DictKey{std::string("k")}), "k");
    EXPECT_EQ(Value::dictKeyToString(Value::DictKey{true}), "true");
    EXPECT_EQ(Value::dictKeyToString(Value::DictKey{false}), "false");
}

// ============================================================
// 10. 工具方法（toDouble / typeName）
// ============================================================

// 验证 int 转 double 保持数值（含负值）
TEST(ValueUtilityTest, IntToDouble) {
    EXPECT_DOUBLE_EQ(Value(int64_t(42)).toDouble(), 42.0);
    EXPECT_DOUBLE_EQ(Value(int64_t(-7)).toDouble(), -7.0);
}

// 验证 float 转 double 保持精度
TEST(ValueUtilityTest, FloatToDouble) {
    EXPECT_DOUBLE_EQ(Value(3.14).toDouble(), 3.14);
}

// 验证非数字类型（string/bool/null）toDouble 返回 0.0
TEST(ValueUtilityTest, NonNumberToDoubleReturnsZero) {
    EXPECT_DOUBLE_EQ(Value(std::string("hello")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(Value(true).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(Value().toDouble(), 0.0);
}

// 验证 typeName 对全部类型返回正确名称；空类名实例返回 "instance"
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

// 验证实例字段的设置与读取
TEST(ValueInstanceTest, SetAndGetField) {
    Value v = Value::makeInstance("Point");
    v.fields()["x"] = Value(int64_t(10));
    v.fields()["y"] = Value(int64_t(20));
    EXPECT_EQ(v.fields().size(), 2u);
    EXPECT_EQ(v.fields().at("x").intVal(), 10);
    EXPECT_EQ(v.fields().at("y").intVal(), 20);
}

// 验证重复设置同一字段覆盖旧值且 size 不变
TEST(ValueInstanceTest, ModifyField) {
    Value v = Value::makeInstance("Counter");
    v.fields()["count"] = Value(int64_t(0));
    v.fields()["count"] = Value(int64_t(1));
    v.fields()["count"] = Value(int64_t(2));
    EXPECT_EQ(v.fields().at("count").intVal(), 2);
    EXPECT_EQ(v.fields().size(), 1u);
}

// 验证实例字段可为复杂值（数组）
TEST(ValueInstanceTest, FieldWithComplexValue) {
    Value v = Value::makeInstance("Container");
    v.fields()["items"] = Value(std::vector<Value>{Value(int64_t(1)), Value(int64_t(2))});
    EXPECT_EQ(v.fields().at("items").arrayVal().size(), 2u);
}

// 验证 className() 返回构造时指定的类名
TEST(ValueInstanceTest, ClassNameAccess) {
    Value v = Value::makeInstance("MyClass");
    EXPECT_EQ(v.className(), "MyClass");
}

// ============================================================
// 12. 闭包操作
// ============================================================

// 验证闭包 capturedVars 的写入与访问
TEST(ValueClosureTest, CapturedVars) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {"x"}, nullptr);
    v.capturedVars()["y"] = Value(int64_t(42));
    EXPECT_EQ(v.capturedVars().size(), 1u);
    EXPECT_EQ(v.capturedVars().at("y").intVal(), 42);
}

// 验证闭包 closureParams 顺序与内容正确
TEST(ValueClosureTest, ParamsAccess) {
    auto env = std::make_shared<Environment>();
    std::vector<std::string> params = {"a", "b", "c"};
    Value v = Value::makeClosure("f", env, params, nullptr);
    EXPECT_EQ(v.closureParams().size(), 3u);
    EXPECT_EQ(v.closureParams()[0], "a");
    EXPECT_EQ(v.closureParams()[1], "b");
    EXPECT_EQ(v.closureParams()[2], "c");
}

// 验证 closureEnv() 返回与构造环境指向同一对象（弱引用锁定）
TEST(ValueClosureTest, ClosureEnvShared) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    // closureEnv() 返回 weak_ptr.lock() 的 shared_ptr，应指向同一对象
    EXPECT_EQ(v.closureEnv().get(), env.get());
}

// 验证闭包体默认置空（closureBody() 为 nullptr）
TEST(ValueClosureTest, ClosureBodyIsNullByDefault) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_EQ(v.closureBody(), nullptr);
}

// 验证闭包 capturedVars 默认空
TEST(ValueClosureTest, CapturedVarsEmptyByDefault) {
    auto env = std::make_shared<Environment>();
    Value v = Value::makeClosure("f", env, {}, nullptr);
    EXPECT_EQ(v.capturedVars().size(), 0u);
}
