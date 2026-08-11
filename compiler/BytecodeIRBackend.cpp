#include "ast/ASTNode.h"
#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: IR 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/RuntimeLimits.h"   // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"             // R109 TCO: isTailRecursiveReturn（与 Compiler.cpp 共享识别逻辑）
#include "common/TypeChecker.h"     // R163 泛型扩展: isTypeParameter（emitTypeCheckIR 擦除）
#include "compiler/BytecodeCache.h" // L11: 预编译模块 .minic 加载
#include "compiler/IR.h"
#include "interpreter/NumericUtils.h" // #14: OverflowCheck
#include "interpreter/Value.h"
#include "lexer/Lexer.h"   // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h" // VM-IMPORT: 模块源码语法分析
#include <algorithm>       // P2-11: std::sort for namespace import
#include <cassert>
#include <cmath>
#include <filesystem> // L11: 源码路径推导
#include <sstream>
#include <unordered_set>

// ============================================================
// BytecodeIRBackend 实现
// ============================================================

BytecodeIRBackend::BytecodeIRBackend() : chunk_(nullptr) {}

bool BytecodeIRBackend::lower(const IRFunction& ir) {
    // 全新 chunk，避免旧哈希表/状态残留
    chunk_ = std::make_unique<BytecodeChunk>();
    chunk_->name = ir.name;
    // 栈式 VM 约定：方法的 arity/requiredArity 不含 this（this 作为 slot 0 单独推送）。
    // 但 IR 在 visitFunDecl 中为 RegVM 约定对方法 arity/requiredArity += 1（含 this）。
    // 此处还原为栈式 VM 约定：用 IRFunction::isMethod 显式标志（D6 fix，不再从函数名
    // 含 '.' 推断——嵌套类/模块函数名同样可能含 '.'），减去 this。
    // 否则栈 VM 的 executeCall/executeClassNew 会因 argCount < requiredArity 触发
    // "构造函数 init 期望 1-1 个参数，但传入了 0 个" 错误（回归：NestedMemberAccessPush）。
    const bool isMethod = ir.isMethod;
    chunk_->arity = isMethod ? (ir.arity > 0 ? ir.arity - 1 : 0) : ir.arity;
    chunk_->requiredArity = isMethod ? (ir.requiredArity > 0 ? ir.requiredArity - 1 : 0) : ir.requiredArity;
    chunk_->localCount = ir.localCount;
    chunk_->defaultConstIndices = ir.defaultConstIndices;
    chunk_->upvalues = ir.upvalues; // VM-05/06: 复制 upvalue 描述符
    // BUG-IDE-12 fix: 复制 slot→name 映射，供栈式 VM 条件断点求值反查变量名
    chunk_->localSlotNames = ir.localSlotNames;
    // R164 协程/生成器：复制生成器标记和 yieldCount
    chunk_->isGenerator = ir.isGenerator;
    chunk_->yieldCount = ir.yieldCount;
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear(); // 方向四：重置映射

    // 通过 addConstant 复制常量池（保持索引一致，并填充哈希表以便后续去重）
    for (const auto& c : ir.constants) {
        chunk_->addConstant(c);
    }

    // 遍历所有基本块的所有指令，逐条 lowering
    size_t instrIndex = 0; // 方向四：展平的 IR 指令序号（与 IrViewer 一致）
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            // 方向四：记录 IR 指令 → 当前字节码偏移映射（lowering 前）
            irToBytecodeOffset_.push_back({instrIndex, chunk_->code.size()});
            if (!lowerInstruction(instr, ir)) {
                return false;
            }
            // 填充行号表（每条 IR 指令对应若干字节，统一用 instr.line）
            // D12 fix: BUG-IBACKEND-2 修复——columns 填充 instr.column（原恒 0）
            while (chunk_->lines.size() < chunk_->code.size()) {
                chunk_->lines.push_back(instr.line);
                chunk_->columns.push_back(instr.column);
            }
            ++instrIndex;
        }
    }

    // 第二遍：回填跳转目标
    if (!patchJumps())
        return false;

    // L1 fix: 将 IR 指令范围的 slotNameRanges 翻译为字节码 IP 范围。
    // irToBytecodeOffset_ 是 (instrIndex, bytecodeOffset) 的有序映射，
    // 用二分查找将 startInstr/endInstr 转为字节码 IP。endInstr=0 表示函数级
    // 变量（未关闭），startIp=对应字节码偏移，endIp=chunk 末尾（fallback 到 localSlotNames）。
    if (!ir.slotNameRanges.empty() && !irToBytecodeOffset_.empty()) {
        auto translateInstr = [this](size_t instrIdx) -> size_t {
            // 在 irToBytecodeOffset_ 中找 instrIdx 对应的字节码偏移
            // lower 级查找：找最大的 (instrIndex <= instrIdx) 的 bytecodeOffset
            size_t lo = 0, hi = irToBytecodeOffset_.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (irToBytecodeOffset_[mid].first <= instrIdx)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            // lo 指向第一个 first > instrIdx 的位置，回退一个是 <= instrIdx 的最大值
            if (lo == 0)
                return irToBytecodeOffset_[0].second;
            return irToBytecodeOffset_[lo - 1].second;
        };
        for (const auto& range : ir.slotNameRanges) {
            BytecodeChunk::SlotNameRange out;
            out.slot = static_cast<uint8_t>(range.slot);
            out.name = range.name;
            out.startIp = translateInstr(range.startInstr);
            // endInstr=0 表示函数级变量（参数/this/字段/内嵌函数），endIp 设为 0
            // 使 resolveSlotName 不匹配此 range，fallback 到 localSlotNames。
            if (range.endInstr == 0) {
                out.endIp = 0;
            } else {
                // endInstr 是 CLOSE_UPVALUE 之后的指令序号，翻译为对应字节码偏移
                out.endIp = translateInstr(range.endInstr);
            }
            chunk_->slotNameRanges.push_back(std::move(out));
        }
    }
    return true;
}

void BytecodeIRBackend::emitUint16(std::vector<uint8_t>& code, uint16_t v) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

uint16_t BytecodeIRBackend::addStringConstant(const std::string& s, const IRFunction& /*ir*/) {
    // 将字符串名加入常量池（复用 chunk_ 的哈希去重），返回索引
    return chunk_->addConstant(Value(s));
}

bool BytecodeIRBackend::slotToNameConstant(uint32_t slot, const IRFunction& ir, uint16_t& outIdx) {
    // BUG-NEW fix: GLOBAL_SLOT (IMM_UINT) → 变量名 → 字符串常量索引
    // globalSlotNames_ 由 lowerModule 从 IRModule 设置；若未设置（独立 lower 调用），
    // 回退到空名并记录错误，避免静默产生坏字节码。
    // BUG-IR-SLOTNAME-1 fix: 失败时不再生成占位名 "__unknown_slot_X__" 静默产生坏字节码，
    // 改为返回 false 让调用方中止 lowering 并向上传递错误。
    if (!globalSlotNames_ || slot >= globalSlotNames_->size()) {
        Logger::Error("BytecodeIRBackend: slotToNameConstant 槽位名表缺失或越界 (slot=" + std::to_string(slot) + ")",
                      "IR");
        return false;
    }
    const std::string& name = (*globalSlotNames_)[slot];
    (void)ir; // ir 仅用于签名一致性，addStringConstant 不依赖 ir
    outIdx = chunk_->addConstant(Value(name));
    return true;
}

bool BytecodeIRBackend::lowerModule(const IRModule& module) {
    // 重置状态
    resetState();

    // BUG-NEW fix: 保存全局槽位名表，供 lowerInstruction 中 WRITEBACK_*_VAR
    // IMM_UINT 分支将 slot→name 转为字符串常量索引。
    globalSlotNames_ = &module.globalSlotNames;

    // 降低 main 函数 → chunk_
    if (module.mainFunction) {
        if (!lower(*module.mainFunction))
            return false;
    } else {
        chunk_ = std::make_unique<BytecodeChunk>();
        chunk_->name = "main";
    }

    // 降低所有子函数 → functionChunks_
    // 使用独立 backend 避免覆盖 chunk_（main）
    for (const auto& fn : module.functions) {
        if (!fn)
            continue;
        BytecodeIRBackend fnBackend;
        // 子函数共享同一全局槽位名表
        fnBackend.globalSlotNames_ = globalSlotNames_;
        if (!fnBackend.lower(*fn))
            return false;
        auto fnChunk = fnBackend.takeChunk();
        if (fnChunk) {
            functionChunks_[fn->name] = std::move(*fnChunk);
        }
    }

    // 全局槽位名表从 AstIRBuilder.globalSlotAllocator_ 获取。
    return true;
}

void BytecodeIRBackend::resetState() {
    chunk_.reset();
    functionChunks_.clear();
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear(); // 方向四
}

bool BytecodeIRBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    // ============================================================
    // 单条 IR 指令 → 栈式 VM 字节码 的 lowering 核心（按 IROp 类别分发）。
    // ------------------------------------------------------------
    // 设计要点（关键算法，逐步说明）：
    //   1. 栈式 VM 是「操作数栈机」：每条 IROp 对应固定次数的压栈/弹栈。
    //      例如 ADD 弹 2 压 1，LOAD_CONST 压 1，POP 弹 1，JUMP 不碰栈。
    //      因为 IR 是 SSA 风格（每个 vreg 只被定义一次、按定义顺序被使用），
    //      操作数在运行时的入栈顺序与 IR 指令顺序天然一致，lowering 无需
    //      显式维护「vreg→栈偏移」映射也能保证栈平衡——这正是栈式后端比
    //      寄存器式后端简单的原因（寄存器式需在 vregToReg 阶段物化每个 vreg）。
    //   2. 发射布局：每个 case 直接 push_back opcode，再按该指令的定长格式
    //      push 操作数（2 字节小端 short 或 1 字节 slot）。变长指令（仅
    //      OP_CLOSURE）在编译期已展开为「nameIdx + upvalueCount + 2*描述符」。
    //   3. vregStackDepth_ 辅助表：对每条产生 dest vreg 的 load 指令，记录其
    //      发射位置 (code offset)。它实际不用于正确性判定（见要点 1），而是作为
    //      调试/潜在栈深度分析的辅助信息维护；绝不可据其反向推算栈偏移。
    //   4. 控制流两遍法：LABEL 不产生字节码，仅写入 labelToOffset_；跳转类
    //      （JUMP / JUMP_IF_FALSE）先 push opcode 并占位 2 字节操作数，把待回填
    //      项加入 pendingJumps_。本函数（第一遍）结束后由 patchJumps()（第二遍）
    //      查 labelToOffset_ 回填绝对/相对偏移（见 BUG-EXC-1：OP_TRY_BEGIN 用
    //      相对偏移，其余用绝对偏移）。
    //   5. 防御性边界检查：槽位/索引 >=256 或常量池索引越界时 Logger::Error
    //      并返回 false，让 lower() 中止并向上传播错误，避免静态截断生成坏字节码。
    //   6. WRITEBACK_*_VAR 的特殊处理：其 GLOBAL_SLOT 以 IMM_UINT 编码槽位号，
    //      但栈式 VM 的 OP_WRITEBACK_*_VAR 把操作数当作「常量池中的变量名索引」，
    //      故需经 slotToNameConstant() 转换为变量名常量索引（否则误读为未定义变量）。
    //
    // 重构：原 841 行单 switch 拆分为 8 个类别 helper（lowerConstOp/lowerVarOp/
    // lowerArithOp/lowerCompareOp/lowerControlOp/lowerCallOp/lowerContainerOp/
    // lowerMiscOp），主函数仅保留 switch 外壳做类别分发。语义零变更，所有审计
    // 注释（BUG-IR-BOUND-1/AUDIT-STACKCLOSURE/BUG-EXC-1/AUDIT-P1.1/AUDIT-P2/
    // BUG-INH-1/BUG-INH-IR-1/BUG-NEW/BUG-IR-SLOTNAME-1/BUG-IR-TRY-1 等）原样
    // 保留至各 helper 对应 case。原 globalName lambda 改为 globalNameOf 静态方法。
    // ============================================================
    switch (instr.op) {
    // ---- 常量加载 ----
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
        return lowerConstOp(instr, ir);

    // ---- 变量访问 ----
    case IROp::LOAD_LOCAL:
    case IROp::STORE_LOCAL:
    case IROp::LOAD_GLOBAL:
    case IROp::STORE_GLOBAL:
    case IROp::DEFINE_GLOBAL:
    case IROp::DELETE_VAR:
    case IROp::LOAD_UPVALUE:
    case IROp::STORE_UPVALUE:
    case IROp::CLOSE_UPVALUE:
        return lowerVarOp(instr, ir);

    // ---- 算术 + 逻辑 ----
    case IROp::ADD:
    case IROp::SUB:
    case IROp::MUL:
    case IROp::DIV:
    case IROp::MOD:
    case IROp::NEGATE:
    case IROp::NOT:
        return lowerArithOp(instr, ir);

    // ---- 比较 ----
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE:
        return lowerCompareOp(instr, ir);

    // ---- 控制流 ----
    case IROp::LABEL:
    case IROp::JUMP:
    case IROp::JUMP_IF_FALSE:
    // R164 协程/生成器：YIELD 可中断执行（抛 VMYieldSignal），归入控制流组
    case IROp::YIELD:
    // 七特性 MVP 阶段 4：AWAIT 同步 drain（可能抛异常/错误），归入控制流组
    case IROp::AWAIT:
        return lowerControlOp(instr, ir);

    // ---- 调用 + 闭包 ----
    case IROp::CALL:
    case IROp::CALL_EXPR:
    case IROp::TAIL_CALL: // L18 eng-tailcall
    case IROp::RETURN:
    case IROp::RETURN_NULL:
    case IROp::MAKE_CLOSURE:
        return lowerCallOp(instr, ir);

    // ---- 容器 + 成员访问 + 方法调用 ----
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
    case IROp::INDEX_GET:
    case IROp::INDEX_SET:
    case IROp::MEMBER_GET:
    case IROp::MEMBER_SET:
    case IROp::MEMBER_SET_LOCAL: // L7 fix
    case IROp::SUPER_MEMBER_GET:
    case IROp::METHOD_CALL:
    case IROp::SUPER_CALL:
    // R99 枚举与 ADT：enum variant 构造/检查/取字段（与容器指令同属 lowerContainerOp）
    case IROp::BUILD_ENUM_VARIANT:
    case IROp::ENUM_VARIANT_NAME:
    case IROp::ENUM_VARIANT_FIELD:
    // R134 模式匹配扩展：容器长度（与容器指令同属 lowerContainerOp，复用 OP_LEN）
    case IROp::LEN:
        return lowerContainerOp(instr, ir);

    // ---- 其他（类/异常/写回/杂项）----
    case IROp::DEFINE_CLASS:
    case IROp::CLASS_NEW:
    case IROp::INIT_FIELD:
    case IROp::TRY_BEGIN:
    case IROp::TRY_END:
    case IROp::THROW:
    case IROp::LOAD_EXCEPTION:
    case IROp::PUSH_JUMP_TARGET:
    case IROp::FINALLY_END:
    case IROp::WRITEBACK_MEMBER_VAR:
    case IROp::WRITEBACK_MEMBER_LOCAL:
    case IROp::WRITEBACK_INDEX_VAR:
    case IROp::WRITEBACK_INDEX_LOCAL:
    case IROp::WRITEBACK_MEMBER_UPVALUE:
    case IROp::WRITEBACK_INDEX_UPVALUE:
    case IROp::PRINT:
    case IROp::POP:
    case IROp::DUP:
    case IROp::LOAD_MUTATED:
    case IROp::TYPE_CHECK:
    case IROp::TYPE_TEST: // R134: 软类型测试，路由到 lowerMiscOp
        return lowerMiscOp(instr, ir);

    default:
        Logger::Error("BytecodeIRBackend: 未知 IR 操作码 " + std::to_string(static_cast<int>(instr.op)), "IR");
        return false;
    }
}

// 辅助：从 globalNames 取名（索引越界时返回空串）。原 lowerInstruction 中的 lambda，重构为静态方法。
std::string BytecodeIRBackend::globalNameOf(const IRFunction& ir, uint32_t idx) {
    return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
}

bool BytecodeIRBackend::lowerConstOp(const IRInstruction& instr, const IRFunction& ir) {
    // ---- 常量加载 ----
    switch (instr.op) {
    case IROp::LOAD_CONST: {
        // 按 Value 类型分发 OP_INT/OP_FLOAT/OP_STRING
        if (instr.operands.size() < 2)
            return false;
        uint32_t constIdx = instr.operands[1].index;
        if (constIdx >= ir.constants.size())
            return false;
        const Value& v = ir.constants[constIdx];
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        switch (v.getType()) {
        case ValueType::VAL_INT:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INT));
            break;
        case ValueType::VAL_FLOAT:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FLOAT));
            break;
        case ValueType::VAL_STRING:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_STRING));
            break;
        default:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CONSTANT));
            break;
        }
        emitUint16(chunk_->code, static_cast<uint16_t>(constIdx));
        break;
    }
    case IROp::LOAD_NULL: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
        break;
    }
    case IROp::LOAD_TRUE: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRUE));
        break;
    }
    case IROp::LOAD_FALSE: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FALSE));
        break;
    }
    default:
        return false; // 非本类别（不应触达，主 dispatcher 已按类别分发）
    }
    return true;
}

bool BytecodeIRBackend::lowerVarOp(const IRInstruction& instr, const IRFunction& ir) {
    // ---- 变量访问 ----
    switch (instr.op) {
    case IROp::LOAD_LOCAL: {
        if (instr.operands.size() < 2)
            return false;
        // slot 编码为 1 字节，超 255 静默截断会读写错误栈槽
        if (instr.operands[1].index >= 256) {
            Logger::Error(
                "IR lowering: 局部变量槽位索引超出 255 上限 (slot=" + std::to_string(instr.operands[1].index) + ")",
                "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index)); // slot(1B)
        break;
    }
    case IROp::STORE_LOCAL: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error(
                "IR lowering: 局部变量槽位索引超出 255 上限 (slot=" + std::to_string(instr.operands[0].index) + ")",
                "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        // 注意：OP_SET_LOCAL 用 peek(0) 不消费栈顶值（与 Compiler.cpp 一致）。
        // IR 的 and/or 短路逻辑依赖此行为：STORE_LOCAL 将值写入 temp slot 后值仍在栈上，
        // 后续 LOAD_LOCAL 从同一 slot 读取。需要消费源值的场景（如 super.method() 的
        // LOAD_MUTATED + STORE_LOCAL）应在 IR 层面显式 emit IROp::POP。
        break;
    }
    case IROp::LOAD_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_GET_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_GET_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2)
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        if (instr.operands[1].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[1].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::STORE_GLOBAL: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::DEFINE_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_DEFINE_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_DEFINE_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::DELETE_VAR: {
        // BUG-IR-TRY-1 fix: 删除全局变量（catch 块退出后清理 catch 变量）
        // operands: [global_idx]，kind 可为 IMM_UINT (GLOBAL_SLOT) 或 GLOBAL_NAME
        // → OP_DELETE_VAR nameConstIdx(2B)（栈式 VM 按名称常量删除）
        if (instr.operands.size() < 1)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            // BUG-IR-SLOTNAME-1 fix: slot→name 转换可能失败，需检查返回值
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DELETE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DELETE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::LOAD_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[1].index >= 256) {
            Logger::Error(
                "IR lowering: upvalue 索引超出 255 上限 (idx=" + std::to_string(instr.operands[1].index) + ")", "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index));
        break;
    }
    case IROp::STORE_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error(
                "IR lowering: upvalue 索引超出 255 上限 (idx=" + std::to_string(instr.operands[0].index) + ")", "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        // 注意：OP_SET_UPVALUE 用 peek(0) 不消费栈顶值（与 STORE_LOCAL 一致）。
        // 需要消费源值的场景应在 IR 层面显式 emit IROp::POP。
        break;
    }
    case IROp::CLOSE_UPVALUE: {
        // B1 fix: operand 现为 slot_base（块作用域基址），非 upvalue 索引。
        // 运行时关闭所有指向 slot >= basePointer+slot_base 的 open upvalues。
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: CLOSE_UPVALUE slot_base 超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSE_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerArithOp(const IRInstruction& instr, const IRFunction& /*ir*/) {
    // ---- 算术 + 逻辑（无操作数，单字节 opcode）----
    switch (instr.op) {
    case IROp::ADD:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_ADD));
        break;
    case IROp::SUB:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUBTRACT));
        break;
    case IROp::MUL:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MULTIPLY));
        break;
    case IROp::DIV:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DIVIDE));
        break;
    case IROp::MOD:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MODULO));
        break;
    case IROp::NEGATE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NEGATE));
        break;
    case IROp::NOT:
        // ---- 逻辑 ----
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT));
        break;
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerCompareOp(const IRInstruction& instr, const IRFunction& /*ir*/) {
    // ---- 比较（无操作数，单字节 opcode）----
    switch (instr.op) {
    case IROp::EQ:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_EQUAL));
        break;
    case IROp::NEQ:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT_EQUAL));
        break;
    case IROp::LT:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS));
        break;
    case IROp::GT:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER));
        break;
    case IROp::LTE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS_EQUAL));
        break;
    case IROp::GTE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER_EQUAL));
        break;
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerControlOp(const IRInstruction& instr, const IRFunction& /*ir*/) {
    // ---- 控制流 ----
    switch (instr.op) {
    case IROp::LABEL: {
        // 记录标签对应的字节码偏移（不产生字节码）
        if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
            labelToOffset_[instr.operands[0].index] = chunk_->code.size();
        }
        break;
    }
    case IROp::JUMP: {
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP));
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false});
        emitUint16(chunk_->code, 0); // 占位符，第二遍回填
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        if (instr.operands.size() < 2)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP_IF_FALSE));
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[1].index, false});
        emitUint16(chunk_->code, 0); // 占位符
        break;
    }
    // R164 协程/生成器：YIELD dest, src → OP_YIELD（1 字节无操作数，值在栈顶）
    // 语义：src 的值已由前序 LOAD_* 指令压栈，OP_YIELD peek 栈顶：
    //   - 命中目标 yieldId：抛 VMYieldSignal 返回 yield 值
    //   - 未命中：保留栈顶值作为 yield 表达式结果（dest 继承 src 的栈位置）
    case IROp::YIELD: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_YIELD));
        break;
    }
    // 七特性 MVP 阶段 4：AWAIT dest, src → OP_AWAIT（1 字节无操作数，值在栈顶）
    // 语义：src 的值已由前序指令压栈，OP_AWAIT pop 后同步 drain 协程并 push 最终值
    // （dest 继承 src 的栈位置）。
    case IROp::AWAIT: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_AWAIT));
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerCallOp(const IRInstruction& instr, const IRFunction& ir) {
    // ---- 调用 + 闭包 ----
    switch (instr.op) {
    case IROp::CALL: {
        // CALL dest, name_idx, arg_count, args... → OP_CALL nameIdx argCount
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限，对齐 RegisterBytecodeBackend L410-414
        if (instr.operands[2].index > 255) {
            Logger::Error("BytecodeIRBackend: CALL argCount 超出 255 上限 (" + std::to_string(instr.operands[2].index) +
                              ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::CALL_EXPR: {
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限，对齐 RegisterBytecodeBackend L435-439
        if (instr.operands[2].index > 255) {
            Logger::Error("BytecodeIRBackend: CALL_EXPR argCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL_EXPR));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::TAIL_CALL: {
        // L18 eng-tailcall: 布局同 CALL，lower 为 OP_TAIL_CALL（后随的 IROp::RETURN
        // 由 AstIRBuilder 保证紧跟，lower 为 OP_RETURN 供降级路径使用）。
        if (instr.operands.size() < 3)
            return false;
        if (instr.operands[2].index > 255) {
            Logger::Error("BytecodeIRBackend: TAIL_CALL argCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TAIL_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
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
    case IROp::MAKE_CLOSURE: {
        // ---- 闭包 ----
        // MAKE_CLOSURE dest, name_idx, uv_count, [isLocal, idx]×uv_count
        if (instr.operands.size() < 3)
            return false;
        // AUDIT-STACKCLOSURE fix: 对齐 RegisterBytecodeBackend 的 P2-3 fix，
        // uvCount/isLocal/idx 编码为 1 字节，原 & 0xFF 静默截断会生成错误 upvalue 描述符。
        if (instr.operands[2].index >= 256) {
            Logger::Error("BytecodeIRBackend: MAKE_CLOSURE uvCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSURE));
        emitUint16(chunk_->code, nameConstIdx);
        uint32_t uvCount = instr.operands[2].index;
        chunk_->code.push_back(static_cast<uint8_t>(uvCount));
        for (uint32_t i = 0; i < uvCount; ++i) {
            size_t base = 3 + i * 2;
            if (base + 1 >= instr.operands.size())
                return false;
            if (instr.operands[base].index >= 256) {
                Logger::Error("BytecodeIRBackend: MAKE_CLOSURE isLocal 超出 255 上限 (" +
                                  std::to_string(instr.operands[base].index) + ")",
                              "IR");
                return false;
            }
            if (instr.operands[base + 1].index >= 256) {
                Logger::Error("BytecodeIRBackend: MAKE_CLOSURE upvalue idx 超出 255 上限 (" +
                                  std::to_string(instr.operands[base + 1].index) + ")",
                              "IR");
                return false;
            }
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base].index));     // isLocal
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base + 1].index)); // idx
        }
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerContainerOp(const IRInstruction& instr, const IRFunction& ir) {
    // R106 重构：原 181 行单 switch 拆为分派器 + 3 个单一职责 helper。
    // 按 instr.op 类别路由到 lowerContainerBuildOps / lowerMemberAccessOps / lowerMethodCallOps。
    // 语义零变更：所有 case 内逻辑原样保留至各 helper，仅做机械 extract method。
    switch (instr.op) {
    // ---- 容器构造与索引 ----
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
    case IROp::BUILD_ENUM_VARIANT:
    case IROp::ENUM_VARIANT_NAME:
    case IROp::ENUM_VARIANT_FIELD:
    case IROp::INDEX_GET:
    case IROp::INDEX_SET:
    // R134: LEN 与 INDEX_GET 同属"容器元素/属性访问"语义族，复用 lowerContainerBuildOps
    case IROp::LEN:
        return lowerContainerBuildOps(instr, ir);
    // ---- 成员访问 ----
    case IROp::MEMBER_GET:
    case IROp::MEMBER_SET:
    case IROp::MEMBER_SET_LOCAL: // L7 fix
    case IROp::SUPER_MEMBER_GET:
        return lowerMemberAccessOps(instr, ir);
    // ---- 方法调用 ----
    case IROp::METHOD_CALL:
    case IROp::SUPER_CALL:
        return lowerMethodCallOps(instr, ir);
    default:
        return false; // 非本类别
    }
}

bool BytecodeIRBackend::lowerContainerBuildOps(const IRInstruction& instr, const IRFunction& ir) {
    // lowerContainerOp 子阶段：容器构造与索引 IROp。
    // 覆盖 BUILD_ARRAY/BUILD_DICT/BUILD_TUPLE/BUILD_ENUM_VARIANT/ENUM_VARIANT_NAME/
    //      ENUM_VARIANT_FIELD/INDEX_GET/INDEX_SET。
    // 注：ir 参数在部分 case（如 BUILD_ARRAY/INDEX_GET）中未使用，保留统一签名以与
    // lowerContainerBuildOps/lowerMemberAccessOps/lowerMethodCallOps 对齐。
    (void)ir;
    switch (instr.op) {
    case IROp::BUILD_ARRAY: {
        if (instr.operands.size() < 2)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 count 8 位上限，对齐 RegisterBytecodeBackend L556-560
        if (instr.operands[1].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_ARRAY count 超出 255 上限 (" +
                              std::to_string(instr.operands[1].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_ARRAY));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF)); // count
        break;
    }
    case IROp::BUILD_DICT: {
        if (instr.operands.size() < 2)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 pairCount 8 位上限，对齐 RegisterBytecodeBackend L577-581
        if (instr.operands[1].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_DICT pairCount 超出 255 上限 (" +
                              std::to_string(instr.operands[1].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_DICT));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF)); // pairCount
        break;
    }
    // R98 元组与解构：BUILD_TUPLE → OP_BUILD_TUPLE lowering
    case IROp::BUILD_TUPLE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[1].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_TUPLE count 超出 255 上限 (" +
                              std::to_string(instr.operands[1].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_TUPLE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF)); // count
        break;
    }
    // R99 枚举与 ADT：BUILD_ENUM_VARIANT → OP_BUILD_ENUM_VARIANT lowering
    // IR: [dest, enumNameConstIdx, variantNameConstIdx, argCount, arg1, arg2, ...]
    // 字节码：args 已在栈上（vregToStack 已 push），emit OP_BUILD_ENUM_VARIANT 消费 argCount 个参数。
    case IROp::BUILD_ENUM_VARIANT: {
        if (instr.operands.size() < 4)
            return false;
        if (instr.operands[3].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_ENUM_VARIANT argCount 超出 255 上限", "IR");
            return false;
        }
        uint16_t enumNameConstIdx = static_cast<uint16_t>(instr.operands[1].index);
        uint16_t variantNameConstIdx = static_cast<uint16_t>(instr.operands[2].index);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_ENUM_VARIANT));
        emitUint16(chunk_->code, enumNameConstIdx);
        emitUint16(chunk_->code, variantNameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[3].index & 0xFF)); // argCount
        break;
    }
    // R99 枚举与 ADT：ENUM_VARIANT_NAME → OP_ENUM_VARIANT_NAME lowering
    // IR: [dest_bool, scrut_vreg, enumNameConstIdx, variantNameConstIdx]
    // 字节码：scrut 已在栈上，emit OP_ENUM_VARIANT_NAME pop scrut push bool。
    case IROp::ENUM_VARIANT_NAME: {
        if (instr.operands.size() < 4)
            return false;
        uint16_t enumNameConstIdx = static_cast<uint16_t>(instr.operands[2].index);
        uint16_t variantNameConstIdx = static_cast<uint16_t>(instr.operands[3].index);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_ENUM_VARIANT_NAME));
        emitUint16(chunk_->code, enumNameConstIdx);
        emitUint16(chunk_->code, variantNameConstIdx);
        break;
    }
    // R99 枚举与 ADT：ENUM_VARIANT_FIELD → OP_ENUM_VARIANT_FIELD lowering
    // IR: [dest, scrut_vreg, idx_vreg]
    // 字节码：scrut 和 idx 已在栈上（idx 在顶），emit OP_ENUM_VARIANT_FIELD pop idx, scrut push field。
    case IROp::ENUM_VARIANT_FIELD: {
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_ENUM_VARIANT_FIELD));
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
    // R134 模式匹配扩展：LEN → OP_LEN lowering
    // IR: [dest_vreg, src_vreg]  字节码：scrut 已在栈上，emit OP_LEN pop scrut push len。
    case IROp::LEN: {
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LEN));
        break;
    }
    default:
        return false; // 非本子阶段
    }
    return true;
}

bool BytecodeIRBackend::lowerMemberAccessOps(const IRInstruction& instr, const IRFunction& ir) {
    // lowerContainerOp 子阶段：成员访问 IROp（MEMBER_GET/MEMBER_SET/SUPER_MEMBER_GET）。
    switch (instr.op) {
    case IROp::MEMBER_GET: {
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_GET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::MEMBER_SET: {
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_SET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::MEMBER_SET_LOCAL: {
        // L7 fix: 方法体内 this.field = val → OP_MEMBER_SET_LOCAL slot fieldIdx
        // val 已在栈顶（由调用方推入），OP_MEMBER_SET_LOCAL pops val, modifies stack_[bp+slot].field
        if (instr.operands.size() < 3)
            return false;
        uint8_t slot = static_cast<uint8_t>(instr.operands[0].index);
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_SET_LOCAL));
        chunk_->code.push_back(slot);
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::SUPER_MEMBER_GET: {
        // P1 fix: super.field → OP_SUPER_MEMBER_GET（与 OP_MEMBER_GET 格式相同）
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUPER_MEMBER_GET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    default:
        return false; // 非本子阶段
    }
    return true;
}

bool BytecodeIRBackend::lowerMethodCallOps(const IRInstruction& instr, const IRFunction& ir) {
    // lowerContainerOp 子阶段：方法调用 IROp（METHOD_CALL/SUPER_CALL）。
    switch (instr.op) {
    case IROp::METHOD_CALL: {
        // METHOD_CALL dest, obj, method_idx, arg_count, args...
        // → OP_METHOD_CALL nameIdx(2B) argCount(1B) recvVarIdx(2B) recvSlot(1B)
        if (instr.operands.size() < 4)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L460-464
        if (instr.operands[3].index > 254) {
            Logger::Error("BytecodeIRBackend: METHOD_CALL argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[3].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_METHOD_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[3].index & 0xFF)); // argCount
        emitUint16(chunk_->code, RuntimeLimits::NO_INDEX);                            // recvVarIdx（简化占位）
        chunk_->code.push_back(RuntimeLimits::NO_SLOT);                               // recvSlot（简化占位）
        break;
    }
    case IROp::SUPER_CALL: {
        // P1 fix: super.method(args) → OP_SUPER_CALL
        // operands: [dest, this_vreg, method_idx, class_idx, arg_count, args...]
        // → OP_SUPER_CALL nameIdx(2B) argCount(1B) recvVarIdx(2B) recvSlot(1B) classIdx(2B)
        if (instr.operands.size() < 5)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L487-491
        if (instr.operands[4].index > 254) {
            Logger::Error("BytecodeIRBackend: SUPER_CALL argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[4].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[2].index), ir);
        uint16_t classConstIdx = addStringConstant(globalNameOf(ir, instr.operands[3].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUPER_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[4].index & 0xFF)); // argCount
        emitUint16(chunk_->code, RuntimeLimits::NO_INDEX);                            // recvVarIdx（简化占位）
        chunk_->code.push_back(RuntimeLimits::NO_SLOT);                               // recvSlot（简化占位）
        emitUint16(chunk_->code, classConstIdx);                                      // classIdx
        break;
    }
    default:
        return false; // 非本子阶段
    }
    return true;
}

bool BytecodeIRBackend::lowerMiscOp(const IRInstruction& instr, const IRFunction& ir) {
    // R106 重构：原 324 行单 switch 拆为分派器 + 4 个单一职责 helper。
    // 按 instr.op 类别路由到 lowerClassOps / lowerExceptionOps / lowerWritebackOps / lowerMiscPureOps。
    // 语义零变更：所有 case 内逻辑原样保留至各 helper，仅做机械 extract method。
    switch (instr.op) {
    // ---- 类 ----
    case IROp::DEFINE_CLASS:
    case IROp::CLASS_NEW:
    case IROp::INIT_FIELD:
        return lowerClassOps(instr, ir);
    // ---- 异常 ----
    case IROp::TRY_BEGIN:
    case IROp::TRY_END:
    case IROp::THROW:
    case IROp::LOAD_EXCEPTION:
    case IROp::PUSH_JUMP_TARGET:
    case IROp::FINALLY_END:
        return lowerExceptionOps(instr, ir);
    // ---- 写回指令 ----
    case IROp::WRITEBACK_MEMBER_VAR:
    case IROp::WRITEBACK_MEMBER_LOCAL:
    case IROp::WRITEBACK_INDEX_VAR:
    case IROp::WRITEBACK_INDEX_LOCAL:
    case IROp::WRITEBACK_MEMBER_UPVALUE:
    case IROp::WRITEBACK_INDEX_UPVALUE:
        return lowerWritebackOps(instr, ir);
    // ---- 其他 ----
    case IROp::PRINT:
    case IROp::POP:
    case IROp::DUP:
    case IROp::LOAD_MUTATED:
    case IROp::TYPE_CHECK:
    case IROp::TYPE_TEST: // R134: 软类型测试，与 TYPE_CHECK 同属 misc pure 类
        return lowerMiscPureOps(instr, ir);
    default:
        return false; // 非本类别
    }
}

bool BytecodeIRBackend::lowerClassOps(const IRInstruction& instr, const IRFunction& ir) {
    // lowerMiscOp 子阶段：类相关 IROp（DEFINE_CLASS/CLASS_NEW/INIT_FIELD）
    switch (instr.op) {
    case IROp::DEFINE_CLASS: {
        // 操作数（BUG-INH-1 fix: 新增字段默认值常量索引；
        //   BUG-INH-IR-1 fix: 新增字段表达式临时局部变量槽位）:
        //   [0]=className(FUNC_NAME), [1]=parentName(IMM_UINT, UINT32_MAX=无父类)
        //   [2] fieldCount (IMM_UINT)
        //   [3+i*3] field name (FIELD_NAME)
        //   [3+i*3+1] fieldDefaultConstIdx (IMM_UINT, UINT32_MAX=null/无默认值/有表达式)
        //   [3+i*3+2] fieldExprLocalSlot (IMM_UINT, UINT32_MAX=使用常量或null, 否则使用临时 local slot)
        //   [3+3F] methodCount (IMM_UINT)
        //   [3+3F+1 .. ] (methodName FIELD_NAME, funName FUNC_NAME) × M
        if (instr.operands.size() < 2)
            return false;
        uint32_t nameIdx = instr.operands[0].index;
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, nameIdx), ir);

        // 与 Compiler.cpp visitClassDecl (1792-1823) 对齐：stack VM 的 executeDefineClass
        // 期望栈顶有模板实例（OP_CLASS_NEW 推入），并从 pendingFieldOrder_ 提取字段顺序
        // （OP_INIT_FIELD 填充）。若仅 emit OP_DEFINE_CLASS，pop() 会读到错误栈值导致
        // "模板值不是实例" 运行时错误（回归：NestedMemberAccessPush）。
        // BUG-INH-1 fix: 原实现所有字段默认值为 null，现在从 IR 常量池提取字面量默认值，
        // 对齐 Compiler.cpp:1843-1847 直接路径。
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLASS_NEW));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(0)); // argCount = 0

        // 为每个字段 emit 默认值 + OP_INIT_FIELD（按 IR 操作数中的字段名顺序）
        if (instr.operands.size() >= 3) {
            uint32_t fieldCount = instr.operands[2].index;
            for (uint32_t fi = 0; fi < fieldCount; ++fi) {
                size_t nameOpIdx = 3 + fi * 3;
                size_t defaultOpIdx = 3 + fi * 3 + 1;
                size_t exprLocalSlotOpIdx = 3 + fi * 3 + 2;
                if (exprLocalSlotOpIdx >= instr.operands.size())
                    return false;
                uint32_t fieldGlobalIdx = instr.operands[nameOpIdx].index;
                uint16_t fieldConstIdx = addStringConstant(globalNameOf(ir, fieldGlobalIdx), ir);
                // BUG-INH-1 fix: 从 IR 常量池提取字段默认值
                uint32_t defaultConstIdx = instr.operands[defaultOpIdx].index;
                // BUG-INH-IR-1 fix: 非字面量表达式，从临时 local slot 读取求值结果
                uint32_t exprLocalSlot = instr.operands[exprLocalSlotOpIdx].index;
                if (exprLocalSlot != UINT32_MAX) {
                    // 非字面量表达式：emit OP_GET_LOCAL 将求值结果压栈
                    // （visitClassDecl 已在 DEFINE_CLASS 之前 emit STORE_LOCAL 存入 slot）
                    if (exprLocalSlot >= 256) {
                        Logger::Error("BytecodeIRBackend: DEFINE_CLASS 字段临时局部变量槽位超出 255 上限", "IR");
                        return false;
                    }
                    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_LOCAL));
                    chunk_->code.push_back(static_cast<uint8_t>(exprLocalSlot));
                } else if (defaultConstIdx == UINT32_MAX) {
                    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
                } else {
                    if (defaultConstIdx >= ir.constants.size()) {
                        Logger::Error("BytecodeIRBackend: DEFINE_CLASS 字段默认值常量索引越界", "IR");
                        return false;
                    }
                    const Value& defaultVal = ir.constants[defaultConstIdx];
                    // 根据 Value 类型 emit 对应字节码（对齐 Compiler.cpp 直接路径）
                    if (defaultVal.isInt()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INT));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isFloat()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FLOAT));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isString()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_STRING));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isBool()) {
                        chunk_->code.push_back(
                            static_cast<uint8_t>(defaultVal.boolVal() ? OpCode::OP_TRUE : OpCode::OP_FALSE));
                    } else {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
                    }
                }
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INIT_FIELD));
                emitUint16(chunk_->code, fieldConstIdx);
            }
        }

        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_CLASS));
        emitUint16(chunk_->code, nameConstIdx);
        // 与 Compiler.cpp:1810-1817 对齐：编码父类名索引
        // NO_INDEX 表示无父类；否则为父类名在常量池中的索引
        uint32_t parentIdx = instr.operands[1].index;
        if (parentIdx == UINT32_MAX) {
            emitUint16(chunk_->code, RuntimeLimits::NO_INDEX);
        } else {
            uint16_t superConstIdx = addStringConstant(globalNameOf(ir, parentIdx), ir);
            emitUint16(chunk_->code, superConstIdx);
        }
        break;
    }
    case IROp::CLASS_NEW: {
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L748-752
        if (instr.operands[2].index > 254) {
            Logger::Error("BytecodeIRBackend: CLASS_NEW argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLASS_NEW));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::INIT_FIELD: {
        if (instr.operands.empty())
            return false;
        uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INIT_FIELD));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerExceptionOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    // lowerMiscOp 子阶段：异常相关 IROp（TRY_BEGIN/TRY_END/THROW/LOAD_EXCEPTION/
    // PUSH_JUMP_TARGET/FINALLY_END）
    switch (instr.op) {
    case IROp::TRY_BEGIN: {
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRY_BEGIN));
        // BUG-EXC-1 fix: 标记 isTryBegin=true，patchJumps 对 TRY_BEGIN 写相对偏移
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false, true});
        emitUint16(chunk_->code, 0); // catchOffset 占位符，第二遍回填
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
    case IROp::LOAD_EXCEPTION: {
        // P1-4 fix: 栈式 VM 的 throwException 已将异常值推入栈顶，
        // 此处为 no-op（不发射字节码）。后续 STORE_LOCAL/DEFINE_GLOBAL 从栈顶 pop。
        break;
    }
    // AUDIT-P1.1 fix: break/continue finally 续跳 lowering
    case IROp::PUSH_JUMP_TARGET: {
        // operands: [label_idx]
        // → OP_PUSH_JUMP_TARGET target(2B 绝对偏移，第二遍回填)
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_PUSH_JUMP_TARGET));
        // 与 OP_JUMP 一致：绝对偏移（isTryBegin=false），patchJumps 回填 label→绝对 IP
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false});
        emitUint16(chunk_->code, 0); // 占位符，第二遍回填
        break;
    }
    case IROp::FINALLY_END: {
        // 无操作数 → OP_FINALLY_END（1 字节）
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FINALLY_END));
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerWritebackOps(const IRInstruction& instr, const IRFunction& ir) {
    // lowerMiscOp 子阶段：写回指令 IROp（WRITEBACK_MEMBER_VAR/LOCAL/UPVALUE、
    // WRITEBACK_INDEX_VAR/LOCAL/UPVALUE）
    switch (instr.op) {
    case IROp::WRITEBACK_MEMBER_VAR: {
        // operands: [var_idx, field_idx]
        // → OP_WRITEBACK_MEMBER_VAR varIdx(2B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_VAR));
        // varIdx(2B)：栈式 VM 的 OP_WRITEBACK_*_VAR 将 varIdx 当作常量池索引处理
        // （取 chunk.constants[varIdx].stringVal() 作为变量名）。GLOBAL_SLOT (IMM_UINT)
        // 携带的是槽位号，不是常量索引，直接 emit 会导致误读常量池。
        // BUG-NEW fix: 对 IMM_UINT 分支查 globalSlotNames_ 将 slot→name，再以字符串常量
        // 索引形式 emit，与 GLOBAL_NAME 路径统一。
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        // fieldIdx(2B)：字段名常量索引
        uint16_t fieldConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_MEMBER_LOCAL: {
        // operands: [slot, field_idx]
        // → OP_WRITEBACK_MEMBER_LOCAL slot(1B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：slot 超 255 静默截断会读写错误栈槽
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_MEMBER_LOCAL 槽位索引超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        uint16_t fieldConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_INDEX_VAR: {
        // operands: [var_idx]
        // → OP_WRITEBACK_INDEX_VAR varIdx(2B)
        if (instr.operands.size() < 1)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_VAR));
        // BUG-NEW fix: 同 WRITEBACK_MEMBER_VAR，IMM_UINT 需转 slot→name 常量索引
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalNameOf(ir, instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::WRITEBACK_INDEX_LOCAL: {
        // operands: [slot]
        // → OP_WRITEBACK_INDEX_LOCAL slot(1B)
        if (instr.operands.size() < 1)
            return false;
        // Bug-14 同型修复：slot 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_INDEX_LOCAL 槽位索引超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        break;
    }
    case IROp::WRITEBACK_MEMBER_UPVALUE: {
        // operands: [uv_idx, field_idx]
        // → OP_WRITEBACK_MEMBER_UPVALUE uvIdx(1B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：uvIdx 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_MEMBER_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // uvIdx(1B)
        uint16_t fieldConstIdx = addStringConstant(globalNameOf(ir, instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_INDEX_UPVALUE: {
        // operands: [uv_idx]
        // → OP_WRITEBACK_INDEX_UPVALUE uvIdx(1B)
        if (instr.operands.size() < 1)
            return false;
        // Bug-14 同型修复：uvIdx 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_INDEX_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // uvIdx(1B)
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::lowerMiscPureOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    // lowerMiscOp 子阶段：杂项 IROp（PRINT/POP/DUP/LOAD_MUTATED/TYPE_CHECK）
    switch (instr.op) {
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
    case IROp::LOAD_MUTATED: {
        // MEDIUM-1/2 fix: 读取 lastMutatedReceiver_ 到栈顶（不清除）
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LOAD_MUTATED));
        break;
    }
    // 2026-06-29: 类型注解运行时检查
    // operands: [src_vreg, type_const_idx]  src_vreg 已在栈顶，peek 不弹栈
    case IROp::TYPE_CHECK: {
        if (instr.operands.size() < 2) {
            Logger::Error("BytecodeIRBackend: TYPE_CHECK 操作数不足", "IR");
            return false;
        }
        // type_const_idx 已在常量池复制阶段（lower() 的 for 循环）同步到 BytecodeChunk
        uint16_t typeIdx = static_cast<uint16_t>(instr.operands[1].index);
        // AUDIT-P2 fix: 与 DUP/LOAD_MUTATED 保持一致，更新 src_vreg 的栈深度映射。
        // 虽然 OP_TYPE_CHECK 是 peek 不 push，但记录 src_vreg 在此处"仍然有效"的栈深度，
        // 避免后续指令通过 vregStackDepth_ 查询时拿到过期记录点。
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TYPE_CHECK));
        chunk_->code.push_back(static_cast<uint8_t>(typeIdx & 0xFF));
        chunk_->code.push_back(static_cast<uint8_t>((typeIdx >> 8) & 0xFF));
        break;
    }
    case IROp::TYPE_TEST: {
        // R134: 软类型测试 lowering（与 OP_TYPE_TEST 对应）
        // IR: [dest_vreg, src_vreg, type_const_idx]  字节码：src 已在栈上，emit OP_TYPE_TEST pop src push bool
        if (instr.operands.size() < 3) {
            Logger::Error("BytecodeIRBackend: TYPE_TEST 操作数不足", "IR");
            return false;
        }
        uint16_t typeIdx = static_cast<uint16_t>(instr.operands[2].index);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TYPE_TEST));
        chunk_->code.push_back(static_cast<uint8_t>(typeIdx & 0xFF));
        chunk_->code.push_back(static_cast<uint8_t>((typeIdx >> 8) & 0xFF));
        break;
    }
    default:
        return false; // 非本类别
    }
    return true;
}

bool BytecodeIRBackend::patchJumps() {
    // 遍历所有待回填跳转，查 labelToOffset 写回目标偏移
    for (const auto& pj : pendingJumps_) {
        auto it = labelToOffset_.find(pj.targetLabel);
        if (it == labelToOffset_.end()) {
            Logger::Error("BytecodeIRBackend: 未找到标签 " + std::to_string(pj.targetLabel), "IR");
            return false;
        }
        // BUG-EXC-1 fix: OP_JUMP/OP_JUMP_IF_FALSE 用绝对偏移（ip = target），
        // 但 OP_TRY_BEGIN 用相对偏移（catchIp = ip + 3 + catchOffset）。
        // pj.codeOffset 指向操作数首字节（opcode 在 codeOffset-1），
        // StackVM 执行时 ip = codeOffset-1，故相对偏移 = target - (ip+3) = target - (codeOffset+2)。
        size_t targetOffset = it->second;
        uint16_t target;
        if (pj.isTryBegin) {
            if (targetOffset < pj.codeOffset + 2) {
                Logger::Error(
                    "BytecodeIRBackend: TRY_BEGIN catch 目标在 TRY_BEGIN 之前 (catch=" + std::to_string(targetOffset) +
                        ", tryBegin=" + std::to_string(pj.codeOffset - 1) + ")",
                    "IR");
                return false;
            }
            // BUG-IR-PATCH-1 fix: 检查 catch 相对偏移 64KB 上限，对齐 Compiler.cpp visitTryStmt 的 cleanupThrowOffset >
            // 65535 检查。 原实现 static_cast<uint16_t> 静默截断，导致 VM
            // 跳转到错误位置执行乱码字节码（三后端不一致）。
            size_t relOff = targetOffset - (pj.codeOffset + 2);
            if (relOff > 65535) {
                Logger::Error(
                    "BytecodeIRBackend: TRY_BEGIN catch 相对偏移超过 64KB 限制 (relOff=" + std::to_string(relOff) + ")",
                    "IR");
                return false;
            }
            target = static_cast<uint16_t>(relOff);
        } else {
            // 16-bit 编码限制：字节码体积超过 64KB 时跳转目标会截断，需显式检查
            // （对齐 Compiler::safeCodeOffset() 的 65535 上限保护）
            if (targetOffset > 65535) {
                Logger::Error("BytecodeIRBackend: 跳转目标偏移超过 64KB 限制 (offset=" + std::to_string(targetOffset) +
                                  ")",
                              "IR");
                return false;
            }
            target = static_cast<uint16_t>(targetOffset);
        }
        if (pj.codeOffset + 1 >= chunk_->code.size()) {
            Logger::Error("BytecodeIRBackend: 跳转操作数越界", "IR");
            return false;
        }
        chunk_->code[pj.codeOffset] = static_cast<uint8_t>(target & 0xFF);
        chunk_->code[pj.codeOffset + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    return true;
}
