// ============================================================
// BackendExecutionService.cpp — 后端执行服务实现（ARCH-10）
// ------------------------------------------------------------
// 封装 Lexer → Parser → Compiler → Backend 完整执行流程，
// 消除 GUI 教学面板对编译器/VM 内部头文件的直接依赖。
//
// 设计要点：
//   - 所有 Lexer/Parser/Compiler/VM/Interpreter 对象均为函数内局部变量
//   - 错误处理统一：词法/语法/编译/运行时错误通过 errorPrefix + errorMsg 返回
//   - 不持有状态：每次 execute 调用独立执行
//   - 诊断信息（diagnostics 字段）保留原始文本列表供面板渲染
// ============================================================

#include "common/BackendExecutionService.h"

#include "common/Diagnostic.h"
#include "common/IBackend.h"   // IVmBackend 完整定义（createBackend 返回类型）
#include "compiler/Bytecode.h" // BytecodeChunk::isGenerator/yieldCount/kDynamicYieldCount
#include "compiler/Compiler.h"
#include "compiler/RegisterBytecode.h" // RegBytecodeChunk::isGenerator/yieldCount
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/GcManager.h" // GcManager::instance().trackedCount()（executeWithDetail 用）
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <chrono>
#include <sstream>

// ============================================================
// 工具函数
// ============================================================

namespace {

/// 收集 DiagnosticBag 中所有错误/警告的格式化文本
std::vector<std::string> collectDiagnostics(const DiagnosticBag& bag) {
    std::vector<std::string> result;
    for (const auto& d : bag.all()) {
        result.push_back(d.format());
    }
    return result;
}

/// 提取 DiagnosticBag 中所有错误条目（含 format() 文本），用换行拼接
std::string joinErrorDiagnostics(const DiagnosticBag& bag) {
    std::string result;
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            if (!result.empty())
                result += "\n";
            result += d.format();
        }
    }
    return result;
}

/// int64_t → std::string
std::string intToString(int64_t v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

} // namespace

// ============================================================
// 后端类型显示文本
// ============================================================

const char* backendTypeToString(BackendType type) {
    switch (type) {
    case BackendType::Interpreter:
        return "Interpreter";
    case BackendType::StackVM_IR:
        return "StackVM (IR)";
    case BackendType::RegisterVM_IR:
        return "RegisterVM (IR)";
    }
    return "Unknown";
}

const char* backendTypeToShortName(BackendType type) {
    switch (type) {
    case BackendType::Interpreter:
        return "interp";
    case BackendType::StackVM_IR:
        return "stackvm";
    case BackendType::RegisterVM_IR:
        return "regvm";
    }
    return "unknown";
}

// ============================================================
// BackendExecResult 便捷方法
// ============================================================

std::string BackendExecResult::instrCountText() const {
    if (instrCount < 0)
        return "N/A";
    return intToString(instrCount);
}

std::string BackendExecResult::statusText() const {
    if (success)
        return "✅ 成功";
    std::string prefix = errorPrefix.empty() ? "错误" : errorPrefix;
    return "❌ " + prefix + ": " + errorMsg;
}

std::string BackendExecResult::compileStageText() const {
    if (!success && (errorPrefix == "词法错误" || errorPrefix == "语法错误")) {
        return "❌ " + errorPrefix + ": " + errorMsg;
    }
    if (!success && errorPrefix == "编译错误") {
        return "❌ 编译错误: " + errorMsg;
    }
    // 编译成功
    switch (backend) {
    case BackendType::Interpreter:
        return "✅ 编译成功（Interpreter 直接执行 AST，无字节码）";
    case BackendType::StackVM_IR:
        return "✅ 编译成功（IR 路径，字节码 " + instrCountText() + " 字节）";
    case BackendType::RegisterVM_IR:
        return "✅ 编译成功（寄存器 IR 路径，字节码 " + instrCountText() + " 字节）";
    }
    return "✅ 编译成功";
}

// ============================================================
// BackendExecutionService::execute
// ============================================================

BackendExecResult BackendExecutionService::execute(const std::string& src, BackendType backend) {
    BackendExecResult r;
    r.backend = backend;

    // ------------------------------------------------------------
    // 阶段 1：Lexer
    // ------------------------------------------------------------
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (const std::exception& e) {
        r.success = false;
        r.errorPrefix = "词法错误";
        r.errorMsg = e.what();
        r.diagnostics = collectDiagnostics(lex.getDiagnostics());
        return r;
    }
    if (lex.getDiagnostics().hasErrors()) {
        r.success = false;
        r.errorPrefix = "词法错误";
        r.errorMsg = joinErrorDiagnostics(lex.getDiagnostics());
        r.diagnostics = collectDiagnostics(lex.getDiagnostics());
        return r;
    }

    // ------------------------------------------------------------
    // 阶段 2：Parser
    // ------------------------------------------------------------
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        r.success = false;
        r.errorPrefix = "语法错误";
        r.errorMsg = e.what();
        r.diagnostics = collectDiagnostics(parser.getDiagnostics());
        return r;
    }
    if (!ast || parser.hasErrors()) {
        r.success = false;
        r.errorPrefix = "语法错误";
        r.errorMsg = ast ? joinErrorDiagnostics(parser.getDiagnostics()) : "AST 为空";
        r.diagnostics = collectDiagnostics(parser.getDiagnostics());
        return r;
    }

    // ------------------------------------------------------------
    // 阶段 3：根据后端类型分发执行
    // ------------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();
    std::string out;

    if (backend == BackendType::Interpreter) {
        // Interpreter 直接执行 AST，无字节码
        Interpreter interp;
        interp.setOutputCallback([&](const std::string& s) { out += s; });

        bool hasRuntimeError = false;
        std::string runtimeErr;
        try {
            interp.execute(*ast);
        } catch (const RuntimeError& e) {
            hasRuntimeError = true;
            runtimeErr = e.what();
        } catch (const std::exception& e) {
            hasRuntimeError = true;
            runtimeErr = e.what();
        }
        auto t1 = std::chrono::steady_clock::now();
        r.elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        r.elapsedMs = r.elapsedUs / 1000; // AUDIT-R2 P2-2 fix: 由微秒派生，保留兼容
        r.output = out;
        r.instrCount = -1; // Interpreter 无字节码

        if (hasRuntimeError) {
            r.success = false;
            r.errorPrefix = "运行时错误";
            r.errorMsg = runtimeErr;
        } else {
            r.success = true;
        }
        return r;
    }

    // StackVM / RegisterVM 路径：先编译
    Compiler compiler;
    if (backend == BackendType::StackVM_IR) {
        compiler.setUseIR(true);
    } else { // RegisterVM_IR
        compiler.setUseRegisterVM(true);
    }

    if (backend == BackendType::StackVM_IR) {
        CompileResult cr;
        try {
            cr = compiler.compile(*ast);
        } catch (const std::exception& e) {
            r.success = false;
            r.errorPrefix = "编译错误";
            r.errorMsg = e.what();
            r.diagnostics = collectDiagnostics(compiler.getDiagnostics());
            return r;
        }
        if (compiler.getDiagnostics().hasErrors()) {
            r.success = false;
            r.errorPrefix = "编译错误";
            r.errorMsg = compiler.getLastError();
            if (r.errorMsg.empty())
                r.errorMsg = joinErrorDiagnostics(compiler.getDiagnostics());
            r.diagnostics = collectDiagnostics(compiler.getDiagnostics());
            return r;
        }
        r.instrCount = static_cast<int64_t>(cr.mainChunk.code.size());

        VM vm;
        vm.setOutputCallback([&](const std::string& s) { out += s; });
        // AUDIT-R2 P2-3 fix: 重置计时基线——此前 t0 在编译前取，VM 路径耗时含
        // 编译时间而 Interpreter 路径不含，三后端耗时对比口径不对称。
        t0 = std::chrono::steady_clock::now();
        // AUDIT-R2 P1-4 fix: 包裹执行异常——与 Interpreter 路径及 executeWithDetail
        // 的 try/catch 处理对齐（后两者均已包裹），避免 std::bad_alloc 等异常
        // 穿透到 GUI 槽函数导致 Qt 事件循环终止。
        bool execThrew = false;
        std::string execThrewMsg;
        try {
            vm.execute(cr);
        } catch (const std::exception& e) {
            execThrew = true;
            execThrewMsg = e.what();
        } catch (...) {
            execThrew = true;
            execThrewMsg = "未知异常";
        }

        auto t1 = std::chrono::steady_clock::now();
        r.elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        r.elapsedMs = r.elapsedUs / 1000;
        r.output = out;

        if (execThrew) {
            r.success = false;
            r.errorPrefix = "运行时错误";
            r.errorMsg = execThrewMsg;
        } else if (vm.hasError()) {
            r.success = false;
            r.errorPrefix = "运行时错误";
            r.errorMsg = vm.getLastError();
        } else {
            r.success = true;
        }
        r.diagnostics = collectDiagnostics(vm.getDiagnostics());
        return r;
    }

    // RegisterVM_IR 路径
    try {
        compiler.compile(*ast);
    } catch (const std::exception& e) {
        r.success = false;
        r.errorPrefix = "编译错误";
        r.errorMsg = e.what();
        r.diagnostics = collectDiagnostics(compiler.getDiagnostics());
        return r;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        r.success = false;
        r.errorPrefix = "编译错误";
        r.errorMsg = compiler.getLastError();
        if (r.errorMsg.empty())
            r.errorMsg = joinErrorDiagnostics(compiler.getDiagnostics());
        r.diagnostics = collectDiagnostics(compiler.getDiagnostics());
        return r;
    }

    const auto& regResult = compiler.getLastRegisterResult();
    r.instrCount = static_cast<int64_t>(regResult.mainChunk.code.size());

    RegisterVM vm;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    // AUDIT-R2 P2-3 fix: 重置计时基线（同 StackVM 路径，不含编译时间）
    t0 = std::chrono::steady_clock::now();
    // AUDIT-R2 P1-4 fix: 包裹执行异常（同 StackVM 路径）
    bool execThrew = false;
    std::string execThrewMsg;
    try {
        vm.execute(regResult);
    } catch (const std::exception& e) {
        execThrew = true;
        execThrewMsg = e.what();
    } catch (...) {
        execThrew = true;
        execThrewMsg = "未知异常";
    }

    auto t1 = std::chrono::steady_clock::now();
    r.elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    r.elapsedMs = r.elapsedUs / 1000;
    r.output = out;

    if (execThrew) {
        r.success = false;
        r.errorPrefix = "运行时错误";
        r.errorMsg = execThrewMsg;
    } else if (vm.hasError()) {
        r.success = false;
        r.errorPrefix = "运行时错误";
        r.errorMsg = vm.getLastError();
    } else {
        r.success = true;
    }
    r.diagnostics = collectDiagnostics(vm.getDiagnostics());
    return r;
}

// ============================================================
// BackendExecutionService::executeWithDetail
// ============================================================
// 与 execute() 流程一致，但额外提取：
//   - generatorChunks：扫描 CompileResult/RegisterCompileResult 的 functionChunks，
//     过滤 isGenerator=true 的 chunk 并提取 yieldCount（动态则标记 isDynamic=true）
//   - opcodeCounts：通过 VM/RegisterVM 的 stepCallback 机制累加每条指令的 opcode
//   - peakTrackedCount：GcManager 跟踪对象相对执行前基线的净增峰值
//    （AUDIT-R2 P1-5 fix：不再 reset 全局单例）
//
// Interpreter 路径：
//   - generatorChunks 为空（无字节码概念）
//   - opcodeCounts 全为 0（无 opcode 概念）
//   - peakTrackedCount 仍有效（Interpreter 也使用 GcManager）

namespace {

/// 从 BytecodeChunk map 提取生成器 chunk 信息
std::vector<GeneratorChunkInfo> extractStackGeneratorChunks(const std::map<std::string, BytecodeChunk>& chunks) {
    std::vector<GeneratorChunkInfo> result;
    for (const auto& kv : chunks) {
        if (kv.second.isGenerator) {
            GeneratorChunkInfo info;
            info.name = kv.first;
            if (kv.second.yieldCount == BytecodeChunk::kDynamicYieldCount) {
                info.isDynamic = true;
                info.yieldCount = -1;
            } else {
                info.isDynamic = false;
                info.yieldCount = kv.second.yieldCount;
            }
            result.push_back(std::move(info));
        }
    }
    return result;
}

/// 从 RegBytecodeChunk map 提取生成器 chunk 信息
std::vector<GeneratorChunkInfo> extractRegGeneratorChunks(const std::map<std::string, RegBytecodeChunk>& chunks) {
    std::vector<GeneratorChunkInfo> result;
    for (const auto& kv : chunks) {
        if (kv.second.isGenerator) {
            GeneratorChunkInfo info;
            info.name = kv.first;
            if (kv.second.yieldCount == RegBytecodeChunk::kDynamicYieldCount) {
                info.isDynamic = true;
                info.yieldCount = -1;
            } else {
                info.isDynamic = false;
                info.yieldCount = kv.second.yieldCount;
            }
            result.push_back(std::move(info));
        }
    }
    return result;
}

} // namespace

BackendExecDetail BackendExecutionService::executeWithDetail(const std::string& src, BackendType backend) {
    BackendExecDetail d;
    // 复用 execute() 完成基类字段填充
    static_cast<BackendExecResult&>(d) = execute(src, backend);

    // 若基类执行失败（词法/语法/编译错误），无字节码可分析，直接返回
    if (!d.success && (d.errorPrefix == "词法错误" || d.errorPrefix == "语法错误" || d.errorPrefix == "编译错误")) {
        return d;
    }

    // 重新执行一次以收集详细数据（generator chunks + opcode counts）
    // 注：重新执行开销与 execute() 相同，但仅在用户主动请求详细分析时触发
    // （ProfileDashboardPanel/CoroutineVisualizerPanel），频率可接受。
    // AUDIT-R2 P1-5 fix: 不再调用 GcManager::instance().reset()——reset 会清空
    // 全局单例的 tracked_/aliveSet_，若此时 worker 线程正在执行用户程序，其容器
    // 节点会从跟踪结构中消失，后续 collectCycle 的存活判定被破坏，违反头文件
    // "不同线程并发调用是安全的"的契约。改为记录执行前基线，peakTrackedCount
    // 报告相对基线的净增峰值。
    // AUDIT-R2 P2-6 fix: 峰值采样不再仅在执行结束后一次——VM 路径借用
    // stepCallback 周期性采样（每 64 条指令），Interpreter 路径借用输出回调
    // 采样，执行中途分配又释放的峰值不再完全漏计。
    const size_t baseTracked = GcManager::instance().trackedCount();
    size_t peakTracked = 0;

    // Lexer + Parser（与 execute 共享逻辑，但因 detail 路径独立，此处重新执行）
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(src);
    } catch (...) {
        return d; // 词法错误已在 execute() 中记录
    }
    if (lex.getDiagnostics().hasErrors()) {
        return d;
    }
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (...) {
        return d;
    }
    if (!ast || parser.hasErrors()) {
        return d;
    }

    auto updatePeak = [&peakTracked, baseTracked]() {
        size_t cur = GcManager::instance().trackedCount();
        size_t delta = (cur > baseTracked) ? (cur - baseTracked) : 0;
        if (delta > peakTracked)
            peakTracked = delta;
    };

    if (backend == BackendType::Interpreter) {
        // Interpreter：无 opcode/generatorChunks，仅测 peakTracked
        Interpreter interp;
        // AUDIT-R2 P2-6 fix: 输出回调中采样，捕捉执行中途峰值
        interp.setOutputCallback([&updatePeak](const std::string&) { updatePeak(); });
        try {
            interp.execute(*ast);
        } catch (...) {
            // 已在 execute() 中记录
        }
        updatePeak();
        d.peakTrackedCount = peakTracked;
        return d;
    }

    if (backend == BackendType::StackVM_IR) {
        Compiler compiler;
        compiler.setUseIR(true);
        CompileResult cr;
        try {
            cr = compiler.compile(*ast);
        } catch (...) {
            return d;
        }
        if (compiler.getDiagnostics().hasErrors()) {
            return d;
        }
        d.generatorChunks = extractStackGeneratorChunks(cr.functionChunks);

        VM vm;
        vm.setOutputCallback([](const std::string&) {});
        // AUDIT-R2 P2-6 fix: 每 64 条指令采样一次 tracked 峰值
        uint64_t stepCounter = 0;
        vm.setStepCallback([&d, &stepCounter, &updatePeak](const VMStepInfo& info) {
            d.opcodeCounts[static_cast<uint8_t>(info.opcode)]++;
            if ((++stepCounter & 63u) == 0)
                updatePeak();
        });
        vm.setStepCallbackEnabled(true);
        try {
            vm.execute(cr);
        } catch (...) {
        }
        vm.setStepCallbackEnabled(false);
        updatePeak();
        d.peakTrackedCount = peakTracked;
        return d;
    }

    // RegisterVM_IR 路径
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    try {
        compiler.compile(*ast);
    } catch (...) {
        return d;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return d;
    }
    const auto& regResult = compiler.getLastRegisterResult();
    d.generatorChunks = extractRegGeneratorChunks(regResult.functionChunks);

    RegisterVM vm;
    vm.setOutputCallback([](const std::string&) {});
    // AUDIT-R2 P2-6 fix: 每 64 条指令采样一次 tracked 峰值
    uint64_t stepCounter = 0;
    vm.setStepCallback([&d, &stepCounter, &updatePeak](const RegVMStepInfo& info) {
        d.opcodeCounts[static_cast<uint8_t>(info.opcode)]++;
        if ((++stepCounter & 63u) == 0)
            updatePeak();
    });
    vm.setStepCallbackEnabled(true);
    try {
        vm.execute(regResult);
    } catch (...) {
    }
    vm.setStepCallbackEnabled(false);
    updatePeak();
    d.peakTrackedCount = peakTracked;
    return d;
}

// ============================================================
// BackendExecutionService::createBackend
// ============================================================
// ARCH-10 IVmBackend 工厂方法：创建指定类型的后端实例，返回多态 unique_ptr。
// 调用方持有 unique_ptr 自动管理生命周期，通过 IVmBackend* 多态调用
// stepOnce / getStack / getCallStack 等方法，无需依赖具体 VM/RegisterVM 类。
//
// 设计说明：
//   - Interpreter 不实现 IVmBackend（仅实现 IBackend 的基础接口），
//     传入 BackendType::Interpreter 返回 nullptr。
//   - 工厂仅创建对象，不执行代码；调用方需自行调用具体子类的 execute 入口
//     开始执行（VM::execute / RegisterVM::execute），然后通过 IVmBackend*
//     接口读取状态。
//   - 返回 unique_ptr 而非裸指针，确保 RAII 生命周期管理，避免面板忘记
//     delete 导致泄漏。
std::unique_ptr<IVmBackend> BackendExecutionService::createBackend(BackendType backend) {
    switch (backend) {
    case BackendType::StackVM_IR:
        return std::make_unique<VM>();
    case BackendType::RegisterVM_IR:
        return std::make_unique<RegisterVM>();
    case BackendType::Interpreter:
        // Interpreter 不实现 IVmBackend 的步进接口（无 opcode 概念），
        // 返回 nullptr 表示不支持。调用方应改用 execute() 走完整流程。
        return nullptr;
    }
    return nullptr;
}
