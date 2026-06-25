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
#include <cmath>  // BUG 8.1 fix: std::fmod
#include <sstream>
#include <unordered_set>

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
    currentFunctionReturnType_.clear();
    recursionDepth_ = 0;
    replAsts_.clear();  // 释放 REPL 保留的 AST
    diagnostics_.clear();  // 清空诊断信息
    // P0-1 fix: 清理模块缓存，避免 replAsts_.clear() 后缓存中的悬垂指针
    moduleCache_.clear();
    moduleExports_.clear();
    moduleLoadingStack_.clear();
    exportedNames_.clear();

    // 顶层块不创建新作用域，直接在全局环境中执行语句
    Value result = Value::nullValue();
    try {
        for (auto& stmt : program.statements) {
            result = evaluate(stmt.get());
        }
    }
    catch (const ReturnException&) {
        runtimeError("return 只能在函数体内使用", 0, 0);
    }
    catch (const BreakException&) {
        // BUG-I1 fix: 捕获泄漏的 BreakException，提供友好错误信息
        runtimeError("break 只能在循环体内使用", 0, 0);
    }
    catch (const ContinueException&) {
        // BUG-I1 fix: 捕获泄漏的 ContinueException，提供友好错误信息
        runtimeError("continue 只能在循环体内使用", 0, 0);
    }
    catch (const ThrowException& e) {
        runtimeError("未捕获的异常: " + e.thrownValue.toString(), 0, 0);
    }
    catch (const RuntimeError& e) {
        // 记录到诊断包后重新抛出，保持原有异常传播机制
        diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Interpreter);
        throw;
    }
    return result;
}

Value Interpreter::executeRepl(Block& program) {
    // 不重置环境，保留已有变量/函数/类定义
    // 清除 funRegistry_ 中的 AST 裸指针（旧 AST 可能已被销毁，闭包自带 body 指针不受影响）
    funRegistry_.clear();
    funRegistryGen_++;  // H5 fix: 使所有旧缓存的 resolvedDecl 指针失效，防止野指针访问
    // classRegistry_ 不清除 — 类定义需要跨 REPL 行保留（AST 由 replAsts_ 保持存活）
    // 确保当前环境回到全局
    currentEnv_ = globalEnv_;
    recursionDepth_ = 0;
    diagnostics_.clear();  // 清空诊断信息

    Value result = Value::nullValue();
    try {
        for (auto& stmt : program.statements) {
            result = evaluate(stmt.get());
        }
    }
    catch (const ReturnException&) {
        runtimeError("return 只能在函数体内使用", 0, 0);
    }
    catch (const BreakException&) {
        // BUG-I1 fix: 捕获泄漏的 BreakException
        runtimeError("break 只能在循环体内使用", 0, 0);
    }
    catch (const ContinueException&) {
        // BUG-I1 fix: 捕获泄漏的 ContinueException
        runtimeError("continue 只能在循环体内使用", 0, 0);
    }
    catch (const ThrowException& e) {
        runtimeError("未捕获的异常: " + e.thrownValue.toString(), 0, 0);
    }
    catch (const RuntimeError& e) {
        // 记录到诊断包后重新抛出，保持原有异常传播机制
        diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Interpreter);
        throw;
    }
    return result;
}

void Interpreter::retainReplAst(std::unique_ptr<Block> ast) {
    replAsts_.push_back(std::move(ast));
}

void Interpreter::saveReplState() {
    savedGlobalEnv_ = globalEnv_;
    savedClassRegistry_ = std::move(classRegistry_);
    savedReplAsts_ = std::move(replAsts_);
    // BUG7 fix: 保存函数注册表，避免 Run→REPL 切换后 funRegistry_ 丢失
    savedFunRegistry_ = std::move(funRegistry_);
    savedFunRegistryGen_ = funRegistryGen_;
    // P1-1 fix: 保存模块相关状态，避免 Run→REPL 切换后悬垂指针
    savedModuleCache_ = std::move(moduleCache_);
    savedModuleExports_ = std::move(moduleExports_);
    savedExportedNames_ = exportedNames_;
    savedModuleLoadingStack_ = moduleLoadingStack_;
}

void Interpreter::restoreReplState() {
    globalEnv_ = savedGlobalEnv_;
    currentEnv_ = globalEnv_;
    classRegistry_ = std::move(savedClassRegistry_);
    replAsts_ = std::move(savedReplAsts_);
    // BUG7 fix: 恢复函数注册表
    funRegistry_ = std::move(savedFunRegistry_);
    funRegistryGen_ = savedFunRegistryGen_;
    // P1-1 fix: 恢复模块相关状态
    moduleCache_ = std::move(savedModuleCache_);
    moduleExports_ = std::move(savedModuleExports_);
    exportedNames_ = std::move(savedExportedNames_);
    moduleLoadingStack_ = std::move(savedModuleLoadingStack_);
    savedGlobalEnv_.reset();
}

void Interpreter::setOutputCallback(std::function<void(const std::string&)> callback) {
    outputCallback_ = callback;
}

void Interpreter::setInputCallback(std::function<std::string(const std::string&)> callback) {
    inputCallback_ = callback;
}

void Interpreter::setModuleLoader(std::function<std::string(const std::string&)> loader) {
    moduleLoader_ = loader;
}

void Interpreter::setCurrentFilePath(const std::string& path) {
    currentFilePath_ = path;
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

// GUI-03 fix + DBG-A fix: 安全条件断点求值 — 保存/恢复所有可变状态（含 currentEnv_），防止重入损坏
Value Interpreter::evaluateCondition(ASTNode* node) {
    if (!node) return Value::nullValue();
    auto savedCallStack = callStack_;
    auto savedClassCtx = classContextStack_;
    auto savedEnv = currentEnv_;  // DBG-A fix: 保存环境指针，条件求值不修改程序状态
    int savedDepth = recursionDepth_;
    bool savedDebugMode = debugMode_;
    debugMode_ = false;
    try {
        Value result = node->accept(*this);
        callStack_ = std::move(savedCallStack);
        classContextStack_ = std::move(savedClassCtx);
        currentEnv_ = savedEnv;  // DBG-A fix: 恢复环境
        recursionDepth_ = savedDepth;
        debugMode_ = savedDebugMode;
        return result;
    }
    catch (...) {
        callStack_ = std::move(savedCallStack);
        classContextStack_ = std::move(savedClassCtx);
        currentEnv_ = savedEnv;  // DBG-A fix: 异常时也恢复环境
        recursionDepth_ = savedDepth;
        debugMode_ = savedDebugMode;
        throw;
    }
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
    Logger::Error(msg + " (行 " + std::to_string(line) + ", 列 " + std::to_string(col) + ")", "Interpreter");
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
        if (left.isInt() && right.isInt()) {
            int64_t a = left.intVal(), b = right.intVal();
            if (OverflowCheck::addOverflow(a, b))
                runtimeError("整数加法溢出", line, col);
            return Value(a + b);
        }
        return Value(left.toDouble() + right.toDouble());
    case BinOpType::BIN_SUB:
        if (left.isInt() && right.isInt()) {
            int64_t a = left.intVal(), b = right.intVal();
            if (OverflowCheck::subOverflow(a, b))
                runtimeError("整数减法溢出", line, col);
            return Value(a - b);
        }
        return Value(left.toDouble() - right.toDouble());
    case BinOpType::BIN_MUL:
        if (left.isInt() && right.isInt()) {
            int64_t a = left.intVal(), b = right.intVal();
            if (OverflowCheck::mulOverflow(a, b))
                runtimeError("整数乘法溢出", line, col);
            return Value(a * b);
        }
        return Value(left.toDouble() * right.toDouble());
    case BinOpType::BIN_DIV:
        if (left.isInt() && right.isInt()) {
            int64_t b = right.intVal();
            if (b == 0) runtimeError("除零错误", line, col);
            if (OverflowCheck::divOverflow(left.intVal(), b)) runtimeError("整数除法溢出", line, col);
            return Value(left.intVal() / b);  // int/int → int (截断除法)
        }
        {
            double r = right.toDouble();
            if (r == 0.0) runtimeError("除零错误", line, col);
            return Value(left.toDouble() / r);
        }
    case BinOpType::BIN_MOD:
        // BUG 8.1 fix: 浮点操作数应使用 fmod，而非截断为整数后取模
    {
        // 如果两个操作数都是 int，使用整数取模
        if (left.isInt() && right.isInt()) {
            int64_t a = left.intVal();
            int64_t b = right.intVal();
            if (b == 0) runtimeError("除零错误", line, col);
            if (OverflowCheck::modOverflow(a, b)) runtimeError("整数取模溢出", line, col);
            return Value(a % b);
        }
        // 至少一个 float → 使用 fmod 进行浮点取模
        if (!left.isNumber() || !right.isNumber()) {
            runtimeError("取模运算需要数值类型", line, col);
        }
        double a = left.toDouble();
        double b = right.toDouble();
        if (b == 0.0) runtimeError("除零错误", line, col);
        return Value(std::fmod(a, b));
    }
    default:
        runtimeError("不支持的算术运算符", line, col);
    }
    return Value::nullValue();  // 不可达，但消除编译器警告
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
    if (val.isInstance()) {
        // H1 fix: 遍历继承链，使子类实例可以匹配父类类型注解
        auto classIt = classRegistry_.find(val.className());
        int depth = 0;
        while (classIt != classRegistry_.end()) {
            if (++depth > MAX_INHERITANCE_DEPTH) break;
            if (classIt->second.name == annotation) return true;
            if (!classIt->second.superClassName.empty()) {
                classIt = classRegistry_.find(classIt->second.superClassName);
            }
            else {
                break;
            }
        }
    }
    // M7 fix: null 不再隐式匹配所有类型注解，仅匹配 "null" 类型
    if (val.isNull() && annotation == "null") return true;
    return false;
}

// checkType 已模板化移至 Interpreter.h（P20 fix）

const std::string* Interpreter::findTypeAnnotation(const std::string& varName) const {
    // B2 fix: 沿作用域链查找类型注解（不再使用 flat map）
    return currentEnv_->getTypeAnnotation(varName);
}

// ---- writeBack 辅助方法 ----

Interpreter::ChainInfo Interpreter::collectAndEvaluateChain(ASTNode* objectNode, bool errorOnNonVarRef, int line, int col) {
    ChainInfo info;
    ASTNode* cur = objectNode;
    while (cur->nodeType == NodeType::NODE_MEMBER_ACCESS ||
        cur->nodeType == NodeType::NODE_INDEX_ACCESS) {
        info.chain.push_back(cur);
        if (cur->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            cur = static_cast<MemberAccess*>(cur)->object.get();
        }
        else {
            cur = static_cast<IndexAccess*>(cur)->object.get();
        }
    }
    info.chain.push_back(cur); // 最外层 VarRef

    int n = static_cast<int>(info.chain.size());

    // 从外到内逐级求值，收集每级的值和索引
    info.vals.resize(n);
    info.idxs.resize(n);

    if (info.chain[n - 1]->nodeType != NodeType::NODE_VAR_REF) {
        if (errorOnNonVarRef) {
            runtimeError("赋值目标必须是变量引用", line, col);
        }
        info.varRef = nullptr;
        return info;
    }
    info.varRef = static_cast<VarRef*>(info.chain[n - 1]);
    const Value* baseVal = currentEnv_->get(info.varRef->name);
    if (!baseVal) {
        runtimeError("未定义的变量: " + info.varRef->name, line, col);
    }
    info.vals[n - 1] = *baseVal;  // #18 fix: 明确报错而非静默nullValue

    for (int i = n - 2; i >= 0; i--) {
        ASTNode* nd = info.chain[i];
        const Value& parent = info.vals[i + 1];
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parent.isInstance()) {
                auto it = parent.fields().find(ma->fieldName);
                info.vals[i] = (it != parent.fields().end()) ? it->second : Value::nullValue();
            }
            else if (parent.isDict()) {
                auto it = parent.dictVal().find(ma->fieldName);
                info.vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            }
            else {
                runtimeError("该类型不支持成员访问", line, col);
            }
        }
        else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(nd);
            info.idxs[i] = evaluate(ia->index.get());
            const Value& indexVal = info.idxs[i];
            if (parent.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parent.arrayVal().size())
                    runtimeError("数组索引越界: " + std::to_string(indexVal.intVal()) + ", 有效范围 [0, " + std::to_string(parent.arrayVal().size()) + ")", line, col);
                info.vals[i] = parent.arrayVal()[indexVal.intVal()];
            }
            else if (parent.isDict() && indexVal.isString()) {
                auto it = parent.dictVal().find(indexVal.stringVal());
                info.vals[i] = (it != parent.dictVal().end()) ? it->second : Value::nullValue();
            }
            else {
                runtimeError("该类型不支持索引访问", line, col);
            }
        }
    }

    return info;
}

void Interpreter::writeBackChain(ChainInfo& info, Value innermost, int line, int col) {
    int n = static_cast<int>(info.chain.size());
    Value currentVal = std::move(innermost);
    for (int i = 0; i < n - 1; i++) {
        ASTNode* nd = info.chain[i];
        Value parentVal = std::move(info.vals[i + 1]);  // #19: move避免深拷贝
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parentVal.isInstance()) {
                // S1 fix: 优先使用 tryGetMutableFields 跳过 COW 深拷贝
                if (auto* fields = parentVal.tryGetMutableFields()) {
                    (*fields)[ma->fieldName] = currentVal;
                } else {
                    parentVal.fields()[ma->fieldName] = currentVal;
                }
            }
            else if (parentVal.isDict()) {
                // S1 fix: 优先使用 tryGetMutableDict 跳过 COW 深拷贝
                if (auto* entries = parentVal.tryGetMutableDict()) {
                    (*entries)[ma->fieldName] = currentVal;
                } else {
                    parentVal.dictVal()[ma->fieldName] = currentVal;
                }
            }
            else {
                runtimeError("该类型不支持成员赋值", line, col);
            }
        }
        else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            const Value& indexVal = info.idxs[i];
            if (parentVal.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parentVal.arrayVal().size())
                    runtimeError("数组索引越界: " + std::to_string(indexVal.intVal()) + ", 有效范围 [0, " + std::to_string(parentVal.arrayVal().size()) + ")", line, col);
                // S1 fix: 优先使用 tryGetMutableArray 跳过 COW 深拷贝
                if (auto* arr = parentVal.tryGetMutableArray()) {
                    (*arr)[indexVal.intVal()] = currentVal;
                } else {
                    parentVal.arrayVal()[indexVal.intVal()] = currentVal;
                }
            }
            else if (parentVal.isDict() && indexVal.isString()) {
                // S1 fix: 优先使用 tryGetMutableDict 跳过 COW 深拷贝
                if (auto* entries = parentVal.tryGetMutableDict()) {
                    (*entries)[indexVal.stringVal()] = currentVal;
                } else {
                    parentVal.dictVal()[indexVal.stringVal()] = currentVal;
                }
            }
            else {
                runtimeError("该类型不支持索引赋值", line, col);
            }
        }
        currentVal = std::move(parentVal);  // #19: move
    }

    currentEnv_->set(info.varRef->name, std::move(currentVal));  // #19: move
}

// ---- writeBack 写回左值 ----
// 链式求值方式：从最外层到最内层逐级求值（每级仅一次），在最内层执行赋值，
// 再从内到外逐级写回，避免对含副作用的子表达式重复求值。

Value Interpreter::writeBack(ASTNode* objectNode, bool isIndexAssign, ASTNode* indexNode,
    const std::string& fieldName, ASTNode* valueNode, int line, int col) {
    ChainInfo info = collectAndEvaluateChain(objectNode, true, line, col);

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
    Value modifiedObj = std::move(info.vals[0]); // A2: move 而非拷贝，保持 refcount=1 跳过 COW detach
    if (isIndexAssign) {
        if (modifiedObj.isArray() && idx.isInt()) {
            if (idx.intVal() < 0 || static_cast<size_t>(idx.intVal()) >= modifiedObj.arrayVal().size())
                runtimeError("数组索引越界: " + std::to_string(idx.intVal()) + ", 有效范围 [0, " + std::to_string(modifiedObj.arrayVal().size()) + ")", line, col);
            modifiedObj.arrayVal()[idx.intVal()] = val;
        }
        else if (modifiedObj.isDict() && idx.isString()) {
            modifiedObj.dictVal()[idx.stringVal()] = val;
        }
        else {
            runtimeError("该类型不支持索引赋值", line, col);
        }
    }
    else {
        if (modifiedObj.isInstance()) {
            modifiedObj.fields()[fieldName] = val;
        }
        else if (modifiedObj.isDict()) {
            modifiedObj.dictVal()[fieldName] = val;
        }
        else {
            runtimeError("该类型不支持成员赋值", line, col);
        }
    }

    writeBackChain(info, std::move(modifiedObj), line, col);
    return val;
}

// ---- writeBack 重载：写回已修改的值 ----
// 用于方法调用等已自行修改对象的场景，链式求值避免重复求值副作用

void Interpreter::writeBack(ASTNode* objectNode, const Value& modifiedValue, int line, int col) {
    ChainInfo info = collectAndEvaluateChain(objectNode, false, line, col);
    if (!info.varRef) {
        // O2 fix: 临时值（如 Foo(1).setX(99)）方法正常执行但修改不写回
        return;
    }
    writeBackChain(info, modifiedValue, line, col);  // const ref, 不能move
}

// ---- 16 个原有 visit 方法 ----

Value Interpreter::visitBinaryOp(BinaryOp& node) {
    checkBreak(&node);

    // 使用预计算的枚举类型进行快速分发（避免运行时字符串比较）
    switch (node.opType) {
    case BinOpType::BIN_AND: {
        Value left = evaluate(node.left.get());
        if (!left.isTruthy()) return left;   // M1 fix: 返回原始左值而非 Value(false)
        return evaluate(node.right.get());   // M1 fix: 返回原始右值而非 Value(right.isTruthy())
    }
    case BinOpType::BIN_OR: {
        Value left = evaluate(node.left.get());
        if (left.isTruthy()) return left;    // M1 fix: 返回原始左值而非 Value(true)
        return evaluate(node.right.get());   // M1 fix: 返回原始右值而非 Value(right.isTruthy())
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
        // M4 fix: 支持字符串字典序比较
        if (left.isString() && right.isString())
            return Value(left.stringVal() < right.stringVal());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值或字符串类型", node.line, node.column);
        return Value(left.toDouble() < right.toDouble());
    }
    case BinOpType::BIN_GT: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (left.isString() && right.isString())
            return Value(left.stringVal() > right.stringVal());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值或字符串类型", node.line, node.column);
        return Value(left.toDouble() > right.toDouble());
    }
    case BinOpType::BIN_LTE: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (left.isString() && right.isString())
            return Value(left.stringVal() <= right.stringVal());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值或字符串类型", node.line, node.column);
        return Value(left.toDouble() <= right.toDouble());
    }
    case BinOpType::BIN_GTE: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        if (left.isString() && right.isString())
            return Value(left.stringVal() >= right.stringVal());
        if (!left.isNumber() || !right.isNumber())
            runtimeError("比较运算需要数值或字符串类型", node.line, node.column);
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
        runtimeError("未知运算符: " + std::string(BinaryOp::opTypeStr(node.opType)), node.line, node.column);
        return Value::nullValue();
    }
}

Value Interpreter::visitUnaryOp(UnaryOp& node) {
    checkBreak(&node);

    Value operand = evaluate(node.operand.get());

    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        if (operand.isInt()) {
            if (OverflowCheck::negateOverflow(operand.intVal())) {
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
    case UnaryOp::UnaryOpType::UOP_PLUS:
        return operand;  // 一元 + 恒等操作
    default:
        runtimeError("未知一元运算符: " + std::string(UnaryOp::opTypeStr(node.opType)), node.line, node.column);
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
    return node.takeValue();
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
    }
    else if (!node.typeAnnotation.empty()) {
        // 无初始化表达式但有类型注解 — 检查是否是类名
        auto classIt = classRegistry_.find(node.typeAnnotation);
        if (classIt != classRegistry_.end()) {
            // 自动创建类实例（无参构造）
            ClassInfo& cls = classIt->second;
            Value instance = Value::makeInstance(cls.name);

            // 复制类默认字段值（含继承链）— B8 fix: 加入循环检测
            ClassInfo* curCls = &cls;
            std::unordered_set<std::string> visitedClasses;
            while (curCls) {
                if (!visitedClasses.insert(curCls->name).second) break; // 检测到循环继承
                for (const auto& kv : curCls->fields) {
                    if (instance.fields().find(kv.first) == instance.fields().end()) {
                        instance.fields()[kv.first] = kv.second;
                    }
                }
                if (!curCls->superClassName.empty()) {
                    auto superIt = classRegistry_.find(curCls->superClassName);
                    curCls = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
                }
                else {
                    curCls = nullptr;
                }
            }

            // 如果有 init 方法（0 必需参数，含全默认参数），执行它
            FunDecl* initMethod = findMethod(cls, "init");
            // P1-2 fix: 使用 requiredParamCount==0 判断，支持全默认参数 init
            if (initMethod && initMethod->requiredParamCount == 0) {
                auto parentEnv = cls.closureEnv ? cls.closureEnv : currentEnv_;
                auto initEnv = std::make_shared<Environment>(parentEnv);
                initEnv->define("this", instance);
                auto prevEnv = currentEnv_;
                currentEnv_ = initEnv;

                // H-新2 fix: bindInstance 在 define 之后（此路径 0 参数无 rehash 风险，但保持一致性）
                Value* thisInEnv = const_cast<Value*>(initEnv->get("this"));
                if (thisInEnv) initEnv->bindInstance(thisInEnv);

                // H3 fix: 与 visitFunCall 类构造路径保持一致的状态管理
                std::string savedReturnType = currentFunctionReturnType_;
                currentFunctionReturnType_ = initMethod->returnType;
                callStack_.emplace_back(cls.name + ".init", initEnv, node.line, recursionDepth_ + 1);
                classContextStack_.push_back(cls.name);

                // #8 fix: recursionDepth_ guard for auto-construction
                // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
                if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
                    currentEnv_ = prevEnv;
                    callStack_.pop_back();
                    if (!classContextStack_.empty()) classContextStack_.pop_back();
                    currentFunctionReturnType_ = savedReturnType;
                    runtimeError("递归深度超过限制 (" + std::to_string(MAX_RECURSION_DEPTH) + ")", node.line, node.column);
                }
                RecursionGuard guard{ recursionDepth_ };
                try {
                    evaluate(initMethod->body.get());
                }
                catch (const ReturnException&) {
                }
                catch (...) {
                    currentEnv_ = prevEnv;
                    callStack_.pop_back();
                    if (!classContextStack_.empty()) classContextStack_.pop_back();
                    currentFunctionReturnType_ = savedReturnType;
                    throw;
                }

                // 恢复状态
                currentEnv_ = prevEnv;
                callStack_.pop_back();
                if (!classContextStack_.empty()) classContextStack_.pop_back();
                currentFunctionReturnType_ = savedReturnType;

                auto* thisPtr = initEnv->get("this");
                if (thisPtr) {
                    instance = *thisPtr;
                }
                // M3 fix: 移除冗余的局部变量→字段同步。P5 bindInstance 使 init 中的字段赋值
                // 直接写入 instance.fields()，此处同步是多余的且会因同名局部变量覆盖字段值。
            }

            initVal = instance;
        }
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
    }

    // 类型检查：如果有类型注解且有初始化表达式
    if (!node.typeAnnotation.empty() && node.initializer) {
        checkType(initVal, node.typeAnnotation, [&] { return "变量 " + node.name + " 的类型"; }, node.line, node.column);
    }
    // 记录类型注解（B2 fix: 存入当前作用域环境）
    if (!node.typeAnnotation.empty()) {
        currentEnv_->defineTypeAnnotation(node.name, node.typeAnnotation);
    }

    // P21 fix: 原子性检查+插入，消除 find+define 双次查找
    if (!currentEnv_->tryDefineNew(node.name, initVal)) {
        runtimeError("变量 '" + node.name + "' 已在当前作用域中定义", node.line, node.column);
    }

    return initVal;
}

Value Interpreter::visitAssignment(Assignment& node) {
    checkBreak(&node);

    Value val = evaluate(node.value.get());

    // 类型检查
    const std::string* typeAnn = findTypeAnnotation(node.name);
    if (typeAnn) {
        checkType(val, *typeAnn, [&] { return std::string("赋值给 ") + node.name; }, node.line, node.column);
    }

    if (!currentEnv_->set(node.name, val)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return val;
}

Value Interpreter::visitVarRef(VarRef& node) {
    checkBreak(&node);

    // C7: get() 返回指针，nullptr 表示变量未定义，消除 hasVariable() 双重遍历
    const Value* val = currentEnv_->get(node.name);
    if (!val) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    return *val;
}

Value Interpreter::visitIfStmt(IfStmt& node) {
    checkBreak(&node);

    Value cond = evaluate(node.condition.get());
    if (cond.isTruthy()) {
        return evaluate(node.thenBranch.get());
    }
    else if (node.elseBranch) {
        return evaluate(node.elseBranch.get());
    }
    return Value::nullValue();
}

Value Interpreter::visitWhileStmt(WhileStmt& node) {
    checkBreak(&node);

    Value result = Value::nullValue();
    int64_t iterationCount = 0;  // S-01 fix: 循环迭代计数
    while (evaluate(node.condition.get()).isTruthy()) {
        // S-01 fix: 防止无限循环导致 DoS
        if (++iterationCount > MAX_LOOP_ITERATIONS) {
            runtimeError("循环迭代次数超过上限 " + std::to_string(MAX_LOOP_ITERATIONS) +
                         "，疑似无限循环", node.line, node.column);
        }
        // 每次迭代重新检查断点（MODE_RUN 下确保 while 行断点每次迭代都能命中；
        // STEP_IN/STEP_OVER 下 lastPausedLine_ 机制保证同行不重复暂停）
        checkBreak(&node);
        try {
            result = evaluate(node.body.get());
        }
        catch (const BreakException&) {
            // break 跳出循环
            break;
        }
        catch (const ContinueException&) {
            // continue 跳到下一次条件检查
            continue;
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
        try {
            evaluate(node.initializer.get());
        }
        catch (...) {
            // M2 fix: 初始化器异常时恢复外层环境，再传播异常
            currentEnv_ = forEnv->parent;
            throw;
        }
    }

    Value result = Value::nullValue();
    int64_t iterationCount = 0;  // S-01 fix: 循环迭代计数
    try {
        while (true) {
            // S-01 fix: 防止无限循环导致 DoS
            if (++iterationCount > MAX_LOOP_ITERATIONS) {
                runtimeError("循环迭代次数超过上限 " + std::to_string(MAX_LOOP_ITERATIONS) +
                             "，疑似无限循环", node.line, node.column);
            }
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
            }
            catch (const BreakException&) {
                // break 跳出循环
                break;
            }
            catch (const ContinueException&) {
                // continue 跳到更新步骤
            }
            catch (const ReturnException&) {
                // 恢复环境，传播 return
                currentEnv_ = forEnv->parent;
                throw;
            }

            // 更新
            if (node.update) {
                evaluate(node.update.get());
            }
        }
    }
    catch (...) {
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

    // C1 fix: 快照捕获当前所有可见变量，作为 weak_ptr 过期后的回退环境
    funVal.capturedVars() = currentEnv_->allVariablesMap();

    currentEnv_->define(node.name, funVal);

    // 保留 funRegistry_ 作为后备（处理 AST 生命周期问题）
    funRegistry_[node.name] = &node;
    funRegistryGen_++;  // M7: 函数注册/重定义时递增代数，使旧缓存失效

    return funVal;
}

Value Interpreter::visitReturnStmt(ReturnStmt& node) {
    checkBreak(&node);

    Value val = Value::nullValue();
    if (node.value) {
        val = evaluate(node.value.get());
    }

    // 返回类型检查
    if (!currentFunctionReturnType_.empty()) {
        checkType(val, currentFunctionReturnType_, [] { return std::string("返回值"); }, node.line, node.column);
    }

    throw ReturnException(std::move(val));
}

Value Interpreter::visitBreakStmt(BreakStmt& node) {
    checkBreak(&node);
    throw BreakException();
}

Value Interpreter::visitContinueStmt(ContinueStmt& node) {
    checkBreak(&node);
    throw ContinueException();
}

Value Interpreter::visitThrowStmt(ThrowStmt& node) {
    checkBreak(&node);
    Value val = evaluate(node.expression.get());
    throw ThrowException(std::move(val));
}

Value Interpreter::visitTryStmt(TryStmt& node) {
    checkBreak(&node);
    try {
        if (node.tryBlock) {
            evaluate(node.tryBlock.get());
        }
    } catch (ThrowException& e) {
        // 在 catch 块的新作用域中绑定异常变量
        auto catchEnv = std::make_shared<Environment>(currentEnv_);
        auto savedEnv = currentEnv_;
        currentEnv_ = catchEnv;
        // P2-1 fix: 使用 std::move 避免不必要的 Value 拷贝
        currentEnv_->define(node.catchVarName, std::move(e.thrownValue));
        try {
            if (node.catchBlock) {
                evaluate(node.catchBlock.get());
            }
        } catch (...) {
            currentEnv_ = savedEnv;
            throw;  // 重新抛出 break/continue/return/throw
        }
        currentEnv_ = savedEnv;
    }
    return Value::nullValue();
}

Value Interpreter::visitPrintStmt(PrintStmt& node) {
    checkBreak(&node);

    std::string result;
    // P-08 fix: 预估结果字符串容量
    result.reserve(node.values.size() * 16);
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
    }
    catch (...) {
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

    // P0-3 fix: 使用 const 引用避免在只读访问时触发 COW 深拷贝
    const Value& objC = obj;

    // 数组索引访问
    if (objC.isArray()) {
        if (!idx.isInt()) {
            runtimeError("数组索引必须是整数", node.line, node.column);
        }
        int64_t i = idx.intVal();
        const auto& arr = objC.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            runtimeError("数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(arr.size()) + ")", node.line, node.column);
        }
        return arr[static_cast<size_t>(i)];
    }

    // 字典索引访问
    if (objC.isDict()) {
        if (!idx.isString()) {
            runtimeError("字典键必须是字符串", node.line, node.column);
        }
        const auto& dict = objC.dictVal();
        auto it = dict.find(idx.stringVal());
        if (it == dict.end()) {
            return Value::nullValue();
        }
        return it->second;
    }

    // 字符串索引访问：返回单字符字符串（M6 fix: 基于 UTF-8 码位而非字节）
    if (objC.isString()) {
        if (!idx.isInt()) {
            runtimeError("字符串索引必须是整数", node.line, node.column);
        }
        int64_t i = idx.intVal();
        const std::string& s = objC.stringVal();
        // 计算 UTF-8 字符数
        size_t charCount = 0;
        size_t bytePos = 0;
        size_t targetBytePos = 0;
        size_t targetByteLen = 0;
        bool found = false;
        while (bytePos < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[bytePos]);
            size_t charLen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            if (static_cast<size_t>(i) == charCount) {
                targetBytePos = bytePos;
                targetByteLen = charLen;
                found = true;
            }
            bytePos += charLen;
            charCount++;
        }
        if (i < 0 || !found) {
            runtimeError("字符串索引越界: " + std::to_string(i) + ", 有效范围 [0, "
                + std::to_string(charCount) + ")", node.line, node.column);
        }
        return Value(s.substr(targetBytePos, targetByteLen));
    }

    runtimeError("该类型不支持索引访问", node.line, node.column);
}

Value Interpreter::visitIndexAssign(IndexAssign& node) {
    checkBreak(&node);
    // 左到右求值：object → index → value（由 writeBack 内部按序求值）
    return writeBack(node.object.get(), true, node.index.get(), "", node.value.get(), node.line, node.column);
}

Value Interpreter::visitMethodCall(MethodCall& node) {
    checkBreak(&node);

    // P0-4 fix: 通过 collectAndEvaluateChain 一次性求值对象链，
    // 避免对含副作用的子表达式（如 arr[sideEffect()].push(1)）重复求值
    ChainInfo info = collectAndEvaluateChain(node.object.get(), false, node.line, node.column);
    Value obj;
    if (info.varRef) {
        obj = std::move(info.vals[0]);
    } else {
        obj = evaluate(node.object.get());
    }

    // ---- 数组内置方法 ----
    if (obj.isArray()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = BuiltinMethods::handleArrayMethod(
            node.methodName, obj, argValues, node.line, node.column);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (builtinResult.objectModified && info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        return builtinResult.result;
    }

    // ---- 字典内置方法 ----
    if (obj.isDict()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = BuiltinMethods::handleDictMethod(
            node.methodName, obj, argValues, node.line, node.column);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (builtinResult.objectModified && info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        return builtinResult.result;
    }

    // ---- 字符串内置方法 ----
    if (obj.isString()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = BuiltinMethods::handleStringMethod(
            node.methodName, obj, argValues, node.line, node.column);
        return builtinResult.result;
    }

    // 类实例的方法调用
    if (obj.isInstance()) {
        Value result = callInstanceMethod(node, obj);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        return result;
    }

    runtimeError("类型 " + obj.typeName() + " 不支持方法调用", node.line, node.column);
}

// ---- P1 重构：类实例方法调用（含 super.method() 处理）----
Value Interpreter::callInstanceMethod(MethodCall& node, Value& obj) {
    auto classIt = classRegistry_.find(obj.className());
    if (classIt != classRegistry_.end()) {
        // O1: super.method() — 从父类开始查找方法
        bool isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
        ClassInfo* searchClass = &classIt->second;
        if (isSuperCall) {
            // 使用 classContextStack_ 确定当前执行类的父类（修复多层 super.init() 递归）
            std::string currentClassName;
            if (!classContextStack_.empty()) {
                currentClassName = classContextStack_.back();
            }
            else {
                currentClassName = obj.className();
            }
            auto ctxIt = classRegistry_.find(currentClassName);
            if (ctxIt == classRegistry_.end() || ctxIt->second.superClassName.empty()) {
                runtimeError("类 " + currentClassName + " 没有父类，不能使用 super", node.line, node.column);
            }
            auto superIt = classRegistry_.find(ctxIt->second.superClassName);
            if (superIt == classRegistry_.end()) {
                runtimeError("未定义的父类: " + ctxIt->second.superClassName, node.line, node.column);
            }
            searchClass = &superIt->second;
        }
        FunDecl* method = findMethod(*searchClass, node.methodName);
        if (method) {
            // 求值参数
            std::vector<Value> argValues;
            // F10: 支持默认参数
            size_t argCount = node.arguments.size();
            if (argCount < static_cast<size_t>(method->requiredParamCount) ||
                argCount > method->params.size()) {
                runtimeError("方法 " + node.methodName + " 期望 " +
                    std::to_string(method->requiredParamCount) + "-" +
                    std::to_string(method->params.size()) + " 个参数，但传入了 " +
                    std::to_string(argCount) + " 个",
                    node.line, node.column);
            }
            argValues.reserve(argCount);

            // #2 fix: 缓存closureEnv
            // H4 fix: 对 super.method() 调用，使用方法实际定义所在类（searchClass）的环境，
            // 而非实例所属类（classIt）的环境，确保继承链中跨作用域的变量绑定正确
            auto cachedParentEnv = searchClass->closureEnv;
            for (auto& arg : node.arguments) {
                argValues.push_back(evaluate(arg.get()));
            }

            // F10: 为缺失的参数填充默认值（在类定义的闭包环境中求值）
            if (argCount < method->params.size()) {
                auto savedEnv = currentEnv_;
                if (cachedParentEnv) {
                    currentEnv_ = cachedParentEnv;
                }
                for (size_t i = argCount; i < method->params.size(); ++i) {
                    if (method->defaultValues[i]) {
                        argValues.push_back(evaluate(method->defaultValues[i].get()));
                    } else {
                        argValues.push_back(Value::nullValue());
                    }
                }
                currentEnv_ = savedEnv;
            }

            // 保存调用状态（在try外）
            std::string savedReturnType = currentFunctionReturnType_;
            currentFunctionReturnType_ = method->returnType;
            auto prevEnv = currentEnv_;

            std::shared_ptr<Environment> methodEnv;  // 声明在try外，使catch后可访问
            Value result = Value::nullValue();

            // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
            if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
                currentFunctionReturnType_ = savedReturnType;
                runtimeError("递归深度超过限制 (" + std::to_string(MAX_RECURSION_DEPTH) + ")", node.line, node.column);
            }
            RecursionGuard recursionGuard{ recursionDepth_ };

            try {
                // O5: 使用类定义时捕获的环境作为父级（闭包），而非调用者的环境
                auto parentEnv = cachedParentEnv ? cachedParentEnv : currentEnv_;  // #2 fix: 使用缓存值
                methodEnv = std::make_shared<Environment>(parentEnv);

                // 绑定 this
                methodEnv->define("this", obj);

                // 绑定参数（参数覆盖同名字段）
                for (size_t i = 0; i < method->params.size(); ++i) {
                    // P1-4 fix: 补充参数类型检查（与 callNamedFunction 一致）
                    if (i < method->paramTypes.size() && !method->paramTypes[i].empty()) {
                        checkType(argValues[i], method->paramTypes[i],
                            [&] { return "方法 " + node.methodName + " 的参数 " + method->params[i]; },
                            node.line, node.column);
                    }
                    methodEnv->define(method->params[i], std::move(argValues[i]));
                }

                // H-新2 fix: bindInstance 必须在所有 define 之后，避免 map rehash 使指针悬空
                Value* thisInEnv = const_cast<Value*>(methodEnv->get("this"));
                if (thisInEnv) methodEnv->bindInstance(thisInEnv);

                // 压入调用栈
                callStack_.emplace_back(obj.className() + "." + node.methodName,
                    methodEnv, node.line, recursionDepth_);

                // 切换环境
                currentEnv_ = methodEnv;

                // 压入类上下文（super 解析用）
                classContextStack_.push_back(searchClass->name);

                result = evaluate(method->body.get());
            }
            catch (ReturnException& e) {
                result = std::move(e.returnValue);
            }
            catch (...) {
                // 运行时错误：先恢复调用状态，再重抛
                // S2 fix: recursionDepth_ 由 RecursionGuard 自动恢复
                currentEnv_ = prevEnv;
                if (!callStack_.empty()) callStack_.pop_back();
                if (!classContextStack_.empty()) classContextStack_.pop_back();
                currentFunctionReturnType_ = savedReturnType;
                throw;
            }

            // 从方法环境中读取 this 的更新值
            auto* thisPtr = methodEnv->get("this");
            Value updatedThis = thisPtr ? *thisPtr : Value::nullValue();

            // 恢复环境
            // S2 fix: recursionDepth_ 由 RecursionGuard 自动恢复
            currentEnv_ = prevEnv;
            callStack_.pop_back();
            if (!classContextStack_.empty()) classContextStack_.pop_back();
            currentFunctionReturnType_ = savedReturnType;

            // P0-4 fix: 不再调用 writeBack（会重复求值对象链），
            // 而是将更新后的 this 写回 obj（按引用传递），由 visitMethodCall 统一通过 writeBackChain 写回
            obj = std::move(updatedThis);

            // super.method() 调用后，需将更新后的 this 写回调用者的环境
            // （writeBackChain 无法处理 SuperExpr，因为它不是 VarRef）
            // P2-6 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
            if (isSuperCall) {
                currentEnv_->set("this", std::move(obj));
            }

            return result;
        }
    }

    runtimeError("类 " + obj.className() + " 没有方法 " + node.methodName,
        node.line, node.column);
}

Value Interpreter::visitNullLiteral(NullLiteral& node) {
    checkBreak(&node);
    return Value::nullValue();
}
