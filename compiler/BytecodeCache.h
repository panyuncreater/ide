#pragma once

// ============================================================
// BytecodeCache — P1-7: 字节码磁盘缓存
// ------------------------------------------------------------
// 将 CompileResult 序列化为二进制文件,下次同源码命中时跳过编译。
//
// 缓存 key = FNV-1a(source) + compilerMode。
//   - source 变化 → hash 变化 → 自动失效
//   - compiler 模式切换(direct/IR)→ compilerMode 不同 → 自动隔离
//
// 二进制格式:
//   [Header 32B]
//     magic[4]            = "MLBC" (StackVM) 或 "MLRC" (RegisterVM, L11)
//     format_version(u16) = 1
//     compiler_mode(u8)   0=直接 codegen, 1=IR 路径
//     reserved(u8)        = 0
//     source_hash(u64)    FNV-1a 64bit of source
//     source_mtime(i64)   源码 mtime(毫秒,0=不校验)
//     payload_checksum(u32) FNV-1a 32bit of payload
//     payload_size(u32)
//   [Payload]
//     CompileResult 或 RegisterCompileResult 序列化数据(见 .cpp 序列化函数)
//
// 安全保证:
//   - magic/version/compiler_mode/hash/checksum 五重校验,任一不匹配返回 nullopt
//   - L13 扩展：Value 支持标量(int/float/bool/null/string) + 容器堆类型
//     (array/dict/tuple/enum_variant)，递归序列化元素 + 环检测
//   - 不可序列化类型(closure/instance/channel/mutex/rwlock/thread/coroutine)
//     标记为 kNonSerializableTag 使反序列化失败,安全降级到正常编译
//   - 所有越界/截断/类型错误均返回 nullopt,不抛异常
//
// 已知限制:
//   - REPL 路径不走 Compiler,无法受益
//   - 注：当前编译器 addConstant 调用点仅放入标量，L13 的容器堆类型序列化为
//     防御性支持（应对未来 IR 常量折叠扩展可能产生 array/dict 常量）
//
// L12 预编译模块源码失效校验（2026-07-24）:
//   - storeToFile 接受可选 sourcePath,写入源码 mtime + content hash 到头部
//   - tryLoadFromFile 接受可选 sourcePath,加载时校验源码 mtime+hash,
//     不匹配返回 nullopt（源码已修改,.minic 失效,回退源码编译）
//   - 向后兼容:旧 .minic 头部 source_mtime=0 自动跳过校验
//
// L13 容器堆类型常量序列化（2026-07-24）:
//   - writeValue/readValue 扩展支持 Array/Dict/Tuple/EnumVariant
//   - BoxedInt（超 int48 范围）自动通过 isInt() 路径处理
//   - 环检测：thread_local visited 集合记录已访问堆对象指针
//   - isSerializable 改为递归检查（含环检测）
//
// L11 RegisterVM 预编译模块支持（2026-07-24）:
//   - 新增 storeRegisterToFile / tryLoadRegisterFromFile 文件级 API
//   - 使用独立 magic "MLRC"（MiniLang Register Cache）与 StackVM "MLBC" 隔离
//   - 头部布局相同（32B），仅 magic 不同，复用 L12 mtime+hash 校验
//   - RegBytecodeChunk 序列化字段：比 BytecodeChunk 多 registerCount 字段，
//     localRegNames 对应 localSlotNames
//   - RegisterCompileResult 序列化字段：含 moduleExports（L11 新增）
//
// L17 PipelineRunner 内存级 RegisterVM 缓存（2026-07-24）:
//   - 新增 tryLoadRegister(CacheKey) / storeRegister(CacheKey, RegisterCompileResult) 内存级 API
//   - 与 tryLoad/store 对称：基于源码 hash 的缓存查询，使用 `.mrc` 扩展名
//   - 与 L11 文件级 API 区别：L11 是 .minic 路径-based（模块预编译），
//     L17 是 source hash-based（PipelineRunner 主程序编译缓存）
//   - PipelineRunner.runCompiler 检测 useRegisterVM_，RegisterVM 路径走 register 缓存
// ============================================================

#include "compiler/Bytecode.h"
#include "compiler/RegisterBytecode.h" // L11: RegisterCompileResult 序列化
#include <cstdint>
#include <optional>
#include <string>

class BytecodeCache {
public:
    /// 缓存查询 key
    struct CacheKey {
        std::string source;       ///< 源码内容
        int64_t mtime = 0;        ///< 源码文件 mtime(毫秒);0 表示不校验 mtime
        uint8_t compilerMode = 0; ///< 0=直接编译, 1=IR 路径
        /// AUDIT-R3 P2-4 fix: 优化开关位图（bit0=irOptimize, bit1=irSSAOptimize）。
        /// 原缓存键不含优化配置，切换优化开关后旧缓存仍命中，产出与当前
        /// 配置不一致的字节码。写入头部 reserved 字节，加载时比对不符即失效。
        uint8_t optFlags = 0;
    };

    BytecodeCache();
    ~BytecodeCache();

    /// 尝试从缓存加载 CompileResult。
    /// @return 命中且五重校验通过返回反序列化结果;否则返回 nullopt
    std::optional<CompileResult> tryLoad(const CacheKey& key);

    /// 写入缓存。失败时静默(缓存非关键路径,失败不影响正确性)。
    void store(const CacheKey& key, const CompileResult& result);

    /// L17 PipelineRunner 内存级 RegisterVM 缓存：基于源码 hash 的缓存查询。
    /// 与 tryLoad 不同：使用 kRegisterMagic "MLRC" + `.mrc` 扩展名，
    /// 反序列化为 RegisterCompileResult（含 registerCount / localRegNames /
    /// moduleExports 等 RegisterVM 专属字段）。
    /// @param key 缓存键（source + compilerMode + mtime）
    /// @return 命中且五重校验通过返回反序列化结果;否则返回 nullopt
    std::optional<RegisterCompileResult> tryLoadRegister(const CacheKey& key);

    /// L17 PipelineRunner 内存级 RegisterVM 缓存：写入 RegisterCompileResult。
    /// 使用 kRegisterMagic "MLRC" + `.mrc` 扩展名，与 StackVM `.mbc` 隔离。
    /// 失败时静默(缓存非关键路径,失败不影响正确性)。
    void storeRegister(const CacheKey& key, const RegisterCompileResult& result);

    /// P2-11 预编译模块：从指定文件路径加载 CompileResult（.minic 文件）。
    /// 与 tryLoad 不同，不基于源码内容计算缓存 key（文件路径即 key）。
    /// 校验 magic/version/checksum 三重；当 sourcePath 非空且 .minic 头部
    /// 嵌入了 source mtime/hash（L12）时，额外校验源码 mtime+content hash，
    /// 任一不匹配返回 nullopt（源码已修改，.minic 失效，调用方回退到源码编译）。
    /// @param filePath .minic 文件路径
    /// @param sourcePath 对应源码路径（可选；空则跳过 mtime/hash 校验）
    /// @return 加载成功返回反序列化结果；否则返回 nullopt
    std::optional<CompileResult> tryLoadFromFile(const std::string& filePath, const std::string& sourcePath = "");

    /// P2-11 预编译模块：将 CompileResult 序列化到指定文件路径（.minic 文件）。
    /// @param filePath 目标 .minic 文件路径
    /// @param result 要序列化的 CompileResult
    /// @param modulePath 模块路径（用于调试识别；sourcePath 为空时写入 source_hash 字段）
    /// @param sourcePath 源码文件路径（可选；非空且存在时写入 source mtime + content hash，
    ///                   供 tryLoadFromFile 校验源码是否被修改）
    /// @return 成功返回 true；常量池含非标量或 I/O 失败返回 false
    bool storeToFile(const std::string& filePath, const CompileResult& result, const std::string& modulePath = "",
                     const std::string& sourcePath = "");

    /// L11 RegisterVM 预编译模块：从 .minic 文件加载 RegisterCompileResult。
    /// 使用独立 magic "MLRC" 与 StackVM "MLBC" 隔离。
    /// 校验逻辑与 tryLoadFromFile 相同（magic/version/checksum + 可选 mtime+hash）。
    /// @param filePath .minic 文件路径
    /// @param sourcePath 对应源码路径（可选；空则跳过 mtime/hash 校验）
    /// @return 加载成功返回反序列化结果；否则返回 nullopt
    std::optional<RegisterCompileResult> tryLoadRegisterFromFile(const std::string& filePath,
                                                                 const std::string& sourcePath = "");

    /// L11 RegisterVM 预编译模块：将 RegisterCompileResult 序列化到 .minic 文件。
    /// 使用独立 magic "MLRC" 与 StackVM "MLBC" 隔离。
    /// @param filePath 目标 .minic 文件路径
    /// @param result 要序列化的 RegisterCompileResult
    /// @param modulePath 模块路径（用于调试识别；sourcePath 为空时写入 source_hash 字段）
    /// @param sourcePath 源码文件路径（可选；非空且存在时写入 source mtime + content hash）
    /// @return 成功返回 true；常量池含非标量或 I/O 失败返回 false
    bool storeRegisterToFile(const std::string& filePath, const RegisterCompileResult& result,
                             const std::string& modulePath = "", const std::string& sourcePath = "");

    /// 清空缓存目录下所有 .mbc 文件
    void clear();

    /// 设置缓存目录(默认 <temp>/minilang_bytecache)
    void setCacheDir(const std::string& dir) { cacheDir_ = dir; }
    const std::string& cacheDir() const { return cacheDir_; }

    /// 获取当前缓存条目数(主要用于测试)
    size_t entryCount() const;

private:
    std::string cacheDir_;
};
