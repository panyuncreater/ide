#include "interpreter/Interpreter.h"
#include "debug/DebugController.h"
#include <cmath>
#include <sstream>

// ============================================================
// Interpreter 解释器实现
// ============================================================

Interpreter::Interpreter()
    : globalEnv_(new Environment())
    , currentEnv_(globalEnv_)
    , debugger_(nullptr)
    , recursionDepth_(0)
{
    outputCallback_ = [](const std::string&) {};
}

Interpreter::~Interpreter() {
    delete globalEnv_;
}

Value Interpreter::execute(Block& program) {
    // 重置状态
    delete globalEnv_;
    globalEnv_ = new Environment();
    currentEnv_ = globalEnv_;
    callStack_.clear();
    funRegistry_.clear();
    classRegistry_.clear();
    recursionDepth_ = 0;

    // 顶层块不创建新作用域，直接在全局环境中执行语句
    Value result = Value::nullValue();
    for (auto& stmt : program.statements) {
        result = evaluate(stmt.get());
    }
    return result;
}

Value Interpreter::executeRepl(Block& program) {
    // 不重置环境，保留已有变量/函数/类定义
    // 确保当前环境回到全局
    currentEnv_ = globalEnv_;
    recursionDepth_ = 0;

    Value result = Value::nullValue();
    for (auto& stmt : program.statements) {
        result = evaluate(stmt.get());
    }
    return result;
}

void Interpreter::setOutputCallback(std::function<void(const std::string&)> callback) {
    outputCallback_ = callback;
}

void Interpreter::setDebugger(DebugController* dbg) {
    debugger_ = dbg;
}

void Interpreter::setDebugMode(bool enabled) {
    debugMode_ = enabled;
}

Environment* Interpreter::currentEnvironment() const {
    return currentEnv_;
}

const std::vector<CallFrame>& Interpreter::getCallStack() const {
    return callStack_;
}

// ---- 辅助方法 ----

Value Interpreter::evaluate(ASTNode* node) {
    if (!node) return Value::nullValue();
    return node->accept(*this);
}

void Interpreter::checkBreak(ASTNode* node) {
    if (debugMode_ && debugger_) {
        debugger_->checkBreak(node);
    }
}

void Interpreter::output(const std::string& text) {
    outputCallback_(text);
}

void Interpreter::runtimeError(const std::string& msg, int line, int col) {
    throw RuntimeError(msg, line, col);
}

// ---- 数值二元运算 ----

Value Interpreter::numericBinaryOp(const std::string& op, const Value& left,
                                    const Value& right, int line, int col) {
    // 字符串拼接
    if (op == "+" && left.isString() && right.isString()) {
        return Value(left.stringVal + right.stringVal);
    }
    // 字符串 + 其他类型：转为字符串拼接
    if (op == "+" && (left.isString() || right.isString())) {
        return Value(left.toString() + right.toString());
    }

    // 数值运算
    if (op == "+") {
        if (left.isInt() && right.isInt()) return Value(left.intVal + right.intVal);
        return Value(left.toDouble() + right.toDouble());
    }
    if (op == "-") {
        if (left.isInt() && right.isInt()) return Value(left.intVal - right.intVal);
        return Value(left.toDouble() - right.toDouble());
    }
    if (op == "*") {
        if (left.isInt() && right.isInt()) return Value(left.intVal * right.intVal);
        return Value(left.toDouble() * right.toDouble());
    }
    if (op == "/") {
        double r = right.toDouble();
        if (r == 0.0) runtimeError("除零错误", line, col);
        if (left.isInt() && right.isInt()) return Value(left.intVal / right.intVal);
        return Value(left.toDouble() / r);
    }
    if (op == "%") {
        if (!left.isInt() || !right.isInt()) runtimeError("取模运算仅支持整数", line, col);
        if (right.intVal == 0) runtimeError("除零错误", line, col);
        return Value(left.intVal % right.intVal);
    }

    runtimeError("未知运算符: " + op, line, col);
}

// ---- 类辅助方法 ----

FunDecl* Interpreter::findMethod(ClassInfo& cls, const std::string& methodName) {
    auto it = cls.methods.find(methodName);
    if (it != cls.methods.end()) return it->second;
    // 沿继承链查找
    if (cls.superClass) return findMethod(*cls.superClass, methodName);
    return nullptr;
}

Value Interpreter::findFieldDefault(ClassInfo& cls, const std::string& fieldName) {
    auto it = cls.fields.find(fieldName);
    if (it != cls.fields.end()) return it->second;
    // 沿继承链查找
    if (cls.superClass) return findFieldDefault(*cls.superClass, fieldName);
    return Value::nullValue();
}

// ---- 16 个原有 visit 方法 ----

Value Interpreter::visitBinaryOp(BinaryOp& node) {
    checkBreak(&node);

    // 短路求值：and
    if (node.op == "and") {
        Value left = evaluate(node.left.get());
        if (!left.isTruthy()) return Value(false);
        Value right = evaluate(node.right.get());
        return Value(right.isTruthy());
    }

    // 短路求值：or
    if (node.op == "or") {
        Value left = evaluate(node.left.get());
        if (left.isTruthy()) return Value(true);
        Value right = evaluate(node.right.get());
        return Value(right.isTruthy());
    }

    Value left = evaluate(node.left.get());
    Value right = evaluate(node.right.get());

    // 比较运算
    if (node.op == "==") return Value(left.equals(right));
    if (node.op == "!=") return Value(!left.equals(right));
    if (node.op == "<") {
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() < right.toDouble());
    }
    if (node.op == ">") {
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() > right.toDouble());
    }
    if (node.op == "<=") {
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() <= right.toDouble());
    }
    if (node.op == ">=") {
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() >= right.toDouble());
    }

    // 算术运算
    return numericBinaryOp(node.op, left, right, node.line, node.column);
}

Value Interpreter::visitUnaryOp(UnaryOp& node) {
    checkBreak(&node);

    Value operand = evaluate(node.operand.get());

    if (node.op == "-") {
        if (operand.isInt()) return Value(-operand.intVal);
        if (operand.isFloat()) return Value(-operand.floatVal);
        runtimeError("一元减运算需要数值类型", node.line, node.column);
    }
    if (node.op == "not") {
        return Value(!operand.isTruthy());
    }

    runtimeError("未知一元运算符: " + node.op, node.line, node.column);
}

Value Interpreter::visitNumberLiteral(NumberLiteral& node) {
    checkBreak(&node);
    return node.value;
}

Value Interpreter::visitStringLiteral(StringLiteral& node) {
    checkBreak(&node);
    return Value(node.value);
}

Value Interpreter::visitBoolLiteral(BoolLiteral& node) {
    checkBreak(&node);
    return Value(node.value);
}

Value Interpreter::visitVarDecl(VarDecl& node) {
    checkBreak(&node);

    Value initVal = Value::nullValue();
    if (node.initializer) {
        initVal = evaluate(node.initializer.get());
    } else if (!node.typeAnnotation.empty()) {
        // 无初始化表达式但有类型注解 — 检查是否是类名
        auto classIt = classRegistry_.find(node.typeAnnotation);
        if (classIt != classRegistry_.end()) {
            // 自动创建类实例（无参构造）
            ClassInfo& cls = classIt->second;
            Value instance = Value::makeInstance(cls.name);

            // 复制类默认字段值（含继承链）
            ClassInfo* curCls = &cls;
            while (curCls) {
                for (const auto& kv : curCls->fields) {
                    if (instance.fields.find(kv.first) == instance.fields.end()) {
                        instance.fields[kv.first] = kv.second;
                    }
                }
                curCls = curCls->superClass;
            }

            // 如果有 init 方法（0 参数），执行它
            FunDecl* initMethod = findMethod(cls, "init");
            if (initMethod && initMethod->params.empty()) {
                Environment* initEnv = new Environment(currentEnv_);
                initEnv->define("this", instance);
                // 将实例字段注入 init 环境
                for (const auto& kv : instance.fields) {
                    initEnv->define(kv.first, kv.second);
                }
                Environment* prevEnv = currentEnv_;
                currentEnv_ = initEnv;
                try {
                    evaluate(initMethod->body.get());
                } catch (const ReturnException& e) {}
                instance = initEnv->get("this");
                // 同步字段变更
                for (const auto& kv : instance.fields) {
                    if (initEnv->hasVariable(kv.first)) {
                        instance.fields[kv.first] = initEnv->get(kv.first);
                    }
                }
                currentEnv_ = prevEnv;
                delete initEnv;
            }

            initVal = instance;
        }
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
    }
    currentEnv_->define(node.name, initVal);
    return initVal;
}

Value Interpreter::visitAssignment(Assignment& node) {
    checkBreak(&node);

    Value val = evaluate(node.value.get());
    if (!currentEnv_->set(node.name, val)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return val;
}

Value Interpreter::visitVarRef(VarRef& node) {
    checkBreak(&node);

    if (!currentEnv_->hasVariable(node.name)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return currentEnv_->get(node.name);
}

Value Interpreter::visitIfStmt(IfStmt& node) {
    checkBreak(&node);

    Value cond = evaluate(node.condition.get());
    if (cond.isTruthy()) {
        return evaluate(node.thenBranch.get());
    } else if (node.elseBranch) {
        return evaluate(node.elseBranch.get());
    }
    return Value::nullValue();
}

Value Interpreter::visitWhileStmt(WhileStmt& node) {
    checkBreak(&node);

    Value result = Value::nullValue();
    while (evaluate(node.condition.get()).isTruthy()) {
        try {
            result = evaluate(node.body.get());
        } catch (const ReturnException& e) {
            throw;  // 传播 return
        }
    }
    return result;
}

Value Interpreter::visitForStmt(ForStmt& node) {
    checkBreak(&node);

    // 在新作用域中执行初始化
    Environment* forEnv = new Environment(currentEnv_);
    currentEnv_ = forEnv;

    if (node.initializer) {
        evaluate(node.initializer.get());
    }

    Value result = Value::nullValue();
    try {
        while (true) {
            // 条件检查
            if (node.condition) {
                Value cond = evaluate(node.condition.get());
                if (!cond.isTruthy()) break;
            }

            // 执行循环体
            try {
                result = evaluate(node.body.get());
            } catch (const ReturnException& e) {
                // 恢复环境，传播 return
                currentEnv_ = forEnv->parent;
                delete forEnv;
                throw;
            }

            // 更新
            if (node.update) {
                evaluate(node.update.get());
            }
        }
    } catch (...) {
        currentEnv_ = forEnv->parent;
        delete forEnv;
        throw;
    }

    currentEnv_ = forEnv->parent;
    delete forEnv;
    return result;
}

Value Interpreter::visitFunDecl(FunDecl& node) {
    checkBreak(&node);

    // 将函数定义存储为特殊值
    Value funVal(std::string("fun:") + node.name);
    funVal.type = ValueType::VAL_STRING;
    currentEnv_->define(node.name, funVal);

    // 在全局注册函数体（用于调用时查找）
    funRegistry_[node.name] = &node;

    return funVal;
}

Value Interpreter::visitFunCall(FunCall& node) {
    checkBreak(&node);

    // 检查是否是类构造调用
    auto classIt = classRegistry_.find(node.name);
    if (classIt != classRegistry_.end()) {
        ClassInfo& cls = classIt->second;

        // 检查参数数量（构造函数为 init 方法）
        FunDecl* initMethod = findMethod(cls, "init");

        // 递归深度检查
        recursionDepth_++;
        if (recursionDepth_ >= 256) {
            recursionDepth_--;
            runtimeError("递归深度超过限制 (256)", node.line, node.column);
        }

        // 求值参数
        std::vector<Value> argValues;
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        // 检查参数数量
        if (initMethod && argValues.size() != initMethod->params.size()) {
            recursionDepth_--;
            runtimeError("构造函数 init 期望 " +
                         std::to_string(initMethod->params.size()) + " 个参数，但传入了 " +
                         std::to_string(argValues.size()) + " 个",
                         node.line, node.column);
        }

        // 创建实例
        Value instance = Value::makeInstance(cls.name);

        // 复制类默认字段值（含继承链）
        ClassInfo* curCls = &cls;
        while (curCls) {
            for (const auto& kv : curCls->fields) {
                // 子类字段覆盖父类
                if (instance.fields.find(kv.first) == instance.fields.end()) {
                    instance.fields[kv.first] = kv.second;
                }
            }
            curCls = curCls->superClass;
        }

        // 如果有 init 方法，执行它
        if (initMethod) {
            // 创建新环境（父级为当前环境，支持闭包）
            Environment* initEnv = new Environment(currentEnv_);

            // 绑定 this
            initEnv->define("this", instance);

            // 将实例字段注入 init 环境
            for (const auto& kv : instance.fields) {
                initEnv->define(kv.first, kv.second);
            }

            // 绑定参数
            for (size_t i = 0; i < initMethod->params.size(); ++i) {
                initEnv->define(initMethod->params[i], argValues[i]);
            }

            // 压入调用帧
            callStack_.emplace_back(node.name + ".init", initEnv, node.line, recursionDepth_);

            // 切换环境
            Environment* prevEnv = currentEnv_;
            currentEnv_ = initEnv;

            try {
                evaluate(initMethod->body.get());
            } catch (const ReturnException& e) {
                // init 方法的返回值忽略，但更新实例字段
            }

            // 从 init 环境中读取 this 的更新值
            instance = initEnv->get("this");

            // 将 init 内对字段的直接修改同步回 this 实例
            for (const auto& kv : instance.fields) {
                if (initEnv->hasVariable(kv.first)) {
                    instance.fields[kv.first] = initEnv->get(kv.first);
                }
            }

            // 恢复环境
            currentEnv_ = prevEnv;
            callStack_.pop_back();
            recursionDepth_--;

            delete initEnv;
        }

        return instance;
    }

    // 查找函数定义
    auto it = funRegistry_.find(node.name);
    if (it == funRegistry_.end()) {
        runtimeError("未定义的函数: " + node.name, node.line, node.column);
    }

    FunDecl* funDecl = it->second;

    // 检查参数数量
    if (node.arguments.size() != funDecl->params.size()) {
        runtimeError("函数 " + node.name + " 期望 " +
                     std::to_string(funDecl->params.size()) + " 个参数，但传入了 " +
                     std::to_string(node.arguments.size()) + " 个",
                     node.line, node.column);
    }

    // 递归深度检查
    recursionDepth_++;
    if (recursionDepth_ >= 256) {
        recursionDepth_--;
        runtimeError("递归深度超过限制 (256)", node.line, node.column);
    }

    // 求值参数
    std::vector<Value> argValues;
    for (auto& arg : node.arguments) {
        argValues.push_back(evaluate(arg.get()));
    }

    // 创建新环境（使用当前环境作为父级，支持闭包/嵌套函数访问外层变量）
    Environment* funEnv = new Environment(currentEnv_);

    // 绑定参数
    for (size_t i = 0; i < funDecl->params.size(); ++i) {
        funEnv->define(funDecl->params[i], argValues[i]);
    }

    // 压入调用帧
    callStack_.emplace_back(node.name, funEnv, node.line, recursionDepth_);

    // 切换环境
    Environment* prevEnv = currentEnv_;
    currentEnv_ = funEnv;

    Value result = Value::nullValue();
    try {
        // 执行函数体
        result = evaluate(funDecl->body.get());
    } catch (const ReturnException& e) {
        result = e.returnValue;
    }

    // 恢复环境
    currentEnv_ = prevEnv;
    callStack_.pop_back();
    recursionDepth_--;

    // 释放函数环境
    delete funEnv;

    return result;
}

Value Interpreter::visitReturnStmt(ReturnStmt& node) {
    checkBreak(&node);

    Value val = Value::nullValue();
    if (node.value) {
        val = evaluate(node.value.get());
    }
    throw ReturnException(val);
}

Value Interpreter::visitPrintStmt(PrintStmt& node) {
    checkBreak(&node);

    std::string result;
    for (size_t i = 0; i < node.values.size(); ++i) {
        Value val = evaluate(node.values[i].get());
        if (i > 0) result += " ";
        result += val.toString();
    }
    output(result);
    return Value::nullValue();
}

Value Interpreter::visitBlock(Block& node) {
    checkBreak(&node);

    // 为代码块创建新作用域
    Environment* blockEnv = new Environment(currentEnv_);
    currentEnv_ = blockEnv;

    Value result = Value::nullValue();
    for (auto& stmt : node.statements) {
        result = evaluate(stmt.get());
    }

    currentEnv_ = blockEnv->parent;
    delete blockEnv;

    return result;
}

// ---- 新增 9 个 visit 方法 ----

Value Interpreter::visitArrayLiteral(ArrayLiteral& node) {
    checkBreak(&node);

    std::vector<Value> elements;
    for (auto& elem : node.elements) {
        elements.push_back(evaluate(elem.get()));
    }
    return Value(elements);
}

Value Interpreter::visitDictLiteral(DictLiteral& node) {
    checkBreak(&node);

    std::unordered_map<std::string, Value> dict;
    for (auto& pair : node.pairs) {
        Value key = evaluate(pair.first.get());
        Value val = evaluate(pair.second.get());
        // 字典的键必须是字符串
        dict[key.toString()] = val;
    }
    return Value(dict);
}

Value Interpreter::visitIndexAccess(IndexAccess& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());
    Value idx = evaluate(node.index.get());

    // 数组索引访问
    if (obj.isArray()) {
        if (!idx.isInt()) {
            runtimeError("数组索引必须是整数", node.line, node.column);
        }
        int i = idx.intVal;
        if (i < 0 || i >= static_cast<int>(obj.arrayVal.size())) {
            runtimeError("数组索引越界: " + std::to_string(i), node.line, node.column);
        }
        return obj.arrayVal[i];
    }

    // 字典索引访问
    if (obj.isDict()) {
        if (!idx.isString()) {
            runtimeError("字典键必须是字符串", node.line, node.column);
        }
        auto it = obj.dictVal.find(idx.stringVal);
        if (it == obj.dictVal.end()) {
            return Value::nullValue();
        }
        return it->second;
    }

    runtimeError("该类型不支持索引访问", node.line, node.column);
}

Value Interpreter::visitIndexAssign(IndexAssign& node) {
    checkBreak(&node);

    // 需要获取对象的左值，因此用变量名查找
    // 先求值 index 和 value
    Value idx = evaluate(node.index.get());
    Value val = evaluate(node.value.get());

    // object 必须是 VarRef（简单变量名），以便我们修改其内容
    VarRef* varRef = dynamic_cast<VarRef*>(node.object.get());
    if (varRef) {
        if (!currentEnv_->hasVariable(varRef->name)) {
            runtimeError("未定义的变量: " + varRef->name, node.line, node.column);
        }
        Value obj = currentEnv_->get(varRef->name);

        if (obj.isArray()) {
            if (!idx.isInt()) {
                runtimeError("数组索引必须是整数", node.line, node.column);
            }
            int i = idx.intVal;
            if (i < 0 || i >= static_cast<int>(obj.arrayVal.size())) {
                runtimeError("数组索引越界: " + std::to_string(i), node.line, node.column);
            }
            // 修改数组元素：获取副本，修改，写回
            obj.arrayVal[i] = val;
            currentEnv_->set(varRef->name, obj);
            return val;
        }

        if (obj.isDict()) {
            if (!idx.isString()) {
                runtimeError("字典键必须是字符串", node.line, node.column);
            }
            // 修改字典元素：获取副本，修改，写回
            obj.dictVal[idx.stringVal] = val;
            currentEnv_->set(varRef->name, obj);
            return val;
        }

        runtimeError("该类型不支持索引赋值", node.line, node.column);
    }

    // 也可能是对 IndexAccess 的链式赋值（多维数组等），这里简化处理
    runtimeError("索引赋值的对象必须是变量", node.line, node.column);
}

Value Interpreter::visitClassDecl(ClassDecl& node) {
    checkBreak(&node);

    ClassInfo cls;
    cls.name = node.name;
    cls.superClassName = node.superClassName;
    cls.superClass = nullptr;

    // 如果有父类，查找父类信息
    if (!node.superClassName.empty()) {
        auto it = classRegistry_.find(node.superClassName);
        if (it == classRegistry_.end()) {
            runtimeError("未定义的父类: " + node.superClassName, node.line, node.column);
        }
        cls.superClass = &(it->second);
    }

    // 注册类名到环境
    Value classVal(std::string("class:") + node.name);
    classVal.type = ValueType::VAL_STRING;
    currentEnv_->define(node.name, classVal);

    // 先注册类信息（以便方法中可以递归引用自身）
    classRegistry_[node.name] = cls;
    ClassInfo& registeredCls = classRegistry_[node.name];

    // 处理类成员
    for (auto& member : node.members) {
        // VarDecl: 字段默认值
        VarDecl* varDecl = dynamic_cast<VarDecl*>(member.get());
        if (varDecl) {
            Value defaultVal = Value::nullValue();
            if (varDecl->initializer) {
                defaultVal = evaluate(varDecl->initializer.get());
            }
            registeredCls.fields[varDecl->name] = defaultVal;
            continue;
        }

        // FunDecl: 方法
        FunDecl* funDecl = dynamic_cast<FunDecl*>(member.get());
        if (funDecl) {
            registeredCls.methods[funDecl->name] = funDecl;
            // 同时注册到全局函数表（方法名带类名前缀避免冲突）
            std::string methodKey = node.name + "." + funDecl->name;
            funRegistry_[methodKey] = funDecl;
            continue;
        }
    }

    return classVal;
}

Value Interpreter::visitMemberAccess(MemberAccess& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());

    // 类实例的成员访问
    if (obj.isInstance()) {
        auto it = obj.fields.find(node.fieldName);
        if (it != obj.fields.end()) {
            return it->second;
        }

        // 检查是否访问的是方法（返回一个标记值）
        auto classIt = classRegistry_.find(obj.className);
        if (classIt != classRegistry_.end()) {
            FunDecl* method = findMethod(classIt->second, node.fieldName);
            if (method) {
                // 方法作为字段访问，返回特殊标记
                Value methodVal(std::string("method:") + obj.className + "." + node.fieldName);
                methodVal.type = ValueType::VAL_STRING;
                return methodVal;
            }
        }

        return Value::nullValue();
    }

    // 字典的成员访问（同索引访问）
    if (obj.isDict()) {
        auto it = obj.dictVal.find(node.fieldName);
        if (it != obj.dictVal.end()) {
            return it->second;
        }
        return Value::nullValue();
    }

    runtimeError("该类型不支持成员访问", node.line, node.column);
}

Value Interpreter::visitMemberAssign(MemberAssign& node) {
    checkBreak(&node);

    Value val = evaluate(node.value.get());

    // object 必须是 VarRef，以便我们修改实例
    VarRef* varRef = dynamic_cast<VarRef*>(node.object.get());
    if (varRef) {
        if (!currentEnv_->hasVariable(varRef->name)) {
            runtimeError("未定义的变量: " + varRef->name, node.line, node.column);
        }
        Value obj = currentEnv_->get(varRef->name);

        if (obj.isInstance()) {
            obj.fields[node.fieldName] = val;
            currentEnv_->set(varRef->name, obj);
            return val;
        }

        if (obj.isDict()) {
            obj.dictVal[node.fieldName] = val;
            currentEnv_->set(varRef->name, obj);
            return val;
        }

        runtimeError("该类型不支持成员赋值", node.line, node.column);
    }

    // 也可能是 MemberAccess 的链式赋值
    runtimeError("成员赋值的对象必须是变量", node.line, node.column);
}

Value Interpreter::visitMethodCall(MethodCall& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());

    // 类实例的方法调用
    if (obj.isInstance()) {
        auto classIt = classRegistry_.find(obj.className);
        if (classIt != classRegistry_.end()) {
            FunDecl* method = findMethod(classIt->second, node.methodName);
            if (method) {
                // 递归深度检查
                recursionDepth_++;
                if (recursionDepth_ >= 256) {
                    recursionDepth_--;
                    runtimeError("递归深度超过限制 (256)", node.line, node.column);
                }

                // 求值参数
                std::vector<Value> argValues;
                for (auto& arg : node.arguments) {
                    argValues.push_back(evaluate(arg.get()));
                }

                // 检查参数数量
                if (argValues.size() != method->params.size()) {
                    recursionDepth_--;
                    runtimeError("方法 " + node.methodName + " 期望 " +
                                 std::to_string(method->params.size()) + " 个参数，但传入了 " +
                                 std::to_string(argValues.size()) + " 个",
                                 node.line, node.column);
                }

                // 创建方法环境（父级为当前环境，支持闭包）
                Environment* methodEnv = new Environment(currentEnv_);

                // 绑定 this
                methodEnv->define("this", obj);

                // 将实例字段注入方法环境，使方法内可直接用 name 访问 this.name
                for (const auto& kv : obj.fields) {
                    methodEnv->define(kv.first, kv.second);
                }

                // 绑定参数（参数覆盖同名字段）
                for (size_t i = 0; i < method->params.size(); ++i) {
                    methodEnv->define(method->params[i], argValues[i]);
                }

                // 压入调用帧
                callStack_.emplace_back(obj.className + "." + node.methodName,
                                         methodEnv, node.line, recursionDepth_);

                // 切换环境
                Environment* prevEnv = currentEnv_;
                currentEnv_ = methodEnv;

                Value result = Value::nullValue();
                try {
                    result = evaluate(method->body.get());
                } catch (const ReturnException& e) {
                    result = e.returnValue;
                }

                // 从方法环境中读取 this 的更新值
                Value updatedThis = methodEnv->get("this");

                // 将方法内对字段的直接修改同步回 this 实例
                for (const auto& kv : obj.fields) {
                    if (methodEnv->hasVariable(kv.first)) {
                        updatedThis.fields[kv.first] = methodEnv->get(kv.first);
                    }
                }
                // 也检查方法中新添加到实例的字段（字段名在 this 中但不在原始 obj 中）
                // 注意：方法内 name = val 这种赋值可能创建了新的局部变量而非字段
                // 但如果是已有的字段名被重新赋值，我们需要同步

                // 恢复环境
                currentEnv_ = prevEnv;
                callStack_.pop_back();
                recursionDepth_--;

                delete methodEnv;

                // 更新实例（如果 this 被修改了）
                // 需要在环境中找到原始的实例变量并更新
                // 这里简化处理：如果 object 是 VarRef，更新环境中的值
                VarRef* objRef = dynamic_cast<VarRef*>(node.object.get());
                if (objRef) {
                    currentEnv_->set(objRef->name, updatedThis);
                }

                return result;
            }
        }

        runtimeError("类 " + obj.className + " 没有方法 " + node.methodName,
                     node.line, node.column);
    }

    runtimeError("方法调用需要类实例", node.line, node.column);
}

Value Interpreter::visitNullLiteral(NullLiteral& node) {
    checkBreak(&node);
    return Value::nullValue();
}
