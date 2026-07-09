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
    return result; // OK
}

void PipelineRunner::cachePipelineResult(const std::string& source, const PipelineResult& result) {
    cachedPipelineSource_ = source;
    cachedPipelineResult_ = result;
}
