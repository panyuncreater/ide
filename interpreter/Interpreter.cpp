#include "interpreter/Interpreter.h"
#include "debug/DebugController.h"
#include <cmath>
#include <cctype>
#include <cstdint>
#include <climits>
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
    // 但清除注册表中的 AST 裸指针（旧 AST 可能已被销毁）
    funRegistry_.clear();
    classRegistry_.clear();
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

Value Interpreter::evaluateExpr(ASTNode* node) {
    if (!node) return Value::nullValue();
    // RAII 守卫：确保异常时也能恢复 debugMode_
    struct DebugModeGuard {
        bool& ref;
        bool saved;
        DebugModeGuard(bool& r) : ref(r), saved(r) { r = false; }
        ~DebugModeGuard() { ref = saved; }
    } guard(debugMode_);
    return node->accept(*this);
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

Value Interpreter::numericBinaryOp(BinOpType opType, const Value& left,
                                    const Value& right, int line, int col) {
    // 字符串拼接（仅加法）
    if (opType == BinOpType::BIN_ADD) {
        if (left.isString() && right.isString()) {
            return Value(left.stringVal() + right.stringVal());
        }
        if (left.isString() || right.isString()) {
            return Value(left.toString() + right.toString());
        }
    }

    // 非数值类型检查（字符串拼接已在上面处理）
    if (!left.isNumber() || !right.isNumber()) {
        runtimeError("算术运算需要数值类型", line, col);
    }

    // 数值运算（switch 分发，零字符串比较）
    switch (opType) {
    case BinOpType::BIN_ADD:
        if (left.isInt() && right.isInt()) return Value(left.intVal() + right.intVal());
        return Value(left.toDouble() + right.toDouble());
    case BinOpType::BIN_SUB:
        if (left.isInt() && right.isInt()) return Value(left.intVal() - right.intVal());
        return Value(left.toDouble() - right.toDouble());
    case BinOpType::BIN_MUL:
        if (left.isInt() && right.isInt()) return Value(left.intVal() * right.intVal());
        return Value(left.toDouble() * right.toDouble());
    case BinOpType::BIN_DIV:
        { double r = right.toDouble();
          if (r == 0.0) runtimeError("除零错误", line, col);
          if (left.isInt() && right.isInt()) {
              if (left.intVal() == INT64_MIN && right.intVal() == -1) runtimeError("整数除法溢出", line, col);
              return Value(left.intVal() / right.intVal());
          }
          return Value(left.toDouble() / r); }
    case BinOpType::BIN_MOD:
        if (!left.isInt() || !right.isInt()) runtimeError("取模运算仅支持整数", line, col);
        if (right.intVal() == 0) runtimeError("除零错误", line, col);
        if (left.intVal() == INT64_MIN && right.intVal() == -1) return Value(0);
        return Value(left.intVal() % right.intVal());
    default:
        runtimeError("不支持的算术运算符", line, col);
    }
    return Value::nullValue();  // 不可达，但消除编译器警告
}

// ---- 类辅助方法 ----

FunDecl* Interpreter::findMethod(ClassInfo& cls, const std::string& methodName) {
    auto it = cls.methods.find(methodName);
    if (it != cls.methods.end()) return it->second;
    // 沿继承链查找（通过名称查找，避免悬空指针）
    if (!cls.superClassName.empty()) {
        auto superIt = classRegistry_.find(cls.superClassName);
        if (superIt != classRegistry_.end()) return findMethod(superIt->second, methodName);
    }
    return nullptr;
}

Value Interpreter::findFieldDefault(ClassInfo& cls, const std::string& fieldName) {
    auto it = cls.fields.find(fieldName);
    if (it != cls.fields.end()) return it->second;
    // 沿继承链查找
    if (!cls.superClassName.empty()) {
        auto superIt = classRegistry_.find(cls.superClassName);
        if (superIt != classRegistry_.end()) return findFieldDefault(superIt->second, fieldName);
    }
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
    if (annotation.size() >= 2 && annotation.back() == ']' && annotation[annotation.size() - 2] == '[') {
        if (!val.isArray()) return false;
        std::string elemType = annotation.substr(0, annotation.size() - 2);
        for (const auto& elem : val.arrayVal()) {
            if (!typeMatch(elem, elemType)) return false;
        }
        return true;
    }
    if (val.isInstance() && val.className() == annotation) return true;
    if (val.isNull()) return true;
    return false;
}

void Interpreter::checkType(const Value& val, const std::string& annotation,
                            const std::function<std::string()>& contextBuilder, int line, int col) {
    if (!typeMatch(val, annotation)) {
        runtimeError(contextBuilder() + " 期望类型 " + annotation + "，实际为 " + val.typeName(), line, col);
    }
}

const std::string* Interpreter::findTypeAnnotation(const std::string& varName) const {
    auto it = typeAnnotations_.find(varName);
    if (it != typeAnnotations_.end()) return &it->second;
    return nullptr;
}

// ---- writeBack 写回左值 ----
// 链式求值方式：从最外层到最内层逐级求值（每级仅一次），在最内层执行赋值，
// 再从内到外逐级写回，避免对含副作用的子表达式重复求值。

Value Interpreter::writeBack(ASTNode* objectNode, bool isIndexAssign, ASTNode* indexNode,
                            const std::string& fieldName, ASTNode* valueNode, int line, int col) {
    // 收集从 objectNode 到最外层 VarRef 的节点链
    std::vector<ASTNode*> chain;
    ASTNode* cur = objectNode;
    while (cur->nodeType == NodeType::NODE_MEMBER_ACCESS ||
           cur->nodeType == NodeType::NODE_INDEX_ACCESS) {
        chain.push_back(cur);
        if (cur->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            cur = static_cast<MemberAccess*>(cur)->object.get();
        } else {
            cur = static_cast<IndexAccess*>(cur)->object.get();
        }
    }
    chain.push_back(cur); // 最外层 VarRef

    int n = static_cast<int>(chain.size());

    // 从外到内逐级求值，收集每级的值和索引
    std::vector<Value> vals(n);
    std::vector<Value> idxs(n); // IndexAccess 节点的索引值

    auto* varRef = static_cast<VarRef*>(chain[n - 1]);
    vals[n - 1] = currentEnv_->get(varRef->name);

    for (int i = n - 2; i >= 0; i--) {
        ASTNode* nd = chain[i];
        const Value& parent = vals[i + 1];
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parent.isInstance()) {
                auto it = parent.fields().find(ma->fieldName);
                vals[i] = (it != parent.fields().end()) ? it->second : Value::nullValue();
            } else if (parent.isDict()) {
                auto it = parent.dictVal().find(ma->fieldName);
                vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            } else {
                runtimeError("该类型不支持成员访问", line, col);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(nd);
            idxs[i] = evaluate(ia->index.get());
            const Value& indexVal = idxs[i];
            if (parent.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parent.arrayVal().size())
                    runtimeError("数组索引越界", line, col);
                vals[i] = parent.arrayVal()[indexVal.intVal()];
            } else if (parent.isDict() && indexVal.isString()) {
                auto it = parent.dictVal().find(indexVal.stringVal());
                vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            } else {
                runtimeError("该类型不支持索引访问", line, col);
            }
        }
    }

    // 链式求值完成，现在按左到右顺序求值 index 和 value
    Value idx;
    if (isIndexAssign && indexNode) {
        idx = evaluate(indexNode);
    }
    Value val;
    if (valueNode) {
        val = evaluate(valueNode);
    }

    // 在最内层对象上执行赋值
    Value modifiedObj = vals[0]; // 拷贝
    if (isIndexAssign) {
        if (modifiedObj.isArray() && idx.isInt()) {
            if (idx.intVal() < 0 || static_cast<size_t>(idx.intVal()) >= modifiedObj.arrayVal().size())
                runtimeError("数组索引越界", line, col);
            modifiedObj.arrayVal()[idx.intVal()] = val;
        } else if (modifiedObj.isDict() && idx.isString()) {
            modifiedObj.dictVal()[idx.stringVal()] = val;
        } else {
            runtimeError("该类型不支持索引赋值", line, col);
        }
    } else {
        if (modifiedObj.isInstance()) {
            modifiedObj.fields()[fieldName] = val;
        } else if (modifiedObj.isDict()) {
            modifiedObj.dictVal()[fieldName] = val;
        } else {
            runtimeError("该类型不支持成员赋值", line, col);
        }
    }

    // 从内到外逐级写回
    Value currentVal = modifiedObj;
    for (int i = 0; i < n - 1; i++) {
        ASTNode* nd = chain[i];
        Value parentVal = vals[i + 1]; // 拷贝，将在其上修改
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parentVal.isInstance()) {
                parentVal.fields()[ma->fieldName] = currentVal;
            } else if (parentVal.isDict()) {
                parentVal.dictVal()[ma->fieldName] = currentVal;
            } else {
                runtimeError("该类型不支持成员赋值", line, col);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            const Value& indexVal = idxs[i];
            if (parentVal.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parentVal.arrayVal().size())
                    runtimeError("数组索引越界", line, col);
                parentVal.arrayVal()[indexVal.intVal()] = currentVal;
            } else if (parentVal.isDict() && indexVal.isString()) {
                parentVal.dictVal()[indexVal.stringVal()] = currentVal;
            } else {
                runtimeError("该类型不支持索引赋值", line, col);
            }
        }
        currentVal = parentVal;
    }

    // 写回最外层变量
    currentEnv_->set(varRef->name, currentVal);
    return val;
}

// ---- writeBack 重载：写回已修改的值 ----
// 用于方法调用等已自行修改对象的场景，链式求值避免重复求值副作用

void Interpreter::writeBack(ASTNode* objectNode, const Value& modifiedValue, int line, int col) {
    // 收集从 objectNode 到最外层 VarRef 的节点链
    std::vector<ASTNode*> chain;
    ASTNode* cur = objectNode;
    while (cur->nodeType == NodeType::NODE_MEMBER_ACCESS ||
           cur->nodeType == NodeType::NODE_INDEX_ACCESS) {
        chain.push_back(cur);
        if (cur->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            cur = static_cast<MemberAccess*>(cur)->object.get();
        } else {
            cur = static_cast<IndexAccess*>(cur)->object.get();
        }
    }
    chain.push_back(cur); // 最外层 VarRef

    int n = static_cast<int>(chain.size());

    // 从外到内逐级求值，收集每级的值和索引
    std::vector<Value> vals(n);
    std::vector<Value> idxs(n);

    auto* varRef = static_cast<VarRef*>(chain[n - 1]);
    vals[n - 1] = currentEnv_->get(varRef->name);

    for (int i = n - 2; i >= 0; i--) {
        ASTNode* nd = chain[i];
        const Value& parent = vals[i + 1];
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parent.isInstance()) {
                auto it = parent.fields().find(ma->fieldName);
                vals[i] = (it != parent.fields().end()) ? it->second : Value::nullValue();
            } else if (parent.isDict()) {
                auto it = parent.dictVal().find(ma->fieldName);
                vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(nd);
            idxs[i] = evaluate(ia->index.get());
            const Value& indexVal = idxs[i];
            if (parent.isArray() && indexVal.isInt()) {
                int64_t idx = indexVal.intVal();
                if (idx < 0 || static_cast<size_t>(idx) >= parent.arrayVal().size()) {
                    runtimeError("数组索引越界: " + std::to_string(idx), ia->line, ia->column);
                }
                vals[i] = parent.arrayVal()[static_cast<size_t>(idx)];
            } else if (parent.isDict() && indexVal.isString()) {
                auto it = parent.dictVal().find(indexVal.stringVal());
                vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            }
        }
    }

    // 从内到外逐级写回（最内层使用 modifiedValue）
    Value currentVal = modifiedValue;
    for (int i = 0; i < n - 1; i++) {
        ASTNode* nd = chain[i];
        Value parentVal = vals[i + 1];
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parentVal.isInstance()) {
                parentVal.fields()[ma->fieldName] = currentVal;
            } else if (parentVal.isDict()) {
                parentVal.dictVal()[ma->fieldName] = currentVal;
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            const Value& indexVal = idxs[i];
            if (parentVal.isArray() && indexVal.isInt()) {
                int64_t idx = indexVal.intVal();
                if (idx < 0 || static_cast<size_t>(idx) >= parentVal.arrayVal().size()) {
                    runtimeError("数组越界: 索引 " + std::to_string(idx) +
                                 " 超出范围 [0, " + std::to_string(parentVal.arrayVal().size()) + ")",
                                 line, col);
                }
                parentVal.arrayVal()[static_cast<size_t>(idx)] = currentVal;
            } else if (parentVal.isDict() && indexVal.isString()) {
                parentVal.dictVal()[indexVal.stringVal()] = currentVal;
            }
        }
        currentVal = parentVal;
    }

    currentEnv_->set(varRef->name, currentVal);
}

// ---- 16 个原有 visit 方法 ----

Value Interpreter::visitBinaryOp(BinaryOp& node) {
    checkBreak(&node);

    // 使用预计算的枚举类型进行快速分发（避免运行时字符串比较）
    switch (node.opType) {
    case BinOpType::BIN_AND: {
        Value left = evaluate(node.left.get());
        if (!left.isTruthy()) return Value(false);
        Value right = evaluate(node.right.get());
        return Value(right.isTruthy());
    }
    case BinOpType::BIN_OR: {
        Value left = evaluate(node.left.get());
        if (left.isTruthy()) return Value(true);
        Value right = evaluate(node.right.get());
        return Value(right.isTruthy());
    }
    case BinOpType::BIN_EQ: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        return Value(left.equals(right));
    }
    case BinOpType::BIN_NEQ: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        return Value(!left.equals(right));
    }
    case BinOpType::BIN_LT: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() < right.toDouble());
    }
    case BinOpType::BIN_GT: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() > right.toDouble());
    }
    case BinOpType::BIN_LTE: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() <= right.toDouble());
    }
    case BinOpType::BIN_GTE: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值类型", node.line, node.column);
        return Value(left.toDouble() >= right.toDouble());
    }
    case BinOpType::BIN_ADD:
    case BinOpType::BIN_SUB:
    case BinOpType::BIN_MUL:
    case BinOpType::BIN_DIV:
    case BinOpType::BIN_MOD: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        return numericBinaryOp(node.opType, left, right, node.line, node.column);
    }
    default:
        runtimeError("未知运算符: " + node.op, node.line, node.column);
        return Value::nullValue();
    }
}

Value Interpreter::visitUnaryOp(UnaryOp& node) {
    checkBreak(&node);

    Value operand = evaluate(node.operand.get());

    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        if (operand.isInt()) {
            if (operand.intVal() == INT64_MIN) {
                runtimeError("整数溢出：无法对最小值取负", node.line, node.column);
                break;
            }
            return Value(-operand.intVal());
        }
        if (operand.isFloat()) return Value(-operand.floatVal());
        runtimeError("一元减运算需要数值类型", node.line, node.column);
        break;
    case UnaryOp::UnaryOpType::UOP_NOT:
        return Value(!operand.isTruthy());
    default:
        runtimeError("未知一元运算符: " + node.op, node.line, node.column);
        break;
    }
    return Value::nullValue();
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
                    if (instance.fields().find(kv.first) == instance.fields().end()) {
                        instance.fields()[kv.first] = kv.second;
                    }
                }
                if (!curCls->superClassName.empty()) {
                    auto superIt = classRegistry_.find(curCls->superClassName);
                    curCls = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
                } else {
                    curCls = nullptr;
                }
            }

            // 如果有 init 方法（0 参数），执行它
            FunDecl* initMethod = findMethod(cls, "init");
            if (initMethod && initMethod->params.empty()) {
                auto initEnv = std::make_shared<Environment>(currentEnv_);
                initEnv->define("this", instance);
                // 将实例字段注入 init 环境
                for (const auto& kv : instance.fields()) {
                    initEnv->define(kv.first, kv.second);
                }
                auto prevEnv = currentEnv_;
                currentEnv_ = initEnv;
                try {
                    evaluate(initMethod->body.get());
                } catch (const ReturnException&) {}
                instance = initEnv->get("this");
                // 同步 init 环境中的字段变量回 this 对象（直接查找局部变量，O(1)）
                const auto& initLocals = initEnv->localVariables();
                for (auto& fieldKV : instance.fields()) {
                    auto it = initLocals.find(fieldKV.first);
                    if (it != initLocals.end()) {
                        fieldKV.second = it->second;
                    }
                }
                currentEnv_ = prevEnv;
            }

            initVal = instance;
        }
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
    }

    // 类型检查：如果有类型注解且有初始化表达式
    if (!node.typeAnnotation.empty() && node.initializer) {
        checkType(initVal, node.typeAnnotation, [&]{ return "变量 " + node.name + " 的类型"; }, node.line, node.column);
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
    const std::string* typeAnn = findTypeAnnotation(node.name);
    if (typeAnn) {
        checkType(val, *typeAnn, [&]{ return std::string("赋值给 ") + node.name; }, node.line, node.column);
    }

    if (!currentEnv_->set(node.name, val)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return val;
}

Value Interpreter::visitVarRef(VarRef& node) {
    checkBreak(&node);

    // 优化：单次 get() 调用代替 hasVariable() + get() 双重作用域链遍历
    // get() 对未定义变量返回 null，而已定义变量的值不会是 null
    // （null 字面量绑定到变量时 get() 返回 null，isNull() 为 true，但此时变量是已定义的）
    Value val = currentEnv_->get(node.name);
    if (val.isNull() && !currentEnv_->hasVariable(node.name)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return val;
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
        // 每次迭代重新检查断点（MODE_RUN 下确保 while 行断点每次迭代都能命中；
        // STEP_IN/STEP_OVER 下 lastPausedLine_ 机制保证同行不重复暂停）
        checkBreak(&node);
        result = evaluate(node.body.get());
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
            // 每次迭代重新检查断点（同 visitWhileStmt 的修复原因）
            checkBreak(&node);

            // 条件检查
            if (node.condition) {
                Value cond = evaluate(node.condition.get());
                if (!cond.isTruthy()) break;
            }

            // 执行循环体
            try {
                result = evaluate(node.body.get());
            } catch (const ReturnException&) {
                // 恢复环境，传播 return
                currentEnv_ = forEnv->parent;
                throw;
            }

            // 更新
            if (node.update) {
                evaluate(node.update.get());
            }
        }
    } catch (const ReturnException&) {
        currentEnv_ = forEnv->parent;
        throw;
    } catch (...) {
        currentEnv_ = forEnv->parent;
        throw;
    }

    currentEnv_ = forEnv->parent;
    return result;
}

Value Interpreter::visitFunDecl(FunDecl& node) {
    checkBreak(&node);

    // 创建闭包值，捕获当前环境并存储函数体指针（自包含，不依赖 funRegistry_）
    Value funVal = Value::makeClosure(node.name, currentEnv_, node.params, &node);
    currentEnv_->define(node.name, funVal);

    // 保留 funRegistry_ 作为后备（处理 AST 生命周期问题）
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
        argValues.reserve(node.arguments.size());
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
        if (!initMethod && !argValues.empty()) {
            recursionDepth_--;
            runtimeError("类 " + cls.name + " 没有 init 方法，但传入了 " +
                         std::to_string(argValues.size()) + " 个参数",
                         node.line, node.column);
        }

        // 创建实例
        Value instance = Value::makeInstance(cls.name);

        // 复制类默认字段值（含继承链）
        ClassInfo* curCls = &cls;
        while (curCls) {
            for (const auto& kv : curCls->fields) {
                // 子类字段覆盖父类
                if (instance.fields().find(kv.first) == instance.fields().end()) {
                    instance.fields()[kv.first] = kv.second;
                }
            }
            if (!curCls->superClassName.empty()) {
                auto superIt = classRegistry_.find(curCls->superClassName);
                curCls = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
            } else {
                curCls = nullptr;
            }
        }

        // 如果有 init 方法，执行它
        if (initMethod) {
            // 创建新环境（父级为当前环境，支持闭包）
            auto initEnv = std::make_shared<Environment>(currentEnv_);

            // 绑定 this
            initEnv->define("this", instance);

            // 将实例字段注入 init 环境
            for (const auto& kv : instance.fields()) {
                initEnv->define(kv.first, kv.second);
            }

            // 绑定参数
            for (size_t i = 0; i < initMethod->params.size(); ++i) {
                initEnv->define(initMethod->params[i], std::move(argValues[i]));
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
            } catch (const ReturnException&) {
                // init 方法的返回值忽略，但更新实例字段
            } catch (...) {
                // 运行时错误：先恢复调用状态，再重抛，避免 currentEnv_/调用栈/递归深度错乱
                currentEnv_ = prevEnv;
                callStack_.pop_back();
                recursionDepth_--;
                currentFunctionReturnType_ = savedReturnType;
                throw;
            }

            // 从 init 环境中读取 this 的更新值
            instance = initEnv->get("this");

            // 同步 init 环境中的字段变量回 this 对象（直接查找局部变量，O(1)）
            const auto& initLocals2 = initEnv->localVariables();
            for (auto& fieldKV : instance.fields()) {
                auto it = initLocals2.find(fieldKV.first);
                if (it != initLocals2.end()) {
                    fieldKV.second = it->second;
                }
            }

            // 恢复环境
            currentEnv_ = prevEnv;
            callStack_.pop_back();
            currentFunctionReturnType_ = savedReturnType;
        }

        // 无论是否有 init 方法，都需成对恢复递归深度
        // （recursionDepth_ 在进入类构造路径时已无条件递增）
        recursionDepth_--;
        return instance;
    }

    // 检查环境中是否有闭包值
    std::shared_ptr<Environment> closureEnv;
    FunDecl* funDecl = nullptr;
    std::string effectiveName = node.name;  // 实际函数名（闭包时可能不同于调用变量名）

    // 快速路径：使用缓存的函数体（跳过环境查找和 funRegistry_ 查找）
    if (node.isResolved && node.resolvedDecl) {
        funDecl = static_cast<FunDecl*>(node.resolvedDecl);
        // 单次 get() 获取闭包环境（避免 hasVariable + get 双重遍历）
        Value callee = currentEnv_->get(node.name);
        if (callee.isClosure()) {
            closureEnv = callee.closureEnv();
            effectiveName = callee.closureName();
        }
    } else {
        // 慢路径：完整解析（单次 get() 调用）
        Value callee = currentEnv_->get(node.name);
        if (callee.isClosure()) {
            closureEnv = callee.closureEnv();
            effectiveName = callee.closureName();
            // 优先从闭包值中获取函数体（自包含，不依赖 funRegistry_）
            funDecl = callee.closureBody();
            if (!funDecl) {
                // 后备路径：从 funRegistry_ 查找（处理 AST 生命周期问题）
                auto it = funRegistry_.find(callee.closureName());
                if (it != funRegistry_.end()) {
                    funDecl = it->second;
                }
            }
        }

        // 如果闭包路径未找到函数体，走 funRegistry_ 后备
        if (!funDecl) {
            auto it = funRegistry_.find(effectiveName);
            if (it == funRegistry_.end()) {
                runtimeError("未定义的函数: " + node.name, node.line, node.column);
            }
            funDecl = it->second;
        }

        // 缓存解析结果供后续调用使用
        node.resolvedDecl = funDecl;
        node.isResolved = true;
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
    argValues.reserve(node.arguments.size());
    for (auto& arg : node.arguments) {
        argValues.push_back(evaluate(arg.get()));
    }

    // 参数类型检查
    for (size_t i = 0; i < funDecl->params.size() && i < funDecl->paramTypes.size(); ++i) {
        if (!funDecl->paramTypes[i].empty()) {
            checkType(argValues[i], funDecl->paramTypes[i],
                      [&]{ return "函数 " + node.name + " 的参数 " + funDecl->params[i]; },
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

    // 绑定参数（move 避免深拷贝）
    for (size_t i = 0; i < funDecl->params.size(); ++i) {
        funEnv->define(funDecl->params[i], std::move(argValues[i]));
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
        result = std::move(const_cast<ReturnException&>(e).returnValue);
    } catch (...) {
        // 运行时错误：先恢复调用状态，再重抛
        currentEnv_ = prevEnv;
        callStack_.pop_back();
        recursionDepth_--;
        currentFunctionReturnType_ = savedReturnType;
        throw;
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
        checkType(val, currentFunctionReturnType_, []{ return std::string("返回值"); }, node.line, node.column);
    }

    throw ReturnException(std::move(val));
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

    // 快速路径：空块直接返回
    if (node.statements.empty()) return Value::nullValue();

    // 为代码块创建新作用域（即使只有单条语句，也需创建作用域以防 var 声明泄漏到父作用域）
    auto blockEnv = std::make_shared<Environment>(currentEnv_);
    auto savedEnv = currentEnv_;
    currentEnv_ = blockEnv;

    Value result = Value::nullValue();
    try {
        for (auto& stmt : node.statements) {
            result = evaluate(stmt.get());
        }
    } catch (...) {
        currentEnv_ = savedEnv;
        throw;
    }

    currentEnv_ = savedEnv;

    return result;
}

// ---- 新增 9 个 visit 方法 ----

Value Interpreter::visitArrayLiteral(ArrayLiteral& node) {
    checkBreak(&node);

    std::vector<Value> elements;
    elements.reserve(node.elements.size());
    for (auto& elem : node.elements) {
        elements.push_back(evaluate(elem.get()));
    }
    return Value(std::move(elements));
}

Value Interpreter::visitDictLiteral(DictLiteral& node) {
    checkBreak(&node);

    std::unordered_map<std::string, Value> dict;
    dict.reserve(node.pairs.size());
    for (auto& pair : node.pairs) {
        Value key = evaluate(pair.first.get());
        Value val = evaluate(pair.second.get());
        // 字典的键必须是字符串
        dict[key.toString()] = std::move(val);
    }
    return Value(std::move(dict));
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
        int64_t i = idx.intVal();
        if (i < 0 || static_cast<size_t>(i) >= obj.arrayVal().size()) {
            runtimeError("数组索引越界: " + std::to_string(i), node.line, node.column);
        }
        return obj.arrayVal()[static_cast<size_t>(i)];
    }

    // 字典索引访问
    if (obj.isDict()) {
        if (!idx.isString()) {
            runtimeError("字典键必须是字符串", node.line, node.column);
        }
        auto it = obj.dictVal().find(idx.stringVal());
        if (it == obj.dictVal().end()) {
            return Value::nullValue();
        }
        return it->second;
    }

    runtimeError("该类型不支持索引访问", node.line, node.column);
}

Value Interpreter::visitIndexAssign(IndexAssign& node) {
    checkBreak(&node);
    // 左到右求值：object → index → value（由 writeBack 内部按序求值）
    return writeBack(node.object.get(), true, node.index.get(), "", node.value.get(), node.line, node.column);
}

Value Interpreter::visitClassDecl(ClassDecl& node) {
    checkBreak(&node);

    ClassInfo cls;
    cls.name = node.name;
    cls.superClassName = node.superClassName;
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
            registeredCls.fields[varDecl->name] = defaultVal;
            continue;
        }

        // FunDecl: 方法
        if (member->nodeType == NodeType::NODE_FUN_DECL) {
            FunDecl* funDecl = static_cast<FunDecl*>(member.get());
            registeredCls.methods[funDecl->name] = funDecl;
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
        auto it = obj.fields().find(node.fieldName);
        if (it != obj.fields().end()) {
            return it->second;
        }

        // 检查是否访问的是方法（返回一个标记值）
        auto classIt = classRegistry_.find(obj.className());
        if (classIt != classRegistry_.end()) {
            FunDecl* method = findMethod(classIt->second, node.fieldName);
            if (method) {
                // 方法作为字段访问，返回特殊标记
                Value methodVal(std::string("method:") + obj.className() + "." + node.fieldName);
                return methodVal;
            }
        }

        return Value::nullValue();
    }

    // 字典的成员访问（同索引访问）
    if (obj.isDict()) {
        auto it = obj.dictVal().find(node.fieldName);
        if (it != obj.dictVal().end()) {
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

Value Interpreter::visitMethodCall(MethodCall& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());

    // ---- 数组内置方法 ----
    if (obj.isArray()) {
        // 求值参数
        std::vector<Value> argValues;
        argValues.reserve(node.arguments.size());
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "push") {
            if (argValues.size() != 1)
                runtimeError("push 期望 1 个参数", node.line, node.column);
            obj.arrayVal().push_back(argValues[0]);
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        if (node.methodName == "pop") {
            if (obj.arrayVal().empty())
                runtimeError("对空数组调用 pop", node.line, node.column);
            Value last = obj.arrayVal().back();
            obj.arrayVal().pop_back();
            writeBack(node.object.get(), obj, node.line, node.column);
            return last;
        }
        if (node.methodName == "len") {
            return Value(static_cast<int64_t>(obj.arrayVal().size()));
        }
        if (node.methodName == "remove") {
            if (argValues.size() != 1)
                runtimeError("remove 期望 1 个参数(索引)", node.line, node.column);
            if (!argValues[0].isInt())
                runtimeError("remove 参数必须是整数索引", node.line, node.column);
            int64_t idx = argValues[0].intVal();
            if (idx < 0 || static_cast<size_t>(idx) >= obj.arrayVal().size())
                runtimeError("数组索引越界: " + std::to_string(idx), node.line, node.column);
            obj.arrayVal().erase(obj.arrayVal().begin() + static_cast<size_t>(idx));
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        if (node.methodName == "contains") {
            if (argValues.size() != 1)
                runtimeError("contains 期望 1 个参数", node.line, node.column);
            for (const auto& elem : obj.arrayVal()) {
                if (elem.equals(argValues[0])) return Value(true);
            }
            return Value(false);
        }
        if (node.methodName == "join") {
            std::string sep = argValues.empty() ? "" : argValues[0].toString();
            std::string result;
            const auto& arr = obj.arrayVal();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) result += sep;
                result += arr[i].toString();
            }
            return Value(std::move(result));
        }
        runtimeError("数组没有方法 " + node.methodName, node.line, node.column);
    }

    // ---- 字典内置方法 ----
    if (obj.isDict()) {
        std::vector<Value> argValues;
        argValues.reserve(node.arguments.size());
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "len") {
            return Value(static_cast<int64_t>(obj.dictVal().size()));
        }
        if (node.methodName == "keys") {
            std::vector<Value> keys;
            for (const auto& kv : obj.dictVal()) {
                keys.push_back(Value(kv.first));
            }
            return Value(std::move(keys));
        }
        if (node.methodName == "values") {
            std::vector<Value> vals;
            for (const auto& kv : obj.dictVal()) {
                vals.push_back(kv.second);
            }
            return Value(std::move(vals));
        }
        if (node.methodName == "has" || node.methodName == "contains") {
            if (argValues.size() != 1)
                runtimeError(node.methodName + " 期望 1 个参数(键)", node.line, node.column);
            return Value(obj.dictVal().find(argValues[0].toString()) != obj.dictVal().end());
        }
        if (node.methodName == "remove") {
            if (argValues.size() != 1)
                runtimeError("remove 期望 1 个参数(键)", node.line, node.column);
            obj.dictVal().erase(argValues[0].toString());
            writeBack(node.object.get(), obj, node.line, node.column);
            return Value::nullValue();
        }
        runtimeError("字典没有方法 " + node.methodName, node.line, node.column);
    }

    // ---- 字符串内置方法 ----
    if (obj.isString()) {
        std::vector<Value> argValues;
        argValues.reserve(node.arguments.size());
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }

        if (node.methodName == "len") {
            return Value(static_cast<int64_t>(obj.stringVal().size()));
        }
        if (node.methodName == "upper") {
            std::string s = obj.stringVal();
            for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
            return Value(std::move(s));
        }
        if (node.methodName == "lower") {
            std::string s = obj.stringVal();
            for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c));
            return Value(std::move(s));
        }
        if (node.methodName == "split") {
            // str.split(sep) — 按 sep 分割返回数组
            std::string sep = argValues.empty() ? " " : argValues[0].toString();
            if (sep.empty()) {
                // 空分隔符下 std::string::find("") 总返回 start，会导致死循环
                runtimeError("split 的分隔符不能为空字符串", node.line, node.column);
            }
            std::vector<Value> parts;
            size_t start = 0, pos;
            const std::string& str = obj.stringVal();
            while ((pos = str.find(sep, start)) != std::string::npos) {
                parts.push_back(Value(str.substr(start, pos - start)));
                start = pos + sep.size();
            }
            parts.push_back(Value(str.substr(start)));
            return Value(std::move(parts));
        }
        if (node.methodName == "trim") {
            std::string s = obj.stringVal();
            size_t l = s.find_first_not_of(" \t\r\n");
            size_t r = s.find_last_not_of(" \t\r\n");
            if (l == std::string::npos) return Value(std::string(""));
            return Value(s.substr(l, r - l + 1));
        }
        runtimeError("字符串没有方法 " + node.methodName, node.line, node.column);
    }

    // 类实例的方法调用
    if (obj.isInstance()) {
        auto classIt = classRegistry_.find(obj.className());
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
                argValues.reserve(node.arguments.size());
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
                for (const auto& kv : obj.fields()) {
                    methodEnv->define(kv.first, kv.second);
                }

                // 绑定参数（参数覆盖同名字段）
                for (size_t i = 0; i < method->params.size(); ++i) {
                    methodEnv->define(method->params[i], std::move(argValues[i]));
                }

                // 压入调用帧
                callStack_.emplace_back(obj.className() + "." + node.methodName,
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
                    result = std::move(const_cast<ReturnException&>(e).returnValue);
                } catch (...) {
                    // 运行时错误：先恢复调用状态，再重抛
                    currentEnv_ = prevEnv;
                    callStack_.pop_back();
                    recursionDepth_--;
                    currentFunctionReturnType_ = savedReturnType;
                    throw;
                }

                // 从方法环境中读取 this 的更新值
                Value updatedThis = methodEnv->get("this");

                // 关键：将方法环境中的字段变量同步回 this 对象（直接查找局部变量，O(1)）
                const auto& methodLocals = methodEnv->localVariables();
                for (auto& fieldKV : updatedThis.fields()) {
                    auto it = methodLocals.find(fieldKV.first);
                    if (it != methodLocals.end()) {
                        fieldKV.second = it->second;
                    }
                }

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

        runtimeError("类 " + obj.className() + " 没有方法 " + node.methodName,
                     node.line, node.column);
    }

    runtimeError("方法调用需要类实例", node.line, node.column);
}

Value Interpreter::visitNullLiteral(NullLiteral& node) {
    checkBreak(&node);
    return Value::nullValue();
}
