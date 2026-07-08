#pragma once

// ============================================================
// RegisterBytecodeBackend.h — PERF-14: IR → 寄存器式字节码 lowering
// ------------------------------------------------------------
// 实现 IRBackend 接口，将 IRFunction lowering 为 RegBytecodeChunk。
// 与 BytecodeIRBackend（栈式）并存，通过 setUseRegisterVM(true) 启用。
// ============================================================

#include "compiler/IR.h"
#include "compiler/RegisterBytecode.h"
#include <memory>
#include <map>
#include <unordered_map>

class RegisterBytecodeBackend : public IRBackend {
public:
    RegisterBytecodeBackend();
    ~RegisterBytecodeBackend() override = default;

    /// lower 单个 IRFunction（IRBackend 接口）
    bool lower(const IRFunction& ir) override;

    /// 取生成的寄存器字节码 chunk
    std::unique_ptr<RegBytecodeChunk> takeChunk() { return std::move(chunk_); }

    /// lower 整个 IRModule（main + 子函数）
    bool lowerModule(const IRModule& module);

    /// 取生成的函数 chunks
    std::map<std::string, RegBytecodeChunk> takeFunctionChunks() { return std::move(functionChunks_); }

    /// IR 指令 → 字节码偏移映射（调试器用）
    const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset() const { return irToBytecodeOffset_; }

private:
    std::unique_ptr<RegBytecodeChunk> chunk_;
    std::map<std::string, RegBytecodeChunk> functionChunks_;
    std::vector<std::pair<size_t, size_t>> irToBytecodeOffset_;
    bool hasError_ = false;  // vregToReg 溢出等错误标志，lower 末尾检查

    // vreg → 寄存器号映射（活跃 vreg 的当前分配）
    // P0-REGALLOC fix: 原实现 vreg N → reg localCount+N 不复用 vreg，导致稍长程序溢出 32 上限。
    // 改为基于"全局最后使用点"的线性扫描寄存器分配：vreg 首次出现时分配寄存器，
    // 最后使用后释放到 freeRegs_ 供后续 vreg 复用。IR 的 vreg 是 SSA-like 的（每个
    // vreg 只定义一次），因此生命周期从首次定义到最后使用，释放点安全。
    std::unordered_map<uint32_t, uint8_t> vregToReg_;        // 活跃 vreg → 物理寄存器
    std::unordered_map<uint32_t, size_t> vregLastUse_;       // vreg → 全局最后使用 instrIndex
    std::unordered_map<size_t, std::vector<uint32_t>> lastUseToVregs_;  // instrIndex → 在此处最后使用的 vreg 列表
    std::vector<uint8_t> freeRegs_;                          // 空闲物理寄存器栈（可复用）

    // 标签 → 字节码偏移映射
    std::unordered_map<uint32_t, size_t> labelToOffset_;

    // 待回填跳转
    struct PendingJump {
        size_t codeOffset;      // 占位符在 code 中的偏移
        uint32_t targetLabel;   // 目标标签
    };
    std::vector<PendingJump> pendingJumps_;

    // 辅助方法
    void emitShort(uint16_t v, int line);
    uint16_t addStringConstant(const std::string& s, const IRFunction& ir);
    uint8_t vregToReg(uint32_t vreg);
    bool lowerInstruction(const IRInstruction& instr, const IRFunction& ir);
    bool patchJumps();
    void resetState();
    // P0-REGALLOC fix: 寄存器分配辅助
    void collectVRegLastUse(const IRFunction& ir);  // 预扫描构建 vregLastUse_ 与 lastUseToVregs_
    void releaseDeadVRegs(size_t instrIndex);        // 释放最后使用点 == instrIndex 的 vreg 寄存器
};
