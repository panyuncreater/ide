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
// IR 优化 Pass 实现（方向二）
// ============================================================
// 各优化 pass 共同依赖的 IR 表示约定（理解下方算法的前提）：
//   · IR 以基本块（IRBlock::instructions）为单位，函数 IRFunction 持有若干基本块；
//     当前 pass 多为"块内"局部分析（不跨块传播），仅保守地在块边界重置摘要信息。
//   · 指令 IRInstruction 形如 op + 操作数列表 operands。操作数约定：
//       - 操作数[0]（首操作数）通常是"目标"dest vreg（纯计算/加载类指令）；
//         STORE_*/JUMP 等指令首操作数可能是 slot/label 而非 vreg。
//       - 其余操作数是源：VIRTUAL=SSA 风格虚拟寄存器(vreg，allocVReg 单调递增不复用)，
//         CONSTANT=常量池索引，LOCAL_SLOT=局部槽，LABEL=跳转标签，GLOBAL_NAME/GLOBAL_SLOT=全局。
//   · vreg 的 SSA 不变量：每个 vreg 应只被"定义"一次（见 loopUnrollingPass 的 vreg 重命名修复）。
//     多数 pass 通过"建立 vreg→定义/值"映射并替换引用"来化简，而非修改指令语义。
//   · 折叠/化简必须保守：带运行时副作用的指令（除零、溢出、LOAD_GLOBAL 未定义、字符串拼接、
//     ADD 的 COW detach）不可被 DCE 随意删除，否则会抑制本应抛出的运行时错误（见 isPureCompute）。

namespace {

/// 辅助：判断指令是否为"常量加载"（LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE）
/// 若是，返回 true 并输出对应的常量值
bool isConstLoad(const IRInstruction& instr, const IRFunction& ir, Value& outVal) {
    switch (instr.op) {
    case IROp::LOAD_CONST:
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[1].index >= ir.constants.size())
            return false;
        outVal = ir.constants[instr.operands[1].index];
        return true;
    case IROp::LOAD_NULL:
        outVal = Value::nullValue();
        return true;
    case IROp::LOAD_TRUE:
        outVal = Value(true);
        return true;
    case IROp::LOAD_FALSE:
        outVal = Value(false);
        return true;
    default:
        return false;
    }
}

// 辅助：判断指令是否为纯计算（无副作用，仅定义 dest vreg）
/// Phase-5 fix: 算术指令 ADD/SUB/MUL/DIV/MOD/NEGATE 现在也列为纯计算。
/// 原 BUG-IR-DCE-1 的担忧是 `1/0;` 表达式语句会被 DCE 误删而抑制除零错误，
/// 但 Phase-5 让 IROp::POP 携带所消费的 vreg 操作数（对齐 IR spec `POP: [src_vreg]`），
/// DCE 看到 POP 引用该 vreg → 算术指令不再是"死代码" → 不会被删除。
/// 仅当 CSE 使某算术指令的 dest 完全无引用（所有消费者都被重定向到首次计算的 dest）
/// 时 DCE 才删除该冗余指令——此时首次计算已保留相同副作用，删除安全。
bool isPureCompute(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
    case IROp::LOAD_LOCAL:
    // #23 fix: LOAD_GLOBAL/LOAD_UPVALUE 不列为纯计算——它们在运行时会检查
    // 变量是否定义并可能抛"未定义的变量"错误。若 DCE 删除 dest 未引用的加载，
    // 会抑制该错误（如 `undefinedVar;` 表达式语句本应报错却被静默删除）。
    // LOAD_LOCAL 可保留：局部变量由编译期静态绑定，不存在运行时未定义。
    case IROp::ADD:
    case IROp::SUB:
    case IROp::MUL:
    // BUGFIX-P2 #57: DIV/MOD 可能触发运行时除零错误（副作用），
    // 不应被DCE删除。仅在有明确证据证明除数非零时才可删除。
    // case IROp::DIV:
    // case IROp::MOD:
    case IROp::NEGATE:
    case IROp::NOT:
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE:
        // AUDIT-R6 F2 fix: DUP 从纯计算列表移除——栈式后端的 DUP 是位置性压栈指令
        // （lowering 仅 emit OP_DUP 复制栈顶，不读 src vreg），DCE 删除后消费者
        // （INDEX_GET 等）会弹走原始值导致栈下溢。实证：irOptimize + 两元素解构
        // 即触发“栈下溢”（CSE 将同源 DUP 重定向后 DCE 删除第二条 DUP）。
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
        // #14 fix: 整数 ADD/SUB/MUL 折叠需检查溢出（与 RegisterVM::executeArith 一致），
        // 溢出时不折叠（返回 false），留待运行时按 OverflowCheck 路径报错。
        // 原实现直接 a+b/a-b/a*b 在溢出时是 UB（int64_t 有符号溢出）。
        case IROp::ADD:
            if (OverflowCheck::addOverflow(a, b))
                return false;
            result = Value(a + b);
            return true;
        case IROp::SUB:
            if (OverflowCheck::subOverflow(a, b))
                return false;
            result = Value(a - b);
            return true;
        case IROp::MUL:
            if (OverflowCheck::mulOverflow(a, b))
                return false;
            result = Value(a * b);
            return true;
        case IROp::DIV:
            if (b == 0)
                return false; // 除零不折叠，留待运行时报错
            if (OverflowCheck::divOverflow(a, b))
                return false; // INT64_MIN / -1
            result = Value(a / b);
            return true;
        case IROp::MOD:
            if (b == 0)
                return false;
            if (OverflowCheck::modOverflow(a, b))
                return false; // INT64_MIN % -1
            result = Value(a % b);
            return true;
        default:
            return false;
        }
    }
    // 浮点算术（至少一方为 float）
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::ADD:
            result = Value(a + b);
            return true;
        case IROp::SUB:
            result = Value(a - b);
            return true;
        case IROp::MUL:
            result = Value(a * b);
            return true;
        case IROp::DIV:
            if (b == 0.0)
                return false;
            result = Value(a / b);
            return true;
        case IROp::MOD:
            if (b == 0.0)
                return false;
            // C++ fmod
            result = Value(std::fmod(a, b));
            return true;
        default:
            return false;
        }
    }
    // R97 #4 fix: 字符串拼接折叠（对齐 Compiler::tryFoldBinary 字符串 BIN_ADD 分支）
    // 字符串拼接本身无副作用（不抛异常），可安全折叠。
    // 注意：isPureCompute 仍将 ADD 列为非纯计算（因 int/float ADD 可能溢出/除零），
    // 但 foldArith 在常量传播阶段被调用时仅在两操作数均为常量时尝试，安全。
    if (lhs.isString() && rhs.isString() && op == IROp::ADD) {
        result = Value(lhs.stringVal() + rhs.stringVal());
        return true;
    }
    return false;
}

/// 辅助：对两个 Value 执行比较运算，成功返回 true
bool foldCompare(IROp op, const Value& lhs, const Value& rhs, Value& result) {
    // 同类型比较
    if (lhs.isInt() && rhs.isInt()) {
        int64_t a = lhs.intVal(), b = rhs.intVal();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        case IROp::LT:
            result = Value(a < b);
            return true;
        case IROp::GT:
            result = Value(a > b);
            return true;
        case IROp::LTE:
            result = Value(a <= b);
            return true;
        case IROp::GTE:
            result = Value(a >= b);
            return true;
        default:
            return false;
        }
    }
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        case IROp::LT:
            result = Value(a < b);
            return true;
        case IROp::GT:
            result = Value(a > b);
            return true;
        case IROp::LTE:
            result = Value(a <= b);
            return true;
        case IROp::GTE:
            result = Value(a >= b);
            return true;
        default:
            return false;
        }
    }
    if (lhs.isBool() && rhs.isBool()) {
        bool a = lhs.boolVal(), b = rhs.boolVal();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        default:
            return false; // bool 不支持 < > <= >=
        }
    }
    if (lhs.isNull() && rhs.isNull()) {
        switch (op) {
        case IROp::EQ:
            result = Value(true);
            return true;
        case IROp::NEQ:
            result = Value(false);
            return true;
        default:
            return false;
        }
    }
    return false;
}

/// 辅助：对 Value 执行一元运算
bool foldUnary(IROp op, const Value& operand, Value& result) {
    switch (op) {
    case IROp::NEGATE:
        // #14 fix: -INT64_MIN 是 UB，需检查（与 RegisterVM::executeArith 一致）。
        if (operand.isInt()) {
            if (OverflowCheck::negateOverflow(operand.intVal()))
                return false;
            result = Value(-operand.intVal());
            return true;
        }
        if (operand.isFloat()) {
            result = Value(-operand.floatVal());
            return true;
        }
        return false;
    case IROp::NOT:
        if (operand.isBool()) {
            result = Value(!operand.boolVal());
            return true;
        }
        return false;
    default:
        return false;
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
            if (instr.operands.empty())
                continue;
            if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
                vregDefs[instr.operands[0].index] = {bi, ii};
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
                bool isArith =
                    (op == IROp::ADD || op == IROp::SUB || op == IROp::MUL || op == IROp::DIV || op == IROp::MOD);
                bool isCompare = (op == IROp::EQ || op == IROp::NEQ || op == IROp::LT || op == IROp::GT ||
                                  op == IROp::LTE || op == IROp::GTE);
                if (!isArith && !isCompare)
                    continue;

                // dest, src1, src2
                IROperand dest = instr.operands[0];
                IROperand src1 = instr.operands[1];
                IROperand src2 = instr.operands[2];
                if (src1.kind != IROperandKind::VIRTUAL || src2.kind != IROperandKind::VIRTUAL)
                    continue;

                // 查找 src1/src2 的定义
                auto it1 = vregDefs.find(src1.index);
                auto it2 = vregDefs.find(src2.index);
                if (it1 == vregDefs.end() || it2 == vregDefs.end())
                    continue;

                // 获取定义指令
                const auto& def1 = ir.blocks[it1->second.first].instructions[it1->second.second];
                const auto& def2 = ir.blocks[it2->second.first].instructions[it2->second.second];

                Value v1, v2;
                if (!isConstLoad(def1, ir, v1) || !isConstLoad(def2, ir, v2))
                    continue;

                // 尝试折叠
                Value folded;
                bool ok = isArith ? foldArith(op, v1, v2, folded) : foldCompare(op, v1, v2, folded);
                if (!ok)
                    continue;

                // 替换：原指令改为 LOAD_CONST dest, constIdx
                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = {dest, IROperand::constant(constIdx)};
                // 更新 vregDefs 中 dest 的定义
                vregDefs[dest.index] = {bi, ii};
                modified = true;
            }
            // 一元运算：NEGATE/NOT
            else if (instr.operands.size() == 2) {
                IROp op = instr.op;
                if (op != IROp::NEGATE && op != IROp::NOT)
                    continue;

                IROperand dest = instr.operands[0];
                IROperand src = instr.operands[1];
                if (src.kind != IROperandKind::VIRTUAL)
                    continue;

                auto it = vregDefs.find(src.index);
                if (it == vregDefs.end())
                    continue;
                const auto& def = ir.blocks[it->second.first].instructions[it->second.second];

                Value v;
                if (!isConstLoad(def, ir, v))
                    continue;

                Value folded;
                if (!foldUnary(op, v, folded))
                    continue;

                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = {dest, IROperand::constant(constIdx)};
                vregDefs[dest.index] = {bi, ii};
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
                if (i == 0 && isPureCompute(instr.op))
                    continue;
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
    // BUGFIX-P1 (#24): 此 pass 的替换逻辑会产生非法 CONSTANT 操作数（大多数 IR 指令不支持
    // IROperandKind::CONSTANT 操作数），保持禁用状态（所有调用点 enableCopyPropagation=false）。
    // 正确实现应使用 vreg→vreg 传播而非直接替换为 CONSTANT。
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
        vregConst.clear(); // 基本块边界重置
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
                        // Bug-6 同型修复：枚举扩展时静默跳过替换，留下不一致状态
                        default:
                            // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
                            Logger::Error("copyPropagationPass: 未处理的 ConstKind 枚举值", "IR");
                            assert(false && "copyPropagationPass: 未处理的 ConstKind 枚举值");
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
            if (instr.op == IROp::STORE_LOCAL || instr.op == IROp::STORE_GLOBAL || instr.op == IROp::STORE_UPVALUE ||
                instr.op == IROp::DEFINE_GLOBAL) {
                // 清除局部变量的等价关系（简化：不清除，因为 vreg 是 SSA 风格）
                // 实际上 vreg 不会被 STORE 修改，只有 LOCAL_SLOT 概念会
            }
        }
    }
    return modified;
}

// ---- 公共子表达式消除（CSE，局部 — 基本块内）----
// C4: 在每个基本块内对纯计算指令做值编号，相同表达式 dest 复用。
// 与 BUG-IR-DCE-1 的关键差异：本 pass 不删除指令，仅替换后续引用。
// 算术指令（含副作用：除零/溢出）即使 dest 已被替换，仍保留原指令执行。
bool commonSubexpressionEliminationPass(IRFunction& ir) {
    bool modified = false;
    // 值编号 key：将 (op, operands) 编码为字符串
    auto encodeKey = [](const IRInstruction& instr) -> std::string {
        std::string key;
        key.push_back(static_cast<char>(instr.op));
        key.push_back('|');
        for (size_t idx = 0; idx < instr.operands.size(); ++idx) {
            const auto& op = instr.operands[idx];
            // BUGFIX-P1 (#23): 跳过 dest vreg（operands[0]）。SSA 下每条指令 dest 唯一，
            // 若编入 key 会导致相同表达式永远无法匹配，CSE 完全失效。
            // 与 IRSSA.cpp gvnPass 的 encodeValueKey 保持一致。
            if (idx == 0 && op.kind == IROperandKind::VIRTUAL)
                continue;
            key.push_back(static_cast<char>(op.kind));
            key.push_back(':');
            key.append(std::to_string(op.index));
            key.push_back(',');
        }
        return key;
    };
    // 判断指令是否可参与 CSE（纯计算 + 有 dest vreg）
    auto isCSEable = [](const IRInstruction& instr) -> bool {
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].kind != IROperandKind::VIRTUAL)
            return false;
        // 包含算术（保留原指令执行副作用，仅替换 dest 引用）
        switch (instr.op) {
        case IROp::ADD:
        case IROp::SUB:
        case IROp::MUL:
        case IROp::DIV:
        case IROp::MOD:
        case IROp::NEGATE:
        case IROp::EQ:
        case IROp::NEQ:
        case IROp::LT:
        case IROp::GT:
        case IROp::LTE:
        case IROp::GTE:
        case IROp::NOT:
            // AUDIT-R6 F2 fix: DUP 从 CSE 可参与列表移除。解构绑定为每个变量发射一条
            // DUP，其源操作数均为同一 tuple vreg，encodeKey 跳过 dest 后两条 DUP 键完全
            // 相同→CSE 重定向第二条 dest→DCE 删除第二条 DUP（压栈指令）→栈失衡下溢。
            // DUP 是唯一“源 vreg 可重复”的原 CSEable 指令，移除后 Phase-5 安全前提恢复。
            return true;
        default:
            return false;
        }
    };

    for (auto& block : ir.blocks) {
        // 块内值编号表：key → 已记录的 dest vreg
        std::unordered_map<std::string, uint32_t> valueMap;
        // 替换映射：被替换的 dest vreg → 替代 vreg
        std::unordered_map<uint32_t, uint32_t> replacements;
        // 待标记为"dest 已替换"的指令索引（保留指令以触发副作用，但 dest 不再被引用）
        // 实际操作：在第二轮遍历中替换后续指令对该 dest 的引用

        // 第一遍：构建替换映射
        for (auto& instr : block.instructions) {
            // 先按替换映射更新本指令的 operands（处理级联替换）
            // BUGFIX-P2 #56: 对 CSE-able 指令（dest 在 operands[0]），从 i=1 开始
            // 跳过 dest 操作数。dest 是定义点而非使用点，替换 dest 会破坏指令的
            // 定义语义（冗余指令写入错误 vreg）。非 CSE-able 指令（如 JUMP_IF_FALSE/
            // RETURN/POP）的 operands[0] 是源操作数，仍需替换。
            size_t startIdx = isCSEable(instr) ? 1 : 0;
            for (size_t opi = startIdx; opi < instr.operands.size(); ++opi) {
                auto& op = instr.operands[opi];
                if (op.kind == IROperandKind::VIRTUAL) {
                    auto it = replacements.find(op.index);
                    if (it != replacements.end()) {
                        op.index = it->second;
                        modified = true;
                    }
                }
            }

            if (!isCSEable(instr))
                continue;
            // 二元/一元运算的 dest vreg
            uint32_t destVReg = instr.operands[0].index;
            std::string key = encodeKey(instr);
            auto it = valueMap.find(key);
            if (it != valueMap.end()) {
                // 已存在相同表达式：本指令 dest 引用替换为已存在的 vreg
                replacements[destVReg] = it->second;
                // 不删除本指令（保留副作用），后续对该 dest 的引用会在循环开头被替换
            } else {
                valueMap[key] = destVReg;
            }
        }
    }
    return modified;
}

// ---- 循环展开（保守策略 — 仅展开常量边界的小循环）----
// C4: 检测 `for (var i = 0; i < N; i = i + 1) { body }` 模式，N 为小整常量时展开 N 次。
// 实现：扫描 IR 指令序列，识别循环模式（LABEL start; cond; JUMP_IF_FALSE exit; POP; body;
// counter update; JUMP start; LABEL exit; POP），将 body 复制 N 份替换原循环。

namespace {
/// 循环展开模式匹配结果（analyzeLoopUnrollPattern 填充）
struct LoopUnrollPattern {
    size_t startIndex;    // 模式起点（LABEL L1）的指令索引
    size_t bodyStart;     // body 起始指令索引（i+6）
    size_t bodyEnd;       // body 结束指令索引（不含，即尾部 LOAD_LOCAL 起点）
    uint32_t counterSlot; // 计数器局部变量槽位
    int64_t unrollCount;  // 展开次数（N）
    uint32_t startLabel;  // 循环起始 label
    uint32_t exitLabel;   // 循环退出 label
};

/// 在 instrs 中从 startIndex 开始分析循环展开模式。
/// 模式：
///   [i+0] LABEL L1
///   [i+1] LOAD_LOCAL slot        (i)
///   [i+2] LOAD_CONST N
///   [i+3] LT dest, i, N
///   [i+4] JUMP_IF_FALSE dest, L_exit
///   [i+5] POP
///   [i+6..i+6+bodySize-1] body
///   [i+6+bodySize] LOAD_LOCAL slot  (i)
///   [i+6+bodySize+1] LOAD_CONST 1
///   [i+6+bodySize+2] ADD dest, i, 1
///   [i+6+bodySize+3] STORE_LOCAL slot, dest
///   [i+6+bodySize+4] JUMP L1
///   [i+6+bodySize+5] LABEL L_exit
///   [i+6+bodySize+6] POP
/// 匹配成功返回 true 并填充 outPattern；否则返回 false。
bool analyzeLoopUnrollPattern(const std::vector<IRInstruction>& instrs, size_t startIndex, const IRFunction& ir,
                              LoopUnrollPattern& outPattern) {
    constexpr int kMaxUnrollCount = 4;        // 最大展开次数（避免代码膨胀）
    constexpr size_t kMaxUnrollBodySize = 20; // body 指令数上限

    const auto& labelStart = instrs[startIndex];
    if (labelStart.op != IROp::LABEL)
        return false;
    uint32_t startLabel = labelStart.operands[0].index;

    const auto& loadI1 = instrs[startIndex + 1];
    if (loadI1.op != IROp::LOAD_LOCAL || loadI1.operands.size() < 2)
        return false;
    if (loadI1.operands[1].kind != IROperandKind::LOCAL_SLOT)
        return false;
    uint32_t counterSlot = loadI1.operands[1].index;

    // AUDIT-R3 P1-9 fix: 回溯验证计数器初值确为常量 0——emitUnrolledLoopBody 硬编码
    // "LOAD_CONST 0; STORE_LOCAL slot" 归零初始化，若循环实际初值非 0（如 i 从 5
    // 开始），展开后执行次数错误（语义破坏）。向前扫描 LABEL 之前最近的
    // "LOAD_CONST(int 0) → STORE_LOCAL(counterSlot)" 序列（允许中间隔 POP，
    // 因 VarDecl 初始化 emit STORE_LOCAL+POP 模式），不匹配则拒绝展开。
    {
        size_t k = startIndex;
        while (k > 0 && instrs[k - 1].op == IROp::POP)
            --k;
        if (k < 2)
            return false;
        const auto& initStore = instrs[k - 1];
        const auto& initLoad = instrs[k - 2];
        if (initStore.op != IROp::STORE_LOCAL || initStore.operands.size() < 2 ||
            initStore.operands[0].kind != IROperandKind::LOCAL_SLOT || initStore.operands[0].index != counterSlot)
            return false;
        if (initLoad.op != IROp::LOAD_CONST || initLoad.operands.size() < 2 ||
            initLoad.operands[1].kind != IROperandKind::CONSTANT)
            return false;
        uint32_t initConstIdx = initLoad.operands[1].index;
        if (initConstIdx >= ir.constants.size() || !ir.constants[initConstIdx].isInt() ||
            ir.constants[initConstIdx].intVal() != 0)
            return false;
    }

    const auto& loadN = instrs[startIndex + 2];
    if (loadN.op != IROp::LOAD_CONST || loadN.operands.size() < 2)
        return false;
    if (loadN.operands[1].kind != IROperandKind::CONSTANT)
        return false;
    uint32_t nConstIdx = loadN.operands[1].index;
    if (nConstIdx >= ir.constants.size())
        return false;
    const Value& nVal = ir.constants[nConstIdx];
    if (!nVal.isInt())
        return false;
    int64_t nInt = nVal.intVal();
    if (nInt < 1 || nInt > kMaxUnrollCount)
        return false;

    const auto& ltInstr = instrs[startIndex + 3];
    if (ltInstr.op != IROp::LT || ltInstr.operands.size() < 3)
        return false;

    const auto& jumpFalse = instrs[startIndex + 4];
    if (jumpFalse.op != IROp::JUMP_IF_FALSE || jumpFalse.operands.size() < 2)
        return false;
    uint32_t exitLabel = jumpFalse.operands[1].index;

    const auto& pop1 = instrs[startIndex + 5];
    if (pop1.op != IROp::POP)
        return false;

    // 在剩余指令中找模式尾部：LOAD_LOCAL slot; LOAD_CONST 1; ADD; STORE_LOCAL slot; JUMP L1; LABEL L_exit; POP
    size_t tailStart = startIndex + 6;
    size_t bodyEnd = tailStart;
    bool foundTail = false;
    for (size_t probe = 0; probe < kMaxUnrollBodySize && bodyEnd + 7 <= instrs.size(); ++probe, ++bodyEnd) {
        const auto& t0 = instrs[bodyEnd];
        const auto& t1 = instrs[bodyEnd + 1];
        const auto& t2 = instrs[bodyEnd + 2];
        const auto& t3 = instrs[bodyEnd + 3];
        const auto& t4 = instrs[bodyEnd + 4];
        const auto& t5 = instrs[bodyEnd + 5];
        const auto& t6 = instrs[bodyEnd + 6];
        if (t0.op != IROp::LOAD_LOCAL || t0.operands.size() < 2)
            continue;
        if (t0.operands[1].kind != IROperandKind::LOCAL_SLOT)
            continue;
        if (t0.operands[1].index != counterSlot)
            continue;
        if (t1.op != IROp::LOAD_CONST)
            continue;
        if (t2.op != IROp::ADD)
            continue;
        if (t3.op != IROp::STORE_LOCAL || t3.operands.size() < 2)
            continue;
        if (t3.operands[0].kind != IROperandKind::LOCAL_SLOT)
            continue;
        if (t3.operands[0].index != counterSlot)
            continue;
        if (t4.op != IROp::JUMP || t4.operands.size() < 1)
            continue;
        if (t4.operands[0].kind != IROperandKind::LABEL)
            continue;
        if (t4.operands[0].index != startLabel)
            continue;
        if (t5.op != IROp::LABEL || t5.operands.size() < 1)
            continue;
        if (t5.operands[0].index != exitLabel)
            continue;
        if (t6.op != IROp::POP)
            continue;
        foundTail = true;
        break;
    }
    if (!foundTail)
        return false;

    // body 必须不包含 break/continue/return/throw
    // AUDIT-R3 P1-9 fix: 排除清单扩充——体内嵌套 LABEL/条件跳转（嵌套控制流）
    // 与对计数器槽位的再写入（体内修改 i）同样会被错误展开，一并拒绝。
    for (size_t j = tailStart; j < bodyEnd; ++j) {
        const auto& bodyInstr = instrs[j];
        IROp op = bodyInstr.op;
        if (op == IROp::RETURN || op == IROp::RETURN_NULL || op == IROp::THROW || op == IROp::JUMP ||
            op == IROp::LABEL || op == IROp::JUMP_IF_FALSE) {
            return false;
        }
        if (op == IROp::STORE_LOCAL && !bodyInstr.operands.empty() &&
            bodyInstr.operands[0].kind == IROperandKind::LOCAL_SLOT && bodyInstr.operands[0].index == counterSlot) {
            return false; // 体内写计数器，展开不安全
        }
    }

    outPattern.startIndex = startIndex;
    outPattern.bodyStart = tailStart;
    outPattern.bodyEnd = bodyEnd;
    outPattern.counterSlot = counterSlot;
    outPattern.unrollCount = nInt;
    outPattern.startLabel = startLabel;
    outPattern.exitLabel = exitLabel;
    return true;
}

/// 发射展开后的循环体到 newInstrs。
/// 展开：生成 N 份 body 副本 + 计数器初始化（LOAD_CONST 0; STORE_LOCAL slot）+ 每份迭代后的计数器递增。
///
/// BUG-IR-OPT-AUDIT-5 fix: vreg 重命名。
/// 原实现直接复制 body 指令（含 operands）N 次，导致同一 vreg 在多个迭代中
/// 被重复定义（每个迭代的 ADD dest 都用同一个 vreg），违反 IR 的 SSA-like
/// 不变量（每个 vreg 应只被赋值一次）。后续 CSE/DCE 等优化 pass 会因重复
/// 定义而误判（如 DCE 看到 dest 被多个指令定义时无法正确删除无引用指令，
/// CSE 的 valueMap 会因 dest vreg 已被覆盖而误命中）。
/// 修复：对 iter >= 1 的副本，收集 body 内定义的 vreg（作为纯计算指令的 dest
/// 或 LOAD_LOCAL/LOAD_CONST 的 dest），分配 fresh vreg 并重命名副本中所有引用
/// （包括 dest 和 src），保证每个迭代的 vreg 唯一。
void emitUnrolledLoopBody(std::vector<IRInstruction>& newInstrs, const std::vector<IRInstruction>& instrs,
                          const LoopUnrollPattern& pattern, IRFunction& ir) {
    const size_t tailStart = pattern.bodyStart;
    const size_t bodyEnd = pattern.bodyEnd;
    const size_t bodySize = bodyEnd - tailStart;
    const int64_t nInt = pattern.unrollCount;
    const uint32_t counterSlot = pattern.counterSlot;

    newInstrs.reserve(static_cast<size_t>(nInt) * bodySize + 4);
    // 补 i=0 初始化
    uint32_t zeroConst = ir.addConstant(Value(static_cast<int64_t>(0)));
    newInstrs.emplace_back(IROp::LOAD_CONST,
                           std::vector<IROperand>{IROperand::vreg(ir.nextVReg++), IROperand::constant(zeroConst)},
                           instrs[pattern.startIndex].line);
    newInstrs.emplace_back(IROp::STORE_LOCAL,
                           std::vector<IROperand>{IROperand::local(counterSlot), IROperand::vreg(ir.nextVReg - 1)},
                           instrs[pattern.startIndex].line);
    // 收集 body 内定义的 vreg（dest 为 VIRTUAL 的指令：纯计算 / LOAD_LOCAL / LOAD_CONST 等）
    std::unordered_set<uint32_t> bodyDefinedVRegs;
    for (size_t j = tailStart; j < bodyEnd; ++j) {
        const auto& instr = instrs[j];
        if (instr.operands.empty())
            continue;
        if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
            bodyDefinedVRegs.insert(instr.operands[0].index);
        }
    }
    // Bug #85 fix: 常量 1 在循环外仅注册一次，避免每次迭代重复调用 addConstant
    // （addConstant 内部会去重遍历常量池，循环内调用产生 O(nInt * poolSize) 开销）
    uint32_t oneConst = ir.addConstant(Value(static_cast<int64_t>(1)));
    // 生成 nInt 份 body
    for (int64_t iter = 0; iter < nInt; ++iter) {
        // BUG-IR-OPT-AUDIT-5: 为本次迭代构建 vreg 重命名表
        // iter 0 用原 vreg（与原 body 一致，保持 SSA 单赋值），
        // iter >= 1 分配 fresh vreg 替换 body 内定义的 vreg，避免重复赋值。
        std::unordered_map<uint32_t, uint32_t> vregRemap;
        if (iter > 0) {
            for (uint32_t v : bodyDefinedVRegs) {
                vregRemap[v] = ir.nextVReg++;
            }
        }
        for (size_t j = tailStart; j < bodyEnd; ++j) {
            IRInstruction copy = instrs[j]; // 复制指令（含 operands）
            // 重命名所有 VIRTUAL 操作数（dest 和 src 都需重命名）
            for (auto& op : copy.operands) {
                if (op.kind == IROperandKind::VIRTUAL) {
                    auto it = vregRemap.find(op.index);
                    if (it != vregRemap.end()) {
                        op.index = it->second;
                    }
                }
            }
            newInstrs.push_back(std::move(copy));
        }
        // 计数器递增：LOAD_LOCAL slot; LOAD_CONST 1; ADD; STORE_LOCAL slot
        // Bug #85 fix: 复用循环外注册的 oneConst
        uint32_t iReg = ir.nextVReg++;
        uint32_t oneReg = ir.nextVReg++;
        uint32_t sumReg = ir.nextVReg++;
        newInstrs.emplace_back(IROp::LOAD_LOCAL,
                               std::vector<IROperand>{IROperand::vreg(iReg), IROperand::local(counterSlot)},
                               instrs[bodyEnd].line);
        newInstrs.emplace_back(IROp::LOAD_CONST,
                               std::vector<IROperand>{IROperand::vreg(oneReg), IROperand::constant(oneConst)},
                               instrs[bodyEnd].line);
        newInstrs.emplace_back(
            IROp::ADD, std::vector<IROperand>{IROperand::vreg(sumReg), IROperand::vreg(iReg), IROperand::vreg(oneReg)},
            instrs[bodyEnd].line);
        newInstrs.emplace_back(IROp::STORE_LOCAL,
                               std::vector<IROperand>{IROperand::local(counterSlot), IROperand::vreg(sumReg)},
                               instrs[bodyEnd].line);
    }
}
} // namespace

bool loopUnrollingPass(IRFunction& ir) {
    constexpr size_t kMaxUnrollBodySize = 20; // body 指令数上限
    bool modified = false;

    for (auto& block : ir.blocks) {
        const auto& instrs = block.instructions;
        if (instrs.size() < 12)
            continue; // 最小循环长度估算

        bool unrolled = false;
        for (size_t i = 0; i + 11 < instrs.size(); ++i) {
            LoopUnrollPattern pattern;
            if (!analyzeLoopUnrollPattern(instrs, i, ir, pattern))
                continue;

            // body 范围：[tailStart, bodyEnd)
            size_t bodySize = pattern.bodyEnd - pattern.bodyStart;
            if (bodySize == 0 || bodySize > kMaxUnrollBodySize)
                continue;

            // 展开：生成 nInt 份 body 副本 + 计数器初始化
            std::vector<IRInstruction> newInstrs;
            emitUnrolledLoopBody(newInstrs, instrs, pattern, ir);

            // 替换原循环结构 [i, bodyEnd+7) 为 newInstrs
            auto& blockInstrs = block.instructions;
            size_t replaceEnd = pattern.bodyEnd + 7;
            blockInstrs.erase(blockInstrs.begin() + i, blockInstrs.begin() + replaceEnd);
            blockInstrs.insert(blockInstrs.begin() + i, newInstrs.begin(), newInstrs.end());
            modified = true;
            unrolled = true;
            break; // 本块已修改，跳出内层循环重新扫描
        }
        if (unrolled) {
            // 块已修改，外层 for 会继续扫描后续块
        }
    }
    return modified;
}

// ---- 运行全部优化 pass ----
// BUG-IR-OPT-AUDIT-1/2/3/4 fix: IR 优化日志统一
// - 统一 source tag 为 "IR-Opt"（原 "IR" / "IR-Optimize" 混用）
// - 每个 pass 在 modified=true 时发射 LOG_DEBUG（原 4/5 pass 完全静默）
// - 统一术语：常量折叠 / 复制传播 / CSE / 循环展开 / DCE（原中英混用）
// - 汇总日志改用 LOG_INFO + 旗标矩阵（原仅报告启用旗标，缺失修改事实）
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation, bool enableDCE, bool enableCSE, bool enableLoopUnroll) {
    bool modified = false;
    // PERF-15: 常量折叠 → [复制传播] → [CSE] → [循环展开] → 死代码消除
    // 复制传播仅在寄存器式后端启用（栈式后端删除 LOAD_CONST 会导致栈下溢）
    // BUG-IR-DCE-2 fix: DCE 与复制传播控制解耦——DCE 通过 enableDCE 独立控制，
    // 避免禁用复制传播时连带禁用 DCE（寄存器式后端复制传播暂禁但 DCE 仍可安全启用）。
    // C4: CSE 与循环展开默认禁用；详见 IR.h 的安全约束文档。
    //
    // Phase-5 fix: 原 BUG-IR-OPT-AUDIT-6 防御性检查已放宽。
    // Phase-5 让 POP 携带 vreg 操作数，使 DCE 看到消费关系，配合 isPureCompute 扩展
    // （算术指令可安全删除），栈式 VM 后端现可安全启用 DCE+CSE。
    // 仅当 CSE 启用但 DCE 禁用时告警（CSE 产生的死指令需 DCE 清理）。
    if (enableCSE && !enableDCE) {
        LOG_ERROR("optimizeIR: CSE 启用但 DCE 禁用，CSE 产生的死指令无法清理"
                  "（可能导致栈式 VM 栈不平衡），已强制禁用 CSE",
                  "IR-Opt");
        // AUDIT-R6 F2 fix: 强制禁用 CSE（与 TestAuditBatch1.CSEWithoutDCEAutoDisabled 的
        // 测试意图对齐）。原实现仅告警仍照常执行 CSE，依赖“CSE 后必有 DCE 清理”
        // 的调用方约定；防御性关闭消除未来调用点误用风险。
        enableCSE = false;
    }
    // 同理：复制传播在栈式后端也不安全（删除 LOAD_CONST 会导致栈下溢），
    // 但寄存器式后端的复制传播因 RegisterBytecodeBackend 不检查操作数 kind 也暂禁（见 IR.h:720-723）。
    // 此处仅做 CSE 的强校验；复制传播的默认禁用由调用方控制（Compiler.cpp optimizeIR 调用点均传 false）。

    // BUG-IR-OPT-AUDIT-1: 启动日志记录启用旗标
    LOG_DEBUG("optimizeIR: 启用旗标 copyProp=" + std::string(enableCopyPropagation ? "Y" : "N") +
                  " dce=" + std::string(enableDCE ? "Y" : "N") + " cse=" + std::string(enableCSE ? "Y" : "N") +
                  " loopUnroll=" + std::string(enableLoopUnroll ? "Y" : "N"),
              "IR-Opt");

    // 多轮迭代直到收敛（最多 3 轮，避免无限循环）
    int totalRounds = 0;
    for (int round = 0; round < 3; ++round) {
        bool m1 = constantFoldingPass(ir);
        // BUG-IR-OPT-AUDIT-2: 每个 pass 修改时发射 LOG_DEBUG（原 4/5 pass 静默）
        if (m1)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 常量折叠 修改", "IR-Opt");
        bool m2 = enableCopyPropagation ? copyPropagationPass(ir) : false;
        if (m2)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 复制传播 修改", "IR-Opt");
        bool m3 = enableCSE ? commonSubexpressionEliminationPass(ir) : false;
        if (m3)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " CSE 修改", "IR-Opt");
        bool m4 = enableLoopUnroll ? loopUnrollingPass(ir) : false;
        if (m4)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 循环展开 修改", "IR-Opt");
        // Phase-5 fix: 栈式后端 DCE 现已安全——POP 携带所消费的 vreg 操作数，
        // DCE 看到 POP 对 vreg 的消费关系，不会删除仅被 POP 消费的纯计算指令。
        bool m5 = enableDCE ? deadCodeEliminationPass(ir) : false;
        if (m5)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " DCE 修改", "IR-Opt");
        modified = modified || m1 || m2 || m3 || m4 || m5;
        ++totalRounds;
        if (!m1 && !m2 && !m3 && !m4 && !m5)
            break; // 收敛
    }
    // BUG-IR-OPT-AUDIT-3/4: 汇总日志统一术语 + 报告迭代轮数与最终规模
    if (modified) {
        // Perf-LazyLog: LOG_INFO 宏级别过滤后跳过字符串构造
        LOG_INFO("IR 优化完成: " + std::to_string(totalRounds) + " 轮迭代, " + std::to_string(ir.constants.size()) +
                     " 常量, " + std::to_string(ir.nextVReg) + " vreg" +
                     (enableCopyPropagation ? " [copyProp=on]" : "") + (enableCSE ? " [CSE=on]" : "") +
                     (enableLoopUnroll ? " [loopUnroll=on]" : "") + (enableDCE ? " [DCE=on]" : ""),
                 "IR-Opt");
    } else {
        LOG_DEBUG("IR 优化完成: 无修改 (rounds=" + std::to_string(totalRounds) + ")", "IR-Opt");
    }
    return modified;
}
