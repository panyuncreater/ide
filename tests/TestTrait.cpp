// ============================================================
// TestTrait.cpp — 七特性 MVP 阶段 3：Trait/Mixin（parse 期方法合入）
// ------------------------------------------------------------
// 语法：
//   trait T { fun m() {...} ... }          —— trait 声明（MVP 仅方法成员）
//   class C with T1, T2 { ... }            —— 混入
//   class C extends B with T { ... }       —— 与继承并存
// 语义：
//   - Parser 在类体解析完成后将 trait 方法（shared_ptr 共享 FunDecl）append
//     到 ClassDecl.members 尾部 → 三后端按普通类方法注册/编译，语义天然一致
//   - 冲突规则：类自身方法优先；两个 trait 同名方法且类未覆盖 → ParseError
//   - trait 须先声明后使用；trait 体内仅允许 fun 方法
//   - Formatter 打印 with 子句 + 仅类自身成员（往返等价）
// ============================================================

#include "common/ThreeBackends.h"
#include "formatter/Formatter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

// ============================================================
// 1. 单 trait 合入
// ============================================================

TEST(TraitMixin, SingleTraitMethodMerged) {
    std::string src = R"(
trait Greeter {
    fun greet() { return "hello"; }
}
class Person with Greeter {
    var name = "p";
}
var p = Person();
print(p.greet());
)";
    EXPECT_ALL_BACKENDS(src, "hello");
}

// ============================================================
// 2. 多 trait 合入
// ============================================================

TEST(TraitMixin, MultipleTraitsMerged) {
    std::string src = R"(
trait Walker {
    fun walk() { return "walking"; }
}
trait Swimmer {
    fun swim() { return "swimming"; }
}
class Duck with Walker, Swimmer {
}
var d = Duck();
print(d.walk());
print(d.swim());
)";
    EXPECT_ALL_BACKENDS(src, "walkingswimming");
}

// ============================================================
// 3. 类自身方法优先于 trait 方法
// ============================================================

TEST(TraitMixin, OwnMethodOverridesTrait) {
    std::string src = R"(
trait Speaker {
    fun speak() { return "trait-speak"; }
}
class Robot with Speaker {
    fun speak() { return "beep"; }
}
var r = Robot();
print(r.speak());
)";
    EXPECT_ALL_BACKENDS(src, "beep");
}

// ============================================================
// 4. trait 方法访问 this 字段
// ============================================================

TEST(TraitMixin, TraitMethodAccessesThisFields) {
    std::string src = R"(
trait Describable {
    fun describe() { return "I am " + this.name; }
}
class Cat with Describable {
    var name = "kitty";
}
var c = Cat();
print(c.describe());
)";
    EXPECT_ALL_BACKENDS(src, "I am kitty");
}

// ============================================================
// 5. 与继承并存（extends + with）
// ============================================================

TEST(TraitMixin, CoexistsWithInheritance) {
    std::string src = R"(
trait Logger {
    fun log() { return "log:" + this.tag(); }
}
class Base {
    fun tag() { return "base"; }
}
class Derived extends Base with Logger {
    fun tag() { return "derived"; }
}
var d = Derived();
print(d.log());
print(d.tag());
)";
    EXPECT_ALL_BACKENDS(src, "log:derivedderived");
}

// ============================================================
// 6. 同一 trait 混入多个类（共享 FunDecl 节点无状态冲突）
// ============================================================

TEST(TraitMixin, SameTraitMixedIntoMultipleClasses) {
    std::string src = R"(
trait Counter {
    fun bump() {
        this.count = this.count + 1;
        return this.count;
    }
}
class A with Counter {
    var count = 0;
}
class B with Counter {
    var count = 100;
}
var a = A();
var b = B();
print(a.bump());
print(a.bump());
print(b.bump());
)";
    EXPECT_ALL_BACKENDS(src, "12101");
}

// ============================================================
// 7. 错误路径（ParseError）
// ============================================================

namespace {
bool traitParseHasErrors(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    return p.hasErrors() || !ast;
}
} // namespace

TEST(TraitMixin, DiamondConflictIsParseError) {
    // 两个 trait 提供同名方法且类未覆盖 → diamond 冲突
    EXPECT_TRUE(traitParseHasErrors("trait T1 { fun m() { return 1; } }"
                                    "trait T2 { fun m() { return 2; } }"
                                    "class C with T1, T2 {}"));
}

TEST(TraitMixin, DiamondResolvedByOwnOverride) {
    // 类自身覆盖冲突方法 → 合法
    std::string src = R"(
trait T1 {
    fun m() { return 1; }
}
trait T2 {
    fun m() { return 2; }
}
class C with T1, T2 {
    fun m() { return 3; }
}
print(C().m());
)";
    EXPECT_ALL_BACKENDS(src, "3");
}

TEST(TraitMixin, UndefinedTraitIsParseError) {
    EXPECT_TRUE(traitParseHasErrors("class C with NoSuchTrait {}"));
}

TEST(TraitMixin, DuplicateTraitDefinitionIsParseError) {
    EXPECT_TRUE(traitParseHasErrors("trait T { fun m() { return 1; } } trait T { fun n() { return 2; } }"));
}

TEST(TraitMixin, DuplicateMixinIsParseError) {
    EXPECT_TRUE(traitParseHasErrors("trait T { fun m() { return 1; } } class C with T, T {}"));
}

TEST(TraitMixin, FieldInTraitIsParseError) {
    // MVP：trait 体内仅支持方法
    EXPECT_TRUE(traitParseHasErrors("trait T { var x = 1; }"));
}

// ============================================================
// 8. Formatter 往返等价
// ============================================================

namespace {
std::string formatTraitSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";
    Formatter formatter;
    formatter.setComments(lexer.comments());
    return formatter.format(*ast);
}
} // namespace

TEST(TraitMixin, FormatterPreservesTraitAndWithClause) {
    std::string src = "trait Greeter {\n    fun greet() {\n        return \"hi\";\n    }\n}\n"
                      "class P with Greeter {\n    fun own() {\n        return 1;\n    }\n}";
    std::string result = formatTraitSource(src);
    EXPECT_NE(result.find("trait Greeter"), std::string::npos) << "实际: " << result;
    EXPECT_NE(result.find("with Greeter"), std::string::npos) << "实际: " << result;
    // 类体内不应打印合入的 trait 方法（仅自身成员）
    EXPECT_EQ(result.find("greet"), result.rfind("greet")) << "greet 应只出现一次（trait 体内）: " << result;
    // 往返稳定
    EXPECT_EQ(formatTraitSource(result), result);
}

TEST(TraitMixin, FormattedSourceStillExecutes) {
    std::string src = "trait T { fun m() { return 42; } }\nclass C with T {}\nprint(C().m());";
    std::string formatted = formatTraitSource(src);
    ASSERT_FALSE(formatted.empty());
    EXPECT_ALL_BACKENDS(formatted, "42");
}
