#include "PipelineRunner.h"

// ============================================================
// PipelineRunner — 编译管线实现（ARCH-11 拆分自 IdeController）
// ============================================================

PipelineRunner::PipelineRunner(QObject* parent) : QObject(parent) {}

// ============================================================
// 管线操作
// ============================================================

bool PipelineRunner::runLexer(const std::string& source) {
    // R54-1 fix: 直接调用 runLexer/runParser 会修改 lexer_/parser_ 内部状态（诊断、
    // lastTokens_、astRoot_），若不失效缓存，后续 runFrontendPipeline 命中陈旧缓存
    // 时返回的 diagnostics 裸指针指向已被改写的 DiagnosticBag，且 astRoot_ 已被替换
    // 为其他源码（如 VmStackSandbox 关卡代码）的 AST，导致编辑器代码执行错误。
    // invalidatePipelineCache() 已存在但从未被调用——此处补全。
    invalidatePipelineCache();
    lastTokens_ = lexer_.scan(source);
    emit diagnosticsReady(lexer_.getDiagnostics());
    return !lexer_.getDiagnostics().hasErrors();
}

bool PipelineRunner::runParser() {
    // R54-1 fix: 同 runLexer，直接调用会改变管线状态，必须失效缓存。
    invalidatePipelineCache();
    astRoot_ = parser_.parse(lastTokens_);
    emit diagnosticsReady(parser_.getDiagnostics());
    return astRoot_ != nullptr && !parser_.hasErrors();
}

bool PipelineRunner::runCompiler() {
    if (!astRoot_)
        return false;

    // P1-7 + L17: 字节码磁盘缓存。
    //   - StackVM 路径(useRegisterVM_=false): tryLoad/store，magic "MLBC"，扩展名 .mbc
    //   - RegisterVM 路径(useRegisterVM_=true): tryLoadRegister/storeRegister，
    //     magic "MLRC"，扩展名 .mrc（L17 新增，复用 L11 序列化层）
    // 缓存 key = FNV-1a(source) + compilerMode(direct=0/IR=1) + optFlags（AUDIT-R3 P2-4）。
    if (bytecodeCacheEnabled_ && !lastSource_.empty()) {
        BytecodeCache::CacheKey key;
        key.source = lastSource_;
        key.compilerMode = compiler_.getUseIR() ? 1 : 0;
        // AUDIT-R3 P2-4 fix: 优化开关入键——切换 irOptimize/irSSAOptimize 后
        // 旧缓存不再命中，避免产出与当前优化配置不一致的字节码。
        key.optFlags =
            static_cast<uint8_t>((compiler_.getIROptimize() ? 1 : 0) | (compiler_.getIRSSAOptimize() ? 2 : 0));
        // mtime 暂不校验(PipelineRunner 不持有文件路径);依赖 source hash 失效
        key.mtime = 0;

        if (compiler_.getUseRegisterVM()) {
            // L17: RegisterVM 路径缓存
            auto cachedReg = bytecodeCache_.tryLoadRegister(key);
            if (cachedReg) {
                // 缓存命中:跳过编译,直接注入到 compiler_.lastRegisterResult_
                compiler_.setLastRegisterResult(std::move(*cachedReg));
                compiler_.clearDiagnostics();
                emit diagnosticsReady(compiler_.getDiagnostics());
                return true;
            }

            // 缓存未命中:正常编译
            lastCompileResult_ = compiler_.compile(*astRoot_);
            emit diagnosticsReady(compiler_.getDiagnostics());

            // 仅在编译成功时写入缓存(避免缓存错误结果)
            if (!compiler_.getDiagnostics().hasErrors()) {
                bytecodeCache_.storeRegister(key, compiler_.getLastRegisterResult());
            }
            return !compiler_.getDiagnostics().hasErrors();
        }

        // StackVM 路径缓存
        auto cached = bytecodeCache_.tryLoad(key);
        if (cached) {
            // 缓存命中:跳过编译,直接使用反序列化的 CompileResult
            lastCompileResult_ = std::move(*cached);
            compiler_.clearDiagnostics(); // 清空陈旧诊断(上次某次编译的)
            emit diagnosticsReady(compiler_.getDiagnostics());
            return true; // 缓存命中的结果必然是成功编译的产物
        }

        // 缓存未命中:正常编译
        lastCompileResult_ = compiler_.compile(*astRoot_);
        emit diagnosticsReady(compiler_.getDiagnostics());

        // 仅在编译成功时写入缓存(避免缓存错误结果)
        if (!compiler_.getDiagnostics().hasErrors()) {
            bytecodeCache_.store(key, lastCompileResult_);
        }
        return !compiler_.getDiagnostics().hasErrors();
    }

    lastCompileResult_ = compiler_.compile(*astRoot_);
    emit diagnosticsReady(compiler_.getDiagnostics());
    return !compiler_.getDiagnostics().hasErrors();
}

bool PipelineRunner::formatCode(std::string& formatted) {
    if (!astRoot_)
        return false;
    formatter_.setComments(lexer_.comments());
    formatted = formatter_.format(*astRoot_);
    return true;
}

// C9 fix: 统一前端管线实现
PipelineRunner::PipelineResult PipelineRunner::runFrontendPipeline(const std::string& source) {
    // P1-7: 记录源码,供 runCompiler 查询字节码缓存
    lastSource_ = source;

    // BUG-ORCH-7 fix: 源码级缓存——同一源码的连续调用（如 blockIfHasErrors + prepareRun）
    // 直接复用上次的 Lexer/Parser 结果，避免重复执行前端管线
    if (!cachedPipelineSource_.empty() && cachedPipelineSource_ == source) {
        return cachedPipelineResult_;
    }

    PipelineResult result;

    // 词法分析
    try {
        if (!runLexer(source)) {
            result.status = PipelineStatus::LexerFailed;
            result.setDiagnostics(lexer_.getDiagnostics()); // AUDIT-R5 R7 fix: 自持快照
            cachePipelineResult(source, result);
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::LexerFailed;
        result.errorMessage = e.what();
        cachePipelineResult(source, result);
        return result;
    }

    // 语法分析
    try {
        if (!runParser()) {
            result.status = PipelineStatus::ParserFailed;
            result.setDiagnostics(parser_.getDiagnostics()); // AUDIT-R5 R7 fix: 自持快照
            cachePipelineResult(source, result);
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::ParserFailed;
        result.errorMessage = e.what();
        cachePipelineResult(source, result);
        return result;
    }

    cachePipelineResult(source, result);
    return result; // OK
}

void PipelineRunner::cachePipelineResult(const std::string& source, const PipelineResult& result) {
    cachedPipelineSource_ = source;
    cachedPipelineResult_ = result;
}
