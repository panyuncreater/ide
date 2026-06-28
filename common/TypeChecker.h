#pragma once

#include <string>
#include <vector>
#include "Diagnostic.h"

// ============================================================
// TypeChecker 静态类型检查接口（ARCH-10 预留）
// ============================================================
// 设计目标：为 MiniLang 预留静态类型检查通道。当前 MiniLang 是动态类型语言，
// 所有类型检查在运行时进行。本接口为未来引入可选的渐进式类型注解
//（如 `var x: int = 10;`）和编译期类型推断预留扩展点。
//
// 设计原则：
//   1. 不破坏现有动态类型语义 — 类型检查是可选 pass，失败时仅产生警告
//   2. 不侵入 Compiler — TypeChecker 作为独立 pass 在 AST→字节码之间运行
//   3. 渐进式采用 — 用户可逐步添加类型注解，未注解部分保持动态类型
//
// 未来实现路径：
//   - 阶段 1：类型注解收集（VarDecl/ParamDecl/ReturnStmt 的类型注解）
//   - 阶段 2：类型推断（未注解变量从初始化表达式推断类型）
//   - 阶段 3：类型检查（运算符/赋值/调用参数的类型兼容性验证）
//   - 阶段 4：类型错误报告（通过 DiagnosticBag 报告类型不匹配）
//
// 与 IR 框架（ARCH-06）的协同：
//   TypeChecker 在 IR 生成前运行，可为 IR 提供类型信息用于特化优化
//   （如 int+int 生成整数加法指令而非通用 OP_ADD）

namespace minilang {

/// 类型种类（预留，未来扩展）
enum class TypeKind {
    UNKNOWN,    // 未推断（动态类型）
    INT,        // 整数
    FLOAT,      // 浮点数
    BOOL,       // 布尔
    STRING,     // 字符串
    NULL_T,     // null
    ARRAY,      // 数组（元素类型待推断）
    DICT,       // 字典（键/值类型待推断）
    INSTANCE,   // 类实例（类名在 className 中）
    CLOSURE,    // 闭包（参数/返回类型待推断）
    ANY         // 任意类型（类型通配，与所有类型兼容）
};

/// 类型信息（预留，未来扩展）
struct TypeInfo {
    TypeKind kind = TypeKind::UNKNOWN;
    std::string className;  // INSTANCE 类型的类名
    std::vector<TypeInfo> typeArgs;  // 泛型类型参数（ARRAY 的元素类型、CLOSURE 的参数/返回类型）

    bool isNumeric() const { return kind == TypeKind::INT || kind == TypeKind::FLOAT; }

    /// 类型兼容性检查（预留，当前动态类型语义下默认兼容）
    /// 未来实现严格的子类型/转换规则
    bool isCompatible(const TypeInfo& /*other*/) const {
        // ARCH-10 预留：动态类型阶段，所有类型默认兼容
        return true;
    }

    /// 类型字符串表示（用于诊断信息）
    std::string toString() const {
        switch (kind) {
        case TypeKind::INT:     return "int";
        case TypeKind::FLOAT:   return "float";
        case TypeKind::BOOL:    return "bool";
        case TypeKind::STRING:  return "string";
        case TypeKind::NULL_T:  return "null";
        case TypeKind::ARRAY:   return "array";
        case TypeKind::DICT:    return "dict";
        case TypeKind::CLOSURE: return "closure";
        case TypeKind::ANY:     return "any";
        case TypeKind::INSTANCE: return className.empty() ? "instance" : className;
        case TypeKind::UNKNOWN:
        default:                return "unknown";
        }
    }
};

/// 静态类型检查器接口（预留）
class TypeChecker {
public:
    TypeChecker() = default;
    virtual ~TypeChecker() = default;

    /// 对 AST 执行类型检查 pass
    /// @param program AST 根节点（Block）
    /// @return 类型检查产生的诊断（警告/错误）
    /// @note 当前为空实现，未来扩展时填充具体逻辑
    virtual DiagnosticBag check(const class Block& /*program*/) {
        // ARCH-10 预留：当前动态类型语言无需静态检查，返回空诊断包
        return DiagnosticBag{};
    }

    /// 获取变量/表达式的推断类型（未来实现）
    /// @param name 变量名
    /// @return 推断的类型信息，未推断则返回 UNKNOWN
    virtual TypeInfo inferType(const std::string& /*name*/) const {
        // ARCH-10 预留：未实现类型推断，统一返回 UNKNOWN
        return TypeInfo{};
    }

    /// 是否启用严格模式（类型不匹配时报错而非警告）
    void setStrictMode(bool strict) { strictMode_ = strict; }
    bool isStrictMode() const { return strictMode_; }

private:
    bool strictMode_ = false;
    // 未来扩展：符号表、类型环境、推断缓存等
};

// ============================================================
// A4 fix: MiniLangTypeChecker — 具体可实例化的类型检查器 stub
// ------------------------------------------------------------
// 当前为 stub 实现：check() 返回空 DiagnosticBag（动态类型语言下无强制
// 类型规则），但提供了完整的 API 表面（setStrictMode/inferType/check），
// 可被 Compiler 通过 setEnableTypeCheck(true) 启用并接入 pipeline。
// 未来扩展点：
//   - 收集 VarDecl 类型注解（待 Parser 支持类型注解语法）
//   - 表达式类型推断（NumberLiteral→INT, StringLiteral→STRING, ...）
//   - 类型不匹配警告（如 int + string 报警告）
//   - 严格模式下升级为错误
// ============================================================
class MiniLangTypeChecker : public TypeChecker {
public:
    MiniLangTypeChecker() = default;
    ~MiniLangTypeChecker() override = default;

    /// 类型检查 pass（stub：当前返回空 DiagnosticBag，未来扩展实际检查逻辑）
    DiagnosticBag check(const class Block& /*program*/) override {
        // A4 stub: 当前不产生任何诊断，未来填充实际类型检查
        return DiagnosticBag{};
    }

    /// 推断变量类型（stub：当前返回 UNKNOWN，未来实现实际推断）
    TypeInfo inferType(const std::string& /*name*/) const override {
        return TypeInfo{};
    }
};

} // namespace minilang
