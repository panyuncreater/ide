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
    // BUG-M4 fix: loaderPath 保留大小写（供 loader 请求），cacheKey 在 Windows 上小写折叠
    // （供所有缓存/去重 map 键），使同一文件的不同大小写拼写去重为同一模块。
    std::string loaderPath = resolveModulePath(node);
    std::string cacheKey = moduleCacheKey(loaderPath);

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

    // P2-14 循环导入延迟加载：不再报错，由 loadModuleOrGetCached 返回部分加载的模块环境。
    // 循环回路闭合时，模块环境已创建但可能尚未填充所有导出名称——访问未定义名称时
    // 由 Environment::get 抛"未定义变量"错误（保持现有语义），访问已定义名称返回当前值。
    // 语义参考 ES Modules + Python：模块对象立即创建，内容按执行顺序填充。
    // 深度保护仍保留（防止 C++ 栈溢出）
    if (moduleLoadingStack_.size() >= MAX_RECURSION_DEPTH) {
        runtimeError("模块导入深度超过限制 (" + std::to_string(MAX_RECURSION_DEPTH) + ")", node.line, node.column);
        // R53-4 fix: 同上，防御性 return
        return;
    }

    // 加载或获取缓存的模块环境
    auto moduleEnv = loadModuleOrGetCached(loaderPath, cacheKey, node, loader, mtimeChecker);

    // 导入名称到当前环境
    importNamesFromModule(cacheKey, node, moduleEnv);

    lastValue_ = Value::nullValue();
}

// ============================================================
// R132-C fix: visitImportStmt 拆分 helper
// ============================================================

std::string Interpreter::resolveModulePath(ImportStmt& node) {
    // 解析模块路径（相对于当前文件）— SEC-1 fix: 路径校验在 loader 检查之前，
    // 输入验证应在资源加载前，确保即使无 loader 也能拒绝恶意路径。
    // AUDIT-R5 R6 fix: 规范化改用 normalizeModulePathKey（common/ModulePath.h
    // 单一事实源，与 clearModuleCache/Compiler/AstIRBuilder/pathHash 五处同步），
    // 补齐连续斜杠折叠与 Windows 大小写折叠（原仅 pathHash 折叠双斜杠，
    // 等价拼写导致同一模块双重加载、副作用翻倍）。
    std::string modulePath = normalizeModulePathKey(node.modulePath);
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
Interpreter::loadModuleOrGetCached(const std::string& loaderPath, const std::string& cacheKey, ImportStmt& node,
                                   const std::function<std::string(const std::string&)>& loader,
                                   const std::function<int64_t(const std::string&)>& mtimeChecker) {
    // BUG-M4 fix: 所有缓存/去重 map 键用 cacheKey（Windows 小写折叠），loader/mtimeChecker
    // 与错误消息用 loaderPath（保留大小写，供大小写敏感的 loader 正确解析）。
    // 检查缓存
    auto cacheIt = moduleCache_.find(cacheKey);
    std::shared_ptr<Environment> moduleEnv;
    // P2-14 循环导入延迟加载：缓存命中时区分完整/部分加载
    if (cacheIt != moduleCache_.end() && moduleLoadingSet_.count(cacheKey) > 0) {
        // 部分加载（循环导入）：模块正在加载中，返回已创建的部分 env
        // 不检查 mtime（模块还在加载中，文件未被修改）
        // 模块环境可能尚未填充所有导出名称——访问未定义名称时由 Environment::get
        // 抛"未定义变量"错误（保持现有语义），访问已定义名称返回当前值
        return cacheIt->second;
    }
    if (cacheIt != moduleCache_.end()) {
        // 完整缓存命中：检查文件 mtime 是否变化，若变化则缓存失效
        if (mtimeChecker) {
            int64_t currentMtime = mtimeChecker(loaderPath);
            auto mtimeIt = moduleMtimes_.find(cacheKey);
            if (mtimeIt != moduleMtimes_.end() && mtimeIt->second != currentMtime) {
                // 文件已修改，缓存失效
                moduleCache_.erase(cacheIt);
                moduleExports_.erase(cacheKey);
                moduleMtimes_.erase(cacheKey);
                cacheIt = moduleCache_.end(); // 标记为未命中
            }
        }
        if (cacheIt != moduleCache_.end()) {
            return cacheIt->second;
        }
    }

    // 加载模块源码（A6 fix: 使用已拷贝的 loader，避免跨线程数据竞争）
    std::string source = loader(loaderPath);
    // 空源码视为加载失败（模块不存在或 0 字节文件）。production loader
    // （IdeController/WorkerManager）在文件不存在/无法打开时均返回 ""，
    // 无法与真正的 0 字节文件区分；对空模块报错可捕获 import 笔误，
    // 与 Compiler/IR 路径行为一致（三后端统一）。
    if (source.empty()) {
        runtimeError("无法加载模块: " + loaderPath, node.line, node.column);
    }

    // 词法分析 + 语法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    // AUDIT-R5 BUG-03 fix: 检查词法错误（对齐 Compiler::loadAndParseModule 的 BUG-FIX）。
    // 原实现吞掉 Lexer 诊断，非法 token 被丢弃后残缺模块被静默执行。
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        std::string detail = diags.empty() ? "" : (": " + diags.front().message);
        runtimeError("模块 " + loaderPath + " 词法错误" + detail, node.line, node.column);
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    // AUDIT-R5 BUG-03 fix: 检查可恢复语法错误（parser.hasErrors()）——原实现仅检查
    // AST 非空，Parser 错误恢复后返回的残缺 AST 会被当作成功加载静默执行，
    // 与 VM/IR 路径（loadAndParseModule 报“模块语法错误”）不一致。
    if (!ast || parser.hasErrors()) {
        const auto& diags = parser.getDiagnostics().all();
        std::string detail = diags.empty() ? "" : (": " + diags.front().message);
        runtimeError("模块 " + loaderPath + " 语法错误" + detail, node.line, node.column);
    }

    // P1-2 fix: 使用独立的空父环境，避免模块访问导入方的全局变量
    // 模块环境隔离：模块只能访问自身定义和导出的名称，不能读写导入方全局变量
    auto moduleParentEnv = std::make_shared<Environment>(nullptr);
    moduleEnv = std::make_shared<Environment>(moduleParentEnv);

    // P2-14 循环导入延迟加载：立即入缓存，支持循环回路闭合时返回部分 env
    // 模块语句执行完成后 env 已被填充，cache 指向同一对象无需重新赋值
    moduleCache_[cacheKey] = moduleEnv;

    // P2-14 预扫描 export 名到 moduleExports_，让循环导入命中时能验证具名导入
    // 对齐 VM/IR 路径的 collectModuleExports（预扫描 ExportStmt 包装的声明名）
    // exportedNames_ 仍按原机制在 visitExportStmt 中填充，加载完成后覆盖此预扫描值
    {
        std::unordered_set<std::string> prescanExports;
        for (auto& stmt : ast->statements) {
            if (!stmt || stmt->nodeType != NodeType::NODE_EXPORT_STMT)
                continue;
            auto* exp = static_cast<ExportStmt*>(stmt.get());
            if (!exp->declaration)
                continue;
            ASTNode* decl = exp->declaration.get();
            switch (decl->nodeType) {
            case NodeType::NODE_VAR_DECL:
                prescanExports.insert(static_cast<VarDecl*>(decl)->name);
                break;
            case NodeType::NODE_CLASS_DECL:
                prescanExports.insert(static_cast<ClassDecl*>(decl)->name);
                break;
            case NodeType::NODE_FUN_DECL:
                prescanExports.insert(static_cast<FunDecl*>(decl)->name);
                break;
            default:
                break;
            }
        }
        moduleExports_[cacheKey] = std::move(prescanExports);
    }

    auto savedEnv = currentEnv_;
    auto savedExported = exportedNames_;
    exportedNames_.clear();
    currentEnv_ = moduleEnv;
    moduleLoadingStack_.push_back(cacheKey);
    moduleLoadingSet_.insert(cacheKey); // D19 fix: 与 stack 同步维护 set

    // RA-A fix: RAII 守卫统一管理异常路径下的状态恢复（moduleEnv close、env、exported、loadingStack/Set），
    // 消除原 catch(...) + throw; 的 rethrow。正常路径通过 dismiss 跳过守卫清理。
    // P2-14: 扩展守卫，异常路径清除 moduleCache_/moduleExports_ 中的部分 env，
    // 避免后续 import 命中缓存返回未填充完整的 env
    struct ModuleEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& saved;
        std::unordered_set<std::string>& exported;
        std::unordered_set<std::string> savedExported;
        std::vector<std::string>& loadingStack;
        std::unordered_set<std::string>& loadingSet;
        std::unordered_map<std::string, std::shared_ptr<Environment>>& cache;
        std::unordered_map<std::string, std::unordered_set<std::string>>& exportsCache;
        const std::string& cacheKey;
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
                loadingSet.erase(cacheKey); // D19 fix: 同步移除
                // P2-14: 清除部分 env 和预扫描的 export 名
                cache.erase(cacheKey);
                exportsCache.erase(cacheKey);
            }
        }
    } envGuard{
        *this,        moduleEnv,      savedEnv, exportedNames_, savedExported, moduleLoadingStack_, moduleLoadingSet_,
        moduleCache_, moduleExports_, cacheKey};

    for (auto& stmt : ast->statements) {
        evaluate(stmt.get());
    }
    // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复

    // RA-A fix: 正常路径，dismiss 守卫后手动执行完整清理
    envGuard.dismissed = true;
    moduleLoadingStack_.pop_back();
    moduleLoadingSet_.erase(cacheKey); // D19 fix: 同步移除
    currentEnv_ = savedEnv;

    // P2-14: moduleCache_[cacheKey] 已在创建时入缓存，无需重新赋值
    // 用实际执行的 export 名覆盖预扫描值（处理条件分支 export 等动态场景）
    moduleExports_[cacheKey] = exportedNames_;
    // BUG-REPL-AUDIT-1 fix: 记录模块文件 mtime，下次 import 时检查是否变化
    if (mtimeChecker) {
        moduleMtimes_[cacheKey] = mtimeChecker(loaderPath);
    }
    exportedNames_ = savedExported;

    // 保留模块 AST（确保函数/类定义指针有效）
    replAsts_.push_back(std::move(ast));

    return moduleEnv;
}

void Interpreter::importNamesFromModule(const std::string& cacheKey, ImportStmt& node,
                                        std::shared_ptr<Environment>& moduleEnv) {
    // 导入名称到当前环境（仅导入已 export 的名称）
    // BUG-M4 fix: moduleExports_ 用 cacheKey（与 loadModuleOrGetCached 入键一致）。
    auto& exports = moduleExports_[cacheKey];
    if (!node.namespaceAlias.empty()) {
        // P2-11: import * as ns from "path" — 构造包含所有导出名称的字典对象
        // 使用 Value::DictMap（R97 #2: 含 DictKeyEqual 透明比较器）构造，
        // 再通过 Value(DictMap&&) 构造堆字典值。键为导出名（string）。
        Value::DictMap dict;
        dict.reserve(exports.size());
        for (const auto& name : exports) {
            const Value* valPtr = moduleEnv->get(name);
            if (valPtr) {
                dict.emplace(std::string(name), *valPtr);
            }
        }
        currentEnv_->define(node.namespaceAlias, Value(std::move(dict)));
    } else if (node.importAll) {
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
                runtimeError("模块 " + cacheKey + " 中未导出名称: " + name, node.line, node.column);
            }
            const Value* valPtr = moduleEnv->get(name);
            if (!valPtr) {
                runtimeError("模块 " + cacheKey + " 中未找到导出名称: " + name, node.line, node.column);
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
