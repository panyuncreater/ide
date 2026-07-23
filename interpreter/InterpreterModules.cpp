// ============================================================
// InterpreterModules.cpp — 模块导入/导出相关方法实现（import/export）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

// 依赖说明：import 语句需要 Lexer::scan + Parser::parse 加载模块源码；
// 其余类型（ImportStmt/ExportStmt/VarDecl/Environment 等）由 Interpreter.h 传递包含。

void Interpreter::visitImportStmt(ImportStmt& node) {
    checkBreak(&node);

    // R132-C fix: 拆为 3 个子任务 helper（路径解析/加载+缓存/名称导入），
    // 原函数 208 行 → thin orchestrator ~35 行 + 3 helper。
    std::string modulePath = resolveModulePath(node);

    // F12: 模块加载 — A6 fix: 加锁拷贝 callback 后解锁检查，避免跨线程数据竞争
    std::function<std::string(const std::string&)> loader;
    std::function<int64_t(const std::string&)> mtimeChecker;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        loader = moduleLoader_;
        mtimeChecker = moduleMtimeChecker_; // BUG-REPL-AUDIT-1 fix
    }
    if (!loader) {
        runtimeError("未设置模块加载器，无法执行 import", node.line, node.column);
    }

    // 循环依赖检测（D19 fix: 用 unordered_set 实现 O(1) 查找，替代线性扫描）
    if (moduleLoadingSet_.count(modulePath) > 0) {
        runtimeError("检测到循环依赖: " + modulePath, node.line, node.column);
        // R53-4 fix: 防御性 return。当前 runtimeError 是 throw 语义，此行不可达；
        // 但若未来 runtimeError 改为非抛出式错误处理（错误码/返回值），
        // 缺少 return 会继续执行后续加载流程，违反"循环依赖即停止"语义。
        return;
    }
    // P1-1 fix: 深度导入链递归保护（防止 C++ 栈溢出）
    if (moduleLoadingStack_.size() >= MAX_RECURSION_DEPTH) {
        runtimeError("模块导入深度超过限制 (" + std::to_string(MAX_RECURSION_DEPTH) + ")", node.line, node.column);
        // R53-4 fix: 同上，防御性 return
        return;
    }

    // 加载或获取缓存的模块环境
    auto moduleEnv = loadModuleOrGetCached(modulePath, node, loader, mtimeChecker);

    // 导入名称到当前环境
    importNamesFromModule(modulePath, node, moduleEnv);

    lastValue_ = Value::nullValue();
}

// ============================================================
// R132-C fix: visitImportStmt 拆分 helper
// ============================================================

std::string Interpreter::resolveModulePath(ImportStmt& node) {
    // 解析模块路径（相对于当前文件）— SEC-1 fix: 路径校验在 loader 检查之前，
    // 输入验证应在资源加载前，确保即使无 loader 也能拒绝恶意路径。
    std::string modulePath = node.modulePath;
    // P2-3 fix: 路径规范化 — 统一路径分隔符为 '/'，去除多余的 "./" 前缀
    // 避免相同模块因路径表示不同（如 "foo\bar.mini" vs "foo/bar.mini"）被重复加载
    for (char& c : modulePath) {
        if (c == '\\')
            c = '/';
    }
    if (modulePath.size() >= 2 && modulePath[0] == '.' && modulePath[1] == '/') {
        modulePath.erase(0, 2);
    }
    // SEC-1 fix: 路径遍历攻击防护 — 拒绝 ".." 路径段和绝对路径，防止 import 读取项目目录外文件。
    // 检查 ".." 作为完整路径段（避免误判 "foo..bar" 这样的合法文件名）。
    if (modulePath.empty()) {
        runtimeError("模块路径不能为空", node.line, node.column);
    }
    // 绝对路径检测（Unix 以 '/' 开头，Windows 以 'X:...' 驱动器路径形式）
    // BUG-MOD-1 fix: 原实现仅检测 'C:/' 形式，未拒绝 'C:foo'（Windows 驱动器相对路径），
    // 可能被 loader 解析到模块目录外的文件。修复：拒绝所有 'X:' 开头形式
    // （X 为任意字符），覆盖 'C:/'、'C:foo'、'D:path' 等。
    // 注：反斜杠已在上方统一转为正斜杠，无需再检测 '\\'。
    if (modulePath[0] == '/' || (modulePath.size() >= 2 && modulePath[1] == ':')) {
        runtimeError("模块路径不能为绝对路径: " + modulePath, node.line, node.column);
    }
    // ".." 路径段检测（按 '/' 分割检查每个段）
    std::string normalized = modulePath;
    size_t pos = 0;
    while (pos < normalized.size()) {
        size_t next = normalized.find('/', pos);
        std::string segment = (next == std::string::npos) ? normalized.substr(pos) : normalized.substr(pos, next - pos);
        if (segment == "..") {
            runtimeError("模块路径不能包含父目录引用 '..': " + modulePath, node.line, node.column);
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return modulePath;
}

std::shared_ptr<Environment>
Interpreter::loadModuleOrGetCached(const std::string& modulePath, ImportStmt& node,
                                   const std::function<std::string(const std::string&)>& loader,
                                   const std::function<int64_t(const std::string&)>& mtimeChecker) {
    // 检查缓存
    auto cacheIt = moduleCache_.find(modulePath);
    std::shared_ptr<Environment> moduleEnv;
    if (cacheIt != moduleCache_.end()) {
        // BUG-REPL-AUDIT-1 fix: 检查文件 mtime 是否变化，若变化则缓存失效
        if (mtimeChecker) {
            int64_t currentMtime = mtimeChecker(modulePath);
            auto mtimeIt = moduleMtimes_.find(modulePath);
            if (mtimeIt != moduleMtimes_.end() && mtimeIt->second != currentMtime) {
                // 文件已修改，缓存失效
                moduleCache_.erase(cacheIt);
                moduleExports_.erase(modulePath);
                moduleMtimes_.erase(modulePath);
                cacheIt = moduleCache_.end(); // 标记为未命中
            }
        }
    }
    if (cacheIt != moduleCache_.end()) {
        return cacheIt->second;
    }

    // 加载模块源码（A6 fix: 使用已拷贝的 loader，避免跨线程数据竞争）
    std::string source = loader(modulePath);
    // 空源码视为加载失败（模块不存在或 0 字节文件）。production loader
    // （IdeController/WorkerManager）在文件不存在/无法打开时均返回 ""，
    // 无法与真正的 0 字节文件区分；对空模块报错可捕获 import 笔误，
    // 与 Compiler/IR 路径行为一致（三后端统一）。
    if (source.empty()) {
        runtimeError("无法加载模块: " + modulePath, node.line, node.column);
    }

    // 词法分析 + 语法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) {
        runtimeError("模块 " + modulePath + " 语法错误", node.line, node.column);
    }

    // P1-2 fix: 使用独立的空父环境，避免模块访问导入方的全局变量
    // 模块环境隔离：模块只能访问自身定义和导出的名称，不能读写导入方全局变量
    auto moduleParentEnv = std::make_shared<Environment>(nullptr);
    moduleEnv = std::make_shared<Environment>(moduleParentEnv);
    auto savedEnv = currentEnv_;
    auto savedExported = exportedNames_;
    exportedNames_.clear();
    currentEnv_ = moduleEnv;
    moduleLoadingStack_.push_back(modulePath);
    moduleLoadingSet_.insert(modulePath); // D19 fix: 与 stack 同步维护 set

    // RA-A fix: RAII 守卫统一管理异常路径下的状态恢复（moduleEnv close、env、exported、loadingStack/Set），
    // 消除原 catch(...) + throw; 的 rethrow。正常路径通过 dismiss 跳过守卫清理。
    struct ModuleEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& saved;
        std::unordered_set<std::string>& exported;
        std::unordered_set<std::string> savedExported;
        std::vector<std::string>& loadingStack;
        std::unordered_set<std::string>& loadingSet;
        const std::string& modulePath;
        bool dismissed = false;
        ~ModuleEnvGuard() {
            if (!dismissed) {
                // AUDIT-BUG-F8 fix: 异常路径必须调用 closeCapturedVariables，将模块内闭包的
                // open upvalues 关闭为最终值快照。原实现仅恢复 env/exported/loadingStack，
                // moduleEnv 即将析构，逃逸闭包（经 throw 逃出模块）的 capturedVars 保持初始值。
                env->closeCapturedVariables();
                interp.currentEnv_ = saved;
                exported = std::move(savedExported);
                loadingStack.pop_back();
                loadingSet.erase(modulePath); // D19 fix: 同步移除
            }
        }
    } envGuard{*this,         moduleEnv,           savedEnv,          exportedNames_,
               savedExported, moduleLoadingStack_, moduleLoadingSet_, modulePath};

    for (auto& stmt : ast->statements) {
        evaluate(stmt.get());
    }
    // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复

    // RA-A fix: 正常路径，dismiss 守卫后手动执行完整清理
    envGuard.dismissed = true;
    moduleLoadingStack_.pop_back();
    moduleLoadingSet_.erase(modulePath); // D19 fix: 同步移除
    currentEnv_ = savedEnv;

    // 缓存模块环境和导出名称（必须在恢复 savedExported 之前捕获当前模块的 exports）
    moduleCache_[modulePath] = moduleEnv;
    moduleExports_[modulePath] = exportedNames_;
    // BUG-REPL-AUDIT-1 fix: 记录模块文件 mtime，下次 import 时检查是否变化
    if (mtimeChecker) {
        moduleMtimes_[modulePath] = mtimeChecker(modulePath);
    }
    exportedNames_ = savedExported;

    // 保留模块 AST（确保函数/类定义指针有效）
    replAsts_.push_back(std::move(ast));

    return moduleEnv;
}

void Interpreter::importNamesFromModule(const std::string& modulePath, ImportStmt& node,
                                        std::shared_ptr<Environment>& moduleEnv) {
    // 导入名称到当前环境（仅导入已 export 的名称）
    auto& exports = moduleExports_[modulePath];
    if (node.importAll) {
        // 导入全部导出名称
        for (const auto& name : exports) {
            const Value* valPtr = moduleEnv->get(name);
            if (valPtr) {
                currentEnv_->define(name, *valPtr);
            }
        }
    } else {
        // P2-1 fix: 原子性导入 — 先验证所有名称都存在，再统一定义
        // 避免部分名称已导入后遇到错误名称导致环境不一致
        for (const auto& name : node.names) {
            if (exports.find(name) == exports.end()) {
                runtimeError("模块 " + modulePath + " 中未导出名称: " + name, node.line, node.column);
            }
            const Value* valPtr = moduleEnv->get(name);
            if (!valPtr) {
                runtimeError("模块 " + modulePath + " 中未找到导出名称: " + name, node.line, node.column);
            }
        }
        // 全部验证通过后，统一导入
        for (const auto& name : node.names) {
            const Value* valPtr = moduleEnv->get(name);
            currentEnv_->define(name, *valPtr);
        }
    }
}

void Interpreter::visitExportStmt(ExportStmt& node) {
    checkBreak(&node);
    // F12: export 语句执行内部声明，并记录导出名称
    if (!node.declaration) {
        lastValue_ = Value::nullValue();
        return;
    }

    // 提取声明名称并标记为导出
    std::string declName;
    switch (node.declaration->nodeType) {
    case NodeType::NODE_VAR_DECL:
        declName = static_cast<VarDecl*>(node.declaration.get())->name;
        break;
    case NodeType::NODE_FUN_DECL:
        declName = static_cast<FunDecl*>(node.declaration.get())->name;
        break;
    case NodeType::NODE_CLASS_DECL:
        declName = static_cast<ClassDecl*>(node.declaration.get())->name;
        break;
    default:
        break;
    }

    // 执行声明
    evaluate(node.declaration.get());

    // 标记为导出
    if (!declName.empty()) {
        exportedNames_.insert(declName);
    }
    lastValue_ = Value::nullValue();
    return;
}
