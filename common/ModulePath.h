/**
 * @file common/ModulePath.h
 * @brief 模块路径缓存键规范化——单一事实源（AUDIT-R5 R6 fix）。
 *
 * 历史上路径规范化在四处重复实现（Interpreter::resolveModulePath /
 * Interpreter::clearModuleCache / Compiler::normalizeModulePath /
 * AstIRBuilder::resolveImportPath），且与 ModuleIsolation::pathHash 的
 * 折叠规则不一致：
 *   - 连续斜杠折叠仅 pathHash 一处实现（AUDIT-P2-ROUND53），其余三处缺失，
 *     "a//b" 与 "a/b" 产生两个缓存键，同一模块被双重加载（副作用翻倍、
 *     两份独立模块状态）。本函数将四处统一到单一事实源。
 *
 * 本函数统一执行：'\' → '/'、折叠连续 '/'、循环剥前缀 "./"、折叠中间 "/./"。
 * 安全校验（绝对路径/".." 段拒绝）仍由各调用方自行执行（错误报告方式不同）。
 *
 * BUG-M4 fix（AUDIT-R5 续）：大小写折叠不在 normalizeModulePathKey 内进行（它返回
 * loader 请求路径，必须保留大小写，否则破坏大小写敏感的 loader）。缓存去重键
 * 改用独立的 moduleCacheKey（仅在 Windows 上 ASCII 小写折叠）：各调用方用
 * normalizeModulePathKey 的返回值调 loader，用 moduleCacheKey 作所有缓存/去重 map 键
 * 与 ModuleTopLevelRenamer::pathHash 输入，使 Windows 上同一文件的不同大小写拼写
 * 去重为同一模块（避免双重加载），而 loader 仍收到原大小写路径。
 */
#pragma once

#include <string>

/// 规范化模块路径为 loader 请求路径（保留大小写）。见文件头注释。
inline std::string normalizeModulePathKey(const std::string& raw) {
    std::string p;
    p.reserve(raw.size());
    // '\' → '/'，同时折叠连续 '/'（与 ModuleIsolation::pathHash 的 R53 折叠对齐）
    bool prevSlash = false;
    for (char c : raw) {
        if (c == '\\')
            c = '/';
        if (c == '/') {
            if (!prevSlash)
                p.push_back('/');
            prevSlash = true;
        } else {
            p.push_back(c);
            prevSlash = false;
        }
    }
    // 循环剥前缀 "./"（AUDIT-R3 P2-10：只剥一次会漏 "././m.mini"）
    while (p.size() >= 2 && p[0] == '.' && p[1] == '/') {
        p.erase(0, 2);
    }
    // 折叠中间 "/./" 段
    for (size_t dotPos = p.find("/./"); dotPos != std::string::npos; dotPos = p.find("/./")) {
        p.erase(dotPos, 2);
    }
    return p;
}

/// BUG-M4 fix: 从已规范化的 loader 路径派生缓存去重键。
/// Windows 文件系统大小写不敏感：缓存键统一 ASCII 小写，使同一文件的不同大小写
/// 拼写去重为同一模块。仅折叠 ASCII A-Z，不触碰 UTF-8 多字节序列。
/// Linux/macOS（大小写敏感文件系统）不折叠，与平台语义一致。
/// 调用方：用 normalizeModulePathKey 的返回值调 loader/mtimeChecker（保留大小写），
/// 用本函数的返回值作所有缓存/去重 map 键与 pathHash 输入。
inline std::string moduleCacheKey(const std::string& normalizedLoaderPath) {
#ifdef _WIN32
    std::string key = normalizedLoaderPath;
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
#else
    return normalizedLoaderPath;
#endif
}
