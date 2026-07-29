/**
 * @file common/TypeChecker.h
 * @brief 静态类型检查接口（ARCH-10 预留）+ 共享运行时类型匹配。
 *
 * 为 MiniLang 预留静态类型检查通道。当前 MiniLang 是动态类型语言，
 * 所有类型检查在运行时进行。本接口为未来引入可选的渐进式类型注解
 *（如 `var x: int = 10;`）和编译期类型推断预留扩展点。
 *
 * 2026-06-29 新增的 typeMatchValue 自由函数是三后端（VM/RegisterVM/
 * Interpreter）共享的运行时类型注解强制逻辑，处理：
 *   - 原始类型（int/float/bool/string）
 *   - null 兼容所有注解
 *   - array/dict 容器类型
 *   - 数组元素类型注解（如 "int[]"）
 *   - 实例精确类名匹配（继承链检查由调用方扩展）
 *
 * 设计原则：
 *   1. 不破坏现有动态类型语义 — 类型检查是可选 pass，失败时仅产生警告
 *   2. 不侵入 Compiler — TypeChecker 作为独立 pass 在 AST→字节码之间运行
 *   3. 渐进式采用 — 用户可逐步添加类型注解，未注解部分保持动态类型
 *
 * 与 IR 框架（ARCH-06）的协同：
 *   TypeChecker 在 IR 生成前运行，可为 IR 提供类型信息用于特化优化
 *   （如 int+int 生成整数加法指令而非通用 OP_ADD）
 *
 * @see DiagnosticBag Value
 */
#pragma once

#include "Diagnostic.h"
#include "interpreter/Value.h" // 2026-06-29: typeMatchValue 需要 Value 类型
#include <string>
#include <unordered_map>
#include <vector>

// 前向声明全局命名空间下的 AST 节点（避免在 minilang 命名空间内
// 用 `class Block` 隐式创建 minilang::Block）
class Block;

// ============================================================
// TypeChecker 静态类型检查接口（ARCH-10 预留）
// ============================================================
// 设计目标：为 MiniLang 预留静态类型检查通道。当前 MiniLang 是动态类型语言，
// 所有类型检查在运行时进行。本接口为未来引入可选的渐进式类型注解
// （如 `var x: int = 10;`）和编译期类型推断预留扩展点。
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

// ============================================================
// 2026-06-29: 共享运行时类型匹配函数（VM/RegisterVM/Interpreter 复用）
// ============================================================
// 提取自 Interpreter::typeMatch，供三后端统一类型注解强制逻辑。
// 处理：原始类型(int/float/bool/string)、null（兼容所有注解）、
// array/dict、数组元素类型注解(如 "int[]")、实例精确类名匹配。
//
// 实例继承链检查由调用方扩展（Interpreter 用 classRegistry_，VM 用 classInfo_）。
// BUG-014 审计结论（2026-07-18）：三后端语义一致，非误报——
//   - Interpreter::typeMatch 内联继承链遍历（classRegistry_）
//   - StackVM OP_TYPE_CHECK (VMContainers.cpp:683-704) 在 typeMatchValue 失败后
//     执行相同继承链 fallback 检查（classInfo_）
//   - RegisterVM REG_TYPE_CHECK (RegisterVM.cpp:1337-1357) 同样有继承链 fallback
//   三个后端对相同输入产生一致结果，仅代码组织不同（共享 helper + 调用方 fallback
//   vs 内联实现）。null 兼容性、T? 剥离、fun() 类型、数组/dict 元素递归均已对齐。
inline bool typeMatchValue(const Value& val, const std::string& annotation) {
    if (annotation.empty())
        return true;
    // null 兼容所有类型注解（2026-06-29 用户决策）
    if (val.isNull())
        return true;
    // 可选类型 T?：剥离末尾 '?' 后递归匹配（对齐 Interpreter::typeMatch AUDIT-P2.7）
    if (annotation.size() >= 2 && annotation.back() == '?') {
        return typeMatchValue(val, annotation.substr(0, annotation.size() - 1));
    }
    if (annotation == TypeName::INT)
        return val.isInt();
    if (annotation == TypeName::FLOAT)
        return val.isFloat() || val.isInt();
    if (annotation == TypeName::BOOL)
        return val.isBool();
    if (annotation == TypeName::STRING)
        return val.isString();
    if (annotation == TypeName::ARRAY)
        return val.isArray();
    if (annotation == TypeName::DICT)
        return val.isDict();
    // R98 元组与解构：tuple 类型注解匹配
    if (annotation == TypeName::TUPLE)
        return val.isTuple();
    if (annotation == TypeName::NULL_T)
        return val.isNull();
    // R99 枚举与 ADT：enum variant 类型注解匹配
    // - "enum"：泛型注解，匹配任意 enum variant
    // - "<EnumName>"：精确 enum 名匹配（如 "Color" 匹配 Color.Red/Color.Green 等）
    if (annotation == TypeName::ENUM) {
        return val.isEnumVariant();
    }
    if (val.isEnumVariant()) {
        return val.enumVariantEnumName() == annotation;
    }
    // dict[K:V] 泛型字典类型注解：key 支持 string/int/bool/float（L4 fix），递归检查每个 value
    // （对齐 Interpreter::typeMatch AUDIT-P2.7）
    if (annotation.size() >= 7 && annotation.substr(0, 5) == "dict[" && annotation.back() == ']') {
        if (!val.isDict())
            return false;
        std::string inner = annotation.substr(5, annotation.size() - 6);
        size_t colonPos = inner.find(':');
        if (colonPos == std::string::npos)
            return false;
        std::string keyType = inner.substr(0, colonPos);
        std::string valType = inner.substr(colonPos + 1);
        for (const auto& kv : val.dictVal()) {
            // L4 fix: kv.first 是 DictKey variant，按 keyType 检查键类型
            if (keyType == "string") {
                if (!std::holds_alternative<std::string>(kv.first))
                    return false;
            } else if (keyType == "int") {
                if (!std::holds_alternative<int64_t>(kv.first))
                    return false;
            } else if (keyType == "bool") {
                if (!std::holds_alternative<bool>(kv.first))
                    return false;
            } else if (keyType == "float") {
                if (!std::holds_alternative<double>(kv.first))
                    return false;
            } else {
                return false; // 未知 keyType
            }
            if (!typeMatchValue(kv.second, valType))
                return false;
        }
        return true;
    }
    // 函数类型 fun(...):ret：仅检查是闭包即可（对齐 Interpreter::typeMatch AUDIT-P2.7，参数/返回类型不做验证）
    if (annotation.size() >= 5 && annotation.substr(0, 4) == "fun(") {
        return val.isClosure();
    }
    // 数组元素类型注解，如 "int[]"、多维 "int[][]"（后缀 [] 剥离递归，对齐 Interpreter::typeMatch AUDIT-P2-CORRECT）
    if (annotation.size() >= 2 && annotation.substr(annotation.size() - 2) == "[]") {
        if (!val.isArray())
            return false;
        std::string elemType = annotation.substr(0, annotation.size() - 2);
        for (const auto& elem : val.arrayVal()) {
            if (!typeMatchValue(elem, elemType))
                return false;
        }
        return true;
    }
    // 实例类型注解：精确类名匹配（继承链由调用方扩展）
    if (val.isInstance()) {
        return val.className() == annotation;
    }
    return false;
}

/// R163 泛型扩展：判断类型名是否是类型参数（如 T、E、K、V）。
/// 用于四后端（Interpreter/StackVM/RegisterVM/IR）的类型参数擦除——
/// 类型参数运行时不校验，仅跳过类型检查。
/// 对齐 Interpreter::isTypeParameter 的实现。
inline bool isTypeParameter(const std::string& typeName, const std::vector<std::string>& typeParams) {
    for (const auto& tp : typeParams) {
        if (typeName == tp) {
            return true;
        }
    }
    return false;
}

/// 类型种类（预留，未来扩展）
enum class TypeKind {
    UNKNOWN,  // 未推断（动态类型）
    INT,      // 整数
    FLOAT,    // 浮点数
    BOOL,     // 布尔
    STRING,   // 字符串
    NULL_T,   // null
    ARRAY,    // 数组（元素类型待推断）
    DICT,     // 字典（键/值类型待推断）
    INSTANCE, // 类实例（类名在 className 中）
    CLOSURE,  // 闭包（参数/返回类型待推断）
    ANY       // 任意类型（类型通配，与所有类型兼容）
};

/// 类型信息（预留，未来扩展）
struct TypeInfo {
    TypeKind kind = TypeKind::UNKNOWN;
    std::string className{};          // INSTANCE 类型的类名
    std::vector<TypeInfo> typeArgs{}; // 泛型类型参数（ARRAY 的元素类型、CLOSURE 的参数/返回类型）

    bool isNumeric() const { return kind == TypeKind::INT || kind == TypeKind::FLOAT; }

    /// 类型兼容性检查：判断 other 类型的值是否可赋给 this 类型的变量。
    /// 规则：UNKNOWN/ANY 兼容一切；float 接受 int（数值提升）；
    /// ARRAY/DICT/INSTANCE 按 className/typeArgs 递归检查；其余严格匹配。
    bool isCompatible(const TypeInfo& other) const {
        // UNKNOWN 或 ANY 兼容一切（保守假设 / 通配符）
        if (kind == TypeKind::UNKNOWN || kind == TypeKind::ANY)
            return true;
        if (other.kind == TypeKind::UNKNOWN || other.kind == TypeKind::ANY)
            return true;
        // null 兼容所有类型（MiniLang 动态语言惯例）
        if (other.kind == TypeKind::NULL_T)
            return true;
        // float 接受 int（数值提升）
        if (kind == TypeKind::FLOAT && other.kind == TypeKind::INT)
            return true;
        // 同类型直接兼容
        if (kind == other.kind) {
            // INSTANCE 需要类名匹配
            if (kind == TypeKind::INSTANCE)
                return className.empty() || other.className.empty() || className == other.className;
            return true;
        }
        return false;
    }

    /// 从类型注解字符串构造 TypeInfo
    static TypeInfo fromAnnotation(const std::string& ann) {
        if (ann.empty())
            return TypeInfo{TypeKind::UNKNOWN};
        if (ann == "int")
            return TypeInfo{TypeKind::INT};
        if (ann == "float")
            return TypeInfo{TypeKind::FLOAT};
        if (ann == "bool")
            return TypeInfo{TypeKind::BOOL};
        if (ann == "string")
            return TypeInfo{TypeKind::STRING};
        if (ann == "null")
            return TypeInfo{TypeKind::NULL_T};
        if (ann == "array")
            return TypeInfo{TypeKind::ARRAY};
        if (ann == "dict")
            return TypeInfo{TypeKind::DICT};
        // 以 [] 结尾的数组元素类型
        if (ann.size() >= 2 && ann.substr(ann.size() - 2) == "[]")
            return TypeInfo{TypeKind::ARRAY};
        // fun(...) 函数类型
        if (ann.size() >= 5 && ann.substr(0, 4) == "fun(")
            return TypeInfo{TypeKind::CLOSURE};
        // 其他视为实例类型（类名）
        return TypeInfo{TypeKind::INSTANCE, ann};
    }

    /// 类型字符串表示（用于诊断信息）
    std::string toString() const {
        switch (kind) {
        case TypeKind::INT:
            return "int";
        case TypeKind::FLOAT:
            return "float";
        case TypeKind::BOOL:
            return "bool";
        case TypeKind::STRING:
            return "string";
        case TypeKind::NULL_T:
            return "null";
        case TypeKind::ARRAY:
            return "array";
        case TypeKind::DICT:
            return "dict";
        case TypeKind::CLOSURE:
            return "closure";
        case TypeKind::ANY:
            return "any";
        case TypeKind::INSTANCE:
            return className.empty() ? "instance" : className;
        case TypeKind::UNKNOWN:
        default:
            return "unknown";
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
    virtual DiagnosticBag check(const Block& /*program*/) {
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

    /// 类型检查 pass（2026-07-25: 基础类型推断 + 字面量检查 + 函数返回类型检查）
    DiagnosticBag check(const Block& program) override;

    /// 推断变量类型（基于上次 check() 运行的符号表）
    TypeInfo inferType(const std::string& name) const override;

private:
    std::unordered_map<std::string, TypeInfo> lastWalkerTypeInfos_; // check() 后保留的符号表
};

} // namespace minilang
