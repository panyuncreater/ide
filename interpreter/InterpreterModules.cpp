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

    // F12: 模块加载
    // A6 fix: 加锁拷贝 callback 后解锁检查，避免跨线程数据竞争
    std::function<std::string(const std::string&)> loader;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        loader = moduleLoader_;
    }
    if (!loader) {
        runtimeError("未设置模块加载器，无法执行 import", node.line, node.column);
    }

    // 解析模块路径（相对于当前文件）
    std::string modulePath = node.modulePath;
    // P2-3 fix: 路径规范化 — 统一路径分隔符为 '/'，去除多余的 "./" 前缀
    // 避免相同模块因路径表示不同（如 "foo\bar.mini" vs "foo/bar.mini"）被重复加载
    for (char& c : modulePath) {
        if (c == '\\') c = '/';
    }
    if (modulePath.size() >= 2 && modulePath[0] == '.' && modulePath[1] == '/') {
        modulePath.erase(0, 2);
    }
    // 简单路径解析：如果模块路径是相对路径且当前文件路径非空，拼接目录
    // （完整路径解析由 moduleLoader_ 回调负责）

    // 循环依赖检测（D19 fix: 用 unordered_set 实现 O(1) 查找，替代线性扫描）
    if (moduleLoadingSet_.count(modulePath) > 0) {
        runtimeError("检测到循环依赖: " + modulePath, node.line, node.column);
    }
    // P1-1 fix: 深度导入链递归保护（防止 C++ 栈溢出）
    if (moduleLoadingStack_.size() >= MAX_RECURSION_DEPTH) {
        runtimeError("模块导入深度超过限制 (" + std::to_string(MAX_RECURSION_DEPTH) + ")", node.line, node.column);
    }

    // 检查缓存
    auto cacheIt = moduleCache_.find(modulePath);
    std::shared_ptr<Environment> moduleEnv;
    if (cacheIt != moduleCache_.end()) {
        moduleEnv = cacheIt->second;
    } else {
        // 加载模块源码（A6 fix: 使用已拷贝的 loader，避免跨线程数据竞争）
        std::string source = loader(modulePath);
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
        moduleLoadingSet_.insert(modulePath);  // D19 fix: 与 stack 同步维护 set

        try {
            for (auto& stmt : ast->statements) {
                evaluate(stmt.get());
            }
        } catch (...) {
            currentEnv_ = savedEnv;
            exportedNames_ = savedExported;
            moduleLoadingStack_.pop_back();
            moduleLoadingSet_.erase(modulePath);  // D19 fix: 同步移除
            throw;
        }

        moduleLoadingStack_.pop_back();
        moduleLoadingSet_.erase(modulePath);  // D19 fix: 同步移除
        currentEnv_ = savedEnv;

        // 缓存模块环境和导出名称（必须在恢复 savedExported 之前捕获当前模块的 exports）
        moduleCache_[modulePath] = moduleEnv;
        moduleExports_[modulePath] = exportedNames_;
        exportedNames_ = savedExported;

        // 保留模块 AST（确保函数/类定义指针有效）
        replAsts_.push_back(std::move(ast));
    }

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
                runtimeError("模块 " + modulePath + " 中未导出名称: " + name,
                            node.line, node.column);
            }
            const Value* valPtr = moduleEnv->get(name);
            if (!valPtr) {
                runtimeError("模块 " + modulePath + " 中未找到导出名称: " + name,
                            node.line, node.column);
            }
        }
        // 全部验证通过后，统一导入
        for (const auto& name : node.names) {
            const Value* valPtr = moduleEnv->get(name);
            currentEnv_->define(name, *valPtr);
        }
    }

    lastValue_ = Value::nullValue(); return;
}

void Interpreter::visitExportStmt(ExportStmt& node) {
    checkBreak(&node);
    // F12: export 语句执行内部声明，并记录导出名称
    if (!node.declaration) { lastValue_ = Value::nullValue(); return; }

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
    lastValue_ = Value::nullValue(); return;
}
