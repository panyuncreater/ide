#include "interpreter/Interpreter.h"
#include "debug/DebugController.h"
#include <cmath>
#include <cctype>
#include <cstdint>
#include <sstream>

// ============================================================
// Interpreter 解释器实现
// ============================================================

Interpreter::Interpreter()
    : globalEnv_(std::make_shared<Environment>())
    , currentEnv_(globalEnv_)
    , debugger_(nullptr)
    , recursionDepth_(0)
{
    outputCallback_ = [](const std::string&) {};
}

Interpreter::~Interpreter() {
    // shared_ptr 自动管理环境生命周期，无需手动 delete
}

Value Interpreter::execute(Block& program) {
    // 重置状态
    globalEnv_ = std::make_shared<Environment>();
    currentEnv_ = globalEnv_;
    callStack_.clear();
    funRegistry_.clear();
    classRegistry_.clear();
    typeAnnotations_.clear();
    currentFunctionReturnType_.clear();
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
    return currentEnv_.get();
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
        // 同步调用深度到调试控制器（Step Over 依赖此值判断是否进入函数）
        debugger_->setCurrentDepth(recursionDepth_);
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

// ---- 类型检查辅助方法 ----

bool Interpreter::typeMatch(const Value& val, const std::string& annotation) const {
    if (annotation.empty()) return true;
    if (annotation == "int") return val.isInt();
    if (annotation == "float") return val.isFloat() || val.isInt();
    if (annotation == "bool") return val.isBool();
    if (annotation == "string") return val.isString();
    if (annotation == "array") return val.isArray();
    if (annotation == "dict") return val.isDict();
    // 数组元素类型注解，如 "int[]"
    if (annotation.size() >= 2 && annotation.back() == ']') {
        if (!val.isArray()) return false;
        std::string elemType = annotation.substr(0, annotation.size() - 2);
        for (const auto& elem : val.arrayVal) {
            if (!typeMatch(elem, elemType)) return false;
        }
        return true;
    }
    if (val.isInstance() && val.className == annotation) return true;
    if (val.isNull()) return true;
    return false;
}

void Interpreter::checkType(const Value& val, const std::string& annotation,
                            const std::string& context, int line, int col) {
    if (!typeMatch(val, annotation)) {
        runtimeError(context + " 期望类型 " + annotation + "，实际为 " + val.typeName(), line, col);
    }
}

std::string Interpreter::findTypeAnnotation(const std::string& varName) const {
    auto it = typeAnnotations_.find(varName);
    if (it != typeAnnotations_.end()) return it->second;
    return "";
}

// ---- writeBack 递归写回左值 ----

void Interpreter::writeBack(ASTNode* node, const Value& modifiedValue, int line, int col) {
    switch (node->nodeType) {
    case NodeType::NODE_VAR_REF: {
        auto* varRef = static_cast<VarRef*>(node);
        currentEnv_->set(varRef->name, modifiedValue);
        break;
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        auto* memberAccess = static_cast<MemberAccess*>(node);
        Value outer = evaluate(memberAccess->object.get());
        if (outer.isInstance()) {
            outer.fields[memberAccess->fieldName] = modifiedValue;
        } else if (outer.isDict()) {
            outer.dictVal[memberAccess->fieldName] = modifiedValue;
        } else {
            runtimeError("该类型不支持成员赋值", line, col);
        }
        writeBack(memberAccess->object.get(), outer, line, col);
        break;
    }
    case NodeType::NODE_INDEX_ACCESS: {
        auto* indexAccess = static_cast<IndexAccess*>(node);
        Value outer = evaluate(indexAccess->object.get());
        Value idx = evaluate(indexAccess->index.get());
        if (outer.isArray() && idx.isInt()) {
            if (idx.intVal < 0 || static_cast<size_t>(idx.intVal) >= outer.arrayVal.size())
                runtimeError("数组索引越界", line, col);
            outer.arrayVal[idx.intVal] = modifiedValue;
        } else if (outer.isDict() && idx.isString()) {
            outer.dictVal[idx.stringVal] = modifiedValue;
        } else {
            runtimeError("该类型不支持索引赋值", line, col);
        }
        writeBack(indexAccess->object.get(), outer, line, col);
        break;
    }
    default:
        break;
    }
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
                auto initEnv = std::make_shared<Environment>(currentEnv_);
                initEnv->define("this", instance);
                // 将实例字段注入 init 环境
                for (const auto& kv : instance.fields) {
                    initEnv->define(kv.first, kv.second);
                }
                auto prevEnv = currentEnv_;
                currentEnv_ = initEnv;
                try {
                    evaluate(initMethod->body.get());
                } catch (const ReturnException& e) {}
                instance = initEnv->get("this");
                currentEnv_ = prevEnv;
            }

            initVal = instance;
        }
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
    }

    // 类型检查：如果有类型注解且有初始化表达式
    if (!node.typeAnnotation.empty() && node.initializer) {
        checkType(initVal, node.typeAnnotation, "变量 " + node.name + " 的类型", node.line, node.column);
    }
    // 记录类型注解
    if (!node.typeAnnotation.empty()) {
        typeAnnotations_[node.name] = node.typeAnnotation;
    }

    currentEnv_->define(node.name, initVal);
    return initVal;
}

Value Interpreter::visitAssignment(Assignment& node) {
    checkBreak(&node);

    Value val = evaluate(node.value.get());

    // 类型检查
    std::string typeAnn = findTypeAnnotation(node.name);
    if (!typeAnn.empty()) {
        checkType(val, typeAnn, "赋值给 " + node.name, node.line, node.column);
    }

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
    auto forEnv = std::make_shared<Environment>(currentEnv_);
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
                throw;
            }

            // 更新
            if (node.update) {
                evaluate(node.update.get());
            }
        }
    } catch (...) {
        currentEnv_ = forEnv->parent;
        throw;
    }

    currentEnv_ = forEnv->parent;
    return result;
}

Value Interpreter::visitFunDecl(FunDecl& node) {
    checkBreak(&node);

    // 创建闭包值，捕获当前环境
    Value funVal = Value::makeClosure(node.name, currentEnv_, node.params);
    currentEnv_->define(node.name, funVal);

    // 在全局注册函数体（用于调用时查找）
    funRegistry_[node.name] = &node;

    return funVal;
}

Value Interpreter::visitFunCall(FunCall& node) {
    checkBreak(&node);

    // 内置函数: dict() 和 array()
    if (node.name == "dict") {
        // dict() 或 dict({"a": 1, ...})
        if (node.arguments.empty()) {
            return Value(std::unordered_map<std::string, Value>());
        }
        // 有参数时，求值参数（期望是字典字面量）
        std::vector<Value> args;
        for (auto& arg : node.arguments) {
            args.push_back(evaluate(arg.get()));
        }
        if (args.size() == 1 && args[0].isDict()) {
            return args[0];
        }
        runtimeError("dict() 期望无参数或一个字典参数", node.line, node.column);
    }
    if (node.name == "array") {
        // array() 或 array(1, 2, 3)
        if (node.arguments.empty()) {
            return Value(std::vector<Value>());
        }
        std::vector<Value> args;
        for (auto& arg : node.arguments) {
            args.push_back(evaluate(arg.get()));
        }
        return Value(args);
    }

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
            auto initEnv = std::make_shared<Environment>(currentEnv_);

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

            // 设置返回类型追踪
            std::string savedReturnType = currentFunctionReturnType_;
            currentFunctionReturnType_ = initMethod->returnType;

            // 切换环境
            auto prevEnv = currentEnv_;
            currentEnv_ = initEnv;

            try {
                evaluate(initMethod->body.get());
            } catch (const ReturnException& e) {
                // init 方法的返回值忽略，但更新实例字段
            }

            // 从 init 环境中读取 this 的更新值
            // writeBack 已经正确更新了 initEnv 中的 this，
            // 直接获取即可，不需要再用环境变量覆盖
            instance = initEnv->get("this");

            // 恢复环境
            currentEnv_ = prevEnv;
            callStack_.pop_back();
            recursionDepth_--;
            currentFunctionReturnType_ = savedReturnType;
        }

        return instance;
    }

    // 检查环境中是否有闭包值
    std::shared_ptr<Environment> closureEnv;
    std::vector<std::string> closureParams;
    FunDecl* funDecl = nullptr;
    std::string effectiveName = node.name;  // 实际函数名（闭包时可能不同于调用变量名）

    if (currentEnv_->hasVariable(node.name)) {
        Value callee = currentEnv_->get(node.name);
        if (callee.isClosure()) {
            closureEnv = callee.closureEnv;
            closureParams = callee.closureParams;
            effectiveName = callee.closureName;  // 使用闭包的实际函数名查找函数体
            auto it = funRegistry_.find(callee.closureName);
            if (it != funRegistry_.end()) {
                funDecl = it->second;
            }
        }
    }

    // 闭包路径：如果没找到闭包，走 funRegistry_ fallback
    if (!funDecl) {
        auto it = funRegistry_.find(effectiveName);
        if (it == funRegistry_.end()) {
            runtimeError("未定义的函数: " + node.name, node.line, node.column);
        }
        funDecl = it->second;
    }

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

    // 参数类型检查
    for (size_t i = 0; i < funDecl->params.size() && i < funDecl->paramTypes.size(); ++i) {
        if (!funDecl->paramTypes[i].empty()) {
            checkType(argValues[i], funDecl->paramTypes[i],
                      "函数 " + node.name + " 的参数 " + funDecl->params[i],
                      node.line, node.column);
        }
    }

    // 创建新环境：使用闭包捕获的环境作为父级（如果有的话）
    std::shared_ptr<Environment> funEnv;
    if (closureEnv) {
        funEnv = std::make_shared<Environment>(closureEnv);
    } else {
        funEnv = std::make_shared<Environment>(currentEnv_);
    }

    // 绑定参数
    for (size_t i = 0; i < funDecl->params.size(); ++i) {
        funEnv->define(funDecl->params[i], argValues[i]);
    }

    // 压入调用帧
    callStack_.emplace_back(node.name, funEnv, node.line, recursionDepth_);

    // 设置返回类型追踪
    std::string savedReturnType = currentFunctionReturnType_;
    currentFunctionReturnType_ = funDecl->returnType;

    // 切换环境
    auto prevEnv = currentEnv_;
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
    currentFunctionReturnType_ = savedReturnType;

    return result;
}

Value Interpreter::visitReturnStmt(ReturnStmt& node) {
    checkBreak(&node);

    Value val = Value::nullValue();
    if (node.value) {
        val = evaluate(node.value.get());
    }

    // 返回类型检查
    if (!currentFunctionReturnType_.empty()) {
        checkType(val, currentFunctionReturnType_, "返回值", node.line, node.column);
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
    auto blockEnv = std::make_shared<Environment>(currentEnv_);
    currentEnv_ = blockEnv;

    Value result = Value::nullValue();
    for (auto& stmt : node.statements) {
        result = evaluate(stmt.get());
    }

    currentEnv_ = blockEnv->parent;

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

    Value idx = evaluate(node.index.get());
    Value val = evaluate(node.value.get());
    Value obj = evaluate(node.object.get());

    if (obj.isArray() && idx.isInt()) {
        if (idx.intVal < 0 || static_cast<size_t>(idx.intVal) >= obj.arrayVal.size())
            runtimeError("数组索引越界", node.line, node.column);
        obj.arrayVal[idx.intVal] = val;
    } else if (obj.isDict() && idx.isString()) {
        obj.dictVal[idx.stringVal] = val;
    } else {
        runtimeError("该类型不支持索引赋值", node.line, node.column);
    }

    writeBack(node.object.get(), obj, node.line, node.column);
    return val;
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
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            VarDecl* varDecl = static_cast<VarDecl*>(member.get());
            Value defaultVal = Value::nullValue();
            if (varDecl->initializer) {
                defaultVal = evaluate(varDecl->initializer.get());
            }
            registeredCls.fields[varDecl->name] = defaultVal;
            continue;
        }

        // FunDecl: 方法
        if (member->nodeType == NodeType::NODE_FUN_DECL) {
            FunDecl* funDecl = static_cast<FunDecl*>(member.get());
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
    Value obj = evaluate(node.object.get());

    if (obj.isInstance()) {
        obj.fields[node.fieldName] = val;
    } else if (obj.isDict()) {
        obj.dictVal[node.fieldName] = val;
    } else {
        runtimeError("该类型不支持成员赋值", node.line, node.column);
    }

    writeBack(node.object.get(), obj, node.line, node.column);
    return val;
}

Value Interpreter::visitMethodCall(MethodCall& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());

    // ---- 数组内置方法 ----
    if (obj.isArray()) {
        // 求值参数
        std::vector<Value> argValues;
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "push") {
            if (argValues.size() != 1)
                runtimeError("push 期望 1 个参数", node.line, node.column);
            obj.arrayVal.push_back(argValues[0]);
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        if (node.methodName == "pop") {
            if (obj.arrayVal.empty())
                runtimeError("对空数组调用 pop", node.line, node.column);
            Value last = obj.arrayVal.back();
            obj.arrayVal.pop_back();
            writeBack(node.object.get(), obj, node.line, node.column);
            return last;
        }
        if (node.methodName == "len") {
            return Value(static_cast<int>(obj.arrayVal.size()));
        }
        if (node.methodName == "remove") {
            if (argValues.size() != 1)
                runtimeError("remove 期望 1 个参数(索引)", node.line, node.column);
            if (!argValues[0].isInt())
                runtimeError("remove 参数必须是整数索引", node.line, node.column);
            int idx = static_cast<int>(argValues[0].intVal);
            if (idx < 0 || static_cast<size_t>(idx) >= obj.arrayVal.size())
                runtimeError("数组索引越界: " + std::to_string(idx), node.line, node.column);
            obj.arrayVal.erase(obj.arrayVal.begin() + idx);
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        if (node.methodName == "contains") {
            if (argValues.size() != 1)
                runtimeError("contains 期望 1 个参数", node.line, node.column);
            for (const auto& elem : obj.arrayVal) {
                if (elem.equals(argValues[0])) return Value(true);
            }
            return Value(false);
        }
        if (node.methodName == "join") {
            std::string sep = argValues.empty() ? "" : argValues[0].toString();
            std::string result;
            for (size_t i = 0; i < obj.arrayVal.size(); ++i) {
                if (i > 0) result += sep;
                result += obj.arrayVal[i].toString();
            }
            return Value(result);
        }
        runtimeError("数组没有方法 " + node.methodName, node.line, node.column);
    }

    // ---- 字典内置方法 ----
    if (obj.isDict()) {
        std::vector<Value> argValues;
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "len") {
            return Value(static_cast<int>(obj.dictVal.size()));
        }
        if (node.methodName == "keys") {
            std::vector<Value> keys;
            for (const auto& kv : obj.dictVal) {
                keys.push_back(Value(kv.first));
            }
            return Value(keys);
        }
        if (node.methodName == "values") {
            std::vector<Value> vals;
            for (const auto& kv : obj.dictVal) {
                vals.push_back(kv.second);
            }
            return Value(vals);
        }
        if (node.methodName == "has" || node.methodName == "contains") {
            if (argValues.size() != 1)
                runtimeError(node.methodName + " 期望 1 个参数(键)", node.line, node.column);
            return Value(obj.dictVal.find(argValues[0].toString()) != obj.dictVal.end());
        }
        if (node.methodName == "remove") {
            if (argValues.size() != 1)
                runtimeError("remove 期望 1 个参数(键)", node.line, node.column);
            obj.dictVal.erase(argValues[0].toString());
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        runtimeError("字典没有方法 " + node.methodName, node.line, node.column);
    }

    // ---- 字符串内置方法 ----
    if (obj.isString()) {
        std::vector<Value> argValues;
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "len") {
            return Value(static_cast<int>(obj.stringVal.size()));
        }
        if (node.methodName == "upper") {
            std::string s = obj.stringVal;
            for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
            return Value(s);
        }
        if (node.methodName == "lower") {
            std::string s = obj.stringVal;
            for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c));
            return Value(s);
        }
        if (node.methodName == "split") {
            // str.split(sep) — 按 sep 分割返回数组
            std::string sep = argValues.empty() ? " " : argValues[0].toString();
            std::vector<Value> parts;
            size_t start = 0, pos;
            while ((pos = obj.stringVal.find(sep, start)) != std::string::npos) {
                parts.push_back(Value(obj.stringVal.substr(start, pos - start)));
                start = pos + sep.size();
            }
            parts.push_back(Value(obj.stringVal.substr(start)));
            return Value(parts);
        }
        if (node.methodName == "trim") {
            std::string s = obj.stringVal;
            size_t l = s.find_first_not_of(" \t\r\n");
            size_t r = s.find_last_not_of(" \t\r\n");
            if (l == std::string::npos) return Value(std::string(""));
            return Value(s.substr(l, r - l + 1));
        }
        runtimeError("字符串没有方法 " + node.methodName, node.line, node.column);
    }

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

                // 创建方法环境（父级为当前环境，方法不创建闭包，在调用时绑定 this）
                auto methodEnv = std::make_shared<Environment>(currentEnv_);

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

                // 设置返回类型追踪
                std::string savedReturnType = currentFunctionReturnType_;
                currentFunctionReturnType_ = method->returnType;

                // 切换环境
                auto prevEnv = currentEnv_;
                currentEnv_ = methodEnv;

                Value result = Value::nullValue();
                try {
                    result = evaluate(method->body.get());
                } catch (const ReturnException& e) {
                    result = e.returnValue;
                }

                // 从方法环境中读取 this 的更新值
                // writeBack 已经正确更新了 methodEnv 中的 this，
                // 直接获取即可，不需要再用环境变量覆盖
                Value updatedThis = methodEnv->get("this");

                // 恢复环境
                currentEnv_ = prevEnv;
                callStack_.pop_back();
                recursionDepth_--;
                currentFunctionReturnType_ = savedReturnType;

                // 更新实例（使用 writeBack 支持嵌套左值，如 arr[i].method()）
                writeBack(node.object.get(), updatedThis, node.line, node.column);

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
