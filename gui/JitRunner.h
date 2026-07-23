#pragma once

// ============================================================
// JitRunner — JIT 运行隔离层（Qt ↔ asmjit 冲突解耦）
// ------------------------------------------------------------
// 问题：compiler/JIT.h 包含 <asmjit/asmjit.h>，asmjit 的标识符
// （InstId/inst_id 等）与 Qt 头文件拉入的 Windows 宏冲突，导致
// 同时包含 Qt 头与 JIT.h 的编译单元解析失败。
//
// 方案：JitRunner 是纯 C++ 桥接层（不包含任何 Qt 头），封装
// "Lexer→Parser→Compiler→JITBackend→采集 getter" 全流程，返回
// STL 数据结构。GUI 面板（JitVisualizerPanel）只包含本头文件，
// 不直接包含 asmjit，彻底回避冲突。
//
// 线程安全：runTypeFeedback/runHotspot 在 UI 线程同步执行 JIT
// （示例代码小，毫秒级）。JITBackend 为局部实例，execute 返回后
// 读 getter，无并发。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace JitRunner {

/// 单个 chunk 的类型反馈行
struct TypeFeedbackRow {
    std::string chunkName;
    uint64_t intCount = 0;
    uint64_t floatCount = 0;
    uint64_t otherCount = 0;
    std::string decision; // "INT 特化" / "FLOAT 特化" / "不特化"
};

/// 单个 chunk 的热点统计行
struct HotspotRow {
    std::string chunkName;
    uint64_t callCount = 0;
    uint64_t recompiledFlag = 0;
};

/// 类型反馈场景运行结果
struct TypeFeedbackResult {
    std::string output; // JIT print 输出
    std::string error;  // 错误信息（ok=false 时有效）
    bool ok = false;    // 是否成功
    std::vector<TypeFeedbackRow> rows;
    std::vector<std::string> specializedChunks; // 被特化的 chunk 名列表
};

/// 热点检测场景运行结果
struct HotspotResult {
    std::string output;
    std::string error;
    bool ok = false;
    std::vector<HotspotRow> rows;
};

/// R160: 单个 chunk 的分层编译/OSR/deopt 指标行
struct TierMetricsRow {
    std::string chunkName;
    std::string tier;           // "Tier 0 (Interpreter)" / "Tier 1 (Baseline)" / "Tier 2 (Specialized)"
    uint64_t osrLoopCount = 0;  // OSR 循环回边计数
    uint64_t osrRecompiled = 0; // OSR 是否触发过重编译（0/1）
    bool osrMigrated = false;   // 是否完成真正 OSR 栈帧迁移
    bool lazyCompiled = false;  // 是否通过 lazy compilation 编译
    bool deoptimized = false;   // 是否被反优化
};

/// R160: 分层编译场景运行结果
struct TierMetricsResult {
    std::string output;
    std::string error;
    bool ok = false;
    std::vector<TierMetricsRow> rows;
    uint64_t totalDeoptCount = 0;                // 总反优化次数
    std::vector<std::string> lazyCompiledChunks; // lazy 编译的 chunk 列表
    std::vector<std::string> osrMigratedChunks;  // OSR 迁移的 chunk 列表
    std::vector<std::string> deoptimizedChunks;  // 反优化的 chunk 列表
    bool tieredEnabled = false;                  // 是否启用了分层编译
};

/// 运行类型反馈场景：编译+执行源码，采集 per-chunk 类型反馈与特化结果。
/// methodChunkName 用于调小热点阈值加速演示（默认阈值 1000）。
/// JIT 未启用时返回 ok=false、error="JIT disabled"。
TypeFeedbackResult runTypeFeedback(const std::string& src, const std::string& methodChunkName, uint64_t threshold);

/// 运行热点检测场景：编译+执行源码，采集 per-chunk 调用计数与重编译标志。
HotspotResult runHotspot(const std::string& src, const std::string& methodChunkName, uint64_t threshold);

/// R160: 运行分层编译场景：启用 tiered compilation + OSR + deopt，
/// 采集 per-chunk tier 状态、OSR 迁移、lazy compilation、反优化指标。
/// osrChunkName 用于调小 OSR 阈值加速演示。
TierMetricsResult runTieredCompilation(const std::string& src, const std::string& osrChunkName, uint64_t osrThreshold);

} // namespace JitRunner
