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
    PipelineResult result;

    // 词法分析
    try {
        if (!runLexer(source)) {
            result.status = PipelineStatus::LexerFailed;
            result.diagnostics = &lexer_.getDiagnostics();
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::LexerFailed;
        result.errorMessage = e.what();
        return result;
    }

    // 语法分析
    try {
        if (!runParser()) {
            result.status = PipelineStatus::ParserFailed;
            result.diagnostics = &parser_.getDiagnostics();
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::ParserFailed;
        result.errorMessage = e.what();
        return result;
    }

    return result;  // OK
}
