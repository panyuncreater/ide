// ============================================================
// InterpreterClasses.cpp — 类与继承相关方法实现（类声明、成员访问、super、方法查找）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "interpreter/Interpreter.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/NumericUtils.h"
#include "debug/DebugController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "Logger.h"
#include <cctype>
#include <cstdint>
#include <climits>
#include <cmath>
#include <sstream>
#include <unordered_set>

Value Interpreter::visitClassDecl(ClassDecl& node) {
    checkBreak(&node);

    ClassInfo cls;
    cls.name = node.name;
    cls.superClassName = node.superClassName;
    cls.closureEnv = currentEnv_;  // O5: 捕获类定义时的环境（闭包）
    // 不再存储 superClass 裸指针，运行时通过 superClassName 查找

    // 如果有父类，验证父类是否已定义
    if (!node.superClassName.empty()) {
        auto it = classRegistry_.find(node.superClassName);
        if (it == classRegistry_.end()) {
            runtimeError("未定义的父类: " + node.superClassName, node.line, node.column);
        }
    }

    // 注册类名到环境
    Value classVal(std::string("class:") + node.name);
    currentEnv_->define(node.name, classVal);

    // 先注册类信息（以便方法中可以递归引用自身）
    classRegistry_[node.name] = cls;
    ClassInfo& registeredCls = classRegistry_[node.name];

    // 处理类成员
    for (auto& member : node.members) {
        // VarDecl: 字段默认值
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            VarDecl* varDecl = static_cast<VarDecl*>(member.get());
            Value defaultVal = Value::nullValue();
            if (varDecl->initializer) {
                defaultVal = evaluate(varDecl->initializer.get());
            }
            classRegistry_[node.name].fields[varDecl->name] = defaultVal;  // #2 fix: 直接索引，避免registeredCls失效
            continue;
        }

        // FunDecl: 方法
        if (member->nodeType == NodeType::NODE_FUN_DECL) {
            FunDecl* funDecl = static_cast<FunDecl*>(member.get());
            classRegistry_[node.name].methods[funDecl->name] = funDecl;  // #2 fix
            continue;
        }
    }

    return classVal;
}

Value Interpreter::visitMemberAccess(MemberAccess& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());

    // P0-3 fix: 使用 const 引用避免在只读访问时触发 COW 深拷贝
    const Value& objC = obj;

    // 类实例的成员访问
    if (objC.isInstance()) {
        // O1: super.field — 字段查找不变（实例已含继承字段），方法查找从父类开始
        bool isSuperAccess = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);

        const auto& flds = objC.fields();
        auto it = flds.find(node.fieldName);
        if (it != flds.end()) {
            return it->second;
        }

        // 检查是否访问的是方法（返回一个标记值）
        auto classIt = classRegistry_.find(objC.className());
        if (classIt != classRegistry_.end()) {
            ClassInfo* searchClass = &classIt->second;
            if (isSuperAccess) {
                if (classIt->second.superClassName.empty()) {
                    runtimeError("类 " + classIt->second.name + " 没有父类，不能使用 super", node.line, node.column);
                }
                auto superIt = classRegistry_.find(classIt->second.superClassName);
                if (superIt != classRegistry_.end()) {
                    searchClass = &superIt->second;
                }
            }
            FunDecl* method = findMethod(*searchClass, node.fieldName);
            if (method) {
                // 方法作为字段访问，返回特殊标记
                Value methodVal(std::string("method:") + objC.className() + "." + node.fieldName);
                return methodVal;
            }
        }

        // 字段和方法都不存在，报告错误
        runtimeError("类 " + objC.className() + " 没有字段或方法 '" + node.fieldName + "'",
            node.line, node.column);
    }

    // 字典的成员访问（同索引访问）
    if (objC.isDict()) {
        const auto& dict = objC.dictVal();
        auto it = dict.find(node.fieldName);
        if (it != dict.end()) {
            return it->second;
        }
        return Value::nullValue();
    }

    runtimeError("该类型不支持成员访问", node.line, node.column);
}

Value Interpreter::visitMemberAssign(MemberAssign& node) {
    checkBreak(&node);
    // 左到右求值：object → value（由 writeBack 内部按序求值）
    return writeBack(node.object.get(), false, nullptr, node.fieldName, node.value.get(), node.line, node.column);
}

Value Interpreter::visitSuperExpr(SuperExpr& node) {
    checkBreak(&node);
    // super 解析为当前 this 实例；调用者通过 NODE_SUPER_EXPR 判断使用父类方法查找
    const Value* thisVal = currentEnv_->get("this");
    if (!thisVal) {
        runtimeError("super 只能在类方法中使用", node.line, node.column);
    }
    return *thisVal;
}

// ---- 类辅助方法 ----

FunDecl* Interpreter::findMethod(ClassInfo& cls, const std::string& methodName) {
    // 使用深度计数器代替 unordered_set，避免每次调用都堆分配
    ClassInfo* cur = &cls;
    int depth = 0;
    while (cur) {
        if (++depth > MAX_INHERITANCE_DEPTH) return nullptr;  // 循环继承或过深继承链
        auto it = cur->methods.find(methodName);
        if (it != cur->methods.end()) return it->second;
        if (!cur->superClassName.empty()) {
            auto superIt = classRegistry_.find(cur->superClassName);
            cur = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
        }
        else {
            cur = nullptr;
        }
    }
    return nullptr;
}

Value Interpreter::findFieldDefault(ClassInfo& cls, const std::string& fieldName) {
    ClassInfo* cur = &cls;
    int depth = 0;
    while (cur) {
        if (++depth > MAX_INHERITANCE_DEPTH) return Value::nullValue();  // 循环继承
        auto it = cur->fields.find(fieldName);
        if (it != cur->fields.end()) return it->second;
        if (!cur->superClassName.empty()) {
            auto superIt = classRegistry_.find(cur->superClassName);
            cur = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
        }
        else {
            cur = nullptr;
        }
    }
    return Value::nullValue();
}
