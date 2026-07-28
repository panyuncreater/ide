// ============================================================
// InterpreterClasses.cpp — 类与继承相关方法实现（类声明、成员访问、super、方法查找）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "common/ErrorMessages.h"  // R161 fix: 三后端共享错误消息常量
#include "debug/DebugController.h" // L19 fix: visitMemberAssign 调用 checkWatchpointHit 需完整定义
#include "interpreter/Interpreter.h"
#include "interpreter/StringIntern.h" // PERF-05 fix: 方法标记字符串驻留

// 依赖说明：ClassInfo/ClassDecl/MemberAccess/FunDecl/VarDecl/NodeType/Environment 等
// 均通过 Interpreter.h 传递包含；本文件不直接使用 Lexer/Parser/BuiltinMethods/NumericUtils。

void Interpreter::visitClassDecl(ClassDecl& node) {
    checkBreak(&node);

    ClassInfo cls;
    cls.name = node.name;
    cls.superClassName = node.superClassName;
    cls.closureEnv = currentEnv_; // O5: 捕获类定义时的环境（闭包）
    // R163 泛型扩展：记录类的类型参数，供 invokeMethod 合并类泛型 + 方法泛型参数
    cls.typeParams = node.typeParams;
    // 不再存储 superClass 裸指针，运行时通过 superClassName 查找

    // 如果有父类，验证父类是否已定义
    if (!node.superClassName.empty()) {
        // R97 #10 fix: 用 lookupClassSafely 替代 find + end() + runtimeError 三步模式
        // （引用必须保留以触发查找副作用，否则编译器可能优化掉导致父类未定义检查被跳过）
        (void)lookupClassSafely(node.superClassName, "未定义的父类: " + node.superClassName, node.line, node.column);
    }

    // 注册类名到环境
    Value classVal(std::string("class:") + node.name);
    currentEnv_->define(node.name, classVal);

    // 先注册类信息（以便方法中可以递归引用自身）
    classRegistry_[node.name] = cls;
    // C6 fix: 任何类定义/重定义都递增 gen，使所有 ClassInfo::methodCache_ 条目失效，
    // 防止子类缓存指向已被替换的父类旧方法指针。
    classRegistryGen_++;

    // 处理类成员
    for (auto& member : node.members) {
        // VarDecl: 字段默认值
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            VarDecl* varDecl = static_cast<VarDecl*>(member.get());
            Value defaultVal = Value::nullValue();
            if (varDecl->initializer) {
                defaultVal = evaluate(varDecl->initializer.get());
            }
            classRegistry_[node.name].fields[varDecl->name] = defaultVal; // #2 fix: 直接索引，避免registeredCls失效
            continue;
        }

        // FunDecl: 方法
        if (member->nodeType == NodeType::NODE_FUN_DECL) {
            // A3 fix: 使用 static_pointer_cast 共享 AST 节点所有权
            auto funDecl = std::static_pointer_cast<FunDecl>(member);
            classRegistry_[node.name].methods[funDecl->name] = funDecl; // #2 fix
            continue;
        }
    }

    lastValue_ = std::move(classVal);
    return;
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
            lastValue_ = it->second;
            return;
        }

        // 检查是否访问的是方法（返回一个标记值）
        auto classIt = classRegistry_.find(objC.className());
        if (classIt != classRegistry_.end()) {
            ClassInfo* searchClass = &classIt->second;
            if (isSuperAccess) {
                // AUDIT-R5 R4 fix: 以词法类（classContextStack_ 顶部 = 当前执行方法的
                // 定义类）而非运行时实例类解析 super 搜索起点，对齐 visitMethodCall
                // 的 isSuperCall 路径（L3344-3351）与 VM 的 OP_SUPER_CALL（编译期编码
                // currentClassName_）。原实现用 objC.className()：A←B←C 且实例为 C 时，
                // B 方法内的 super 成员访问从 C 的父类 B 开始搜索，可能重新命中 B
                // 自身方法造成错误分派甚至无限递归。
                const std::string& lexicalClassName =
                    !classContextStack_.empty() ? classContextStack_.back() : objC.className();
                auto lexIt = classRegistry_.find(lexicalClassName);
                ClassInfo* lexClass = (lexIt != classRegistry_.end()) ? &lexIt->second : &classIt->second;
                if (lexClass->superClassName.empty()) {
                    runtimeError("类 " + lexClass->name + " 没有父类，不能使用 super", node.line, node.column);
                }
                auto superIt = classRegistry_.find(lexClass->superClassName);
                if (superIt != classRegistry_.end()) {
                    searchClass = &superIt->second;
                }
            }
            FunDecl* method = findMethod(*searchClass, node.fieldName);
            if (method) {
                // 方法作为字段访问，返回特殊标记
                // PERF-05 fix: 驻留 "method:Class.field" 字符串，避免每次成员访问重复构造
                Value methodVal(StringIntern::internConcat(StringIntern::internConcat("method:", objC.className()),
                                                           std::string(".") + node.fieldName));
                lastValue_ = std::move(methodVal);
                return;
            }
        }

        // 字段和方法都不存在，报告错误
        runtimeError("类 " + objC.className() + " 没有字段或方法 '" + node.fieldName + "'", node.line, node.column);
    }

    // 字典的成员访问（同索引访问，键为 string 类型字段名）
    if (objC.isDict()) {
        const auto& dict = objC.dictVal();
        auto it = dict.find(Value::DictKey{node.fieldName});
        if (it != dict.end()) {
            lastValue_ = it->second;
            return;
        }
        lastValue_ = Value::nullValue();
        return;
    }

    // P2 fix (null-access): null 值成员访问给出明确的 null-access 诊断码，
    // 供 ErrorHintEngine 按 code 精确匹配教学提示（而非依赖子串匹配）。
    if (objC.isNull()) {
        runtimeError("不能在 null 值上访问属性或调用方法", node.line, node.column, DiagCodes::kNullAccess);
    }
    runtimeError(ErrorMessages::kTypeNotMemberAccessible, node.line, node.column, DiagCodes::kTypeMismatch);
}

void Interpreter::visitMemberAssign(MemberAssign& node) {
    checkBreak(&node);
    // L19 Watchpoint（pre-execution 语义，与 VM OP_MEMBER_SET 对齐）：
    // 字段写入检查。仅当 node.object 是简单 VarRef 时附带接收者变量名，
    // 复杂链式访问（如 obj.a.b = 1）仅按 fieldName 匹配（varName 空通配）。
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    auto dbg = debugger_.load(std::memory_order_acquire);
    if (dbg && dbg->hasWatchpoints()) {
        std::string rootVarName;
        if (node.object->nodeType == NodeType::NODE_VAR_REF) {
            rootVarName = static_cast<VarRef*>(node.object.get())->name;
        }
        dbg->checkWatchpointHit(rootVarName, true, node.fieldName, node.line);
    }
    // 左到右求值：object → value（由 writeBack 内部按序求值）
    lastValue_ = writeBack(node.object.get(), false, nullptr, node.fieldName, node.value.get(), node.line, node.column);
    return;
}

void Interpreter::visitSuperExpr(SuperExpr& node) {
    checkBreak(&node);
    // super 解析为当前 this 实例；调用者通过 NODE_SUPER_EXPR 判断使用父类方法查找
    const Value* thisVal = currentEnv_->get("this");
    if (!thisVal) {
        runtimeError("super 只能在类方法中使用", node.line, node.column);
    }
    lastValue_ = *thisVal;
    return;
}

// ---- 类辅助方法 ----

// P1-3 fix: 继承链查找模板，消除 findMethod/findFieldDefault 的重复遍历结构。
// 沿 superClassName 上移，MAX_INHERITANCE_DEPTH 防止循环继承。
template <typename Lookup, typename NotFound>
auto lookupInheritanceChain(const ClassInfo& cls, const std::unordered_map<std::string, ClassInfo>& registry,
                            Lookup lookup, NotFound notFound) -> decltype(lookup(cls)) {
    const ClassInfo* cur = &cls;
    int depth = 0;
    while (cur) {
        if (++depth > RuntimeLimits::MAX_INHERITANCE_DEPTH)
            return notFound;
        if (auto r = lookup(*cur))
            return r;
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
        return cacheIt->second.first.get(); // A3 fix: cache 持有 shared_ptr，返回裸指针
    }
    // P2-7 fix: const 正确性 — 不修改 cls，使用 const 指针遍历继承链
    // P1-3 fix: 委托给 lookupInheritanceChain 模板
    // A3 fix: methods 表持有 shared_ptr<FunDecl>，lambda 返回 shared_ptr
    std::shared_ptr<FunDecl> result = lookupInheritanceChain(
        cls, classRegistry_,
        [&methodName](const ClassInfo& c) -> std::shared_ptr<FunDecl> {
            auto it = c.methods.find(methodName);
            return (it != c.methods.end()) ? it->second : nullptr;
        },
        nullptr);
    // 写入缓存（记录当前 gen，类重定义时 gen 递增使此条目失效）
    cls.methodCache_[methodName] = {result, classRegistryGen_};
    return result.get(); // 返回裸指针，调用方在 ClassInfo 存活期间安全使用
}

std::string Interpreter::findMethodDefiningClassName(const ClassInfo& startCls, const std::string& methodName) {
    // AUDIT-P1-CORRECT fix: 查找方法实际定义所在的类名，用于 classContextStack_ 压入。
    // 原实现压入 searchClass->name（搜索起始类），当中间类未定义方法时，
    // findMethod 沿继承链向上找到祖先类的方法，但栈中压入的是中间类名，
    // 导致后续 super 调用从错误的类开始搜索，可能找到同一个方法形成无限递归。
    // 本方法不走缓存（缓存只存方法指针不存定义类），直接遍历继承链。
    // 与 findMethod 的遍历逻辑完全一致，仅额外返回 ClassInfo::name。
    const ClassInfo* cur = &startCls;
    int depth = 0;
    while (cur) {
        if (++depth > RuntimeLimits::MAX_INHERITANCE_DEPTH)
            break;
        if (cur->methods.find(methodName) != cur->methods.end()) {
            return cur->name;
        }
        if (!cur->superClassName.empty()) {
            auto superIt = classRegistry_.find(cur->superClassName);
            cur = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
        } else {
            cur = nullptr;
        }
    }
    return startCls.name; // fallback（方法未找到时不应到达）
}

Value Interpreter::findFieldDefault(const ClassInfo& cls, const std::string& fieldName) {
    // P1-3 fix: 委托给 lookupInheritanceChain 模板
    // lookup 返回 const Value*（指向 map 中条目，nullptr 表示未找到）
    const Value* found = lookupInheritanceChain(
        cls, classRegistry_,
        [&fieldName](const ClassInfo& c) -> const Value* {
            auto it = c.fields.find(fieldName);
            return (it != c.fields.end()) ? &it->second : nullptr;
        },
        nullptr);
    return found ? *found : Value::nullValue();
}
