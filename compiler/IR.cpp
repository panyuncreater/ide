#include "compiler/IR.h"
#include "ast/ASTNode.h"
#include "interpreter/Value.h"
#include "Logger.h"
#include <sstream>
#include <unordered_set>
#include <cmath>

// ============================================================
// IR 中间表示层实现（ARCH-06 完整实现）
// ============================================================
// 本文件实现三层：
//   1. AstIRBuilder：AST → IR（Visitor 模式，覆盖全部核心 AST 节点）
//   2. BytecodeIRBackend：IR → VM 字节码（lowering）
//   3. IRToString：调试输出
//
// 已补齐 5 个已知限制：
//   1. 闭包 upvalue 捕获（MAKE_CLOSURE 编码 upvalue 描述符列表）
//   2. 写回指令（WRITEBACK_MEMBER_VAR/LOCAL, WRITEBACK_INDEX_VAR/LOCAL）
//   3. 全局槽位分配（OP_GET_GLOBAL/OP_SET_GLOBAL/OP_DEFINE_GLOBAL 槽位版）
//   4. 默认参数值（defaultConstIndices 记录常量索引）
//   5. 块作用域区分（enterBlockScope/leaveBlockScope）
// ============================================================

namespace {

/// 辅助：IR 操作码 → 字符串（调试用，覆盖全部 IROp）
const char* irOpName(IROp op) {
    switch (op) {
    // 常量加载
    case IROp::LOAD_CONST:      return "LOAD_CONST";
    case IROp::LOAD_NULL:       return "LOAD_NULL";
    case IROp::LOAD_TRUE:       return "LOAD_TRUE";
    case IROp::LOAD_FALSE:      return "LOAD_FALSE";
    // 变量访问
    case IROp::LOAD_LOCAL:      return "LOAD_LOCAL";
    case IROp::STORE_LOCAL:     return "STORE_LOCAL";
    case IROp::LOAD_GLOBAL:     return "LOAD_GLOBAL";
    case IROp::STORE_GLOBAL:    return "STORE_GLOBAL";
    case IROp::DEFINE_GLOBAL:   return "DEFINE_GLOBAL";
    case IROp::LOAD_UPVALUE:    return "LOAD_UPVALUE";
    case IROp::STORE_UPVALUE:   return "STORE_UPVALUE";
    case IROp::CLOSE_UPVALUE:   return "CLOSE_UPVALUE";
    // 算术
    case IROp::ADD:             return "ADD";
    case IROp::SUB:             return "SUB";
    case IROp::MUL:             return "MUL";
    case IROp::DIV:             return "DIV";
    case IROp::MOD:             return "MOD";
    case IROp::NEGATE:          return "NEGATE";
    // 比较
    case IROp::EQ:              return "EQ";
    case IROp::NEQ:             return "NEQ";
    case IROp::LT:              return "LT";
    case IROp::GT:              return "GT";
    case IROp::LTE:             return "LTE";
    case IROp::GTE:             return "GTE";
    // 逻辑
    case IROp::NOT:             return "NOT";
    // 控制流
    case IROp::JUMP:            return "JUMP";
    case IROp::JUMP_IF_FALSE:   return "JUMP_IF_FALSE";
    case IROp::LABEL:           return "LABEL";
    // 调用
    case IROp::CALL:            return "CALL";
    case IROp::CALL_EXPR:       return "CALL_EXPR";
    case IROp::RETURN:          return "RETURN";
    case IROp::RETURN_NULL:     return "RETURN_NULL";
    // 闭包
    case IROp::MAKE_CLOSURE:    return "MAKE_CLOSURE";
    // 容器
    case IROp::BUILD_ARRAY:     return "BUILD_ARRAY";
    case IROp::BUILD_DICT:      return "BUILD_DICT";
    case IROp::INDEX_GET:       return "INDEX_GET";
    case IROp::INDEX_SET:       return "INDEX_SET";
    // 成员访问
    case IROp::MEMBER_GET:      return "MEMBER_GET";
    case IROp::MEMBER_SET:      return "MEMBER_SET";
    // 方法调用
    case IROp::METHOD_CALL:     return "METHOD_CALL";
    // 类
    case IROp::DEFINE_CLASS:    return "DEFINE_CLASS";
    case IROp::CLASS_NEW:       return "CLASS_NEW";
    case IROp::INIT_FIELD:      return "INIT_FIELD";
    // 异常
    case IROp::TRY_BEGIN:       return "TRY_BEGIN";
    case IROp::TRY_END:         return "TRY_END";
    case IROp::THROW:           return "THROW";
    // 写回指令（限制2）
    case IROp::WRITEBACK_MEMBER_VAR:    return "WRITEBACK_MEMBER_VAR";
    case IROp::WRITEBACK_MEMBER_LOCAL:  return "WRITEBACK_MEMBER_LOCAL";
    case IROp::WRITEBACK_INDEX_VAR:     return "WRITEBACK_INDEX_VAR";
    case IROp::WRITEBACK_INDEX_LOCAL:   return "WRITEBACK_INDEX_LOCAL";
    // 其他
    case IROp::PRINT:           return "PRINT";
    case IROp::POP:             return "POP";
    case IROp::DUP:             return "DUP";
    }
    return "?";
}

/// 简化版常量提取（限制4）：支持字面量和负数字面量
/// 成功返回 true 并输出常量值；失败返回 false
bool extractConstant(ASTNode* node, Value& result) {
    if (!node) return false;
    switch (node->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL:
        result = static_cast<NumberLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_STRING_LITERAL:
        result = static_cast<StringLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_BOOL_LITERAL:
        result = static_cast<BoolLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_NULL_LITERAL:
        result = Value::nullValue();
        return true;
    case NodeType::NODE_UNARY_OP: {
        // 仅支持 NEGATE 作用于数字字面量
        auto* un = static_cast<UnaryOp*>(node);
        if (un->opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
            Value operand;
            if (extractConstant(un->operand.get(), operand) && operand.isNumber()) {
                if (operand.isInt()) {
                    result = Value(-operand.intVal());
                } else {
                    result = Value(-operand.floatVal());
                }
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

} // anonymous namespace

// ============================================================
// AstIRBuilder 实现
// ============================================================

AstIRBuilder::AstIRBuilder() {
    // 初始化 ir_ 为新 IRFunction（main chunk）
    ir_ = std::make_unique<IRFunction>();
    ir_->name = "main";
    // 初始化 module_（收集所有函数）
    module_ = std::make_unique<IRModule>();
    // 创建初始基本块
    uint32_t entryLabel = ir_->allocLabel();
    currentBlock_ = &ir_->addBlock(entryLabel);
    currentBlock_->instructions.emplace_back(IROp::LABEL,
        std::vector<IROperand>{ IROperand::label(entryLabel) }, 0);
}

std::unique_ptr<IRFunction> AstIRBuilder::build(Block& program) {
    // 限制3：预扫描顶层声明，分配全局槽位
    preScanTopLevelDecls(program);
    // 遍历顶层语句，逐个转换
    for (auto& stmt : program.statements) {
        if (stmt) visitNode(stmt.get());
    }
    // 将 ir_ 作为 mainFunction 存入 module_
    module_->mainFunction = std::move(ir_);
    // 返回 ir_（转移所有权给调用者；module_ 通过 getModule() 仍可访问 functions）
    return std::move(module_->mainFunction);
}

// ---- 全局槽位管理（限制3 / B4: 已内联到 IR.h，委托给 globalSlotAllocator_）----

void AstIRBuilder::preScanTopLevelDecls(Block& program) {
    // 遍历顶层语句，为 VarDecl 和 ClassDecl 分配全局槽位
    for (auto& stmt : program.statements) {
        if (!stmt) continue;
        if (stmt->nodeType == NodeType::NODE_VAR_DECL) {
            VarDecl* vd = static_cast<VarDecl*>(stmt.get());
            allocateGlobalSlot(vd->name);
        } else if (stmt->nodeType == NodeType::NODE_CLASS_DECL) {
            ClassDecl* cd = static_cast<ClassDecl*>(stmt.get());
            allocateGlobalSlot(cd->name);
        }
    }
}

// ---- 块作用域（限制5）----

void AstIRBuilder::enterBlockScope() {
    blockScopes_.push_back(BlockScope{});
    blockDepth_++;
}

void AstIRBuilder::leaveBlockScope() {
    if (blockScopes_.empty()) return;
    // 取出栈顶 BlockScope
    BlockScope scope = std::move(blockScopes_.back());
    blockScopes_.pop_back();
    if (blockDepth_ > 0) blockDepth_--;
    // 仅函数内：回收局部变量槽位
    // 简化：不实际复用局部槽位编号（避免破坏 slot 编号一致性），
    // 仅清空记录。全局槽位 freeSlots_ 不混入局部槽位。
    (void)scope;
}

// ---- 辅助方法 ----

IRBasicBlock& AstIRBuilder::newBlock() {
    uint32_t label = ir_->allocLabel();
    IRBasicBlock& blk = ir_->addBlock(label);
    currentBlock_ = &blk;
    // 新块起始插入 LABEL 指令，便于 lowering 记录偏移
    currentBlock_->instructions.emplace_back(IROp::LABEL,
        std::vector<IROperand>{ IROperand::label(label) }, 0);
    return *currentBlock_;
}

void AstIRBuilder::emitIR(IROp op, std::vector<IROperand> operands, int line) {
    currentBlock_->instructions.emplace_back(op, std::move(operands), line);
}

IROperand AstIRBuilder::emitConst(const Value& v, int line) {
    uint32_t idx = ir_->addConstant(v);
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_CONST, { dest, IROperand::constant(idx) }, line);
    return dest;
}

IROperand AstIRBuilder::emitLoadVar(const std::string& name, int line) {
    VarInfo info = resolveVar(name);
    IROperand dest = ir_->allocVReg();
    switch (info.kind) {
    case VarInfo::Kind::LOCAL:
        emitIR(IROp::LOAD_LOCAL, { dest, IROperand::local(info.index) }, line);
        break;
    case VarInfo::Kind::GLOBAL_SLOT:
        // 槽位版：用 IMM_UINT 存槽位号
        emitIR(IROp::LOAD_GLOBAL, { dest, IROperand::imm(info.index) }, line);
        break;
    case VarInfo::Kind::GLOBAL_NAME:
        // 名称版：用 GLOBAL_NAME 存名称索引
        emitIR(IROp::LOAD_GLOBAL, { dest, IROperand::global(info.index) }, line);
        break;
    case VarInfo::Kind::UPVALUE:
        emitIR(IROp::LOAD_UPVALUE, { dest, IROperand::upvalue(info.index) }, line);
        break;
    }
    return dest;
}

void AstIRBuilder::emitStoreVar(const std::string& name, IROperand val, int line) {
    VarInfo info = resolveVar(name);
    switch (info.kind) {
    case VarInfo::Kind::LOCAL:
        emitIR(IROp::STORE_LOCAL, { IROperand::local(info.index), val }, line);
        break;
    case VarInfo::Kind::GLOBAL_SLOT:
        emitIR(IROp::STORE_GLOBAL, { IROperand::imm(info.index), val }, line);
        break;
    case VarInfo::Kind::GLOBAL_NAME:
        emitIR(IROp::STORE_GLOBAL, { IROperand::global(info.index), val }, line);
        break;
    case VarInfo::Kind::UPVALUE:
        emitIR(IROp::STORE_UPVALUE, { IROperand::upvalue(info.index), val }, line);
        break;
    }
}

AstIRBuilder::VarInfo AstIRBuilder::resolveVar(const std::string& name) {
    // 1. 优先查 varMap_（当前作用域已声明的变量）
    auto it = varMap_.find(name);
    if (it != varMap_.end()) return it->second;

    // 2. 函数内：尝试 upvalue 捕获（限制1）
    if (inFunction_) {
        // 检查是否为内嵌函数（当前函数内声明的函数）
        auto innerIt = innerFunctions_.find(name);
        if (innerIt != innerFunctions_.end()) {
            auto slotIt = innerFunctionSlots_.find(name);
            if (slotIt != innerFunctionSlots_.end()) {
                return { VarInfo::Kind::LOCAL, static_cast<uint32_t>(slotIt->second) };
            }
        }
        // 尝试添加为 upvalue
        addUpvalue(name);
        // 检查是否成功添加
        auto uvNameIt = currentUpvalueNames_.find(name);
        if (uvNameIt != currentUpvalueNames_.end()) {
            VarInfo info{ VarInfo::Kind::UPVALUE, static_cast<uint32_t>(uvNameIt->second) };
            varMap_[name] = info;
            return info;
        }
        // 未找到 upvalue，继续向下检查全局
    }

    // 3. 顶层/函数内回退：检查全局槽位（限制3）
    int slot = lookupGlobalSlot(name);
    if (slot >= 0) {
        VarInfo info{ VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot) };
        varMap_[name] = info;
        return info;
    }

    // 4. 回退：名称索引（GLOBAL_NAME）
    uint32_t idx = ir_->addGlobal(name);
    VarInfo info{ VarInfo::Kind::GLOBAL_NAME, idx };
    varMap_[name] = info;
    return info;
}

uint32_t AstIRBuilder::addUpvalue(const std::string& name) {
    // 检查是否已在 currentUpvalues_ 中
    auto it = currentUpvalueNames_.find(name);
    if (it != currentUpvalueNames_.end()) return static_cast<uint32_t>(it->second);

    // 检查是否为外层局部变量（isLocal=true，直接捕获）
    auto localIt = outerLocalSlots_.find(name);
    if (localIt != outerLocalSlots_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({ idx, true, localIt->second });
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    // 检查是否为外层 upvalue（isLocal=false，透传）
    auto uvIt = outerUpvalueNames_.find(name);
    if (uvIt != outerUpvalueNames_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({ idx, false, uvIt->second });
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    // 检查是否为外层函数（isLocal=true，按局部变量捕获）
    auto fnIt = outerFunctions_.find(name);
    if (fnIt != outerFunctions_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({ idx, true, fnIt->second });
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    return 0;  // 未找到
}

// ---- AST 节点转换：分派 ----

IROperand AstIRBuilder::visitNode(ASTNode* node) {
    if (!node) return IROperand::vreg(0);  // 空节点返回空 vreg
    switch (node->nodeType) {
    // 表达式（返回 vreg）
    case NodeType::NODE_BINARY_OP:      return visitBinaryOp(static_cast<BinaryOp*>(node));
    case NodeType::NODE_UNARY_OP:       return visitUnaryOp(static_cast<UnaryOp*>(node));
    case NodeType::NODE_NUMBER_LITERAL: return visitNumberLiteral(static_cast<NumberLiteral*>(node));
    case NodeType::NODE_STRING_LITERAL: return visitStringLiteral(static_cast<StringLiteral*>(node));
    case NodeType::NODE_BOOL_LITERAL:   return visitBoolLiteral(static_cast<BoolLiteral*>(node));
    case NodeType::NODE_NULL_LITERAL:   return visitNullLiteral(static_cast<NullLiteral*>(node));
    case NodeType::NODE_VAR_REF:        return visitVarRef(static_cast<VarRef*>(node));
    case NodeType::NODE_ASSIGNMENT:     return visitAssignment(static_cast<Assignment*>(node));
    case NodeType::NODE_FUN_CALL:       return visitFunCall(static_cast<FunCall*>(node));
    case NodeType::NODE_ARRAY_LITERAL:  return visitArrayLiteral(static_cast<ArrayLiteral*>(node));
    case NodeType::NODE_DICT_LITERAL:   return visitDictLiteral(static_cast<DictLiteral*>(node));
    case NodeType::NODE_INDEX_ACCESS:   return visitIndexAccess(static_cast<IndexAccess*>(node));
    case NodeType::NODE_MEMBER_ACCESS:  return visitMemberAccess(static_cast<MemberAccess*>(node));
    case NodeType::NODE_METHOD_CALL:    return visitMethodCall(static_cast<MethodCall*>(node));
    // 语句（返回空 vreg）
    case NodeType::NODE_VAR_DECL:       visitVarDecl(static_cast<VarDecl*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_IF_STMT:        visitIfStmt(static_cast<IfStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_WHILE_STMT:     visitWhileStmt(static_cast<WhileStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_FOR_STMT:       visitForStmt(static_cast<ForStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_FUN_DECL:       visitFunDecl(static_cast<FunDecl*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_RETURN_STMT:    visitReturnStmt(static_cast<ReturnStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_PRINT_STMT:     visitPrintStmt(static_cast<PrintStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_BLOCK:          visitBlock(static_cast<Block*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_INDEX_ASSIGN:   visitIndexAssign(static_cast<IndexAssign*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_CLASS_DECL:     visitClassDecl(static_cast<ClassDecl*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_MEMBER_ASSIGN:  visitMemberAssign(static_cast<MemberAssign*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_BREAK_STMT:     visitBreakStmt(static_cast<BreakStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_CONTINUE_STMT:  visitContinueStmt(static_cast<ContinueStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_TRY_STMT:       visitTryStmt(static_cast<TryStmt*>(node)); return IROperand::vreg(0);
    case NodeType::NODE_THROW_STMT:     visitThrowStmt(static_cast<ThrowStmt*>(node)); return IROperand::vreg(0);
    // 未实现的复杂节点：跳过，返回空 vreg
    case NodeType::NODE_INTERPOLATED_STRING:
    case NodeType::NODE_IMPORT_STMT:
    case NodeType::NODE_EXPORT_STMT:
    case NodeType::NODE_SUPER_EXPR:
    default:
        return IROperand::vreg(0);
    }
}

IROperand AstIRBuilder::visitBinaryOp(BinaryOp* node) {
    // AND/OR 短路求值：JUMP_IF_FALSE + LABEL
    if (node->opType == BinOpType::BIN_AND) {
        IROperand left = visitNode(node->left.get());
        uint32_t endLabel = ir_->allocLabel();
        // 左值为假则短路到 end（结果取 left）
        emitIR(IROp::JUMP_IF_FALSE, { left, IROperand::label(endLabel) }, node->line);
        // 左值为真，求值右操作数（结果取 right）
        IROperand right = visitNode(node->right.get());
        emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
        return right;
    }
    if (node->opType == BinOpType::BIN_OR) {
        IROperand left = visitNode(node->left.get());
        uint32_t evalRightLabel = ir_->allocLabel();
        uint32_t endLabel = ir_->allocLabel();
        // 左值为假则去求值右操作数
        emitIR(IROp::JUMP_IF_FALSE, { left, IROperand::label(evalRightLabel) }, node->line);
        // 左值为真，短路到 end（结果取 left）
        emitIR(IROp::JUMP, { IROperand::label(endLabel) }, node->line);
        emitIR(IROp::LABEL, { IROperand::label(evalRightLabel) }, node->line);
        IROperand right = visitNode(node->right.get());
        emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
        return right;
    }
    // 非短路二元运算
    IROperand left = visitNode(node->left.get());
    IROperand right = visitNode(node->right.get());
    IROperand dest = ir_->allocVReg();
    switch (node->opType) {
    case BinOpType::BIN_ADD: emitIR(IROp::ADD, { dest, left, right }, node->line); break;
    case BinOpType::BIN_SUB: emitIR(IROp::SUB, { dest, left, right }, node->line); break;
    case BinOpType::BIN_MUL: emitIR(IROp::MUL, { dest, left, right }, node->line); break;
    case BinOpType::BIN_DIV: emitIR(IROp::DIV, { dest, left, right }, node->line); break;
    case BinOpType::BIN_MOD: emitIR(IROp::MOD, { dest, left, right }, node->line); break;
    case BinOpType::BIN_EQ:  emitIR(IROp::EQ,  { dest, left, right }, node->line); break;
    case BinOpType::BIN_NEQ: emitIR(IROp::NEQ, { dest, left, right }, node->line); break;
    case BinOpType::BIN_LT:  emitIR(IROp::LT,  { dest, left, right }, node->line); break;
    case BinOpType::BIN_GT:  emitIR(IROp::GT,  { dest, left, right }, node->line); break;
    case BinOpType::BIN_LTE: emitIR(IROp::LTE, { dest, left, right }, node->line); break;
    case BinOpType::BIN_GTE: emitIR(IROp::GTE, { dest, left, right }, node->line); break;
    default: break;
    }
    return dest;
}

IROperand AstIRBuilder::visitUnaryOp(UnaryOp* node) {
    IROperand operand = visitNode(node->operand.get());
    // 一元正号直接返回操作数
    if (node->opType == UnaryOp::UnaryOpType::UOP_PLUS) {
        return operand;
    }
    IROperand dest = ir_->allocVReg();
    if (node->opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
        emitIR(IROp::NEGATE, { dest, operand }, node->line);
    } else if (node->opType == UnaryOp::UnaryOpType::UOP_NOT) {
        emitIR(IROp::NOT, { dest, operand }, node->line);
    }
    return dest;
}

IROperand AstIRBuilder::visitNumberLiteral(NumberLiteral* node) {
    return emitConst(node->getValue(), node->line);
}

IROperand AstIRBuilder::visitStringLiteral(StringLiteral* node) {
    return emitConst(node->getValue(), node->line);
}

IROperand AstIRBuilder::visitBoolLiteral(BoolLiteral* node) {
    IROperand dest = ir_->allocVReg();
    if (node->value) {
        emitIR(IROp::LOAD_TRUE, { dest }, node->line);
    } else {
        emitIR(IROp::LOAD_FALSE, { dest }, node->line);
    }
    return dest;
}

IROperand AstIRBuilder::visitNullLiteral(NullLiteral* node) {
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_NULL, { dest }, node->line);
    return dest;
}

IROperand AstIRBuilder::visitVarRef(VarRef* node) {
    return emitLoadVar(node->name, node->line);
}

IROperand AstIRBuilder::visitAssignment(Assignment* node) {
    IROperand val = visitNode(node->value.get());
    emitStoreVar(node->name, val, node->line);
    return val;  // 赋值表达式返回所赋的值
}

void AstIRBuilder::visitVarDecl(VarDecl* node) {
    IROperand val;
    if (node->initializer) {
        val = visitNode(node->initializer.get());
    } else {
        // 无初始化器：初始化为 null
        val = ir_->allocVReg();
        emitIR(IROp::LOAD_NULL, { val }, node->line);
    }
    if (inFunction_) {
        // 函数内：注册为 LOCAL（限制5：记录到当前 BlockScope）
        uint32_t slot = nextLocalSlot_++;
        varMap_[node->name] = { VarInfo::Kind::LOCAL, slot };
        emitIR(IROp::STORE_LOCAL, { IROperand::local(slot), val }, node->line);
        // 记录到当前 BlockScope 的 localSlots
        if (!blockScopes_.empty()) {
            blockScopes_.back().localSlots.push_back(slot);
        }
    } else {
        // 顶层：使用全局槽位（限制3）
        int slot = lookupGlobalSlot(node->name);
        if (slot >= 0) {
            // 有槽位：GLOBAL_SLOT，emit DEFINE_GLOBAL（槽位版）
            varMap_[node->name] = { VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot) };
            emitIR(IROp::DEFINE_GLOBAL, { IROperand::imm(static_cast<uint32_t>(slot)), val }, node->line);
        } else {
            // 无槽位：回退名称索引
            uint32_t idx = ir_->addGlobal(node->name);
            varMap_[node->name] = { VarInfo::Kind::GLOBAL_NAME, idx };
            emitIR(IROp::DEFINE_GLOBAL, { IROperand::global(idx), val }, node->line);
        }
    }
}

void AstIRBuilder::visitIfStmt(IfStmt* node) {
    IROperand cond = visitNode(node->condition.get());
    uint32_t elseLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    // 条件为假跳到 else
    emitIR(IROp::JUMP_IF_FALSE, { cond, IROperand::label(elseLabel) }, node->line);
    // then 分支（限制5：块作用域包裹）
    if (inFunction_) enterBlockScope();
    visitNode(node->thenBranch.get());
    if (inFunction_) leaveBlockScope();
    emitIR(IROp::JUMP, { IROperand::label(endLabel) }, node->line);
    // else 分支（限制5：块作用域包裹）
    emitIR(IROp::LABEL, { IROperand::label(elseLabel) }, node->line);
    if (node->elseBranch) {
        if (inFunction_) enterBlockScope();
        visitNode(node->elseBranch.get());
        if (inFunction_) leaveBlockScope();
    }
    emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
}

void AstIRBuilder::visitWhileStmt(WhileStmt* node) {
    uint32_t startLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    // while 的 continue 目标 = 条件检查点（startLabel）
    loopStack_.push_back({ startLabel, endLabel, startLabel });
    emitIR(IROp::LABEL, { IROperand::label(startLabel) }, node->line);
    IROperand cond = visitNode(node->condition.get());
    emitIR(IROp::JUMP_IF_FALSE, { cond, IROperand::label(endLabel) }, node->line);
    // 循环体（限制5：块作用域包裹）
    if (inFunction_) enterBlockScope();
    visitNode(node->body.get());
    if (inFunction_) leaveBlockScope();
    emitIR(IROp::JUMP, { IROperand::label(startLabel) }, node->line);
    emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
    loopStack_.pop_back();
}

void AstIRBuilder::visitForStmt(ForStmt* node) {
    // 编译初始化表达式
    if (node->initializer) visitNode(node->initializer.get());
    uint32_t startLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    uint32_t continueLabel = ir_->allocLabel();
    // continue 目标 = update 块（continueLabel 在 body 之后、update 之前）
    loopStack_.push_back({ startLabel, endLabel, continueLabel });
    emitIR(IROp::LABEL, { IROperand::label(startLabel) }, node->line);
    // 条件（nullptr 时视为永真，不 emit 跳转）
    if (node->condition) {
        IROperand cond = visitNode(node->condition.get());
        emitIR(IROp::JUMP_IF_FALSE, { cond, IROperand::label(endLabel) }, node->line);
    } else {
        // 无条件：emit LOAD_TRUE（保持栈平衡，但不跳转）
        IROperand t = ir_->allocVReg();
        emitIR(IROp::LOAD_TRUE, { t }, node->line);
    }
    // 编译循环体（限制5：块作用域包裹）
    if (inFunction_) enterBlockScope();
    visitNode(node->body.get());
    if (inFunction_) leaveBlockScope();
    // continue 目标：update 块入口
    emitIR(IROp::LABEL, { IROperand::label(continueLabel) }, node->line);
    // 编译 update 表达式
    if (node->update) visitNode(node->update.get());
    emitIR(IROp::JUMP, { IROperand::label(startLabel) }, node->line);
    emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
    loopStack_.pop_back();
}

void AstIRBuilder::visitFunDecl(FunDecl* node) {
    // 限制1：完整闭包 upvalue 捕获实现
    // 1. 保存父函数编译状态
    std::unique_ptr<IRFunction> savedIr = std::move(ir_);
    IRBasicBlock* savedBlock = currentBlock_;
    auto savedVarMap = std::move(varMap_);
    bool savedInFunction = inFunction_;
    uint32_t savedLocalSlot = nextLocalSlot_;
    auto savedLoopStack = std::move(loopStack_);
    auto savedBlockScopes = std::move(blockScopes_);
    int savedBlockDepth = blockDepth_;

    // 保存外层变量/upvalue/内嵌函数信息（用于子函数捕获）
    auto savedOuterLocalSlots = std::move(outerLocalSlots_);
    auto savedOuterUpvalueNames = std::move(outerUpvalueNames_);
    auto savedOuterFunctions = std::move(outerFunctions_);
    auto savedCurrentUpvalues = std::move(currentUpvalues_);
    auto savedCurrentUpvalueNames = std::move(currentUpvalueNames_);
    auto savedInnerFunctions = std::move(innerFunctions_);
    auto savedInnerFunctionSlots = std::move(innerFunctionSlots_);

    // 2. 新建子 IRFunction
    ir_ = std::make_unique<IRFunction>();
    ir_->name = node->name;
    ir_->arity = static_cast<int>(node->params.size());
    ir_->requiredArity = node->requiredParamCount;
    inFunction_ = true;
    nextLocalSlot_ = 0;
    varMap_.clear();
    loopStack_.clear();
    blockScopes_.clear();
    blockDepth_ = 0;
    currentUpvalues_.clear();
    currentUpvalueNames_.clear();
    innerFunctions_.clear();
    innerFunctionSlots_.clear();

    // 3. 设置外层局部/upvalue 信息（供 addUpvalue 查找）
    //    outerLocalSlots_ = 父函数的 LOCAL 变量（子函数可捕获为 isLocal=true）
    //    outerUpvalueNames_ = 父函数的 UPVALUE（子函数可透传为 isLocal=false）
    outerLocalSlots_.clear();
    outerUpvalueNames_.clear();
    for (const auto& kv : savedVarMap) {
        if (kv.second.kind == VarInfo::Kind::LOCAL) {
            outerLocalSlots_[kv.first] = static_cast<int>(kv.second.index);
        } else if (kv.second.kind == VarInfo::Kind::UPVALUE) {
            outerUpvalueNames_[kv.first] = static_cast<int>(kv.second.index);
        }
    }
    outerFunctions_.clear();

    // 4. 参数注册为 LOCAL slot 0..n-1
    for (size_t i = 0; i < node->params.size(); ++i) {
        uint32_t slot = nextLocalSlot_++;
        varMap_[node->params[i]] = { VarInfo::Kind::LOCAL, slot };
    }

    // 5. 编译默认参数值（限制4）
    //    对每个非 null 的默认值表达式，尝试用 extractConstant 提取常量
    //    成功则 addConstant 记录索引；失败则记录 0xFFFF（表示复杂表达式，不支持）
    for (auto& dv : node->defaultValues) {
        if (dv) {
            Value constVal;
            if (extractConstant(dv.get(), constVal)) {
                uint32_t constIdx = ir_->addConstant(constVal);
                ir_->defaultConstIndices.push_back(static_cast<uint16_t>(constIdx));
            } else {
                ir_->defaultConstIndices.push_back(0xFFFF);
            }
        }
    }

    // 6. 创建初始基本块
    uint32_t entryLabel = ir_->allocLabel();
    currentBlock_ = &ir_->addBlock(entryLabel);
    currentBlock_->instructions.emplace_back(IROp::LABEL,
        std::vector<IROperand>{ IROperand::label(entryLabel) }, node->line);

    // 7. 编译函数体
    if (node->body) visitNode(node->body.get());

    // 8. 末尾隐式 RETURN_NULL
    emitIR(IROp::RETURN_NULL, {}, node->line);

    // 9. 设置 localCount 和 upvalues
    ir_->localCount = static_cast<int>(nextLocalSlot_);
    ir_->upvalues.clear();
    for (const auto& uv : currentUpvalues_) {
        ir_->upvalues.push_back({ uv.outerIdx, uv.isLocal });
    }

    // 10. 将子 IRFunction 添加到 module_（不再丢弃 childIr）
    std::string fnName = node->name;
    module_->addFunction(std::move(ir_));

    // 11. 恢复父函数状态
    ir_ = std::move(savedIr);
    currentBlock_ = savedBlock;
    varMap_ = std::move(savedVarMap);
    inFunction_ = savedInFunction;
    nextLocalSlot_ = savedLocalSlot;
    loopStack_ = std::move(savedLoopStack);
    blockScopes_ = std::move(savedBlockScopes);
    blockDepth_ = savedBlockDepth;
    outerLocalSlots_ = std::move(savedOuterLocalSlots);
    outerUpvalueNames_ = std::move(savedOuterUpvalueNames);
    outerFunctions_ = std::move(savedOuterFunctions);
    currentUpvalues_ = std::move(savedCurrentUpvalues);
    currentUpvalueNames_ = std::move(savedCurrentUpvalueNames);
    innerFunctions_ = std::move(savedInnerFunctions);
    innerFunctionSlots_ = std::move(savedInnerFunctionSlots);

    // 12. 在父 IR 中 emit MAKE_CLOSURE（含 upvalue 描述符列表）
    //     编码：operands [dest, name_idx, uv_count, isLocal1, idx1, isLocal2, idx2, ...]
    uint32_t nameIdx = ir_->addGlobal(fnName);
    IROperand dest = ir_->allocVReg();
    IRFunction* fnIr = module_->findFunction(fnName);
    uint32_t uvCount = fnIr ? static_cast<uint32_t>(fnIr->upvalues.size()) : 0;
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::funcName(nameIdx));
    ops.push_back(IROperand::imm(uvCount));
    if (fnIr) {
        for (const auto& uv : fnIr->upvalues) {
            ops.push_back(IROperand::imm(uv.isLocal ? 1 : 0));
            ops.push_back(IROperand::imm(static_cast<uint32_t>(uv.index)));
        }
    }
    emitIR(IROp::MAKE_CLOSURE, ops, node->line);

    // 13. 在父函数中注册该函数（供后续引用）
    if (inFunction_) {
        // 函数内：注册为 LOCAL
        uint32_t slot = nextLocalSlot_++;
        varMap_[fnName] = { VarInfo::Kind::LOCAL, slot };
        innerFunctions_.insert(fnName);
        innerFunctionSlots_[fnName] = static_cast<int>(slot);
    } else {
        // 顶层：注册为全局（有槽位则 GLOBAL_SLOT，否则 GLOBAL_NAME）
        int slot = lookupGlobalSlot(fnName);
        if (slot >= 0) {
            varMap_[fnName] = { VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot) };
        } else {
            uint32_t idx = ir_->addGlobal(fnName);
            varMap_[fnName] = { VarInfo::Kind::GLOBAL_NAME, idx };
        }
    }
}

IROperand AstIRBuilder::visitFunCall(FunCall* node) {
    IROperand dest = ir_->allocVReg();
    if (node->callee) {
        // 表达式调用：编译 callee + 参数，emit CALL_EXPR
        IROperand calleeVreg = visitNode(node->callee.get());
        std::vector<IROperand> ops;
        ops.push_back(dest);
        ops.push_back(calleeVreg);
        ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
        for (auto& arg : node->arguments) {
            ops.push_back(visitNode(arg.get()));
        }
        emitIR(IROp::CALL_EXPR, ops, node->line);
    } else {
        // 命名调用：编译参数，emit CALL nameIdx argCount
        uint32_t nameIdx = ir_->addGlobal(node->name);
        std::vector<IROperand> ops;
        ops.push_back(dest);
        ops.push_back(IROperand::funcName(nameIdx));
        ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
        for (auto& arg : node->arguments) {
            ops.push_back(visitNode(arg.get()));
        }
        emitIR(IROp::CALL, ops, node->line);
    }
    return dest;
}

void AstIRBuilder::visitReturnStmt(ReturnStmt* node) {
    if (node->value) {
        IROperand val = visitNode(node->value.get());
        emitIR(IROp::RETURN, { val }, node->line);
    } else {
        emitIR(IROp::RETURN_NULL, {}, node->line);
    }
}

void AstIRBuilder::visitPrintStmt(PrintStmt* node) {
    for (auto& v : node->values) {
        IROperand val = visitNode(v.get());
        emitIR(IROp::PRINT, { val }, node->line);
    }
}

void AstIRBuilder::visitBlock(Block* node) {
    // 限制5：函数内时 enterBlockScope + 编译语句 + leaveBlockScope
    if (inFunction_) enterBlockScope();
    for (auto& s : node->statements) {
        if (s) visitNode(s.get());
    }
    if (inFunction_) leaveBlockScope();
}

IROperand AstIRBuilder::visitArrayLiteral(ArrayLiteral* node) {
    IROperand dest = ir_->allocVReg();
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->elements.size())));
    for (auto& e : node->elements) {
        ops.push_back(visitNode(e.get()));
    }
    emitIR(IROp::BUILD_ARRAY, ops, node->line);
    return dest;
}

IROperand AstIRBuilder::visitDictLiteral(DictLiteral* node) {
    IROperand dest = ir_->allocVReg();
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->pairs.size())));
    for (auto& p : node->pairs) {
        ops.push_back(visitNode(p.first.get()));   // key
        ops.push_back(visitNode(p.second.get()));  // value
    }
    emitIR(IROp::BUILD_DICT, ops, node->line);
    return dest;
}

IROperand AstIRBuilder::visitIndexAccess(IndexAccess* node) {
    IROperand obj = visitNode(node->object.get());
    IROperand idx = visitNode(node->index.get());
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::INDEX_GET, { dest, obj, idx }, node->line);
    return dest;
}

void AstIRBuilder::visitIndexAssign(IndexAssign* node) {
    // 限制2：检测 object 是否为简单 VarRef。若是，emit INDEX_SET + WRITEBACK_INDEX_VAR/LOCAL
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    IROperand obj = visitNode(node->object.get());
    IROperand idx = visitNode(node->index.get());
    IROperand val = visitNode(node->value.get());
    emitIR(IROp::INDEX_SET, { obj, idx, val }, node->line);
    // 仅当 object 是 VarRef 时 emit 写回指令
    if (isVarRef) {
        VarRef* vr = static_cast<VarRef*>(node->object.get());
        VarInfo info = resolveVar(vr->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            emitIR(IROp::WRITEBACK_INDEX_LOCAL, { IROperand::local(info.index) }, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, { IROperand::imm(info.index) }, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, { IROperand::global(info.index) }, node->line);
        }
        // UPVALUE：不 emit 写回（简化）
    }
}

IROperand AstIRBuilder::visitMemberAccess(MemberAccess* node) {
    IROperand obj = visitNode(node->object.get());
    IROperand dest = ir_->allocVReg();
    uint32_t fieldIdx = ir_->addGlobal(node->fieldName);
    emitIR(IROp::MEMBER_GET, { dest, obj, IROperand::field(fieldIdx) }, node->line);
    return dest;
}

void AstIRBuilder::visitMemberAssign(MemberAssign* node) {
    // 限制2：检测 object 是否为简单 VarRef。若是，emit MEMBER_SET + WRITEBACK_MEMBER_VAR/LOCAL
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    IROperand obj = visitNode(node->object.get());
    IROperand val = visitNode(node->value.get());
    uint32_t fieldIdx = ir_->addGlobal(node->fieldName);
    emitIR(IROp::MEMBER_SET, { obj, IROperand::field(fieldIdx), val }, node->line);
    // 仅当 object 是 VarRef 时 emit 写回指令
    if (isVarRef) {
        VarRef* vr = static_cast<VarRef*>(node->object.get());
        VarInfo info = resolveVar(vr->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            emitIR(IROp::WRITEBACK_MEMBER_LOCAL,
                { IROperand::local(info.index), IROperand::field(fieldIdx) }, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            emitIR(IROp::WRITEBACK_MEMBER_VAR,
                { IROperand::imm(info.index), IROperand::field(fieldIdx) }, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            emitIR(IROp::WRITEBACK_MEMBER_VAR,
                { IROperand::global(info.index), IROperand::field(fieldIdx) }, node->line);
        }
        // UPVALUE：不 emit 写回（简化）
    }
}

IROperand AstIRBuilder::visitMethodCall(MethodCall* node) {
    IROperand obj = visitNode(node->object.get());
    IROperand dest = ir_->allocVReg();
    uint32_t methodIdx = ir_->addGlobal(node->methodName);
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(obj);
    ops.push_back(IROperand::field(methodIdx));
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
    for (auto& a : node->arguments) {
        ops.push_back(visitNode(a.get()));
    }
    emitIR(IROp::METHOD_CALL, ops, node->line);
    return dest;
}

void AstIRBuilder::visitClassDecl(ClassDecl* node) {
    uint32_t nameIdx = ir_->addGlobal(node->name);
    emitIR(IROp::DEFINE_CLASS, { IROperand::funcName(nameIdx) }, node->line);
    // 遍历成员：对方法（FunDecl）调用 visitFunDecl（简化处理）
    for (auto& m : node->members) {
        if (m && m->nodeType == NodeType::NODE_FUN_DECL) {
            visitFunDecl(static_cast<FunDecl*>(m.get()));
        }
        // 其他成员（字段声明等）简化跳过
    }
}

void AstIRBuilder::visitBreakStmt(BreakStmt* node) {
    if (!loopStack_.empty()) {
        emitIR(IROp::JUMP, { IROperand::label(loopStack_.back().endLabel) }, node->line);
    }
}

void AstIRBuilder::visitContinueStmt(ContinueStmt* node) {
    if (!loopStack_.empty()) {
        emitIR(IROp::JUMP, { IROperand::label(loopStack_.back().continueLabel) }, node->line);
    }
}

void AstIRBuilder::visitTryStmt(TryStmt* node) {
    uint32_t catchLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    // try 块开始，记录 catch 跳转目标
    emitIR(IROp::TRY_BEGIN, { IROperand::label(catchLabel) }, node->line);
    if (node->tryBlock) visitNode(node->tryBlock.get());
    emitIR(IROp::TRY_END, {}, node->line);
    emitIR(IROp::JUMP, { IROperand::label(endLabel) }, node->line);
    // catch 块
    emitIR(IROp::LABEL, { IROperand::label(catchLabel) }, node->line);
    if (node->catchBlock) visitNode(node->catchBlock.get());
    emitIR(IROp::LABEL, { IROperand::label(endLabel) }, node->line);
}

void AstIRBuilder::visitThrowStmt(ThrowStmt* node) {
    IROperand val = node->expression ? visitNode(node->expression.get()) : IROperand::vreg(0);
    emitIR(IROp::THROW, { val }, node->line);
}

// ============================================================
// BytecodeIRBackend 实现
// ============================================================

BytecodeIRBackend::BytecodeIRBackend() : chunk_(nullptr) {
}

bool BytecodeIRBackend::lower(const IRFunction& ir) {
    // 全新 chunk，避免旧哈希表/状态残留
    chunk_ = std::make_unique<BytecodeChunk>();
    chunk_->name = ir.name;
    chunk_->arity = ir.arity;
    chunk_->requiredArity = ir.requiredArity;
    chunk_->localCount = ir.localCount;
    chunk_->defaultConstIndices = ir.defaultConstIndices;
    chunk_->upvalues = ir.upvalues;  // VM-05/06: 复制 upvalue 描述符
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear();  // 方向四：重置映射

    // 通过 addConstant 复制常量池（保持索引一致，并填充哈希表以便后续去重）
    for (const auto& c : ir.constants) {
        chunk_->addConstant(c);
    }

    // 遍历所有基本块的所有指令，逐条 lowering
    size_t instrIndex = 0;  // 方向四：展平的 IR 指令序号（与 IrViewer 一致）
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            // 方向四：记录 IR 指令 → 当前字节码偏移映射（lowering 前）
            irToBytecodeOffset_.push_back({instrIndex, chunk_->code.size()});
            if (!lowerInstruction(instr, ir)) {
                return false;
            }
            // 填充行号表（每条 IR 指令对应若干字节，统一用 instr.line）
            while (chunk_->lines.size() < chunk_->code.size()) {
                chunk_->lines.push_back(instr.line);
            }
            ++instrIndex;
        }
    }

    // 第二遍：回填跳转目标
    return patchJumps();
}

void BytecodeIRBackend::emitUint16(std::vector<uint8_t>& code, uint16_t v) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

uint16_t BytecodeIRBackend::addStringConstant(const std::string& s, const IRFunction& /*ir*/) {
    // 将字符串名加入常量池（复用 chunk_ 的哈希去重），返回索引
    return chunk_->addConstant(Value(s));
}

bool BytecodeIRBackend::lowerModule(const IRModule& module) {
    // 重置状态
    resetState();

    // 降低 main 函数 → chunk_
    if (module.mainFunction) {
        if (!lower(*module.mainFunction)) return false;
    } else {
        chunk_ = std::make_unique<BytecodeChunk>();
        chunk_->name = "main";
    }

    // 降低所有子函数 → functionChunks_
    // 使用独立 backend 避免覆盖 chunk_（main）
    for (const auto& fn : module.functions) {
        if (!fn) continue;
        BytecodeIRBackend fnBackend;
        if (!fnBackend.lower(*fn)) return false;
        auto fnChunk = fnBackend.takeChunk();
        if (fnChunk) {
            functionChunks_[fn->name] = std::move(*fnChunk);
        }
    }

    // 注：globalSlotNames_ 需从 AstIRBuilder 获取，但 IRModule 不携带槽位名表，
    // B4: slotNames_ 已删除，全局槽位名表从 AstIRBuilder.globalSlotAllocator_ 获取。
    return true;
}

void BytecodeIRBackend::resetState() {
    chunk_.reset();
    functionChunks_.clear();
    // B4: slotNames_ 已删除（dead member，全局槽位名表从 AstIRBuilder 获取）
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear();  // 方向四
}

bool BytecodeIRBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    // 辅助：从 globalNames 取名（索引越界时返回空串）
    auto globalName = [&](uint32_t idx) -> std::string {
        return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
    };

    switch (instr.op) {
    // ---- 常量加载 ----
    case IROp::LOAD_CONST: {
        // 按 Value 类型分发 OP_INT/OP_FLOAT/OP_STRING
        if (instr.operands.size() < 2) return false;
        uint32_t constIdx = instr.operands[1].index;
        if (constIdx >= ir.constants.size()) return false;
        const Value& v = ir.constants[constIdx];
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        switch (v.getType()) {
        case ValueType::VAL_INT:    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INT)); break;
        case ValueType::VAL_FLOAT:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FLOAT)); break;
        case ValueType::VAL_STRING: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_STRING)); break;
        default:                    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CONSTANT)); break;
        }
        emitUint16(chunk_->code, static_cast<uint16_t>(constIdx));
        break;
    }
    case IROp::LOAD_NULL: {
        if (instr.operands.empty()) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
        break;
    }
    case IROp::LOAD_TRUE: {
        if (instr.operands.empty()) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRUE));
        break;
    }
    case IROp::LOAD_FALSE: {
        if (instr.operands.empty()) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FALSE));
        break;
    }

    // ---- 变量访问 ----
    case IROp::LOAD_LOCAL: {
        if (instr.operands.size() < 2) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF));  // slot(1B)
        break;
    }
    case IROp::STORE_LOCAL: {
        if (instr.operands.size() < 2) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));  // slot(1B)
        break;
    }
    case IROp::LOAD_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_GET_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_GET_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        if (instr.operands[1].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[1].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::STORE_GLOBAL: {
        if (instr.operands.size() < 2) return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::DEFINE_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_DEFINE_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_DEFINE_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2) return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::LOAD_UPVALUE: {
        if (instr.operands.size() < 2) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF));
        break;
    }
    case IROp::STORE_UPVALUE: {
        if (instr.operands.size() < 2) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));
        break;
    }
    case IROp::CLOSE_UPVALUE: {
        if (instr.operands.empty()) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSE_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));
        break;
    }

    // ---- 算术 ----
    case IROp::ADD: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_ADD)); break;
    case IROp::SUB: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUBTRACT)); break;
    case IROp::MUL: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MULTIPLY)); break;
    case IROp::DIV: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DIVIDE)); break;
    case IROp::MOD: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MODULO)); break;
    case IROp::NEGATE: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NEGATE)); break;

    // ---- 比较 ----
    case IROp::EQ:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_EQUAL)); break;
    case IROp::NEQ: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT_EQUAL)); break;
    case IROp::LT:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS)); break;
    case IROp::GT:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER)); break;
    case IROp::LTE: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS_EQUAL)); break;
    case IROp::GTE: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER_EQUAL)); break;

    // ---- 逻辑 ----
    case IROp::NOT: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT)); break;

    // ---- 控制流 ----
    case IROp::LABEL: {
        // 记录标签对应的字节码偏移（不产生字节码）
        if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
            labelToOffset_[instr.operands[0].index] = chunk_->code.size();
        }
        break;
    }
    case IROp::JUMP: {
        if (instr.operands.empty()) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP));
        pendingJumps_.push_back({ chunk_->code.size(), instr.operands[0].index, false });
        emitUint16(chunk_->code, 0);  // 占位符，第二遍回填
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        if (instr.operands.size() < 2) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP_IF_FALSE));
        pendingJumps_.push_back({ chunk_->code.size(), instr.operands[1].index, false });
        emitUint16(chunk_->code, 0);  // 占位符
        break;
    }

    // ---- 调用 ----
    case IROp::CALL: {
        // CALL dest, name_idx, arg_count, args... → OP_CALL nameIdx argCount
        if (instr.operands.size() < 3) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF));  // argCount
        break;
    }
    case IROp::CALL_EXPR: {
        if (instr.operands.size() < 3) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL_EXPR));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF));  // argCount
        break;
    }
    case IROp::RETURN: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_RETURN));
        break;
    }
    case IROp::RETURN_NULL: {
        // RETURN_NULL 前先 emit OP_NULL（栈顶作为返回值）
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_RETURN));
        break;
    }

    // ---- 闭包 ----
    case IROp::MAKE_CLOSURE: {
        // MAKE_CLOSURE dest, name_idx, uv_count, [isLocal, idx]×uv_count
        if (instr.operands.size() < 3) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSURE));
        emitUint16(chunk_->code, nameConstIdx);
        uint32_t uvCount = instr.operands[2].index;
        chunk_->code.push_back(static_cast<uint8_t>(uvCount & 0xFF));
        for (uint32_t i = 0; i < uvCount; ++i) {
            size_t base = 3 + i * 2;
            if (base + 1 >= instr.operands.size()) return false;
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base].index & 0xFF));     // isLocal
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base + 1].index & 0xFF)); // idx
        }
        break;
    }

    // ---- 容器 ----
    case IROp::BUILD_ARRAY: {
        if (instr.operands.size() < 2) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_ARRAY));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF));  // count
        break;
    }
    case IROp::BUILD_DICT: {
        if (instr.operands.size() < 2) return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_DICT));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF));  // pairCount
        break;
    }
    case IROp::INDEX_GET: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INDEX_GET));
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        break;
    }
    case IROp::INDEX_SET: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INDEX_SET));
        break;
    }

    // ---- 成员访问 ----
    case IROp::MEMBER_GET: {
        if (instr.operands.size() < 3) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_GET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::MEMBER_SET: {
        if (instr.operands.size() < 3) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_SET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }

    // ---- 方法调用 ----
    case IROp::METHOD_CALL: {
        // METHOD_CALL dest, obj, method_idx, arg_count, args...
        // → OP_METHOD_CALL nameIdx(2B) argCount(1B) recvVarIdx(2B) recvSlot(1B)
        if (instr.operands.size() < 4) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_METHOD_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[3].index & 0xFF));  // argCount
        emitUint16(chunk_->code, 0xFFFF);  // recvVarIdx（简化占位）
        chunk_->code.push_back(0xFF);       // recvSlot（简化占位）
        break;
    }

    // ---- 类 ----
    case IROp::DEFINE_CLASS: {
        if (instr.operands.empty()) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_CLASS));
        emitUint16(chunk_->code, nameConstIdx);
        // 简化填充：OP_DEFINE_CLASS 共 5 字节，补 2 字节占位
        chunk_->code.push_back(0);
        chunk_->code.push_back(0);
        break;
    }
    case IROp::CLASS_NEW: {
        if (instr.operands.size() < 3) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLASS_NEW));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF));  // argCount
        break;
    }
    case IROp::INIT_FIELD: {
        if (instr.operands.empty()) return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INIT_FIELD));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }

    // ---- 异常 ----
    case IROp::TRY_BEGIN: {
        if (instr.operands.empty()) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRY_BEGIN));
        pendingJumps_.push_back({ chunk_->code.size(), instr.operands[0].index, false });
        emitUint16(chunk_->code, 0);  // catchOffset 占位符，第二遍回填
        break;
    }
    case IROp::TRY_END: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRY_END));
        break;
    }
    case IROp::THROW: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_THROW));
        break;
    }

    // ---- 写回指令（限制2）----
    case IROp::WRITEBACK_MEMBER_VAR: {
        // operands: [var_idx, field_idx]
        // → OP_WRITEBACK_MEMBER_VAR varIdx(2B) fieldIdx(2B)
        if (instr.operands.size() < 2) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_VAR));
        // varIdx(2B)：槽位号或名称常量索引
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        // fieldIdx(2B)：字段名常量索引
        uint16_t fieldConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_MEMBER_LOCAL: {
        // operands: [slot, field_idx]
        // → OP_WRITEBACK_MEMBER_LOCAL slot(1B) fieldIdx(2B)
        if (instr.operands.size() < 2) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));  // slot(1B)
        uint16_t fieldConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_INDEX_VAR: {
        // operands: [var_idx]
        // → OP_WRITEBACK_INDEX_VAR varIdx(2B)
        if (instr.operands.size() < 1) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_VAR));
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::WRITEBACK_INDEX_LOCAL: {
        // operands: [slot]
        // → OP_WRITEBACK_INDEX_LOCAL slot(1B)
        if (instr.operands.size() < 1) return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));  // slot(1B)
        break;
    }

    // ---- 其他 ----
    case IROp::PRINT: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_PRINT));
        break;
    }
    case IROp::POP: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_POP));
        break;
    }
    case IROp::DUP: {
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DUP));
        break;
    }

    default:
        Logger::Error("BytecodeIRBackend: 未知 IR 操作码 " +
                      std::to_string(static_cast<int>(instr.op)), "IR");
        return false;
    }
    return true;
}

bool BytecodeIRBackend::patchJumps() {
    // 遍历所有待回填跳转，查 labelToOffset 写回目标偏移
    for (const auto& pj : pendingJumps_) {
        auto it = labelToOffset_.find(pj.targetLabel);
        if (it == labelToOffset_.end()) {
            Logger::Error("BytecodeIRBackend: 未找到标签 " +
                          std::to_string(pj.targetLabel), "IR");
            return false;
        }
        // VM OP_JUMP/OP_JUMP_IF_FALSE/OP_TRY_BEGIN 用绝对偏移（ip = target）
        uint16_t target = static_cast<uint16_t>(it->second);
        if (pj.codeOffset + 1 >= chunk_->code.size()) {
            Logger::Error("BytecodeIRBackend: 跳转操作数越界", "IR");
            return false;
        }
        chunk_->code[pj.codeOffset] = static_cast<uint8_t>(target & 0xFF);
        chunk_->code[pj.codeOffset + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    return true;
}

// ============================================================
// IRToString — 调试输出
// ============================================================

std::string IRToString(const IRFunction& ir) {
    std::ostringstream oss;
    oss << "IRFunction \"" << ir.name << "\" {\n";
    oss << "  constants: " << ir.constants.size() << "\n";
    oss << "  globals: " << ir.globalNames.size() << "\n";
    oss << "  vregs: " << ir.nextVReg << "\n";
    oss << "  labels: " << ir.nextLabel << "\n";
    oss << "  blocks: " << ir.blocks.size() << "\n\n";

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& block = ir.blocks[bi];
        oss << "  BB" << bi << " (label=" << block.labelIndex << "):\n";
        for (const auto& instr : block.instructions) {
            oss << "    " << irOpName(instr.op);
            for (const auto& operand : instr.operands) {
                const char* kindStr = "?";
                switch (operand.kind) {
                case IROperandKind::CONSTANT:    kindStr = "c"; break;
                case IROperandKind::VIRTUAL:     kindStr = "v"; break;
                case IROperandKind::LABEL:       kindStr = "L"; break;
                case IROperandKind::GLOBAL_NAME: kindStr = "g"; break;
                case IROperandKind::LOCAL_SLOT:  kindStr = "s"; break;
                case IROperandKind::UPVALUE_IDX: kindStr = "u"; break;
                case IROperandKind::FIELD_NAME:  kindStr = "f"; break;
                case IROperandKind::FUNC_NAME:   kindStr = "fn"; break;
                case IROperandKind::IMM_UINT:    kindStr = "#"; break;
                }
                oss << " " << kindStr << operand.index;
            }
            if (instr.line > 0) oss << "  ; line " << instr.line;
            oss << "\n";
        }
        oss << "\n";
    }
    oss << "}\n";
    return oss.str();
}

// ============================================================
// IR 优化 Pass 实现（方向二）
// ============================================================

namespace {

/// 辅助：判断指令是否为"常量加载"（LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE）
/// 若是，返回 true 并输出对应的常量值
bool isConstLoad(const IRInstruction& instr, const IRFunction& ir, Value& outVal) {
    switch (instr.op) {
    case IROp::LOAD_CONST:
        if (instr.operands.size() < 2) return false;
        if (instr.operands[1].index >= ir.constants.size()) return false;
        outVal = ir.constants[instr.operands[1].index];
        return true;
    case IROp::LOAD_NULL:  outVal = Value::nullValue(); return true;
    case IROp::LOAD_TRUE:  outVal = Value(true); return true;
    case IROp::LOAD_FALSE: outVal = Value(false); return true;
    default: return false;
    }
}

/// 辅助：判断指令是否为纯计算（无副作用，仅定义 dest vreg）
bool isPureCompute(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
    case IROp::LOAD_LOCAL:
    case IROp::LOAD_GLOBAL:
    case IROp::LOAD_UPVALUE:
    case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::MOD:
    case IROp::NEGATE: case IROp::NOT:
    case IROp::EQ: case IROp::NEQ: case IROp::LT: case IROp::GT: case IROp::LTE: case IROp::GTE:
    case IROp::DUP:
        return true;
    default:
        return false;
    }
}

/// 辅助：对两个 Value 执行算术运算，成功返回 true
bool foldArith(IROp op, const Value& lhs, const Value& rhs, Value& result) {
    // 整数算术
    if (lhs.isInt() && rhs.isInt()) {
        int64_t a = lhs.intVal(), b = rhs.intVal();
        switch (op) {
        case IROp::ADD: result = Value(a + b); return true;
        case IROp::SUB: result = Value(a - b); return true;
        case IROp::MUL: result = Value(a * b); return true;
        case IROp::DIV:
            if (b == 0) return false;  // 除零不折叠，留待运行时报错
            result = Value(a / b); return true;
        case IROp::MOD:
            if (b == 0) return false;
            result = Value(a % b); return true;
        default: return false;
        }
    }
    // 浮点算术（至少一方为 float）
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::ADD: result = Value(a + b); return true;
        case IROp::SUB: result = Value(a - b); return true;
        case IROp::MUL: result = Value(a * b); return true;
        case IROp::DIV:
            if (b == 0.0) return false;
            result = Value(a / b); return true;
        case IROp::MOD:
            if (b == 0.0) return false;
            // C++ fmod
            result = Value(std::fmod(a, b)); return true;
        default: return false;
        }
    }
    return false;
}

/// 辅助：对两个 Value 执行比较运算，成功返回 true
bool foldCompare(IROp op, const Value& lhs, const Value& rhs, Value& result) {
    // 同类型比较
    if (lhs.isInt() && rhs.isInt()) {
        int64_t a = lhs.intVal(), b = rhs.intVal();
        switch (op) {
        case IROp::EQ:  result = Value(a == b); return true;
        case IROp::NEQ: result = Value(a != b); return true;
        case IROp::LT:  result = Value(a < b); return true;
        case IROp::GT:  result = Value(a > b); return true;
        case IROp::LTE: result = Value(a <= b); return true;
        case IROp::GTE: result = Value(a >= b); return true;
        default: return false;
        }
    }
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::EQ:  result = Value(a == b); return true;
        case IROp::NEQ: result = Value(a != b); return true;
        case IROp::LT:  result = Value(a < b); return true;
        case IROp::GT:  result = Value(a > b); return true;
        case IROp::LTE: result = Value(a <= b); return true;
        case IROp::GTE: result = Value(a >= b); return true;
        default: return false;
        }
    }
    if (lhs.isBool() && rhs.isBool()) {
        bool a = lhs.boolVal(), b = rhs.boolVal();
        switch (op) {
        case IROp::EQ:  result = Value(a == b); return true;
        case IROp::NEQ: result = Value(a != b); return true;
        default: return false;  // bool 不支持 < > <= >=
        }
    }
    if (lhs.isNull() && rhs.isNull()) {
        switch (op) {
        case IROp::EQ:  result = Value(true); return true;
        case IROp::NEQ: result = Value(false); return true;
        default: return false;
        }
    }
    return false;
}

/// 辅助：对 Value 执行一元运算
bool foldUnary(IROp op, const Value& operand, Value& result) {
    switch (op) {
    case IROp::NEGATE:
        if (operand.isInt()) { result = Value(-operand.intVal()); return true; }
        if (operand.isFloat()) { result = Value(-operand.floatVal()); return true; }
        return false;
    case IROp::NOT:
        if (operand.isBool()) { result = Value(!operand.boolVal()); return true; }
        return false;
    default: return false;
    }
}

} // anonymous namespace

// ---- 常量折叠 ----
bool constantFoldingPass(IRFunction& ir) {
    bool modified = false;
    // 建立 vreg → 定义指令的映射（仅纯计算指令，且 dest 为首操作数）
    // vregDefs[v] = {blockIdx, instrIdx}
    std::unordered_map<uint32_t, std::pair<size_t, size_t>> vregDefs;
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < ir.blocks[bi].instructions.size(); ++ii) {
            const auto& instr = ir.blocks[bi].instructions[ii];
            if (instr.operands.empty()) continue;
            if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
                vregDefs[instr.operands[0].index] = { bi, ii };
            }
        }
    }

    // 遍历所有算术/比较/一元指令，若操作数均为常量加载则折叠
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < ir.blocks[bi].instructions.size(); ++ii) {
            auto& instr = ir.blocks[bi].instructions[ii];
            // 二元运算：ADD/SUB/MUL/DIV/MOD/EQ/NEQ/LT/GT/LTE/GTE
            if (instr.operands.size() == 3) {
                IROp op = instr.op;
                bool isArith = (op == IROp::ADD || op == IROp::SUB || op == IROp::MUL ||
                               op == IROp::DIV || op == IROp::MOD);
                bool isCompare = (op == IROp::EQ || op == IROp::NEQ || op == IROp::LT ||
                                  op == IROp::GT || op == IROp::LTE || op == IROp::GTE);
                if (!isArith && !isCompare) continue;

                // dest, src1, src2
                IROperand dest = instr.operands[0];
                IROperand src1 = instr.operands[1];
                IROperand src2 = instr.operands[2];
                if (src1.kind != IROperandKind::VIRTUAL || src2.kind != IROperandKind::VIRTUAL) continue;

                // 查找 src1/src2 的定义
                auto it1 = vregDefs.find(src1.index);
                auto it2 = vregDefs.find(src2.index);
                if (it1 == vregDefs.end() || it2 == vregDefs.end()) continue;

                // 获取定义指令
                const auto& def1 = ir.blocks[it1->second.first].instructions[it1->second.second];
                const auto& def2 = ir.blocks[it2->second.first].instructions[it2->second.second];

                Value v1, v2;
                if (!isConstLoad(def1, ir, v1) || !isConstLoad(def2, ir, v2)) continue;

                // 尝试折叠
                Value folded;
                bool ok = isArith ? foldArith(op, v1, v2, folded) : foldCompare(op, v1, v2, folded);
                if (!ok) continue;

                // 替换：原指令改为 LOAD_CONST dest, constIdx
                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = { dest, IROperand::constant(constIdx) };
                // 更新 vregDefs 中 dest 的定义
                vregDefs[dest.index] = { bi, ii };
                modified = true;
            }
            // 一元运算：NEGATE/NOT
            else if (instr.operands.size() == 2) {
                IROp op = instr.op;
                if (op != IROp::NEGATE && op != IROp::NOT) continue;

                IROperand dest = instr.operands[0];
                IROperand src = instr.operands[1];
                if (src.kind != IROperandKind::VIRTUAL) continue;

                auto it = vregDefs.find(src.index);
                if (it == vregDefs.end()) continue;
                const auto& def = ir.blocks[it->second.first].instructions[it->second.second];

                Value v;
                if (!isConstLoad(def, ir, v)) continue;

                Value folded;
                if (!foldUnary(op, v, folded)) continue;

                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = { dest, IROperand::constant(constIdx) };
                vregDefs[dest.index] = { bi, ii };
                modified = true;
            }
        }
    }
    return modified;
}

// ---- 死代码消除 ----
bool deadCodeEliminationPass(IRFunction& ir) {
    // 第一遍：收集所有被引用的 vreg
    std::unordered_set<uint32_t> usedVRegs;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            // dest（首操作数）不算"被引用"，只有后续指令引用才算
            for (size_t i = 0; i < instr.operands.size(); ++i) {
                // 对于纯计算指令，首操作数是 dest，跳过
                // 对于 STORE_* 等指令，首操作数可能是 slot/uv_idx 而非 vreg
                if (i == 0 && isPureCompute(instr.op)) continue;
                if (instr.operands[i].kind == IROperandKind::VIRTUAL) {
                    usedVRegs.insert(instr.operands[i].index);
                }
            }
        }
    }

    // 第二遍：删除 dest 未被引用的纯计算指令
    bool modified = false;
    for (auto& block : ir.blocks) {
        std::vector<IRInstruction> kept;
        kept.reserve(block.instructions.size());
        for (auto& instr : block.instructions) {
            if (isPureCompute(instr.op) && !instr.operands.empty() &&
                instr.operands[0].kind == IROperandKind::VIRTUAL) {
                uint32_t destVReg = instr.operands[0].index;
                if (usedVRegs.find(destVReg) == usedVRegs.end()) {
                    // dest 从未被引用，删除此指令
                    modified = true;
                    continue;
                }
            }
            kept.push_back(std::move(instr));
        }
        block.instructions = std::move(kept);
    }
    return modified;
}

// ---- 复制传播 ----
// PERF-15: 寄存器式 lowering 已启用，复制传播可以安全使用。
// 规则：
//   1. 记录每个 vreg 的"定义"：LOAD_CONST c → 该 vreg 等价于 constant c
//   2. 后续指令引用该 vreg 时，直接用 constant 替换
//   3. 跨 STORE_LOCAL/STORE_GLOBAL/STORE_UPVALUE 后不再传播（变量可能被修改）
//   4. 在基本块边界重置（保守策略，避免跨块分析复杂性）
// 注意：复制传播替换操作数后，原 LOAD_CONST 指令的 dest 变为无引用，
//       由后续的死代码消除 pass 删除。
bool copyPropagationPass(IRFunction& ir) {
    bool modified = false;
    // vreg → (常量索引, 类型) 映射
    // 类型标记区分 LOAD_CONST / LOAD_NULL / LOAD_TRUE / LOAD_FALSE
    enum class ConstKind { NONE, CONST, NULL_, TRUE_, FALSE_ };
    struct ConstDef {
        ConstKind kind = ConstKind::NONE;
        uint32_t constIdx = 0;
    };
    std::unordered_map<uint32_t, ConstDef> vregConst;

    for (auto& block : ir.blocks) {
        vregConst.clear();  // 基本块边界重置
        for (auto& instr : block.instructions) {
            // 替换操作数中的 vreg 为等价常量
            for (size_t i = 0; i < instr.operands.size(); ++i) {
                if (instr.operands[i].kind == IROperandKind::VIRTUAL) {
                    auto it = vregConst.find(instr.operands[i].index);
                    if (it != vregConst.end() && it->second.kind != ConstKind::NONE) {
                        // 替换为等价常量
                        switch (it->second.kind) {
                        case ConstKind::CONST:
                            instr.operands[i] = IROperand::constant(it->second.constIdx);
                            modified = true;
                            break;
                        case ConstKind::NULL_:
                            // 不替换为 LOAD_NULL 的操作数（需要新增指令）
                            // 保持 vreg 引用，由 DCE 删除 LOAD_NULL
                            break;
                        case ConstKind::TRUE_:
                        case ConstKind::FALSE_:
                            // 同上，不替换
                            break;
                        case ConstKind::NONE:
                            break;
                        }
                    }
                }
            }

            // 记录本指令的 dest 定义
            if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::VIRTUAL) {
                uint32_t destVReg = instr.operands[0].index;
                switch (instr.op) {
                case IROp::LOAD_CONST:
                    if (instr.operands.size() >= 2) {
                        vregConst[destVReg] = {ConstKind::CONST, instr.operands[1].index};
                    }
                    break;
                case IROp::LOAD_NULL:
                    vregConst[destVReg] = {ConstKind::NULL_, 0};
                    break;
                case IROp::LOAD_TRUE:
                    vregConst[destVReg] = {ConstKind::TRUE_, 0};
                    break;
                case IROp::LOAD_FALSE:
                    vregConst[destVReg] = {ConstKind::FALSE_, 0};
                    break;
                default:
                    // 其他指令定义 dest，清除等价关系
                    vregConst.erase(destVReg);
                    break;
                }
            }

            // STORE_* 指令可能修改外部状态，清除所有等价关系（保守策略）
            // 实际上只需清除被修改变量的等价关系，但简化处理
            if (instr.op == IROp::STORE_LOCAL || instr.op == IROp::STORE_GLOBAL ||
                instr.op == IROp::STORE_UPVALUE || instr.op == IROp::DEFINE_GLOBAL) {
                // 清除局部变量的等价关系（简化：不清除，因为 vreg 是 SSA 风格）
                // 实际上 vreg 不会被 STORE 修改，只有 LOCAL_SLOT 概念会
            }
        }
    }
    return modified;
}

// ---- 运行全部优化 pass ----
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation) {
    bool modified = false;
    // PERF-15: 常量折叠 → [复制传播] → 死代码消除
    // 复制传播仅在寄存器式后端启用（栈式后端删除 LOAD_CONST 会导致栈下溢）
    // 多轮迭代直到收敛（最多 3 轮，避免无限循环）
    for (int round = 0; round < 3; ++round) {
        bool m1 = constantFoldingPass(ir);
        bool m2 = enableCopyPropagation ? copyPropagationPass(ir) : false;
        bool m3 = deadCodeEliminationPass(ir);
        modified = modified || m1 || m2 || m3;
        if (!m1 && !m2 && !m3) break;  // 收敛
    }
    if (modified) {
        Logger::Info("IR 优化完成: " + std::to_string(ir.constants.size()) + " 常量, " +
                     std::to_string(ir.nextVReg) + " vreg" +
                     (enableCopyPropagation ? " (含复制传播)" : ""), "IR-Optimize");
    }
    return modified;
}
