#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: VM 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/RuntimeLimits.h"   // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"             // R109 TCO: 尾递归自调用识别
#include "compiler/BytecodeCache.h" // P2-11: .minic 文件加载
#include "compiler/Compiler.h"
#include "compiler/ExprStmtPop.h"             // AUDIT-R4 BUG-04: 表达式语句 POP 共享谓词
#include "compiler/IRSSA.h"                   // P2-10: gvnPass/licmPass/inlinePass
#include "compiler/RegisterBytecodeBackend.h" // PERF-14: 寄存器式后端
#include "interpreter/NumericUtils.h"         // 共享溢出检查（B6 fix）
#include "lexer/Lexer.h"                      // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h"                    // VM-IMPORT: 模块源码语法分析
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <sstream>

// === CompilerExpr: expression visitors (split from Compiler.cpp) ===

// PERF: 判断表达式是否可证明为整数类型（保守策略）
// 规则：
//   1. NumberLiteral 且 isInt() == true
//   2. VarRef 且类型注解为 "int"
//   3. UnaryOp(NEGATE) 且操作数为已知 int
//   4. BinaryOp(+,-,*,%) 且两个操作数均为已知 int
// 其他情况均返回 false（回退到通用操作码）
bool Compiler::isExprKnownInt(const ASTNode* expr) const {
    if (!expr)
        return false;
    switch (expr->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL: {
        const auto* num = static_cast<const NumberLiteral*>(expr);
        return num->isInt();
    }
    case NodeType::NODE_VAR_REF: {
        const auto* ref = static_cast<const VarRef*>(expr);
        const std::string* type = findVarType(ref->name);
        return type && *type == "int";
    }
    case NodeType::NODE_UNARY_OP: {
        const auto* unary = static_cast<const UnaryOp*>(expr);
        if (unary->opType == UnaryOp::UnaryOpType::UOP_NEGATE)
            return isExprKnownInt(unary->operand.get());
        return false;
    }
    case NodeType::NODE_BINARY_OP: {
        const auto* bin = static_cast<const BinaryOp*>(expr);
        // 仅对产生 int 结果的运算符递归检查（除法可能产生 float，比较产生 bool）
        if (bin->opType == BinOpType::BIN_ADD || bin->opType == BinOpType::BIN_SUB ||
            bin->opType == BinOpType::BIN_MUL || bin->opType == BinOpType::BIN_MOD) {
            return isExprKnownInt(bin->left.get()) && isExprKnownInt(bin->right.get());
        }
        return false;
    }
    default:
        return false;
    }
}

void Compiler::visitBinaryOp(BinaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldBinary(node.opType, node.left.get(), node.right.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
    }

    // 短路运算特殊处理
    if (node.opType == BinOpType::BIN_AND) {
        compileNode(node.left.get());
        // 如果左操作数为假，跳过右操作数
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);           // 占位
        chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出左操作数
        compileNode(node.right.get());
        // 修补跳转地址
        uint16_t jumpTarget = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(jumpTarget & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((jumpTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return;
    }

    if (node.opType == BinOpType::BIN_OR) {
        compileNode(node.left.get());
        // 如果左操作数为真，跳过右操作数，保留左值
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line); // 占位（先跳到 or 右侧）
        // 如果到这里，左操作数为真 — 保留左值，跳到末尾
        size_t jumpEnd = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line); // 占位
        // 修补第一个跳转：左操作数为假，求值右操作数
        uint16_t rightStart = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 弹出左操作数，求值右操作数
        chunk_.writeOp(OpCode::OP_POP, node.line);
        compileNode(node.right.get());
        // 修补第二个跳转
        uint16_t endTarget = safeCodeOffset();
        chunk_.code[jumpEnd + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[jumpEnd + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return;
    }

    // 普通二元运算
    compileNode(node.left.get());
    compileNode(node.right.get());

    // PERF: 类型特化操作码检测——当两个操作数均可证明为 int 时，发射无类型检查的特化操作码
    // 保守策略：仅当 100% 确定两侧均为 int 时才使用特化版本
    bool bothInt = isExprKnownInt(node.left.get()) && isExprKnownInt(node.right.get());

    OpCode op = OpCode::OP_ADD;
    switch (node.opType) {
    case BinOpType::BIN_ADD:
        op = bothInt ? OpCode::OP_ADD_INT_SPEC : OpCode::OP_ADD;
        break;
    case BinOpType::BIN_SUB:
        op = bothInt ? OpCode::OP_SUB_INT_SPEC : OpCode::OP_SUBTRACT;
        break;
    case BinOpType::BIN_MUL:
        op = bothInt ? OpCode::OP_MUL_INT_SPEC : OpCode::OP_MULTIPLY;
        break;
    case BinOpType::BIN_DIV:
        op = OpCode::OP_DIVIDE;
        break;
    case BinOpType::BIN_MOD:
        op = OpCode::OP_MODULO;
        break;
    case BinOpType::BIN_EQ:
        op = OpCode::OP_EQUAL;
        break;
    case BinOpType::BIN_NEQ:
        op = OpCode::OP_NOT_EQUAL;
        break;
    case BinOpType::BIN_LT:
        op = bothInt ? OpCode::OP_LT_INT_SPEC : OpCode::OP_LESS;
        break;
    case BinOpType::BIN_GT:
        op = OpCode::OP_GREATER;
        break;
    case BinOpType::BIN_LTE:
        op = OpCode::OP_LESS_EQUAL;
        break;
    case BinOpType::BIN_GTE:
        op = OpCode::OP_GREATER_EQUAL;
        break;
    default:
        error("不支持的运算符: " + std::string(BinaryOp::opTypeStr(node.opType)), node.line, node.column);
        return;
    }

    chunk_.writeOp(op, node.line);
    return;
}

void Compiler::visitUnaryOp(UnaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldUnary(node.opType, node.operand.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
    }

    compileNode(node.operand.get());
    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        chunk_.writeOp(OpCode::OP_NEGATE, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_NOT:
        chunk_.writeOp(OpCode::OP_NOT, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_PLUS:
        break; // 一元 + 恒等操作，操作数已在栈上
    default:
        break;
    }
    return;
}

void Compiler::visitNumberLiteral(NumberLiteral& node) {
    // A1 fix: 用 getValue() 按需构造 Value（节点存储为标量）
    Value v = node.getValue();
    uint16_t idx = chunk_.addConstant(v);
    if (v.isInt()) {
        chunk_.writeOp(OpCode::OP_INT, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_FLOAT, node.line);
    }
    chunk_.writeShort(idx, node.line);
    return;
}

void Compiler::visitStringLiteral(StringLiteral& node) {
    // PERF-29 fix: 字符串字面量去重，避免相同字符串（如多次出现的 "hello"）重复存入常量池。
    // 常量池大小减少 20-40%（视程序），字节码加载稍快。
    auto it = stringConstIndex_.find(node.value);
    uint16_t idx;
    if (it != stringConstIndex_.end()) {
        idx = it->second; // 复用已有常量池索引
    } else {
        idx = chunk_.addConstant(node.getValue()); // A1 fix: getValue()
        stringConstIndex_[node.value] = idx;
    }
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(idx, node.line);
    return;
}

// C5 fix: 插值字符串编译 — 发射字面量 + 表达式 + OP_ADD 链
// 等价于原 BinaryOp(BIN_ADD) 链的字节码，但 AST 结构得以保留
void Compiler::visitInterpolatedString(InterpolatedString& node) {
    // 发射首个字面量片段到栈顶
    if (!node.literals.empty()) {
        uint16_t idx = chunk_.addConstant(Value(node.literals[0]));
        chunk_.writeOp(OpCode::OP_STRING, node.line);
        chunk_.writeShort(idx, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 交替发射: 表达式 → OP_ADD → 字面量 → OP_ADD
    for (size_t i = 0; i < node.expressions.size(); ++i) {
        compileNode(node.expressions[i].get());
        chunk_.writeOp(OpCode::OP_ADD, node.line);
        if (i + 1 < node.literals.size() && !node.literals[i + 1].empty()) {
            uint16_t idx = chunk_.addConstant(Value(node.literals[i + 1]));
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(idx, node.line);
            chunk_.writeOp(OpCode::OP_ADD, node.line);
        }
    }
    return;
}

void Compiler::visitBoolLiteral(BoolLiteral& node) {
    chunk_.writeOp(node.value ? OpCode::OP_TRUE : OpCode::OP_FALSE, node.line);
    return;
}

void Compiler::visitVarDecl(VarDecl& node) {
    // 编译初始化表达式
    if (node.initializer) {
        compileNode(node.initializer.get());
    } else if (!node.typeAnnotation.empty() && classFieldNames_.find(node.typeAnnotation) != classFieldNames_.end()) {
        // S2 fix: 类名类型注解无初始化器 → 自动构造实例（与解释器 visitVarDecl 一致）
        uint16_t classNameIdx = identifierIndex(node.typeAnnotation);
        chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
        chunk_.writeShort(classNameIdx, node.line);
        chunk_.write(static_cast<uint8_t>(0), node.line); // argCount = 0
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 2026-06-29: 类型注解运行时检查（三后端统一强制）
    // 在 SET 操作前发射 OP_TYPE_CHECK，peek 栈顶值检查类型兼容性
    // 类名注解也检查（实例类型匹配），null 兼容所有类型
    emitTypeCheck(node.typeAnnotation, node.line);
    // 记录类型注解，供后续 Assignment 检查
    if (!node.typeAnnotation.empty()) {
        varTypes_[node.name] = node.typeAnnotation;
    }

    // 在函数体内使用局部变量
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            // 新局部变量，分配槽位
            int slot = static_cast<int>(currentLocals_.size());
            // C-P1-2 fix: 局部变量槽位上限 255（uint8_t 编码限制）
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", node.line, node.column);
                return;
            }
            currentLocals_[node.name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            // BUG-IDE-12 fix: 记录 slot→name 映射（跨作用域累积，不随作用域退出清除）
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node.name;
            // L1 fix: 记录 (slot, name, startIp) 到 slotNameRanges_，endIp 暂设为 0
            // （由 closeSlotRanges 在块退出时回填）。resolveSlotName 按 ip 反查变量名，
            // 解决兄弟作用域槽位复用导致的变量名错位。
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
        } else {
            // B2 fix: 与解释器行为一致，同作用域重复声明报错
            error("变量 '" + node.name + "' 已在当前作用域中定义", node.line, node.column);
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，补发 OP_POP 平衡栈，
        // 避免函数内循环 var 声明累积导致栈溢出。
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // A2: use integer slot for known globals
        // Bug2 fix: 块作用域内不使用预分配的全局槽位，避免覆盖外层全局变量
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        // B4: topLevelGlobals_ 已删除（write-only 死代码，从未被读取）
    }
    return;
}

void Compiler::visitAssignment(Assignment& node) {
    compileNode(node.value.get());

    // 2026-06-29: 类型注解运行时检查（赋值时强制）
    // 查找变量声明的类型注解，有则发射 OP_TYPE_CHECK（peek 栈顶，不弹栈）
    const std::string* varType = findVarType(node.name);
    if (varType) {
        emitTypeCheck(*varType, node.line);
    }

    // C2 fix: 赋值作为表达式应产生值。先 DUP 保留一份在栈上，
    // SET 操作消费原始值后，DUP 的副本留在栈顶供外层表达式使用。
    chunk_.writeOp(OpCode::OP_DUP, node.line);

    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，配合 OP_POP 清理 DUP 的副本
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        } else {
            // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量赋值）
            int uvIdx = resolveUpvalue(node.name, node.line);
            if (uvIdx >= 0) {
                chunk_.writeOp(OpCode::OP_SET_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                chunk_.writeOp(OpCode::OP_POP, node.line); // 清理 DUP 副本
                return;
            }
            // A2: use integer slot for known globals
            int slot = lookupGlobalSlot(node.name);
            if (slot >= 0) {
                chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            } else {
                uint16_t nameIdx = identifierIndex(node.name);
                chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            }
        }
    } else {
        // A2: use integer slot for known globals
        int slot = lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    }
    return;
}

// VM-05/06: 解析闭包捕获变量为 upvalue 索引
// 返回 upvalue 在 currentUpvalues_ 中的索引，-1 表示不是闭包变量
int Compiler::resolveUpvalue(const std::string& name, int /*line*/) {
    // 1. 检查是否已在当前 upvalue 列表中（去重）
    auto existIt = currentUpvalueNames_.find(name);
    if (existIt != currentUpvalueNames_.end()) {
        return existIt->second;
    }

    // 2. 检查外层局部变量（直接捕获，isLocal=true）
    auto outerIt = outerLocals_.find(name);
    if (outerIt != outerLocals_.end()) {
        UpvalueDesc desc;
        desc.isLocal = true;
        desc.index = outerIt->second;
        desc.name = name; // 条件断点修复(R75): 记录upvalue变量名
        int idx = static_cast<int>(currentUpvalues_.size());
        currentUpvalues_.push_back(desc);
        currentUpvalueNames_[name] = idx;
        return idx;
    }

    // C-P1-3 fix: 3. 检查外层函数的 upvalue（透传捕获，isLocal=false）
    // 原代码将此检查嵌套在步骤 2 内部，导致永远不会执行（变量不可能同时是外层局部变量和 upvalue）
    auto outerUvIt = outerUpvalueNames_.find(name);
    if (outerUvIt != outerUpvalueNames_.end()) {
        UpvalueDesc desc;
        desc.isLocal = false;
        desc.index = outerUvIt->second; // 外层 upvalue 索引
        desc.name = name;               // 条件断点修复(R75): 记录upvalue变量名
        int idx = static_cast<int>(currentUpvalues_.size());
        currentUpvalues_.push_back(desc);
        currentUpvalueNames_[name] = idx;
        return idx;
    }
    return -1;
}

// BUG-UV-1 fix: 前向自由变量分析实现（对齐 IR 路径 AstIRBuilder::computeFreeVars）
// 在编译子函数体前预建 upvalue，使中间函数捕获内层引用的变量供透传。
// 解决 3+ 层嵌套闭包问题：fun outer(){var x=1; fun mid(){ fun inner(){return x;} ... }}
// mid 不直接引用 x，但 inner 需要，mid 必须捕获 x 供 inner 透传。
bool Compiler::isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes,
                                 const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->count(name))
            return true;
    }
    return false;
}

void Compiler::collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                               std::unordered_set<std::string>& freeVars) {
    switch (node.nodeType) {
    case NodeType::NODE_VAR_REF: {
        const auto& ref = static_cast<const VarRef&>(node);
        if (!isDefinedInScopes(scopes, ref.name)) {
            freeVars.insert(ref.name);
        }
        break;
    }
    case NodeType::NODE_ASSIGNMENT: {
        const auto& assign = static_cast<const Assignment&>(node);
        if (!isDefinedInScopes(scopes, assign.name)) {
            freeVars.insert(assign.name);
        }
        if (assign.value)
            collectFreeVars(*assign.value, scopes, freeVars);
        break;
    }
    case NodeType::NODE_VAR_DECL: {
        const auto& decl = static_cast<const VarDecl&>(node);
        if (decl.initializer)
            collectFreeVars(*decl.initializer, scopes, freeVars);
        scopes.back().insert(decl.name);
        break;
    }
    case NodeType::NODE_FUN_DECL: {
        const auto& nestedFn = static_cast<const FunDecl&>(node);
        for (const auto& dv : nestedFn.defaultValues) {
            if (dv)
                collectFreeVars(*dv, scopes, freeVars);
        }
        std::vector<std::unordered_set<std::string>> nestedScopes;
        nestedScopes.emplace_back();
        for (const auto& p : nestedFn.params)
            nestedScopes.back().insert(p);
        // R98 W3: 匿名 lambda（name 为空）不插入空名到作用域
        if (!nestedFn.name.empty()) {
            nestedScopes.back().insert(nestedFn.name);
        }
        std::unordered_set<std::string> nestedFree;
        if (nestedFn.body)
            collectFreeVars(*nestedFn.body, nestedScopes, nestedFree);
        for (const auto& name : nestedFree) {
            if (!isDefinedInScopes(scopes, name)) {
                freeVars.insert(name);
            }
        }
        // R98 W3: 匿名 lambda 不注册到外层作用域
        if (!nestedFn.name.empty()) {
            scopes.back().insert(nestedFn.name);
        }
        break;
    }
    case NodeType::NODE_FUN_CALL: {
        const auto& call = static_cast<const FunCall&>(node);
        if (call.callee) {
            collectFreeVars(*call.callee, scopes, freeVars);
        } else if (!call.name.empty()) {
            if (!isDefinedInScopes(scopes, call.name)) {
                freeVars.insert(call.name);
            }
        }
        for (const auto& arg : call.arguments) {
            if (arg)
                collectFreeVars(*arg, scopes, freeVars);
        }
        break;
    }
    case NodeType::NODE_BLOCK: {
        const auto& block = static_cast<const Block&>(node);
        scopes.emplace_back();
        for (const auto& stmt : block.statements) {
            if (stmt)
                collectFreeVars(*stmt, scopes, freeVars);
        }
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_FOR_STMT: {
        const auto& forStmt = static_cast<const ForStmt&>(node);
        scopes.emplace_back();
        if (forStmt.initializer)
            collectFreeVars(*forStmt.initializer, scopes, freeVars);
        if (forStmt.condition)
            collectFreeVars(*forStmt.condition, scopes, freeVars);
        if (forStmt.update)
            collectFreeVars(*forStmt.update, scopes, freeVars);
        if (forStmt.body)
            collectFreeVars(*forStmt.body, scopes, freeVars);
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_TRY_STMT: {
        const auto& tryStmt = static_cast<const TryStmt&>(node);
        if (tryStmt.tryBlock)
            collectFreeVars(*tryStmt.tryBlock, scopes, freeVars);
        if (tryStmt.catchBlock) {
            scopes.emplace_back();
            if (!tryStmt.catchVarName.empty())
                scopes.back().insert(tryStmt.catchVarName);
            collectFreeVars(*tryStmt.catchBlock, scopes, freeVars);
            scopes.pop_back();
        }
        break;
    }
    case NodeType::NODE_CLASS_DECL: {
        const auto& cls = static_cast<const ClassDecl&>(node);
        scopes.back().insert(cls.name);
        break;
    }
    default:
        for (auto* child : node.children()) {
            if (child)
                collectFreeVars(*child, scopes, freeVars);
        }
        break;
    }
}

std::unordered_set<std::string> Compiler::computeFreeVars(const FunDecl& fn) {
    std::vector<std::unordered_set<std::string>> scopes;
    scopes.emplace_back();
    for (const auto& param : fn.params)
        scopes.back().insert(param);
    scopes.back().insert(fn.name);

    std::unordered_set<std::string> freeVars;
    for (const auto& dv : fn.defaultValues) {
        if (dv)
            collectFreeVars(*dv, scopes, freeVars);
    }
    if (fn.body)
        collectFreeVars(*fn.body, scopes, freeVars);
    return freeVars;
}

void Compiler::visitArrayLiteral(ArrayLiteral& node) {
    // 检查元素数量上限（uint8_t 编码限制）
    if (node.elements.size() > 255) {
        error("数组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
    return;
}

// R98 元组与解构：元组字面量编译
// 栈布局：依次 push 各 element，OP_BUILD_TUPLE count 弹出 count 个值并 push 元组。
void Compiler::visitTupleLiteral(TupleLiteral& node) {
    if (node.elements.size() > 255) {
        error("元组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    chunk_.writeOp(OpCode::OP_BUILD_TUPLE, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
    return;
}

// R98 元组与解构：解构绑定 var (a, b, c) = expr 编译
// 策略：
//   1. 编译 initializer，栈顶为元组值
//   2. 对每个 name，按位置 i 生成：
//        DUP（复制元组到栈顶）+ OP_INT i + OP_INDEX_GET（取第 i 个元素）
//        + 变量绑定（OP_DEFINE_VAR / OP_SET_LOCAL）
//   3. 最后 OP_POP 清理元组（除非在表达式上下文，但解构绑定是语句，无返回值消费）
//   4. 末尾 OP_POP 清理 DUP 产生的栈顶元组副本
// 注意：与 visitVarDecl 不同，解构绑定是语句，不保留栈上值。
void Compiler::visitDestructureBinding(DestructureBinding& node) {
    if (node.names.size() > 255) {
        error("解构绑定变量数超过 255 个上限", node.line, node.column);
        return;
    }

    // 编译 initializer，栈顶为元组
    compileNode(node.initializer.get());

    // 为每个变量取元素并绑定
    for (size_t i = 0; i < node.names.size(); ++i) {
        // DUP 复制元组到栈顶（保留原元组供下一轮使用）
        chunk_.writeOp(OpCode::OP_DUP, node.line);
        // push 索引 i
        uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
        chunk_.writeOp(OpCode::OP_INT, node.line);
        chunk_.writeShort(idxConst, node.line);
        // 取元素：tuple[i]
        chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
        // AUDIT-R6 F4 fix: per-name 类型注解运行时校验（对齐 Interpreter
        // visitDestructureBinding 的 L20 checkType）。原仅 Interpreter 校验，
        // `var (a: int, b) = ("s", 2)` 在 VM 静默通过——三后端不一致。
        // OP_TYPE_CHECK peek 不弹栈，在绑定前检查栈顶元素。
        emitTypeCheck(node.nameTypeAt(i), node.line);
        // 栈顶为 tuple[i]，绑定到变量 name
        bindDestructureVar(node.names[i], node.line, node.column);
    }

    // 清理栈顶元组（DUP 已被消费，仅剩原始元组）
    chunk_.writeOp(OpCode::OP_POP, node.line);
    return;
}

// R98 元组与解构：解构绑定单个变量的绑定逻辑（复用 visitVarDecl 的局部/全局分支）
void Compiler::bindDestructureVar(const std::string& name, int line, int col) {
    if (inFunction_) {
        auto it = currentLocals_.find(name);
        if (it == currentLocals_.end()) {
            int slot = static_cast<int>(currentLocals_.size());
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", line, col);
                return;
            }
            currentLocals_[name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = name;
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, line);
            chunk_.write(static_cast<uint8_t>(slot), line);
        } else {
            error("变量 '" + name + "' 已在当前作用域中定义", line, col);
            chunk_.writeOp(OpCode::OP_SET_LOCAL, line);
            chunk_.write(static_cast<uint8_t>(it->second), line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶，补发 OP_POP 清理元素
        chunk_.writeOp(OpCode::OP_POP, line);
    } else {
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, line);
            chunk_.writeShort(static_cast<uint16_t>(slot), line);
        } else {
            uint16_t nameIdx = identifierIndex(name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, line);
            chunk_.writeShort(nameIdx, line);
        }
    }
}

// ============================================================
// R99 枚举与 ADT + match 表达式
// ============================================================

// enum 声明编译：将 enum 名绑定为标记值 "enum:Name"。
// 不需要向 VM 注册 variant 元信息——OP_BUILD_ENUM_VARIANT 通过
// 常量池中的 enumName/variantName 字符串直接构造 variant 值，
// 类型校验由 Interpreter（visitEnumVariantExpr）和 TypeChecker 负责。
void Compiler::visitEnumDecl(EnumDecl& node) {
    // 编译期：在当前作用域定义 enum 名为标记值 "enum:Name"
    // 运行时：OP_STRING + OP_DEFINE_VAR/OP_DEFINE_GLOBAL
    std::string marker = std::string("enum:") + node.name;
    uint16_t markerIdx = chunk_.addConstant(Value(marker));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(markerIdx, node.line);

    if (inFunction_) {
        // 函数内：使用局部变量槽位
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            int slot = static_cast<int>(currentLocals_.size());
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", node.line, node.column);
                chunk_.writeOp(OpCode::OP_POP, node.line); // 平衡栈
                return;
            }
            currentLocals_[node.name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node.name;
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line); // OP_SET_LOCAL 用 peek，需补 POP
        } else {
            // 重定义：直接写入已有槽位
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    } else {
        // 顶层：使用全局变量槽位或慢路径
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    }

    // R99 enum 校验：收集 enum 元信息到 pendingEnumInfos_，compile() 完成时
    // 写入 CompileResult.enumInfos 供 VM 启动时加载到 enumRegistry_。
    // 对齐 Interpreter::visitEnumDecl 的 enumRegistry_ 注册，使 OP_BUILD_ENUM_VARIANT
    // 能在运行时校验 variant 名存在性 + 参数 arity 一致性，保证三后端一致。
    VMEnumInfo info;
    info.name = node.name;
    // AUDIT-R6 F3 fix: 携带字段类型注解与泛型参数，使 OP_BUILD_ENUM_VARIANT 能对齐
    // Interpreter 的字段类型校验（原仅 arity）。
    info.typeParams = node.typeParams;
    info.variants.reserve(node.variants.size());
    for (const auto& v : node.variants) {
        VMEnumVariantInfo vi;
        vi.name = v.name;
        vi.arity = static_cast<int>(v.paramTypes.size());
        vi.paramTypes = v.paramTypes;
        info.variants.push_back(std::move(vi));
    }
    pendingEnumInfos_.push_back(std::move(info));
    return;
}

// 枚举 variant 构造编译：EnumName.VariantName(arg1, arg2, ...)
// 栈布局：依次 push 各参数（按声明顺序），然后 OP_BUILD_ENUM_VARIANT 消费 argCount 个参数。
void Compiler::visitEnumVariantExpr(EnumVariantExpr& node) {
    if (node.arguments.size() > 255) {
        error("enum variant 参数数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有参数（按顺序 push）
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    // OP_BUILD_ENUM_VARIANT enumNameIdx variantNameIdx argCount
    uint16_t enumIdx = chunk_.addConstant(Value(node.enumName));
    uint16_t varIdx = chunk_.addConstant(Value(node.variantName));
    chunk_.writeOp(OpCode::OP_BUILD_ENUM_VARIANT, node.line);
    chunk_.writeShort(enumIdx, node.line);
    chunk_.writeShort(varIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    return;
}

// match 表达式编译：
//   1. 编译 scrutinee，push 到栈
//   2. 对每个 case：
//      - WILDCARD/DEFAULT：直接执行 body
//      - LITERAL：DUP + literal + OP_EQUAL + JUMP_IF_FALSE → next_case
//      - VARIANT：DUP + OP_ENUM_VARIANT_NAME + JUMP_IF_FALSE → next_case
//                 然后对每个 binding：DUP + OP_INT i + OP_ENUM_VARIANT_FIELD + bind
//      - 编译 body（push 结果）
//      - OP_SWAP（结果与 scrutinee 交换）+ OP_POP（删除 scrutinee）
//      - JUMP → end
//   3. 所有 case 未匹配：OP_POP（删 scrutinee）+ OP_NULL（push null 作为结果）
//   4. end：
void Compiler::visitMatchExpr(MatchExpr& node) {
    // 编译 scrutinee，栈顶为 scrutinee 值
    compileNode(node.scrutinee.get());

    // 收集所有 case 的 JUMP_IF_FALSE 占位（用于跳到 next case）
    std::vector<size_t> caseSkipPatches;
    // 收集所有 case body 末尾的 JUMP（跳到 end）占位
    std::vector<size_t> endJumps;

    auto savedLocals = currentLocals_;
    size_t branchSlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;

    for (auto& mc : node.cases) {
        // case 匹配检查（emitMatchPattern 处理 WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR 六种模式）
        if (!mc.isDefault && mc.pattern) {
            emitMatchPattern(*mc.pattern, caseSkipPatches);
        }
        // default case 无需 pattern 检查
        // R133: guard 编译——pattern 匹配成功后求值 guard，false 则跳到下一 case
        if (mc.guard) {
            compileNode(mc.guard.get());
            size_t guardSkipPatch = chunk_.code.size();
            chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, mc.guard->line);
            chunk_.writeShort(0, mc.guard->line);
            caseSkipPatches.push_back(guardSkipPatch);
            chunk_.writeOp(OpCode::OP_POP, mc.guard->line); // 弹出 guard bool
        }

        // 编译 case body（push 结果到栈），body 可能是 Block 或单表达式
        compileMatchBody(mc.body.get(), node.line);

        // 关闭 case 作用域内声明的 upvalue（与 visitIfStmt 一致）
        if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
        }
        closeSlotRanges(branchSlotBase);
        currentLocals_ = savedLocals;

        // 栈布局：[scrutinee, body_result] → OP_SWAP → [body_result, scrutinee] → OP_POP → [body_result]
        chunk_.writeOp(OpCode::OP_SWAP, node.line); // 交换 scrut 和 result
        chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出 scrut

        // JUMP → end
        size_t endJump = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        endJumps.push_back(endJump);

        // R133: 回填当前 case 的所有 skip patch（pattern + guard）跳到下一 case 起始
        // 原 R99 代码仅回填单个 patch；扩展后每个 case 可能有多个 patch（嵌套 pattern + guard）
        if (!caseSkipPatches.empty()) {
            uint16_t nextCaseStart = safeCodeOffset();
            while (!caseSkipPatches.empty()) {
                size_t skipPatch = caseSkipPatches.back();
                caseSkipPatches.pop_back();
                chunk_.code[skipPatch + 1] = static_cast<uint8_t>(nextCaseStart & 0xFF);
                chunk_.code[skipPatch + 2] = static_cast<uint8_t>((nextCaseStart >> 8) & 0xFF);
            }
            // JUMP_IF_FALSE 不消费条件值，跳过时栈上仍 bool，补 OP_POP
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    }

    // L6 fix: 所有 case 未匹配时，对齐 Interpreter 的 runtimeError 行为（抛异常而非静默返回 null）
    // 无 default 分支时：OP_POP 删 scrut + OP_STRING(msg) + OP_THROW 抛出异常
    chunk_.writeOp(OpCode::OP_POP, node.line);
    uint16_t errIdx = chunk_.addConstant(Value(std::string("match 表达式没有匹配的 case")));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(errIdx, node.line);
    chunk_.writeOp(OpCode::OP_THROW, node.line);

    // 回填所有 end jumps
    uint16_t endTarget = safeCodeOffset();
    for (size_t endJump : endJumps) {
        chunk_.code[endJump + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[endJump + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
    }
    return;
}

// ============================================================
// visitMatchExpr 子阶段实现
// ============================================================

void Compiler::emitMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // R133 模式匹配扩展：递归处理六种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）。
    // 栈布局不变量：调用前 [scrut]，调用后 [scrut]（保持 scrut 在栈顶供后续 case 复用）。
    // 匹配失败跳转 patch 追加到 caseSkipPatches 由调用方回填到下一 case 起始。
    //
    // L27 重构：原 211 行 switch 按模式类型分发到 6 个子函数，降低圈复杂度。
    switch (p.kind) {
    case MatchPatternKind::WILDCARD:
        // 永远匹配，无需检查
        break;
    case MatchPatternKind::LITERAL:
        emitLiteralMatchPattern(p, caseSkipPatches);
        break;
    case MatchPatternKind::VARIABLE:
        emitVariableMatchPattern(p);
        break;
    case MatchPatternKind::VARIANT:
        emitVariantMatchPattern(p, caseSkipPatches);
        break;
    case MatchPatternKind::TUPLE:
        emitTupleMatchPattern(p, caseSkipPatches);
        break;
    case MatchPatternKind::OR:
        emitOrMatchPattern(p, caseSkipPatches);
        break;
    }
}

void Compiler::emitLiteralMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // DUP scrutinee + literal + OP_EQUAL + JUMP_IF_FALSE → next + OP_POP
    chunk_.writeOp(OpCode::OP_DUP, p.line);
    compileNode(p.literal.get());
    chunk_.writeOp(OpCode::OP_EQUAL, p.line);
    size_t skipPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
    chunk_.writeShort(0, p.line);
    caseSkipPatches.push_back(skipPatch);
    chunk_.writeOp(OpCode::OP_POP, p.line);
}

void Compiler::emitVariableMatchPattern(const MatchPattern& p) {
    // R133: DUP scrutinee + 绑定整个 scrut 到 variableName
    // DUP 让 scrut 留在栈顶（保持栈不变量），副本绑定到 variableName
    chunk_.writeOp(OpCode::OP_DUP, p.line);
    bindDestructureVar(p.variableName, p.line, p.column);
}

void Compiler::emitVariantMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // DUP scrutinee + OP_ENUM_VARIANT_NAME + JUMP_IF_FALSE → next + OP_POP
    chunk_.writeOp(OpCode::OP_DUP, p.line);
    uint16_t enumIdx = chunk_.addConstant(Value(p.enumName));
    uint16_t varIdx = chunk_.addConstant(Value(p.variantName));
    chunk_.writeOp(OpCode::OP_ENUM_VARIANT_NAME, p.line);
    chunk_.writeShort(enumIdx, p.line);
    chunk_.writeShort(varIdx, p.line);
    size_t skipPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
    chunk_.writeShort(0, p.line);
    caseSkipPatches.push_back(skipPatch);
    chunk_.writeOp(OpCode::OP_POP, p.line); // 弹出 bool
    // 绑定 variant 字段（递归子 pattern）
    // R133: subPatterns 替代 R99 的 bindings（vector<string>），支持嵌套
    for (size_t i = 0; i < p.subPatterns.size(); ++i) {
        // DUP scrutinee + OP_INT i + OP_ENUM_VARIANT_FIELD + 递归 emitMatchPattern(sub, ...)
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
        chunk_.writeOp(OpCode::OP_INT, p.line);
        chunk_.writeShort(idxConst, p.line);
        chunk_.writeOp(OpCode::OP_ENUM_VARIANT_FIELD, p.line);
        // 栈顶是 fields[i]，递归匹配子 pattern
        // R133 fix: 子 pattern fail 用独立 subFailPatches，失败时先 POP field 再跳到 caseSkip（同 TUPLE pattern）
        // R133 fix-2: 成功路径 OP_POP 后必须 JUMP 跳过失败路径代码（同 TUPLE pattern）
        std::vector<size_t> subFailPatches;
        emitMatchPattern(*p.subPatterns[i], subFailPatches);
        chunk_.writeOp(OpCode::OP_POP, p.line); // 成功路径 POP field
        emitSubPatternFailPath(subFailPatches, caseSkipPatches, p.line);
    }
}

void Compiler::emitTupleMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // R133: 元组模式 - 检查是 tuple + 元素数匹配 + 递归匹配每个元素
    // 软类型检查：DUP + OP_TYPE_TEST "tuple"（push bool 不抛错）+ JUMP_IF_FALSE next + POP
    // 注：用 OP_TYPE_TEST 而非 OP_TYPE_CHECK——后者不匹配时 runtimeError，
    // 而 TUPLE pattern 不匹配时应 fall through 到下一 case 而非抛错。
    chunk_.writeOp(OpCode::OP_DUP, p.line);
    uint16_t tupleTypeNameConst = chunk_.addConstant(Value(std::string("tuple")));
    chunk_.writeOp(OpCode::OP_TYPE_TEST, p.line);
    chunk_.writeShort(tupleTypeNameConst, p.line);
    size_t typeSkipPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
    chunk_.writeShort(0, p.line);
    caseSkipPatches.push_back(typeSkipPatch);
    chunk_.writeOp(OpCode::OP_POP, p.line);
    // 元素数检查：DUP + OP_LEN + OP_INT size + OP_EQUAL + JUMP_IF_FALSE next + POP
    chunk_.writeOp(OpCode::OP_DUP, p.line);
    chunk_.writeOp(OpCode::OP_LEN, p.line);
    uint16_t sizeConst = chunk_.addConstant(Value(static_cast<int64_t>(p.subPatterns.size())));
    chunk_.writeOp(OpCode::OP_INT, p.line);
    chunk_.writeShort(sizeConst, p.line);
    chunk_.writeOp(OpCode::OP_EQUAL, p.line);
    size_t sizeSkipPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
    chunk_.writeShort(0, p.line);
    caseSkipPatches.push_back(sizeSkipPatch);
    chunk_.writeOp(OpCode::OP_POP, p.line);
    // 递归匹配每个元素
    for (size_t i = 0; i < p.subPatterns.size(); ++i) {
        // DUP tuple + OP_INT i + OP_INDEX_GET + 递归 emitMatchPattern(sub, ...) + POP
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
        chunk_.writeOp(OpCode::OP_INT, p.line);
        chunk_.writeShort(idxConst, p.line);
        chunk_.writeOp(OpCode::OP_INDEX_GET, p.line);
        // R133 fix: 子 pattern 的 fail 用独立 subFailPatches，失败时先 POP element 再跳到 caseSkip。
        // 原实现把子 pattern fail 直接加入 caseSkipPatches，失败时栈上有 [scrut, element, bool]，
        // caseSkip 处只 POP bool，element 残留导致后续 case 栈错乱。
        // 正确做法：子 pattern fail 回填到"POP element + JUMP caseSkip"，先清栈再跳转。
        // R133 fix-2: 成功路径 OP_POP 后必须 JUMP 跳过失败路径代码，否则会跌入失败路径
        // 的 OP_POP+OP_JUMP，导致栈下溢。结构：
        //   [子 pattern body] → [scrut, elem, bool]
        //   OP_JUMP_IF_FALSE subFailPos  (失败跳走，栈 [scrut, elem, bool])
        //   OP_POP  (成功路径 LITERAL 自己的 pop bool) → [scrut, elem]
        //   OP_POP  (成功路径 TUPLE loop 的 pop elem) → [scrut]
        //   OP_JUMP afterFail   ← 成功路径跳过失败代码
        //   subFailPos:
        //   OP_POP  (失败路径 pop bool) → [scrut, elem]
        //   OP_JUMP caseSkip → [scrut, elem] (caseSkip 处 OP_POP 消费 elem)
        //   afterFail: (下一 sub-pattern)
        std::vector<size_t> subFailPatches;
        emitMatchPattern(*p.subPatterns[i], subFailPatches);
        // 成功路径：POP element 继续
        chunk_.writeOp(OpCode::OP_POP, p.line);
        emitSubPatternFailPath(subFailPatches, caseSkipPatches, p.line);
    }
}

void Compiler::emitSubPatternFailPath(std::vector<size_t>& subFailPatches, std::vector<size_t>& caseSkipPatches,
                                      int line) {
    // VARIANT/TUPLE 共用：失败路径回填 + POP elem/field + JUMP caseSkip + 回填成功 JUMP。
    // 调用前须已 emit 子 pattern 与成功路径 POP（elem/field）。
    // subFailPatches 为空时无失败路径代码可 emit，直接返回。
    if (subFailPatches.empty()) {
        return;
    }
    // 成功路径 JUMP 跳过失败路径代码（占位，稍后回填）
    size_t successJumpOver = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, line);
    chunk_.writeShort(0, line);
    // 失败路径入口
    uint16_t subFailPos = safeCodeOffset();
    for (auto& sp : subFailPatches) {
        chunk_.code[sp + 1] = static_cast<uint8_t>(subFailPos & 0xFF);
        chunk_.code[sp + 2] = static_cast<uint8_t>((subFailPos >> 8) & 0xFF);
    }
    chunk_.writeOp(OpCode::OP_POP, line); // 失败路径 POP elem/field
    size_t jumpToSkip = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, line);
    chunk_.writeShort(0, line);
    caseSkipPatches.push_back(jumpToSkip);
    // 回填成功路径 JUMP 到此（下一 sub-pattern 或末尾）
    uint16_t afterFail = safeCodeOffset();
    chunk_.code[successJumpOver + 1] = static_cast<uint8_t>(afterFail & 0xFF);
    chunk_.code[successJumpOver + 2] = static_cast<uint8_t>((afterFail >> 8) & 0xFF);
    subFailPatches.clear();
}

void Compiler::emitOrMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // R133: OR pattern - 任一子 pattern 匹配即成功
    // 编译策略：依次尝试每个子 pattern，任一成功跳到 OR 成功位置（OR 末尾）；
    // 全部失败时 emit 永假检查让 case 整体跳到下一 case。
    std::vector<size_t> successJumps; // 各子 pattern 成功后跳到 OR 末尾
    for (size_t i = 0; i < p.subPatterns.size(); ++i) {
        // 编译子 pattern，失败的 patch 暂存到 subPatches
        std::vector<size_t> subPatches;
        emitMatchPattern(*p.subPatterns[i], subPatches);
        // 子 pattern 成功：跳到 OR 末尾
        size_t successJump = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, p.line);
        chunk_.writeShort(0, p.line);
        successJumps.push_back(successJump);
        // 子 pattern 失败位置：所有 subPatches 回填到同一位置，然后只写一次 POP
        // R133 fix: 原实现每个 sp 都 writeOp POP，导致 failPos 处有多个 POP，
        // 第一个 sp 跳转到 failPos 后会连续执行多个 POP，栈下溢。
        // 正确做法：所有 patch 共享同一 failPos，只写一次 POP。
        uint16_t failPos = safeCodeOffset();
        for (auto& sp : subPatches) {
            chunk_.code[sp + 1] = static_cast<uint8_t>(failPos & 0xFF);
            chunk_.code[sp + 2] = static_cast<uint8_t>((failPos >> 8) & 0xFF);
        }
        if (!subPatches.empty()) {
            // 失败路径有 bool 残留需 POP（JUMP_IF_FALSE 不消费条件值）
            chunk_.writeOp(OpCode::OP_POP, p.line);
        }
    }
    // 所有子 pattern 失败：OR 失败
    // R133 fix: caseSkip handler 约定——所有 fail patch 跳转时栈须为 [scrut, X]
    // （X 为 JUMP_IF_FALSE 残留的 bool 或 TUPLE/VARIANT sub-pattern fail 残留的 elem）。
    // handler 执行一次 OP_POP 消费 X，回到 [scrut] 供下一 case 使用。
    // OR 整体失败时栈已回到 [scrut]（sub-pattern failPos 已 POP bool），无残值 X，
    // 故 push 一个 OP_NULL 作为占位残值，让 caseSkip handler 的 OP_POP 正确消费。
    chunk_.writeOp(OpCode::OP_NULL, p.line);
    size_t orFailJump = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, p.line);
    chunk_.writeShort(0, p.line);
    caseSkipPatches.push_back(orFailJump);
    // OR 成功位置：回填所有 successJumps 跳到这里
    uint16_t orSuccess = safeCodeOffset();
    for (auto& sj : successJumps) {
        chunk_.code[sj + 1] = static_cast<uint8_t>(orSuccess & 0xFF);
        chunk_.code[sj + 2] = static_cast<uint8_t>((orSuccess >> 8) & 0xFF);
    }
}

void Compiler::compileMatchBody(ASTNode* body, int line) {
    // 编译 case body 并保留值在栈顶（栈不变量：调用前 []，调用后 [body_result]）。
    // - Block body：编译除最后一条外的所有语句（带 POP），最后一条用 compileNode 保留值；
    //   非表达式语句补 OP_NULL。
    // - 单表达式 body：直接 compileNode。
    // - 空 body：push OP_NULL。
    // 表达式节点类型判定（与 IR.cpp visitMatchExpr 的 needsPopForExprStmt 对齐，
    // AUDIT-R3 P2-7：不再写死数量，以枚举清单为准）：
    //   ASSIGNMENT/FUN_CALL/METHOD_CALL/BINARY_OP/UNARY_OP/VAR_REF/MEMBER_ACCESS/INDEX_ACCESS
    //   /NUMBER/STRING/BOOL/NULL_LITERAL/SUPER_EXPR/ARRAY/DICT/TUPLE/INTERPOLATED/ENUM_VARIANT/MATCH
    if (!body) {
        // 空 body：push null
        chunk_.writeOp(OpCode::OP_NULL, line);
        return;
    }

    if (body->nodeType != NodeType::NODE_BLOCK) {
        compileNode(body);
        return;
    }

    // Block：执行所有语句，最后一条语句的值作为结果
    Block* blk = static_cast<Block*>(body);
    if (blk->statements.empty()) {
        chunk_.writeOp(OpCode::OP_NULL, line);
        return;
    }
    // 编译除最后一条外的所有语句（compileStatement 会消费表达式值）
    for (size_t i = 0; i + 1 < blk->statements.size(); ++i) {
        compileStatement(blk->statements[i].get());
    }
    // 最后一条语句用 compileNode 保留值在栈顶（不弹）
    ASTNode* last = blk->statements.back().get();
    compileNode(last);
    // 若 last 非表达式节点（如 VarDecl/IfStmt），不会产生栈值，补 push null
    // AUDIT-R4 BUG-04 fix: 改用共享谓词 needsPopForExprStmt（ExprStmtPop.h），
    // 消除与 compileStatement/AstIRBuilder 的第三份重复清单。
    // “产生栈值的表达式节点”与“表达式语句需 POP 的节点”是同一集合。
    // AUDIT-R5 BUG-01 fix: 改用节点级重载——若 last 是匿名 lambda，闭包值已在
    // 栈顶作为 case 结果，原类型谓词对 FUN_DECL 返回 false 会误补 OP_NULL，
    // 造成双值栈不平衡（case 结果变 null + 泄漏闭包值）。
    if (!needsPopForExprStmt(last)) {
        // 声明/控制流：无值产生，补 null 作为 case 结果
        chunk_.writeOp(OpCode::OP_NULL, line);
    }
}

void Compiler::visitDictLiteral(DictLiteral& node) {
    // 检查键值对数量上限（uint8_t 编码限制）
    if (node.pairs.size() > 255) {
        error("字典键值对数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有键值对（先键后值，与 OP_BUILD_DICT 消费顺序一致）
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 构建字典指令：弹出 2*count 个值，构建字典，push 到栈
    chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
    chunk_.write(static_cast<uint8_t>(node.pairs.size()), node.line);
    return;
}

void Compiler::visitIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
    return;
}

void Compiler::visitIndexAssign(IndexAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;

    if (objVar) {
        // 简单路径：arr[i] = val（基是 VarRef）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
            return;
        }
        // W3-2-Bug1c fix: upvalue 接收者的索引赋值（闭包内 arr[i] = val）
        // 原 bug: 只检查 currentLocals_，未检查 upvalue，导致 upvalue 接收者错误走全局变量路径
        // 报"未定义的变量"。修复: 用 OP_GET_UPVALUE 获取数组引用 + OP_INDEX_SET 修改元素。
        // W3-2-Bug1b 对齐: OP_INDEX_SET 的 COW ensureUnique 在数组 refcount > 1（upvalue 与栈
        // 副本同时引用）时产生新数组副本，lastMutatedReceiver_ 持有新副本但 upvalue 仍指向旧值。
        // 必须发射 OP_WRITEBACK_INDEX_UPVALUE 将变异后的新数组写回 upvalue。
        // 对齐 IR 路径 visitIndexAssign 的 #7 fix（IR.cpp L3035-3039）与 visitMemberAssign 的
        // W3-2-Bug1b fix（Compiler.cpp L4718-4734）。
        if (inFunction_) {
            int uvIdx = resolveUpvalue(objVar->name, node.line);
            if (uvIdx >= 0) {
                // 栈序: [obj, idx, val] — OP_INDEX_SET 弹出 val/idx/obj
                chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                compileNode(node.index.get());
                compileNode(node.value.get());
                chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
                // COW detach 后写回 upvalue（整体替换语义，lastMutatedReceiver_ = 变异后数组）
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                return;
            }
        }
        // 全局变量路径（原逻辑）
        compileNode(node.index.get());
        compileNode(node.value.get());
        uint16_t nameIdx = identifierIndex(objVar->name);
        chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
        return;
    }

    // M1 fix: 嵌套索引赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node.object.get())
                                : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node.object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // W3-2-Bug1c fix: 嵌套索引赋值的 upvalue 接收者（闭包内 b.field[i] = val / arr[j][i] = val）
        // 原 bug: 嵌套路径只处理 isLocal / 全局变量，未处理 upvalue，导致 upvalue 接收者错误走全局
        // 变量路径报"未定义的变量"。修复: 用 OP_GET_UPVALUE + OP_INDEX_SET/OP_MEMBER_SET + WRITEBACK_*_UPVALUE
        // 链传播变异。对齐 IR 路径 emitNestedAssignWriteback 的 UPVALUE 分支（IR.cpp L3006-3013）。
        int uvIdx = -1;
        if (!isLocal && inFunction_) {
            uvIdx = resolveUpvalue(baseVar->name, node.line);
        }
        if (uvIdx >= 0) {
            // 步骤 1-3: 编译外层表达式 → push base.outer（副本） + 内层索引 + 值
            compileNode(node.object.get());
            compileNode(node.index.get());
            compileNode(node.value.get());
            // 步骤 4: OP_INDEX_SET 弹出 val/idx/obj，修改 obj（COW detach），lastMutatedReceiver_ = new_base.outer
            chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
            // 步骤 5: 加载基变量 + 设置基变量字段/索引为 new_base.outer，产生 new_base
            // 栈序: [base, outerIdx, mut]（outerIdx 路径）或 [base, mut]（outerMem 路径）
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            if (outerIdx) {
                // base[outerIdx] = mut
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET, node.line); // lastMutatedReceiver_ = new_base
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            } else {
                // base.field = mut
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line); // lastMutatedReceiver_ = new_base
                chunk_.writeShort(fieldIdx, node.line);
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
            return;
        }

        // 编译外层表达式 → push base[outerIdx] 或 base.field（正确求值中间值）
        // 注意：不再 push base 变量本身——WRITEBACK_*_LOCAL/VAR 和 MEMBER_SET_LOCAL/VAR
        // 均不需要 base 在栈上，原 push base 会导致栈泄漏（每次嵌套赋值泄漏 1 个 Value）。
        compileNode(node.object.get());
        // 编译内层索引 → push innerIdx
        compileNode(node.index.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_INDEX_SET: 弹出 val/innerIdx/outerValue → 修改 → lastMutatedReceiver_
        chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
        // 将变异后的内层容器写回基变量。
        // 对齐 IR 路径（IR.cpp emitNestedAssignWriteback L2696-2708）：
        //   IR 路径用 LOAD_MUTATED + INDEX_SET(base2, outerIdx, mut) + WRITEBACK 链传播变异；
        //   非 IR 路径改用更简洁的 LOAD_MUTATED + INDEX_SET_LOCAL/VAR 直接原地修改基变量。
        // R156 fix: 原 outerIdx 路径用 WRITEBACK_INDEX_LOCAL/VAR 做整体替换，
        // 将 arr 变量替换为变异后的 arr[i]（而非 arr[i] = mutated_arr[i]），
        // 导致 arr[i][j]=val 在非 IR 路径（StackVM 非 IR + JIT）全部失败。
        // outerMem 路径（base.field[idx]=val）已在 BUG-INH-AUDIT-4 fix 中修正为
        // LOAD_MUTATED + MEMBER_SET_LOCAL/VAR，此路径正确无需修改。
        if (isLocal) {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_LOCAL
                // INDEX_SET_LOCAL 直接修改 stack_[bp+slot][outerIdx] = mutated（原地）
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_LOCAL（BUG-INH-AUDIT-4 fix，不变）
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_VAR
                uint16_t nameIdx = identifierIndex(baseVar->name);
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_VAR（不变）
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
        }
        return;
    }

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    error("不支持 3 层及以上嵌套索引赋值（如 a[b[c]] = d）", node.line, node.column);
    compileNode(node.object.get());
    compileNode(node.index.get());
    compileNode(node.value.get());
    chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
    return;
}
