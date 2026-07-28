// ============================================================
// JitRunner.cpp — JIT 运行隔离层实现
// ------------------------------------------------------------
// 纯 C++ 编译单元（不含 Qt），安全包含 asmjit。封装编译管线 +
// JITBackend 执行 + getter 采集，返回 STL 结构供 GUI 面板消费。
// ============================================================

#include "gui/JitRunner.h"

#ifdef MINILANG_USE_JIT
#include "compiler/Compiler.h"
#include "compiler/JIT.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <set>
#endif

namespace JitRunner {

#ifdef MINILANG_USE_JIT

namespace {

/// 共享的编译流程：Lexer → Parser → Compiler，返回 CompileResult。
/// 失败时设置 error 并返回空 optional。
struct CompileOutput {
    bool ok = false;
    std::string error;
    // CompileResult 通过移动语义返回（调用方持有所有权）
};

/// 编译源码到 CompileResult。成功返回 true。
bool compileSource(const std::string& src, /*out*/ CompileResult& cr, /*out*/ std::string& error) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast) {
        error = "<parse-fail>";
        return false;
    }
    Compiler c;
    cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) {
        error = "<compile:" + c.getLastError() + ">";
        return false;
    }
    return true;
}

} // namespace

TypeFeedbackResult runTypeFeedback(const std::string& src, const std::string& methodChunkName, uint64_t threshold) {
    TypeFeedbackResult r;
    CompileResult cr;
    std::string compileErr;
    if (!compileSource(src, cr, compileErr)) {
        r.error = compileErr;
        return r;
    }
    JITBackend jit;
    if (!methodChunkName.empty()) {
        jit.setHotThreshold(methodChunkName, threshold);
    }
    jit.setOutputCallback([&](const std::string& s) { r.output += s; });
    JitResult res = jit.execute(cr);
    if (res != JitResult::OK) {
        r.error = jit.getLastError();
        return r;
    }
    r.ok = true;

    auto tf = jit.getTypeFeedback();               // vector<pair<string,TypeFeedback>>
    auto specialized = jit.getSpecializedChunks(); // vector<string>（拷贝）
    r.specializedChunks = specialized;

    std::set<std::string> specSet(specialized.begin(), specialized.end());
    r.rows.reserve(tf.size());
    for (const auto& [name, fb] : tf) {
        TypeFeedbackRow row;
        row.chunkName = name;
        row.intCount = fb.intCount;
        row.floatCount = fb.floatCount;
        row.otherCount = fb.otherCount;
        if (specSet.count(name)) {
            // 推断特化类型：floatCount>0 → FLOAT，否则 INT
            row.decision = (fb.floatCount > 0 && fb.otherCount == 0) ? "FLOAT 特化" : "INT 特化";
        } else {
            row.decision = "不特化";
        }
        r.rows.push_back(std::move(row));
    }
    return r;
}

HotspotResult runHotspot(const std::string& src, const std::string& methodChunkName, uint64_t threshold) {
    HotspotResult r;
    CompileResult cr;
    std::string compileErr;
    if (!compileSource(src, cr, compileErr)) {
        r.error = compileErr;
        return r;
    }
    JITBackend jit;
    if (!methodChunkName.empty()) {
        jit.setHotThreshold(methodChunkName, threshold);
    }
    jit.setOutputCallback([&](const std::string& s) { r.output += s; });
    JitResult res = jit.execute(cr);
    if (res != JitResult::OK) {
        r.error = jit.getLastError();
        return r;
    }
    r.ok = true;

    auto callCounts = jit.getHotChunkStats();      // vector<pair<string,uint64_t>>
    auto recompileFlags = jit.getRecompileStats(); // vector<pair<string,uint64_t>>

    // 合并 callCount 与 recompileFlag（按 chunkName 对齐）
    r.rows.reserve(callCounts.size());
    for (const auto& [name, cnt] : callCounts) {
        HotspotRow row;
        row.chunkName = name;
        row.callCount = cnt;
        // 查找对应的 recompileFlag
        for (const auto& [rname, flag] : recompileFlags) {
            if (rname == name) {
                row.recompiledFlag = flag;
                break;
            }
        }
        r.rows.push_back(std::move(row));
    }
    return r;
}

TierMetricsResult runTieredCompilation(const std::string& src, const std::string& osrChunkName, uint64_t osrThreshold) {
    TierMetricsResult r;
    CompileResult cr;
    std::string compileErr;
    if (!compileSource(src, cr, compileErr)) {
        r.error = compileErr;
        return r;
    }
    JITBackend jit;
    // 启用分层编译（隐含 lazy compilation）+ OSR 迁移
    jit.setTieredCompilation(true);
    jit.setOsrMigrationMode(true);
    if (!osrChunkName.empty() && osrThreshold > 0) {
        jit.setOsrThreshold(osrChunkName, osrThreshold);
    }
    jit.setOutputCallback([&](const std::string& s) { r.output += s; });
    JitResult res = jit.execute(cr);
    if (res != JitResult::OK) {
        r.error = jit.getLastError();
        return r;
    }
    r.ok = true;
    r.tieredEnabled = true;

    // 采集 per-chunk tier 状态
    auto tiers = jit.getChunkTiers(); // vector<pair<string, JitTier>>
    auto osrLoops = jit.getOsrLoopStats();
    auto osrRecompiles = jit.getOsrRecompileStats();
    auto lazyChunks = jit.getLazyCompiledChunks();
    auto osrMigrated = jit.getOsrMigratedChunks();
    auto deoptChunks = jit.getDeoptimizedChunks();
    r.totalDeoptCount = jit.getDeoptCount();
    r.lazyCompiledChunks = lazyChunks;
    r.osrMigratedChunks = osrMigrated;
    r.deoptimizedChunks = deoptChunks;

    std::set<std::string> lazySet(lazyChunks.begin(), lazyChunks.end());
    std::set<std::string> osrMigratedSet(osrMigrated.begin(), osrMigrated.end());
    std::set<std::string> deoptSet(deoptChunks.begin(), deoptChunks.end());

    r.rows.reserve(tiers.size());
    for (const auto& [name, tier] : tiers) {
        TierMetricsRow row;
        row.chunkName = name;
        switch (tier) {
        case minilang::JitTier::Interpreter:
            row.tier = "Tier 0 (Interpreter)";
            break;
        case minilang::JitTier::Baseline:
            row.tier = "Tier 1 (Baseline)";
            break;
        case minilang::JitTier::Specialized:
            row.tier = "Tier 2 (Specialized)";
            break;
        }
        row.lazyCompiled = lazySet.count(name) > 0;
        row.osrMigrated = osrMigratedSet.count(name) > 0;
        row.deoptimized = deoptSet.count(name) > 0;
        // 查找 OSR 回边计数和重编译标志
        for (const auto& [oname, cnt] : osrLoops) {
            if (oname == name) {
                row.osrLoopCount = cnt;
                break;
            }
        }
        for (const auto& [oname, flag] : osrRecompiles) {
            if (oname == name) {
                row.osrRecompiled = flag;
                break;
            }
        }
        r.rows.push_back(std::move(row));
    }
    return r;
}

AsmDumpResult runAsmDump(const std::string& src) {
    AsmDumpResult r;
    CompileResult cr;
    std::string compileErr;
    if (!compileSource(src, cr, compileErr)) {
        r.error = compileErr;
        return r;
    }

    // 字节码反汇编：mainChunk + 全部函数/方法 chunk（复用 disassemble）
    r.bytecodeText = "== mainChunk ==\n" + cr.mainChunk.disassemble();
    for (const auto& [name, chunk] : cr.functionChunks) {
        r.bytecodeText += "\n== " + name + " ==\n" + chunk.disassemble();
    }

    // JIT 编译+执行，捕获发射的汇编文本
    JITBackend jit;
    jit.setAsmCapture(true);
    jit.setOutputCallback([&](const std::string& s) { r.output += s; });
    JitResult res = jit.execute(cr);
    if (res != JitResult::OK) {
        r.error = jit.getLastError();
        return r;
    }
    r.asmText = jit.getCapturedAsm();
    r.ok = true;
    return r;
}

#else // !MINILANG_USE_JIT

TypeFeedbackResult runTypeFeedback(const std::string&, const std::string&, uint64_t) {
    TypeFeedbackResult r;
    r.error = "JIT disabled (MINILANG_USE_JIT=OFF)";
    return r;
}

HotspotResult runHotspot(const std::string&, const std::string&, uint64_t) {
    HotspotResult r;
    r.error = "JIT disabled (MINILANG_USE_JIT=OFF)";
    return r;
}

TierMetricsResult runTieredCompilation(const std::string&, const std::string&, uint64_t) {
    TierMetricsResult r;
    r.error = "JIT disabled (MINILANG_USE_JIT=OFF)";
    return r;
}

AsmDumpResult runAsmDump(const std::string&) {
    AsmDumpResult r;
    r.error = "JIT disabled (MINILANG_USE_JIT=OFF)";
    return r;
}

#endif // MINILANG_USE_JIT

} // namespace JitRunner
