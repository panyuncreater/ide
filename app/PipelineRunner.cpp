#include "PipelineRunner.h"

// ============================================================
// PipelineRunner — 编译管线实现（ARCH-11 拆分自 IdeController）
// ============================================================

PipelineRunner::PipelineRunner(QObject* parent)
    : QObject(parent) {
}

// ============================================================
// 管线操作
// ============================================================

bool PipelineRunner::runLexer(const std::string& source) {
    lastTokens_ = lexer_.scan(source);
    emit diagnosticsReady(lexer_.getDiagnostics());
    return !lexer_.getDiagnostics().hasErrors();
}

bool PipelineRunner::runParser() {
    astRoot_ = parser_.parse(lastTokens_);
    emit diagnosticsReady(parser_.getDiagnostics());
    return astRoot_ != nullptr && !parser_.hasErrors();
}

bool PipelineRunner::runCompiler() {
    if (!astRoot_) return false;
    lastCompileResult_ = compiler_.compile(*astRoot_);
    emit diagnosticsReady(compiler_.getDiagnostics());
    return !compiler_.getDiagnostics().hasErrors();
}

bool PipelineRunner::formatCode(std::string& formatted) {
    if (!astRoot_) return false;
    formatter_.setComments(lexer_.comments());
    formatted = formatter_.format(*astRoot_);
    return true;
}

// C9 fix: 统一前端管线实现
PipelineRunner::PipelineResult PipelineRunner::runFrontendPipeline(const std::string& source) {
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
            result.diagnostics = &lexer_.getDiagnostics();
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
            result.diagnostics = &parser_.getDiagnostics();
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
    return result;  // OK
}

void PipelineRunner::cachePipelineResult(const std::string& source, const PipelineResult& result) {
    cachedPipelineSource_ = source;
    cachedPipelineResult_ = result;
}
