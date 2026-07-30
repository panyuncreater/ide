#include "compiler/IR.h"
#include "ast/ASTNode.h"
#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: IR 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/RuntimeLimits.h"     // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"               // R109 TCO: isTailRecursiveReturn（与 Compiler.cpp 共享识别逻辑）
#include "common/TypeChecker.h"       // R163 泛型扩展: isTypeParameter（emitTypeCheckIR 擦除）
#include "compiler/BytecodeCache.h"   // L11: 预编译模块 .minic 加载
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
// IRToString — 调试输出
// ============================================================

// Dedup-4E: 公共 API，供 IrViewer 与 IRToString 共用（避免 IrViewer 维护过时副本）
// 数据驱动查表实现：用 static const std::unordered_map<IROp, const char*> 替代 switch，
// 主函数变为 O(1) 平均时间复杂度的查表。新增 IROp 时只需在表中追加一行。
const char* irOpName(IROp op) {
    // 单次初始化的名称表（C++11 thread-safe static initialization）
    static const std::unordered_map<IROp, const char*> kIrOpNames = {
        // 常量加载
        {IROp::LOAD_CONST, "LOAD_CONST"},
        {IROp::LOAD_NULL, "LOAD_NULL"},
        {IROp::LOAD_TRUE, "LOAD_TRUE"},
        {IROp::LOAD_FALSE, "LOAD_FALSE"},
        // 变量访问
        {IROp::LOAD_LOCAL, "LOAD_LOCAL"},
        {IROp::STORE_LOCAL, "STORE_LOCAL"},
        {IROp::LOAD_GLOBAL, "LOAD_GLOBAL"},
        {IROp::STORE_GLOBAL, "STORE_GLOBAL"},
        {IROp::DEFINE_GLOBAL, "DEFINE_GLOBAL"},
        {IROp::DELETE_VAR, "DELETE_VAR"},
        {IROp::LOAD_UPVALUE, "LOAD_UPVALUE"},
        {IROp::STORE_UPVALUE, "STORE_UPVALUE"},
        {IROp::CLOSE_UPVALUE, "CLOSE_UPVALUE"},
        // 算术
        {IROp::ADD, "ADD"},
        {IROp::SUB, "SUB"},
        {IROp::MUL, "MUL"},
        {IROp::DIV, "DIV"},
        {IROp::MOD, "MOD"},
        {IROp::NEGATE, "NEGATE"},
        // 比较
        {IROp::EQ, "EQ"},
        {IROp::NEQ, "NEQ"},
        {IROp::LT, "LT"},
        {IROp::GT, "GT"},
        {IROp::LTE, "LTE"},
        {IROp::GTE, "GTE"},
        // 逻辑
        {IROp::NOT, "NOT"},
        // 控制流
        {IROp::JUMP, "JUMP"},
        {IROp::JUMP_IF_FALSE, "JUMP_IF_FALSE"},
        {IROp::LABEL, "LABEL"},
        // 调用
        {IROp::CALL, "CALL"},
        {IROp::CALL_EXPR, "CALL_EXPR"},
        {IROp::RETURN, "RETURN"},
        {IROp::RETURN_NULL, "RETURN_NULL"},
        // R164 协程/生成器
        {IROp::YIELD, "YIELD"},
        {IROp::AWAIT, "AWAIT"},
        // 闭包
        {IROp::MAKE_CLOSURE, "MAKE_CLOSURE"},
        // 容器
        {IROp::BUILD_ARRAY, "BUILD_ARRAY"},
        {IROp::BUILD_DICT, "BUILD_DICT"},
        {IROp::BUILD_TUPLE, "BUILD_TUPLE"},
        {IROp::INDEX_GET, "INDEX_GET"},
        {IROp::INDEX_SET, "INDEX_SET"},
        // 成员访问
        {IROp::MEMBER_GET, "MEMBER_GET"},
        {IROp::MEMBER_SET, "MEMBER_SET"},
        {IROp::MEMBER_SET_LOCAL, "MEMBER_SET_LOCAL"},
        {IROp::SUPER_MEMBER_GET, "SUPER_MEMBER_GET"},
        // 方法调用
        {IROp::METHOD_CALL, "METHOD_CALL"},
        {IROp::SUPER_CALL, "SUPER_CALL"},
        // 类
        {IROp::DEFINE_CLASS, "DEFINE_CLASS"},
        {IROp::CLASS_NEW, "CLASS_NEW"},
        {IROp::INIT_FIELD, "INIT_FIELD"},
        // 异常
        {IROp::TRY_BEGIN, "TRY_BEGIN"},
        {IROp::TRY_END, "TRY_END"},
        {IROp::THROW, "THROW"},
        {IROp::LOAD_EXCEPTION, "LOAD_EXCEPTION"},
        // AUDIT-P1.1 fix: finally 续跳
        {IROp::PUSH_JUMP_TARGET, "PUSH_JUMP_TARGET"},
        {IROp::FINALLY_END, "FINALLY_END"},
        // 写回指令（限制2）
        {IROp::WRITEBACK_MEMBER_VAR, "WRITEBACK_MEMBER_VAR"},
        {IROp::WRITEBACK_MEMBER_LOCAL, "WRITEBACK_MEMBER_LOCAL"},
        {IROp::WRITEBACK_INDEX_VAR, "WRITEBACK_INDEX_VAR"},
        {IROp::WRITEBACK_INDEX_LOCAL, "WRITEBACK_INDEX_LOCAL"},
        {IROp::WRITEBACK_MEMBER_UPVALUE, "WRITEBACK_MEMBER_UPVALUE"},
        {IROp::WRITEBACK_INDEX_UPVALUE, "WRITEBACK_INDEX_UPVALUE"},
        // 其他
        {IROp::PRINT, "PRINT"},
        {IROp::POP, "POP"},
        {IROp::DUP, "DUP"},
        {IROp::LOAD_MUTATED, "LOAD_MUTATED"},
        {IROp::TYPE_CHECK, "TYPE_CHECK"},
        {IROp::TYPE_TEST, "TYPE_TEST"},
        // P2-10 IR SSA 基础设施
        {IROp::PHI, "PHI"},
        // L18 eng-tailcall
        {IROp::TAIL_CALL, "TAIL_CALL"},
    };
    auto it = kIrOpNames.find(op);
    if (it != kIrOpNames.end()) {
        return it->second;
    }
    // Bug-6 同型修复：枚举扩展时静默走 "?"，加 default 兜底
    // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
    Logger::Error("irOpName: 未处理的 IROp 枚举值 " + std::to_string(static_cast<int>(op)), "IR");
    assert(false && "irOpName: 未处理的 IROp 枚举值");
    return "?";
}

// Dedup-4F: 单条 IR 指令格式化，IRToString 与 IrViewer 共用（避免 IrViewer 维护过时副本）
std::string formatIRInstruction(const IRInstruction& instr) {
    std::ostringstream oss;
    oss << irOpName(instr.op);
    for (const auto& operand : instr.operands) {
        const char* kindStr = "?";
        switch (operand.kind) {
        case IROperandKind::CONSTANT:
            kindStr = "c";
            break;
        case IROperandKind::VIRTUAL:
            kindStr = "v";
            break;
        case IROperandKind::LABEL:
            kindStr = "L";
            break;
        case IROperandKind::GLOBAL_NAME:
            kindStr = "g";
            break;
        case IROperandKind::LOCAL_SLOT:
            kindStr = "s";
            break;
        case IROperandKind::UPVALUE_IDX:
            kindStr = "u";
            break;
        case IROperandKind::FIELD_NAME:
            kindStr = "f";
            break;
        case IROperandKind::FUNC_NAME:
            kindStr = "fn";
            break;
        case IROperandKind::IMM_UINT:
            kindStr = "#";
            break;
        default:
            kindStr = "?";
            break; // Bug-6: 缺 default 防新增枚举静默走 ?
        }
        oss << " " << kindStr << operand.index;
    }
    if (instr.line > 0)
        oss << "  ; line " << instr.line;
    return oss.str();
}

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
            oss << "    " << formatIRInstruction(instr) << "\n";
        }
        oss << "\n";
    }
    oss << "}\n";
    return oss.str();
}
