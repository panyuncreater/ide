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

    // vreg → 寄存器号映射（IR 的虚拟寄存器直接映射为物理寄存器）
    // 简单线性分配：vreg N → register N（如果 N < 32）
    // 超出 32 的 vreg 溢出到栈（未来实现溢出逻辑）
    std::unordered_map<uint32_t, uint8_t> vregToReg_;

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
};
