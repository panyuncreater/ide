// ============================================================
// InterpreterClasses.cpp — 类与继承相关方法实现（类声明、成员访问、super、方法查找）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "interpreter/Interpreter.h"

// 依赖说明：ClassInfo/ClassDecl/MemberAccess/FunDecl/VarDecl/NodeType/Environment 等
// 均通过 Interpreter.h 传递包含；本文件不直接使用 Lexer/Parser/BuiltinMethods/NumericUtils。


void Interpreter::visitClassDecl(ClassDecl& node) {
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
    // C6 fix: 任何类定义/重定义都递增 gen，使所有 ClassInfo::methodCache_ 条目失效，
    // 防止子类缓存指向已被替换的父类旧方法指针。
    classRegistryGen_++;
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
            // A3 fix: 使用 static_pointer_cast 共享 AST 节点所有权
            auto funDecl = std::static_pointer_cast<FunDecl>(member);
            classRegistry_[node.name].methods[funDecl->name] = funDecl;  // #2 fix
            continue;
        }
    }

    lastValue_ = std::move(classVal); return;
}

void Interpreter::visitMemberAccess(MemberAccess& node) {
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
            lastValue_ = it->second; return;
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
                lastValue_ = std::move(methodVal); return;
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
            lastValue_ = it->second; return;
        }
        lastValue_ = Value::nullValue(); return;
    }

    runtimeError("该类型不支持成员访问", node.line, node.column);
}

void Interpreter::visitMemberAssign(MemberAssign& node) {
    checkBreak(&node);
    // 左到右求值：object → value（由 writeBack 内部按序求值）
    lastValue_ = writeBack(node.object.get(), false, nullptr, node.fieldName, node.value.get(), node.line, node.column); return;
}

void Interpreter::visitSuperExpr(SuperExpr& node) {
    checkBreak(&node);
    // super 解析为当前 this 实例；调用者通过 NODE_SUPER_EXPR 判断使用父类方法查找
    const Value* thisVal = currentEnv_->get("this");
    if (!thisVal) {
        runtimeError("super 只能在类方法中使用", node.line, node.column);
    }
    lastValue_ = *thisVal; return;
}

// ---- 类辅助方法 ----

// P1-3 fix: 继承链查找模板，消除 findMethod/findFieldDefault 的重复遍历结构。
// 沿 superClassName 上移，MAX_INHERITANCE_DEPTH 防止循环继承。
template<typename Lookup, typename NotFound>
auto lookupInheritanceChain(const ClassInfo& cls,
                            const std::unordered_map<std::string, ClassInfo>& registry,
                            Lookup lookup, NotFound notFound)
    -> decltype(lookup(cls)) {
    const ClassInfo* cur = &cls;
    int depth = 0;
    while (cur) {
        if (++depth > RuntimeLimits::MAX_INHERITANCE_DEPTH) return notFound;
        if (auto r = lookup(*cur)) return r;
        if (!cur->superClassName.empty()) {
            auto superIt = registry.find(cur->superClassName);
            cur = (superIt != registry.end()) ? &superIt->second : nullptr;
        } else {
            cur = nullptr;
        }
    }
    return notFound;
}

FunDecl* Interpreter::findMethod(const ClassInfo& cls, const std::string& methodName) {
    // C6 fix: 方法分派缓存。先查缓存（O(1)），gen 不匹配或未命中才走继承链（O(depth)）。
    auto cacheIt = cls.methodCache_.find(methodName);
    if (cacheIt != cls.methodCache_.end() && cacheIt->second.second == classRegistryGen_) {
        return cacheIt->second.first.get();  // A3 fix: cache 持有 shared_ptr，返回裸指针
    }
    // P2-7 fix: const 正确性 — 不修改 cls，使用 const 指针遍历继承链
    // P1-3 fix: 委托给 lookupInheritanceChain 模板
    // A3 fix: methods 表持有 shared_ptr<FunDecl>，lambda 返回 shared_ptr
    std::shared_ptr<FunDecl> result = lookupInheritanceChain(cls, classRegistry_,
        [&methodName](const ClassInfo& c) -> std::shared_ptr<FunDecl> {
            auto it = c.methods.find(methodName);
            return (it != c.methods.end()) ? it->second : nullptr;
        },
        nullptr);
    // 写入缓存（记录当前 gen，类重定义时 gen 递增使此条目失效）
    cls.methodCache_[methodName] = { result, classRegistryGen_ };
    return result.get();  // 返回裸指针，调用方在 ClassInfo 存活期间安全使用
}

Value Interpreter::findFieldDefault(const ClassInfo& cls, const std::string& fieldName) {
    // P1-3 fix: 委托给 lookupInheritanceChain 模板
    // lookup 返回 const Value*（指向 map 中条目，nullptr 表示未找到）
    const Value* found = lookupInheritanceChain(cls, classRegistry_,
        [&fieldName](const ClassInfo& c) -> const Value* {
            auto it = c.fields.find(fieldName);
            return (it != c.fields.end()) ? &it->second : nullptr;
        },
        nullptr);
    return found ? *found : Value::nullValue();
}
