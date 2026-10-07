// ============================================================
// BytecodeIRBackendStackCheck.cpp — D1 栈平衡校验器实现（自 BytecodeIRBackend.cpp 拆分）
// ------------------------------------------------------------
// 审计建议"将 BytecodeIRBackend 进一步拆分为独立的后端单元"的落地：
// 本文件承载 IR lowering 的栈平衡校验器——效应数据表（kFixedStackEffect，
// 编译期完备性由 static_assert(tableIsComplete()) 强制）、公式型效应分支、
// 深度模拟（checkStackBalance）与 IROp 名称表。校验器设计不变量见
// IR.h 中 BytecodeIRBackend 私有段注释；调用点在 BytecodeIRBackend.cpp
// 的 lower() 指令循环（debug 构建启用，Release 零开销）。
// ============================================================

#include "common/Logger.h"
#include "compiler/ir/IR.h"

#include <cstdio>  // 诊断轨迹 fprintf（MINILANG_IR_STACK_DEBUG）
#include <cstdlib> // getenv_s

// ============================================================
// D1 第二步（表驱动）: 固定栈效应数据表 kFixedStackEffect
// ------------------------------------------------------------
// 审计建议的"每条 op 声明 stack_delta"数据化落地：固定效应的 IROp
// 逐条登记于本表（pops/pushes 与 lowering 发射的 StackVM 字节码
// 运行时行为一致，原 switch 版逐 case 的 VM 实现出处见 git 历史
// D1 fix 提交）；操作数依赖型（公式型）10 条在 irStackEffect 的
// switch 分支处理，不占本表。表项携带 op 标识 + 编译期完备性校验
// （tableIsComplete + static_assert）：新增 IROp 未补表/补公式时
// 编译失败，彻底关闭"静默跳过校验"的缺口。
// ============================================================
namespace {

struct FixedStackEffectEntry {
    IROp op;
    uint8_t pops;   // 运行时真消耗的操作数个数（peek 不计入，但计入"栈顶需 >=pops"约束）
    uint8_t pushes; // 压入的操作数个数
};

constexpr FixedStackEffectEntry kFixedStackEffect[] = {
    // ---- 常量/加载：压 1 ----
    {IROp::LOAD_CONST, 0, 1},
    {IROp::LOAD_NULL, 0, 1},
    {IROp::LOAD_TRUE, 0, 1},
    {IROp::LOAD_FALSE, 0, 1},
    {IROp::LOAD_LOCAL, 0, 1},
    {IROp::LOAD_GLOBAL, 0, 1},
    {IROp::LOAD_UPVALUE, 0, 1},
    // ---- peek 型写入：值留在栈上（VM.cpp OP_SET_LOCAL/OP_SET_UPVALUE peek(0)）----
    {IROp::STORE_LOCAL, 1, 1},
    {IROp::STORE_UPVALUE, 1, 1},
    // ---- pop 型写入：消费栈顶（VM.cpp OP_SET_GLOBAL/OP_DEFINE_GLOBAL/OP_SET_VAR/OP_DEFINE_VAR pop()）----
    {IROp::STORE_GLOBAL, 1, 0},
    {IROp::DEFINE_GLOBAL, 1, 0},
    // ---- 仅作用于变量表/upvalue 描述符，不碰操作数栈 ----
    {IROp::DELETE_VAR, 0, 0},
    {IROp::CLOSE_UPVALUE, 0, 0},
    // ---- 算术（VM.cpp numericOp：双操作数写 size-2）----
    {IROp::ADD, 2, 1},
    {IROp::SUB, 2, 1},
    {IROp::MUL, 2, 1},
    {IROp::DIV, 2, 1},
    {IROp::MOD, 2, 1},
    // ---- 原地改写栈顶（VM.cpp OP_NEGATE top=-top、OP_NOT back=!isTruthy）----
    {IROp::NEGATE, 1, 1},
    // ---- 比较（VM.cpp pushCompareResult：写 size-2 + pop_back）----
    {IROp::EQ, 2, 1},
    {IROp::NEQ, 2, 1},
    {IROp::LT, 2, 1},
    {IROp::GT, 2, 1},
    {IROp::LTE, 2, 1},
    {IROp::GTE, 2, 1},
    {IROp::NOT, 1, 1},
    // ---- 控制流 ----
    {IROp::JUMP, 0, 0},          // 仅改 ip；目标深度记录由 checkStackBalance 处理
    {IROp::JUMP_IF_FALSE, 1, 1}, // peek 条件不弹（VMContainers.cpp:1180），条件由后续 IR POP 消费
    {IROp::LABEL, 0, 0},         // 不产生字节码；深度锚定由 checkStackBalance 的 LABEL 分支处理
    // ---- 调用/返回/闭包（固定效应部分）----
    {IROp::RETURN, 1, 1},       // pop 返回值 -> 帧回收（resize 到 bp）-> push 回调用方栈顶
    {IROp::RETURN_NULL, 0, 0},  // 发射 OP_NULL + OP_RETURN：+1 后被 RETURN 消费，净 0
    {IROp::YIELD, 1, 1},        // VMCalls.cpp pop + push 回（净 0；yield 信号路径由生成器机制接管）
    {IROp::AWAIT, 1, 1},        // VMCalls.cpp pop 后 push 最终值（净 0）
    {IROp::MAKE_CLOSURE, 0, 1}, // VMCalls.cpp push(closure)；upvalue 描述符内联在指令流中
    // ---- 枚举/容器（固定效应部分，VMContainers.cpp 对应 case）----
    {IROp::ENUM_VARIANT_NAME, 1, 1},  // pop scrutinee、push bool
    {IROp::ENUM_VARIANT_FIELD, 2, 1}, // pop idx + pop scrutinee、push field
    {IROp::INDEX_GET, 2, 1},          // pop idx + pop obj、push 元素
    {IROp::INDEX_SET, 3, 0},          // pop val + pop idx + pop obj（obj 存 lastMutatedReceiver_）
    {IROp::LEN, 1, 1},                // peek 取长度 + pop + push（净 0）
    // ---- 成员访问（VMContainers.cpp）----
    {IROp::MEMBER_GET, 1, 1},       // pop obj、push 字段值/方法标记
    {IROp::SUPER_MEMBER_GET, 1, 1}, // 与 OP_MEMBER_GET 同 case 同效应
    {IROp::MEMBER_SET, 2, 0},       // pop val + pop obj（obj 存 lastMutatedReceiver_）
    {IROp::MEMBER_SET_LOCAL, 1, 0}, // pop val，直接改 stack_[bp+slot]
    // ---- 类（VMCalls.cpp / VMContainers.cpp）----
    {IROp::DEFINE_CLASS, 0, 0}, // 展开序列净 0：OP_CLASS_NEW +1（argCount=0）、每字段默认值 +1 后
                                // OP_INIT_FIELD -1、OP_DEFINE_CLASS -1
    {IROp::INIT_FIELD, 1, 0},   // pop 值；栈顶实例被 peek 原地修改、保留
    // ---- 异常（VMContainers.cpp / VM.cpp throwException）----
    {IROp::TRY_BEGIN, 0, 0},        // 记录 {catchIp, stackBase} 到 tryStack_；catch 深度锚定由
                                    // checkStackBalance 的 TRY_BEGIN 分支处理（tryBase + 1）
    {IROp::TRY_END, 0, 0},          // 弹 tryStack_ handler（控制栈）
    {IROp::THROW, 1, 0},            // pop 异常值 -> throwException resize 到 handler.stackBase + push
    {IROp::LOAD_EXCEPTION, 0, 0},   // 不发射字节码：throwException 已将异常值压在栈顶
    {IROp::PUSH_JUMP_TARGET, 0, 0}, // pendingJumpStack_（控制栈）
    {IROp::FINALLY_END, 0, 0},      // pendingJumpStack_（控制栈）
    // ---- 写回：全部不碰操作数栈（消费 VM 成员 lastMutatedReceiver_）----
    {IROp::WRITEBACK_MEMBER_VAR, 0, 0},
    {IROp::WRITEBACK_MEMBER_LOCAL, 0, 0},
    {IROp::WRITEBACK_INDEX_VAR, 0, 0},
    {IROp::WRITEBACK_INDEX_LOCAL, 0, 0},
    {IROp::WRITEBACK_MEMBER_UPVALUE, 0, 0},
    {IROp::WRITEBACK_INDEX_UPVALUE, 0, 0},
    // ---- 杂项（VMContainers.cpp executeMiscOps）----
    {IROp::PRINT, 1, 0},        // pop 打印值
    {IROp::POP, 1, 0},          // pop 1
    {IROp::DUP, 1, 2},          // push(peek(0))：要求栈顶 >=1，压入副本
    {IROp::LOAD_MUTATED, 0, 1}, // push(lastMutatedReceiver_)，无前置要求
    {IROp::TYPE_CHECK, 1, 1},   // peek(0) 校验类型，不弹不压
    {IROp::TYPE_TEST, 1, 1},    // pop 值、push bool
};

// 公式型操作（pops 依赖操作数，在 irStackEffect 的 switch 分支处理）：恰 10 条
constexpr bool isFormulaOp(IROp op) {
    switch (op) {
    case IROp::CALL:
    case IROp::TAIL_CALL:
    case IROp::CALL_EXPR:
    case IROp::METHOD_CALL:
    case IROp::SUPER_CALL:
    case IROp::CLASS_NEW:
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
    case IROp::BUILD_ENUM_VARIANT:
        return true;
    default:
        return false;
    }
}

// 编译期完备性校验：固定效应表中每个 op 恰出现一次，且全部非公式型
// IROp（除 PHI——SSA 中间产物，lowerInstruction 的 default 分支显式拒绝，
// 不应到达效应查询）均已登记。新增 IROp 未同步本表时编译失败。
constexpr bool tableIsComplete() {
    constexpr size_t kTableSize = sizeof(kFixedStackEffect) / sizeof(kFixedStackEffect[0]);
    // 1. 表内无重复
    for (size_t i = 0; i < kTableSize; ++i) {
        for (size_t j = i + 1; j < kTableSize; ++j) {
            if (kFixedStackEffect[i].op == kFixedStackEffect[j].op)
                return false;
        }
    }
    // 2. 非公式 op 全部在表中（PHI 除外——不可 lower，见注释）
    for (size_t v = 0; v < static_cast<size_t>(IROp::IROp_COUNT); ++v) {
        const IROp op = static_cast<IROp>(v);
        if (op == IROp::IROp_COUNT)
            continue;
        if (isFormulaOp(op) || op == IROp::PHI)
            continue;
        bool found = false;
        for (size_t i = 0; i < kTableSize; ++i) {
            if (kFixedStackEffect[i].op == op) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static_assert(tableIsComplete(), "kFixedStackEffect 未覆盖全部非公式 IROp（或存在重复登记）——"
                                 "新增 IROp 时必须同步补充效应表或 irStackEffect 的公式分支");

} // namespace

bool BytecodeIRBackend::irStackEffect(const IRInstruction& instr, StackEffect& fx) {
    // ---- 公式型（pops 依赖操作数）优先：调用点 pops=公式、pushes=1（返回值）；
    // 参数在调用点划入被调帧（bp=H-argc，VMCalls.cpp setupFunctionCallFrame），
    // 返回时 resize 统一回收，与逐字节码模拟等价 ----
    switch (instr.op) {
    case IROp::CALL:      // [dest, name, argc, ...] -> pops=argc（VMCalls.cpp 各路径 pop argc）
    case IROp::TAIL_CALL: // 布局同 CALL；降级路径 = executeCall，净效应同
        if (instr.operands.size() < 3)
            return false;
        fx = {static_cast<int>(instr.operands[2].index), 1};
        return true;
    case IROp::CALL_EXPR: // [dest, closure_vreg, argc, ...] -> pops=argc+1（闭包值 + 参数）
        if (instr.operands.size() < 3)
            return false;
        fx = {static_cast<int>(instr.operands[2].index) + 1, 1};
        return true;
    case IROp::METHOD_CALL: // [dest, obj, name, argc, recvVar, recvSlot] -> pops=argc+1
                            // （receiver 在 peek(argc) 处，VMCalls.cpp executeMethodCall）
        if (instr.operands.size() < 4)
            return false;
        fx = {static_cast<int>(instr.operands[3].index) + 1, 1};
        return true;
    case IROp::SUPER_CALL: // [dest, this, name, class_idx, argc, classIdx] -> 同 METHOD_CALL
        if (instr.operands.size() < 5)
            return false;
        fx = {static_cast<int>(instr.operands[4].index) + 1, 1};
        return true;
    case IROp::CLASS_NEW: // [dest, name, argc, ...] -> pop argc、push 实例（VMCalls.cpp:1267-1281）
        if (instr.operands.size() < 3)
            return false;
        fx = {static_cast<int>(instr.operands[2].index), 1};
        return true;
    case IROp::BUILD_ARRAY: // [dest, count, ...] -> pop count、push 数组
    case IROp::BUILD_TUPLE:
        if (instr.operands.size() < 2)
            return false;
        fx = {static_cast<int>(instr.operands[1].index), 1};
        return true;
    case IROp::BUILD_DICT: // [dest, pairCount, ...] -> key/val 交替 pop 2*pairCount
        if (instr.operands.size() < 2)
            return false;
        fx = {static_cast<int>(instr.operands[1].index) * 2, 1};
        return true;
    case IROp::BUILD_ENUM_VARIANT: // [dest, enumName, variantName, argc, ...]
        if (instr.operands.size() < 4)
            return false;
        fx = {static_cast<int>(instr.operands[3].index), 1};
        return true;
    default:
        break;
    }

    // ---- 固定效应：查表 ----
    for (const auto& e : kFixedStackEffect) {
        if (e.op == instr.op) {
            fx = {static_cast<int>(e.pops), static_cast<int>(e.pushes)};
            return true;
        }
    }
    // 表 + 公式均未覆盖：仅可能是 PHI（SSA 中间产物，不可 lower）或
    // 编译期完备性被绕过的新增 op——保守返回 false，交由 checkStackBalance 拒绝
    return false;
}
const char* BytecodeIRBackend::irOpName(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:
        return "LOAD_CONST";
    case IROp::LOAD_NULL:
        return "LOAD_NULL";
    case IROp::LOAD_TRUE:
        return "LOAD_TRUE";
    case IROp::LOAD_FALSE:
        return "LOAD_FALSE";
    case IROp::LOAD_LOCAL:
        return "LOAD_LOCAL";
    case IROp::STORE_LOCAL:
        return "STORE_LOCAL";
    case IROp::LOAD_GLOBAL:
        return "LOAD_GLOBAL";
    case IROp::STORE_GLOBAL:
        return "STORE_GLOBAL";
    case IROp::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case IROp::DELETE_VAR:
        return "DELETE_VAR";
    case IROp::LOAD_UPVALUE:
        return "LOAD_UPVALUE";
    case IROp::STORE_UPVALUE:
        return "STORE_UPVALUE";
    case IROp::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    case IROp::ADD:
        return "ADD";
    case IROp::SUB:
        return "SUB";
    case IROp::MUL:
        return "MUL";
    case IROp::DIV:
        return "DIV";
    case IROp::MOD:
        return "MOD";
    case IROp::NEGATE:
        return "NEGATE";
    case IROp::NOT:
        return "NOT";
    case IROp::EQ:
        return "EQ";
    case IROp::NEQ:
        return "NEQ";
    case IROp::LT:
        return "LT";
    case IROp::GT:
        return "GT";
    case IROp::LTE:
        return "LTE";
    case IROp::GTE:
        return "GTE";
    case IROp::LABEL:
        return "LABEL";
    case IROp::JUMP:
        return "JUMP";
    case IROp::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case IROp::YIELD:
        return "YIELD";
    case IROp::AWAIT:
        return "AWAIT";
    case IROp::CALL:
        return "CALL";
    case IROp::CALL_EXPR:
        return "CALL_EXPR";
    case IROp::TAIL_CALL:
        return "TAIL_CALL";
    case IROp::RETURN:
        return "RETURN";
    case IROp::RETURN_NULL:
        return "RETURN_NULL";
    case IROp::MAKE_CLOSURE:
        return "MAKE_CLOSURE";
    case IROp::BUILD_ARRAY:
        return "BUILD_ARRAY";
    case IROp::BUILD_DICT:
        return "BUILD_DICT";
    case IROp::BUILD_TUPLE:
        return "BUILD_TUPLE";
    case IROp::INDEX_GET:
        return "INDEX_GET";
    case IROp::INDEX_SET:
        return "INDEX_SET";
    case IROp::MEMBER_GET:
        return "MEMBER_GET";
    case IROp::MEMBER_SET:
        return "MEMBER_SET";
    case IROp::MEMBER_SET_LOCAL:
        return "MEMBER_SET_LOCAL";
    case IROp::SUPER_MEMBER_GET:
        return "SUPER_MEMBER_GET";
    case IROp::METHOD_CALL:
        return "METHOD_CALL";
    case IROp::SUPER_CALL:
        return "SUPER_CALL";
    case IROp::BUILD_ENUM_VARIANT:
        return "BUILD_ENUM_VARIANT";
    case IROp::ENUM_VARIANT_NAME:
        return "ENUM_VARIANT_NAME";
    case IROp::ENUM_VARIANT_FIELD:
        return "ENUM_VARIANT_FIELD";
    case IROp::LEN:
        return "LEN";
    case IROp::DEFINE_CLASS:
        return "DEFINE_CLASS";
    case IROp::CLASS_NEW:
        return "CLASS_NEW";
    case IROp::INIT_FIELD:
        return "INIT_FIELD";
    case IROp::TRY_BEGIN:
        return "TRY_BEGIN";
    case IROp::TRY_END:
        return "TRY_END";
    case IROp::THROW:
        return "THROW";
    case IROp::LOAD_EXCEPTION:
        return "LOAD_EXCEPTION";
    case IROp::PUSH_JUMP_TARGET:
        return "PUSH_JUMP_TARGET";
    case IROp::FINALLY_END:
        return "FINALLY_END";
    case IROp::WRITEBACK_MEMBER_VAR:
        return "WRITEBACK_MEMBER_VAR";
    case IROp::WRITEBACK_MEMBER_LOCAL:
        return "WRITEBACK_MEMBER_LOCAL";
    case IROp::WRITEBACK_INDEX_VAR:
        return "WRITEBACK_INDEX_VAR";
    case IROp::WRITEBACK_INDEX_LOCAL:
        return "WRITEBACK_INDEX_LOCAL";
    case IROp::WRITEBACK_MEMBER_UPVALUE:
        return "WRITEBACK_MEMBER_UPVALUE";
    case IROp::WRITEBACK_INDEX_UPVALUE:
        return "WRITEBACK_INDEX_UPVALUE";
    case IROp::PRINT:
        return "PRINT";
    case IROp::POP:
        return "POP";
    case IROp::DUP:
        return "DUP";
    case IROp::LOAD_MUTATED:
        return "LOAD_MUTATED";
    case IROp::TYPE_CHECK:
        return "TYPE_CHECK";
    case IROp::TYPE_TEST:
        return "TYPE_TEST";
    default:
        return "UNKNOWN_IROP";
    }
}

void BytecodeIRBackend::recordJumpDepth(uint32_t label, int depth) {
    if (depth == kUnknownDepth)
        return; // 不可校验的入边（死代码区段的跳转），不记录不校验
    auto it = labelJumpDepth_.find(label);
    if (it == labelJumpDepth_.end()) {
        labelJumpDepth_.emplace(label, depth);
        return;
    }
    if (it->second != depth) {
        // 合并点深度冲突：报告但不拒绝 lowering（AUDIT-R6 break-discard 模式兼容，
        // 详见 checkStackBalance LABEL 分支注释）
        Logger::Error("IR 栈平衡校验: LABEL #" + std::to_string(label) + " 跳转入边深度冲突（已有 " +
                          std::to_string(it->second) + "，本次 " + std::to_string(depth) + "）—— 报告后继续",
                      "IR");
    }
}

bool BytecodeIRBackend::checkStackBalance(const IRInstruction& instr) {
    // 诊断开关（D1 fix 配套）：设 MINILANG_IR_STACK_DEBUG=1 时向 stderr 打印
    // 逐指令栈深模拟轨迹（函数名 + IROp + 处理前深度 + 操作数索引），
    // 用于排查校验器报告的合并点冲突。未设置时零开销（static 初始化只读一次环境变量）。
    static const bool traceDepth = [] {
#ifdef _WIN32
        // MSVC：getenv 触发 C4996 弃用（/WX 下即错），用 secure CRT 版本
        char buf[8] = {};
        size_t sz = 0;
        getenv_s(&sz, buf, sizeof(buf), "MINILANG_IR_STACK_DEBUG");
        return sz > 0;
#else
        // GCC/clang：无 getenv_s（MSVC 专属 CRT），用标准 getenv
        return std::getenv("MINILANG_IR_STACK_DEBUG") != nullptr;
#endif
    }();
    if (traceDepth) {
        fprintf(stderr, "[stackdbg:%s] %-22s depth=%d ops=[", traceName_.c_str(), irOpName(instr.op), simStackDepth_);
        for (size_t i = 0; i < instr.operands.size() && i < 6; ++i)
            fprintf(stderr, "%s%u", i ? "," : "", instr.operands[i].index);
        fprintf(stderr, "]\n");
    }
    // ---- LABEL：按前驱期望锚定/校验深度 ----
    if (instr.op == IROp::LABEL) {
        if (instr.operands.empty() || instr.operands[0].kind != IROperandKind::LABEL)
            return true; // 无标签号（异常形态），不校验
        const uint32_t lbl = instr.operands[0].index;
        auto cit = labelCatchDepth_.find(lbl);
        if (cit != labelCatchDepth_.end()) {
            // 异常入边权威：运行时到达 catch 时高度 = TRY_BEGIN 时高度 + 1（异常值）
            simStackDepth_ = cit->second;
        } else {
            auto jit = labelJumpDepth_.find(lbl);
            if (jit != labelJumpDepth_.end()) {
                if (simStackDepth_ != kUnknownDepth && simStackDepth_ != jit->second) {
                    // 合并点深度冲突：报告但不拒绝（AUDIT-R6 break-discard 模式兼容，
                    // 详见下方 emplace 冲突分支注释）。按跳转侧期望继续校验。
                    Logger::Error("IR 栈平衡校验: LABEL #" + std::to_string(lbl) + " 深度不一致（期望 " +
                                      std::to_string(jit->second) + "，实际 " + std::to_string(simStackDepth_) +
                                      "）—— 按跳转侧继续校验",
                                  "IR");
                }
                simStackDepth_ = jit->second;
            }
            // 无任何期望且线性深度未知 → 保持未知（不可校验，不误报）
        }
        // 记录/复核本标签深度，供后续回边（循环）校验
        if (simStackDepth_ != kUnknownDepth) {
            auto ins = labelJumpDepth_.emplace(lbl, simStackDepth_);
            if (!ins.second && ins.first->second != simStackDepth_) {
                // 合并点深度冲突：报告但不拒绝 lowering。
                // 已知合法形态：AUDIT-R6 的 "break 丢弃挂起 return 值"（try 内 return 压栈
                // 后 finally 中 break 直接跳出）——break 边深度多 1，运行时由 OP_RETURN
                // 的帧回收（stack_.resize(savedBp)）清理残留，静态模拟无法表达该丢弃。
                // 若为真实 lowering 缺陷，运行时会在该合并点之后错位执行，需人工甄别。
                Logger::Error("IR 栈平衡校验: LABEL #" + std::to_string(lbl) + " 入边深度冲突（跳转 " +
                                  std::to_string(ins.first->second) + " vs 本处 " + std::to_string(simStackDepth_) +
                                  "）—— 按跳转侧继续校验",
                              "IR");
                simStackDepth_ = ins.first->second;
            }
        }
        return true;
    }

    // ---- 普通指令：效应表 + 深度更新 ----
    StackEffect fx{0, 0};
    if (!irStackEffect(instr, fx)) {
        // 表 + 公式均未覆盖：仅可能是 PHI（不可 lower，lowerInstruction 的
        // default 分支应先行拒绝）或编译期完备性被绕过——D1 第二步起显式
        // 拒绝，不再保守跳过（静默跳过会让新增 IROp 失去校验）
        Logger::Error("IR 栈平衡校验: " + std::string(irOpName(instr.op)) + " (line=" + std::to_string(instr.line) +
                          ") 未登记栈效应表 —— 拒绝 lowering",
                      "IR");
        return false;
    }
    if (simStackDepth_ != kUnknownDepth) {
        if (fx.pops > simStackDepth_) {
            Logger::Error("IR 栈平衡校验: " + std::string(irOpName(instr.op)) + " (line=" + std::to_string(instr.line) +
                              ") 需弹出 " + std::to_string(fx.pops) + " 个操作数，但栈深仅 " +
                              std::to_string(simStackDepth_) + " —— lowering 栈不平衡",
                          "IR");
            return false;
        }
        simStackDepth_ += fx.pushes - fx.pops;
    }
    // 深度未知区段（上一条为 JUMP/RETURN/THROW 且未到 LABEL 的死代码）：
    // 深度保持未知，下一 LABEL 处按前驱期望重新锚定

    // ---- 控制流特殊处理 ----
    switch (instr.op) {
    case IROp::JUMP: {
        // 无条件跳转：记录目标期望深度；线性流终止（至下一 LABEL 前为死代码）
        if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
            recordJumpDepth(instr.operands[0].index, simStackDepth_);
        }
        simStackDepth_ = kUnknownDepth;
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        // 条件为 peek：跳转侧与 fall-through 侧深度相同
        if (instr.operands.size() >= 2 && instr.operands[1].kind == IROperandKind::LABEL) {
            recordJumpDepth(instr.operands[1].index, simStackDepth_);
        }
        break;
    }
    case IROp::TRY_BEGIN: {
        // catch 入口深度 = TRY_BEGIN 时深度 + 1（throwException resize 到
        // handler.stackBase 后压入异常值，VM.cpp:187-189）
        if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL &&
            simStackDepth_ != kUnknownDepth) {
            labelCatchDepth_[instr.operands[0].index] = simStackDepth_ + 1;
        }
        break;
    }
    case IROp::RETURN:
    case IROp::RETURN_NULL:
    case IROp::THROW:
        // 控制流离开线性路径：至下一 LABEL 前为死代码，深度置未知
        simStackDepth_ = kUnknownDepth;
        break;
    default:
        break;
    }
    return true;
}
