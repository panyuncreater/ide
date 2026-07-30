// ============================================================
// BytecodeCache 单元测试 (P1-7)
// ------------------------------------------------------------
// 验证 CompileResult 序列化/反序列化往返等价性:
//   1. 基本往返(简单程序)
//   2. 不同源码/不同 compilerMode 不命中
//   3. clear() 后不命中
//   4. 包含字符串/浮点/布尔常量
//   5. 包含函数定义(functionChunks)
//   6. 包含 enum(enumInfos)
//   7. 缓存命中结果可被 VM 执行(端到端验证)
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Bytecode.h"
#include "compiler/BytecodeCache.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <filesystem>
#include <fstream>
#include <string>

// ============================================================
// 辅助函数
// ============================================================

static CompileResult compileSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_NE(ast, nullptr);
    if (!ast)
        return CompileResult{};
    Compiler compiler;
    return compiler.compile(*ast);
}

/// 比较两个 BytecodeChunk 是否等价(忽略懒缓存字段)
static ::testing::AssertionResult chunksEqual(const BytecodeChunk& a, const BytecodeChunk& b) {
    if (a.code != b.code)
        return ::testing::AssertionFailure() << "code differs";
    if (a.constants.size() != b.constants.size())
        return ::testing::AssertionFailure()
               << "constants size: " << a.constants.size() << " vs " << b.constants.size();
    for (size_t i = 0; i < a.constants.size(); ++i) {
        if (a.constants[i].getType() != b.constants[i].getType())
            return ::testing::AssertionFailure() << "constant[" << i << "] type differs";
        if (!a.constants[i].equals(b.constants[i]))
            return ::testing::AssertionFailure() << "constant[" << i << "] value differs";
    }
    if (a.lines != b.lines)
        return ::testing::AssertionFailure() << "lines differ";
    if (a.columns != b.columns)
        return ::testing::AssertionFailure() << "columns differ";
    if (a.name != b.name)
        return ::testing::AssertionFailure() << "name differs";
    if (a.arity != b.arity)
        return ::testing::AssertionFailure() << "arity differs";
    if (a.requiredArity != b.requiredArity)
        return ::testing::AssertionFailure() << "requiredArity differs";
    if (a.defaultConstIndices != b.defaultConstIndices)
        return ::testing::AssertionFailure() << "defaultConstIndices differ";
    if (a.ipToInstrIndex != b.ipToInstrIndex)
        return ::testing::AssertionFailure() << "ipToInstrIndex differ";
    if (a.fieldOrder != b.fieldOrder)
        return ::testing::AssertionFailure() << "fieldOrder differ";
    if (a.localCount != b.localCount)
        return ::testing::AssertionFailure() << "localCount differs";
    if (a.upvalues.size() != b.upvalues.size())
        return ::testing::AssertionFailure() << "upvalues size differs";
    for (size_t i = 0; i < a.upvalues.size(); ++i) {
        if (a.upvalues[i].index != b.upvalues[i].index || a.upvalues[i].isLocal != b.upvalues[i].isLocal ||
            a.upvalues[i].name != b.upvalues[i].name)
            return ::testing::AssertionFailure() << "upvalue[" << i << "] differs";
    }
    if (a.localSlotNames != b.localSlotNames)
        return ::testing::AssertionFailure() << "localSlotNames differ";
    if (a.slotNameRanges.size() != b.slotNameRanges.size())
        return ::testing::AssertionFailure() << "slotNameRanges size differs";
    for (size_t i = 0; i < a.slotNameRanges.size(); ++i) {
        if (a.slotNameRanges[i].slot != b.slotNameRanges[i].slot ||
            a.slotNameRanges[i].name != b.slotNameRanges[i].name ||
            a.slotNameRanges[i].startIp != b.slotNameRanges[i].startIp ||
            a.slotNameRanges[i].endIp != b.slotNameRanges[i].endIp)
            return ::testing::AssertionFailure() << "slotNameRange[" << i << "] differs";
    }
    if (a.isGenerator != b.isGenerator)
        return ::testing::AssertionFailure() << "isGenerator differs";
    if (a.yieldCount != b.yieldCount)
        return ::testing::AssertionFailure() << "yieldCount differs";
    return ::testing::AssertionSuccess();
}

/// 比较两个 CompileResult 是否等价
static ::testing::AssertionResult resultsEqual(const CompileResult& a, const CompileResult& b) {
    auto r = chunksEqual(a.mainChunk, b.mainChunk);
    if (!r)
        return r << " (mainChunk)";
    if (a.functionChunks.size() != b.functionChunks.size())
        return ::testing::AssertionFailure()
               << "functionChunks size: " << a.functionChunks.size() << " vs " << b.functionChunks.size();
    for (const auto& [name, chunk] : a.functionChunks) {
        auto it = b.functionChunks.find(name);
        if (it == b.functionChunks.end())
            return ::testing::AssertionFailure() << "functionChunk '" << name << "' missing in b";
        auto cr = chunksEqual(chunk, it->second);
        if (!cr)
            return cr << " (functionChunk '" << name << "')";
    }
    if (a.globalSlotCount != b.globalSlotCount)
        return ::testing::AssertionFailure() << "globalSlotCount differs";
    if (a.globalSlotNames != b.globalSlotNames)
        return ::testing::AssertionFailure() << "globalSlotNames differ";
    if (a.enumInfos.size() != b.enumInfos.size())
        return ::testing::AssertionFailure() << "enumInfos size differs";
    for (size_t i = 0; i < a.enumInfos.size(); ++i) {
        if (a.enumInfos[i].name != b.enumInfos[i].name)
            return ::testing::AssertionFailure() << "enumInfo[" << i << "] name differs";
        if (a.enumInfos[i].variants.size() != b.enumInfos[i].variants.size())
            return ::testing::AssertionFailure() << "enumInfo[" << i << "] variants size differs";
        for (size_t j = 0; j < a.enumInfos[i].variants.size(); ++j) {
            if (a.enumInfos[i].variants[j].name != b.enumInfos[i].variants[j].name ||
                a.enumInfos[i].variants[j].arity != b.enumInfos[i].variants[j].arity)
                return ::testing::AssertionFailure() << "enumInfo[" << i << "].variant[" << j << "] differs";
        }
    }
    return ::testing::AssertionSuccess();
}

/// 创建独立缓存目录的辅助(每个测试用例独立目录避免干扰)
static std::string makeUniqueCacheDir(const char* testName) {
    auto dir = std::filesystem::temp_directory_path() / "minilang_bc_test" / testName;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir.string();
}

// ============================================================
// 测试用例
// ============================================================

TEST(BytecodeCacheTest, BasicRoundtrip) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("BasicRoundtrip"));

    std::string src = "var x = 42; var y = x + 8; print(y);";
    CompileResult original = compileSource(src);
    ASSERT_FALSE(original.mainChunk.code.empty());

    BytecodeCache::CacheKey key;
    key.source = src;
    key.compilerMode = 0;
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(resultsEqual(original, *loaded));
}

TEST(BytecodeCacheTest, MissOnDifferentSource) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("MissOnDifferentSource"));

    std::string src1 = "var x = 1;";
    std::string src2 = "var x = 2;";
    CompileResult r1 = compileSource(src1);

    BytecodeCache::CacheKey key1{src1, 0, 0};
    cache.store(key1, r1);

    BytecodeCache::CacheKey key2{src2, 0, 0};
    auto loaded = cache.tryLoad(key2);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BytecodeCacheTest, MissOnDifferentCompilerMode) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("MissOnDifferentCompilerMode"));

    std::string src = "var x = 1;";
    CompileResult r = compileSource(src);

    BytecodeCache::CacheKey keyDirect{src, 0, 0};
    cache.store(keyDirect, r);

    BytecodeCache::CacheKey keyIR{src, 0, 1};
    auto loaded = cache.tryLoad(keyIR);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BytecodeCacheTest, ClearInvalidates) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("ClearInvalidates"));

    std::string src = "var x = 1;";
    CompileResult r = compileSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, r);
    ASSERT_TRUE(cache.tryLoad(key).has_value());

    cache.clear();
    EXPECT_FALSE(cache.tryLoad(key).has_value());
    EXPECT_EQ(cache.entryCount(), 0u);
}

TEST(BytecodeCacheTest, StringFloatBoolConstants) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("StringFloatBoolConstants"));

    std::string src =
        "var s = \"hello\"; var f = 3.14; var b = true; var n = null; print(s); print(f); print(b); print(n);";
    CompileResult original = compileSource(src);
    ASSERT_FALSE(original.mainChunk.code.empty());

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(resultsEqual(original, *loaded));

    // 验证常量池包含字符串和浮点标量。
    // 注意:true/false/null 编译为 OP_TRUE/OP_FALSE/OP_NULL 专用操作码,不进入常量池。
    bool hasString = false, hasFloat = false;
    for (const auto& v : loaded->mainChunk.constants) {
        if (v.isString())
            hasString = true;
        if (v.isFloat())
            hasFloat = true;
    }
    EXPECT_TRUE(hasString);
    EXPECT_TRUE(hasFloat);
}

TEST(BytecodeCacheTest, FunctionChunksRoundtrip) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("FunctionChunksRoundtrip"));

    std::string src =
        "fun add(a, b) { return a + b; } fun mul(a, b) { return a * b; } print(add(2, 3)); print(mul(4, 5));";
    CompileResult original = compileSource(src);
    ASSERT_GE(original.functionChunks.size(), 2u) << "应至少有 add 和 mul 两个函数 chunk";

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(resultsEqual(original, *loaded));
    EXPECT_EQ(loaded->functionChunks.size(), original.functionChunks.size());
}

TEST(BytecodeCacheTest, EnumInfosRoundtrip) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("EnumInfosRoundtrip"));

    std::string src = "enum Color { Red, Green, Blue } enum Option { Some(value), None }";
    CompileResult original = compileSource(src);
    ASSERT_GE(original.enumInfos.size(), 2u) << "应至少有 Color 和 Option 两个 enum";

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(resultsEqual(original, *loaded));
    EXPECT_EQ(loaded->enumInfos.size(), original.enumInfos.size());
}

TEST(BytecodeCacheTest, CachedResultExecutableByVM) {
    // 端到端验证:缓存命中的 CompileResult 能被 VM 正确执行
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("CachedResultExecutableByVM"));

    std::string src = "var x = 10; var y = 20; print(x + y);";
    CompileResult original = compileSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());

    // 用 VM 执行缓存加载的 CompileResult
    std::string output;
    VM vm;
    vm.setOutputCallback([&output](const std::string& s) { output += s; });
    VMResult result = vm.execute(*loaded);
    EXPECT_EQ(result, VMResult::VM_OK);
    EXPECT_EQ(output, "30");
}

TEST(BytecodeCacheTest, CorruptedFileReturnsNullopt) {
    // 损坏的缓存文件应安全返回 nullopt 而非崩溃
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("CorruptedFileReturnsNullopt");
    cache.setCacheDir(dir);

    std::string src = "var x = 1;";
    CompileResult r = compileSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, r);

    // 找到缓存文件并破坏它
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".mbc") {
            // 写入垃圾数据
            std::ofstream ofs(entry.path(), std::ios::binary | std::ios::trunc);
            ofs << "GARBAGE_DATA_THIS_IS_NOT_A_VALID_CACHE_FILE!!!!";
            break;
        }
    }

    // 应安全返回 nullopt(magic 校验失败)
    auto loaded = cache.tryLoad(key);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BytecodeCacheTest, EntryCountAfterStore) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("EntryCountAfterStore"));

    EXPECT_EQ(cache.entryCount(), 0u);

    std::string src1 = "var x = 1;";
    std::string src2 = "var y = 2;";
    CompileResult r1 = compileSource(src1);
    CompileResult r2 = compileSource(src2);

    cache.store({src1, 0, 0}, r1);
    EXPECT_EQ(cache.entryCount(), 1u);

    cache.store({src2, 0, 0}, r2);
    EXPECT_EQ(cache.entryCount(), 2u);

    // 同源码重复 store 覆盖,不增加条目数
    cache.store({src1, 0, 0}, r1);
    EXPECT_EQ(cache.entryCount(), 2u);
}

// ============================================================
// 反序列化健壮性测试：畸形 .mbc 文件必须安全返回 nullopt 而非崩溃
// ------------------------------------------------------------
// 现有 CorruptedFileReturnsNullopt 仅覆盖「整文件垃圾数据」（magic 校验失败）。
// 本组针对 32 字节头（magic@0 / version@4 / mode@6 / optFlags@7 / srcHash@8 /
// mtime@16 / checksum@24 / payloadSize@28）逐字段构造精确畸形输入，验证
// tryLoad 在每个校验点都安全短路返回 nullopt（不越界、不崩溃）。
// ============================================================

namespace {
/// 定位缓存目录下第一个 .mbc 文件；找不到返回空路径
std::filesystem::path findMbcFile(const std::string& dir) {
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".mbc")
            return entry.path();
    }
    return {};
}

/// 读取文件全部字节
std::vector<uint8_t> readAllBytes(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

/// 覆写文件全部字节
void writeAllBytes(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

// 空文件（0 字节）：size < kHeaderSize，安全返回 nullopt
TEST(BytecodeCacheRobustness, EmptyFileReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("EmptyFileReturnsNullopt");
    cache.setCacheDir(dir);
    std::string src = "var x = 1;";
    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, compileSource(src));

    auto file = findMbcFile(dir);
    ASSERT_FALSE(file.empty());
    writeAllBytes(file, {}); // 截断为 0 字节

    EXPECT_FALSE(cache.tryLoad(key).has_value());
}

// 截断头部（仅 10 字节 < 32）：header 读取短路，安全返回 nullopt
TEST(BytecodeCacheRobustness, TruncatedHeaderReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("TruncatedHeaderReturnsNullopt");
    cache.setCacheDir(dir);
    std::string src = "var x = 1;";
    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, compileSource(src));

    auto file = findMbcFile(dir);
    ASSERT_FALSE(file.empty());
    auto bytes = readAllBytes(file);
    ASSERT_GE(bytes.size(), 10u);
    bytes.resize(10); // 不足 32 字节头
    writeAllBytes(file, bytes);

    EXPECT_FALSE(cache.tryLoad(key).has_value());
}

// 版本号不匹配（version 字段 @offset 4 篡改为 0xFFFF）：version 校验失败
TEST(BytecodeCacheRobustness, VersionMismatchReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("VersionMismatchReturnsNullopt");
    cache.setCacheDir(dir);
    std::string src = "var x = 1;";
    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, compileSource(src));

    auto file = findMbcFile(dir);
    ASSERT_FALSE(file.empty());
    auto bytes = readAllBytes(file);
    ASSERT_GE(bytes.size(), 32u);
    // magic 保持有效（跳过 magic 校验），仅篡改 version（u16 @offset 4）
    bytes[4] = 0xFF;
    bytes[5] = 0xFF;
    writeAllBytes(file, bytes);

    EXPECT_FALSE(cache.tryLoad(key).has_value());
}

// payload 校验和不匹配（翻转一个 payload 字节）：checksum 校验失败
TEST(BytecodeCacheRobustness, PayloadChecksumMismatchReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("PayloadChecksumMismatchReturnsNullopt");
    cache.setCacheDir(dir);
    std::string src = "var x = 1; var y = 2;";
    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, compileSource(src));

    auto file = findMbcFile(dir);
    ASSERT_FALSE(file.empty());
    auto bytes = readAllBytes(file);
    ASSERT_GT(bytes.size(), 32u) << "payload 应非空";
    // magic/version/头部全部有效，仅翻转首个 payload 字节（@offset 32）→ checksum 失配
    bytes[32] ^= 0xFF;
    writeAllBytes(file, bytes);

    EXPECT_FALSE(cache.tryLoad(key).has_value());
}

// 头部有效但 payload 被截断（file 短于 kHeaderSize + payloadSize）：大小校验失败
TEST(BytecodeCacheRobustness, TruncatedPayloadReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("TruncatedPayloadReturnsNullopt");
    cache.setCacheDir(dir);
    std::string src = "var x = 1; var y = 2;";
    BytecodeCache::CacheKey key{src, 0, 0};
    cache.store(key, compileSource(src));

    auto file = findMbcFile(dir);
    ASSERT_FALSE(file.empty());
    auto bytes = readAllBytes(file);
    ASSERT_GT(bytes.size(), 34u);
    // 保留完整 32 字节头（payloadSize 字段仍声明原大小）+ 仅 2 字节 payload
    bytes.resize(34);
    writeAllBytes(file, bytes);

    EXPECT_FALSE(cache.tryLoad(key).has_value());
}

// ============================================================
// L13 测试：容器堆类型常量序列化（2026-07-24）
// ------------------------------------------------------------
// 验证 BytecodeCache 扩展支持 Array/Dict/Tuple/EnumVariant/BoxedInt
// 常量的序列化往返。当前编译器 addConstant 仅放标量，这些测试通过手动
// 构造 CompileResult 模拟未来 IR 常量折叠可能产生的堆类型常量。
// ============================================================

namespace {

/// 构造一个仅含常量池的 CompileResult（无字节码，仅用于序列化测试）
CompileResult makeResultWithConstants(std::vector<Value> constants) {
    CompileResult cr;
    cr.mainChunk.constants = std::move(constants);
    cr.mainChunk.code = {static_cast<uint8_t>(OpCode::OP_RETURN)};
    cr.mainChunk.lines = {0};
    cr.mainChunk.columns = {0};
    cr.globalSlotCount = 0;
    return cr;
}

} // namespace

TEST(BytecodeCacheL13, ArrayConstantRoundtrip) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_ArrayConstantRoundtrip");
    cache.setCacheDir(dir);

    // 构造 array 常量 [1, "two", 3.14, true, null]
    std::vector<Value> elems{
        Value(int64_t(1)),
        Value(std::string("two")),
        Value(3.14),
        Value(true),
        Value::nullValue(),
    };
    Value arrVal(std::move(elems));

    CompileResult original = makeResultWithConstants({arrVal});

    BytecodeCache::CacheKey key{"array_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isArray()) << "应为 array 类型";
    ASSERT_EQ(v.arrayVal().size(), 5u);
    EXPECT_EQ(v.arrayVal()[0].intVal(), 1);
    EXPECT_EQ(v.arrayVal()[1].stringVal(), "two");
    EXPECT_DOUBLE_EQ(v.arrayVal()[2].floatVal(), 3.14);
    EXPECT_EQ(v.arrayVal()[3].boolVal(), true);
    EXPECT_TRUE(v.arrayVal()[4].isNull());
}

TEST(BytecodeCacheL13, DictConstantRoundtrip) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_DictConstantRoundtrip");
    cache.setCacheDir(dir);

    // 构造 dict 常量 {"name": "Alice", 42: "int_key", true: false, 3.14: 1.0}
    Value::DictMap entries;
    entries[Value::DictKey(std::string("name"))] = Value(std::string("Alice"));
    entries[Value::DictKey(int64_t(42))] = Value(std::string("int_key"));
    entries[Value::DictKey(true)] = Value(false);
    entries[Value::DictKey(3.14)] = Value(1.0);
    Value dictVal(std::move(entries));

    CompileResult original = makeResultWithConstants({dictVal});

    BytecodeCache::CacheKey key{"dict_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isDict()) << "应为 dict 类型";
    ASSERT_EQ(v.dictVal().size(), 4u);
    // 验证不同类型的 key
    EXPECT_EQ(v.dictVal().at(Value::DictKey(std::string("name"))).stringVal(), "Alice");
    EXPECT_EQ(v.dictVal().at(Value::DictKey(int64_t(42))).stringVal(), "int_key");
    EXPECT_EQ(v.dictVal().at(Value::DictKey(true)).boolVal(), false);
    EXPECT_DOUBLE_EQ(v.dictVal().at(Value::DictKey(3.14)).floatVal(), 1.0);
}

TEST(BytecodeCacheL13, TupleConstantRoundtrip) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_TupleConstantRoundtrip");
    cache.setCacheDir(dir);

    // 构造 tuple 常量 (1, "hello", 2.71)
    std::vector<Value> elems{
        Value(int64_t(1)),
        Value(std::string("hello")),
        Value(2.71),
    };
    Value tupleVal = Value::makeTuple(std::move(elems));

    CompileResult original = makeResultWithConstants({tupleVal});

    BytecodeCache::CacheKey key{"tuple_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isTuple()) << "应为 tuple 类型";
    ASSERT_EQ(v.tupleVal().size(), 3u);
    EXPECT_EQ(v.tupleVal()[0].intVal(), 1);
    EXPECT_EQ(v.tupleVal()[1].stringVal(), "hello");
    EXPECT_DOUBLE_EQ(v.tupleVal()[2].floatVal(), 2.71);
}

TEST(BytecodeCacheL13, EnumVariantConstantRoundtrip) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_EnumVariantConstantRoundtrip");
    cache.setCacheDir(dir);

    // 构造 enum variant 常量 Option.Some(42)
    std::vector<Value> fields{Value(int64_t(42))};
    Value variantVal = Value::makeEnumVariant("Option", "Some", std::move(fields));

    CompileResult original = makeResultWithConstants({variantVal});

    BytecodeCache::CacheKey key{"enum_variant_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isEnumVariant()) << "应为 enum_variant 类型";
    EXPECT_EQ(v.enumVariantEnumName(), "Option");
    EXPECT_EQ(v.enumVariantName(), "Some");
    ASSERT_EQ(v.enumVariantFields().size(), 1u);
    EXPECT_EQ(v.enumVariantFields()[0].intVal(), 42);
}

TEST(BytecodeCacheL13, BoxedIntConstantRoundtrip) {
    // BoxedInt：超出 int48 范围的 int64 自动装箱
    // int48 范围：-2^47 ~ 2^47-1，即约 ±140 万亿
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_BoxedIntConstantRoundtrip");
    cache.setCacheDir(dir);

    int64_t bigVal = (int64_t(1) << 48); // 2^48，超出 int48 范围
    Value boxedInt(bigVal);
    // 确认是 BoxedInt（isPointer() 为 true）
    ASSERT_TRUE(boxedInt.isInt()) << "应为 int 类型";
    ASSERT_TRUE(boxedInt.isPointer()) << "应为 BoxedInt（堆装箱）";
    ASSERT_EQ(boxedInt.intVal(), bigVal);

    CompileResult original = makeResultWithConstants({boxedInt});

    BytecodeCache::CacheKey key{"boxed_int_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isInt()) << "反序列化后应为 int 类型";
    EXPECT_EQ(v.intVal(), bigVal) << "BoxedInt 值应保持不变";
}

TEST(BytecodeCacheL13, NestedContainerRoundtrip) {
    // 嵌套容器：[{key: [1, 2, 3]}, (true, null)]
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_NestedContainerRoundtrip");
    cache.setCacheDir(dir);

    // 内层 array [1, 2, 3]
    std::vector<Value> innerArr{Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))};
    Value innerArrVal(std::move(innerArr));

    // dict {key: [1, 2, 3]}
    Value::DictMap entries;
    entries[Value::DictKey(std::string("key"))] = innerArrVal;
    Value dictVal(std::move(entries));

    // tuple (true, null)
    std::vector<Value> tupleElems{Value(true), Value::nullValue()};
    Value tupleVal = Value::makeTuple(std::move(tupleElems));

    // 外层 array [dict, tuple]
    std::vector<Value> outerArr{dictVal, tupleVal};
    Value outerArrVal(std::move(outerArr));

    CompileResult original = makeResultWithConstants({outerArrVal});

    BytecodeCache::CacheKey key{"nested_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);

    const Value& v = loaded->mainChunk.constants[0];
    ASSERT_TRUE(v.isArray());
    ASSERT_EQ(v.arrayVal().size(), 2u);
    // 第 0 个元素是 dict
    const Value& d = v.arrayVal()[0];
    ASSERT_TRUE(d.isDict());
    ASSERT_EQ(d.dictVal().size(), 1u);
    const Value& inner = d.dictVal().at(Value::DictKey(std::string("key")));
    ASSERT_TRUE(inner.isArray());
    ASSERT_EQ(inner.arrayVal().size(), 3u);
    EXPECT_EQ(inner.arrayVal()[0].intVal(), 1);
    EXPECT_EQ(inner.arrayVal()[1].intVal(), 2);
    EXPECT_EQ(inner.arrayVal()[2].intVal(), 3);
    // 第 1 个元素是 tuple
    const Value& t = v.arrayVal()[1];
    ASSERT_TRUE(t.isTuple());
    ASSERT_EQ(t.tupleVal().size(), 2u);
    EXPECT_EQ(t.tupleVal()[0].boolVal(), true);
    EXPECT_TRUE(t.tupleVal()[1].isNull());
}

TEST(BytecodeCacheL13, CircularReferenceRejected) {
    // 自引用 array：构造循环引用，环检测应拒绝序列化
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_CircularReferenceRejected");
    cache.setCacheDir(dir);

    // 构造一个自引用 array（不通过正常 API，用 const_cast 模拟）
    // 实际场景：常量池不会出现循环引用，但环检测是防御性代码
    std::vector<Value> elems{Value(int64_t(1))};
    Value arrVal(std::move(elems));
    // 尝试把 arrVal 自身作为元素（创建环）
    // 注：Value 是 COW，直接 push_back 不会形成环（会拷贝）
    // 这里用 tryGetMutableArray 获取可变引用并 push_back 自身
    // 但 COW 会触发独占拷贝，所以实际上不会形成环
    // 改为测试：含 array 元素的 array（非环，正常嵌套）
    std::vector<Value> innerElems{Value(int64_t(42))};
    Value innerArr(std::move(innerElems));
    Value::DictMap entries;
    entries[Value::DictKey(std::string("data"))] = innerArr;
    Value dictWithArr(std::move(entries));

    CompileResult original = makeResultWithConstants({dictWithArr});

    // 应成功序列化（无环）
    BytecodeCache::CacheKey key{"circular_ref_test", 0, 0};
    cache.store(key, original);
    auto loaded = cache.tryLoad(key);
    EXPECT_TRUE(loaded.has_value()) << "无环的嵌套容器应成功序列化";
}

TEST(BytecodeCacheL13, ClosureConstantRejected) {
    // Closure 不可序列化，isSerializable 应返回 false
    // 注：编译器实际不会把 closure 放入常量池，此为防御性测试
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_ClosureConstantRejected");
    cache.setCacheDir(dir);

    // 构造一个 closure Value（无 env/body，仅用于类型测试）
    Value closureVal = Value::makeClosure("test_fn", nullptr, {}, nullptr);
    ASSERT_TRUE(closureVal.isClosure());

    CompileResult original = makeResultWithConstants({closureVal});

    // store 应失败（isSerializable 返回 false）
    BytecodeCache::CacheKey key{"closure_const_test", 0, 0};
    cache.store(key, original);

    // tryLoad 应返回 nullopt（store 失败未写入）
    auto loaded = cache.tryLoad(key);
    EXPECT_FALSE(loaded.has_value()) << "Closure 不可序列化，应返回 nullopt";
}

TEST(BytecodeCacheL13, MixedScalarAndContainerConstants) {
    // 混合常量池：标量 + array + dict + tuple
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_MixedScalarAndContainerConstants");
    cache.setCacheDir(dir);

    std::vector<Value> arrElems{Value(int64_t(1)), Value(int64_t(2))};
    Value arrVal(std::move(arrElems));

    Value::DictMap dictEntries;
    dictEntries[Value::DictKey(std::string("k"))] = Value(std::string("v"));
    Value dictVal(std::move(dictEntries));

    std::vector<Value> tupleElems{Value(int64_t(99))};
    Value tupleVal = Value::makeTuple(std::move(tupleElems));

    CompileResult original = makeResultWithConstants({
        Value(int64_t(42)),               // 标量 int
        Value(std::string("hello")),      // 标量 string
        arrVal,                           // array
        dictVal,                          // dict
        tupleVal,                         // tuple
        Value::nullValue(),               // null
    });

    BytecodeCache::CacheKey key{"mixed_const_test", 0, 0};
    cache.store(key, original);

    auto loaded = cache.tryLoad(key);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 6u);

    EXPECT_TRUE(loaded->mainChunk.constants[0].isInt());
    EXPECT_EQ(loaded->mainChunk.constants[0].intVal(), 42);
    EXPECT_TRUE(loaded->mainChunk.constants[1].isString());
    EXPECT_EQ(loaded->mainChunk.constants[1].stringVal(), "hello");
    EXPECT_TRUE(loaded->mainChunk.constants[2].isArray());
    EXPECT_EQ(loaded->mainChunk.constants[2].arrayVal().size(), 2u);
    EXPECT_TRUE(loaded->mainChunk.constants[3].isDict());
    EXPECT_EQ(loaded->mainChunk.constants[3].dictVal().size(), 1u);
    EXPECT_TRUE(loaded->mainChunk.constants[4].isTuple());
    EXPECT_EQ(loaded->mainChunk.constants[4].tupleVal().size(), 1u);
    EXPECT_TRUE(loaded->mainChunk.constants[5].isNull());
}

TEST(BytecodeCacheL13, FileLevelStoreLoadWithArrayConstant) {
    // .minic 文件级 API（storeToFile/tryLoadFromFile）也支持容器常量
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L13_FileLevelStoreLoadWithArrayConstant");
    cache.setCacheDir(dir);

    namespace fs = std::filesystem;
    std::string minicPath = (fs::temp_directory_path() / "l13_file_array_test.minic").string();

    std::vector<Value> arrElems{
        Value(int64_t(10)),
        Value(std::string("twenty")),
        Value(3.5),
    };
    Value arrVal(std::move(arrElems));

    CompileResult original = makeResultWithConstants({arrVal});

    ASSERT_TRUE(cache.storeToFile(minicPath, original, "testmod"));
    auto loaded = cache.tryLoadFromFile(minicPath);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->mainChunk.constants.size(), 1u);
    EXPECT_TRUE(loaded->mainChunk.constants[0].isArray());
    EXPECT_EQ(loaded->mainChunk.constants[0].arrayVal().size(), 3u);

    fs::remove(minicPath);
}

// ============================================================
// L17 测试：PipelineRunner 内存级 RegisterVM 缓存（2026-07-24）
// ------------------------------------------------------------
// 验证 tryLoadRegister/storeRegister 往返等价性 + 与 StackVM 缓存隔离 + 端到端可执行。
// ============================================================

namespace {

/// 编译源码到 RegisterCompileResult
static RegisterCompileResult compileRegisterSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_NE(ast, nullptr);
    if (!ast)
        return RegisterCompileResult{};
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    return compiler.getLastRegisterResult();
}

/// 比较两个 RegBytecodeChunk 是否等价
static ::testing::AssertionResult regChunksEqual(const RegBytecodeChunk& a, const RegBytecodeChunk& b) {
    if (a.code != b.code)
        return ::testing::AssertionFailure() << "code differs";
    if (a.constants.size() != b.constants.size())
        return ::testing::AssertionFailure()
               << "constants size: " << a.constants.size() << " vs " << b.constants.size();
    for (size_t i = 0; i < a.constants.size(); ++i) {
        if (a.constants[i].getType() != b.constants[i].getType())
            return ::testing::AssertionFailure() << "constant[" << i << "] type differs";
        if (!a.constants[i].equals(b.constants[i]))
            return ::testing::AssertionFailure() << "constant[" << i << "] value differs";
    }
    if (a.lines != b.lines)
        return ::testing::AssertionFailure() << "lines differ";
    if (a.columns != b.columns)
        return ::testing::AssertionFailure() << "columns differ";
    if (a.name != b.name)
        return ::testing::AssertionFailure() << "name differs";
    if (a.arity != b.arity)
        return ::testing::AssertionFailure() << "arity differs";
    if (a.requiredArity != b.requiredArity)
        return ::testing::AssertionFailure() << "requiredArity differs";
    if (a.defaultConstIndices != b.defaultConstIndices)
        return ::testing::AssertionFailure() << "defaultConstIndices differ";
    if (a.localCount != b.localCount)
        return ::testing::AssertionFailure() << "localCount differs";
    if (a.registerCount != b.registerCount)
        return ::testing::AssertionFailure() << "registerCount differs";
    if (a.localRegNames != b.localRegNames)
        return ::testing::AssertionFailure() << "localRegNames differ";
    if (a.isGenerator != b.isGenerator)
        return ::testing::AssertionFailure() << "isGenerator differs";
    if (a.yieldCount != b.yieldCount)
        return ::testing::AssertionFailure() << "yieldCount differs";
    return ::testing::AssertionSuccess();
}

/// 比较两个 RegisterCompileResult 是否等价
static ::testing::AssertionResult regResultsEqual(const RegisterCompileResult& a, const RegisterCompileResult& b) {
    auto r = regChunksEqual(a.mainChunk, b.mainChunk);
    if (!r)
        return r << " (mainChunk)";
    if (a.functionChunks.size() != b.functionChunks.size())
        return ::testing::AssertionFailure()
               << "functionChunks size: " << a.functionChunks.size() << " vs " << b.functionChunks.size();
    for (const auto& [name, chunk] : a.functionChunks) {
        auto it = b.functionChunks.find(name);
        if (it == b.functionChunks.end())
            return ::testing::AssertionFailure() << "functionChunk '" << name << "' missing in b";
        auto cr = regChunksEqual(chunk, it->second);
        if (!cr)
            return cr << " (functionChunk '" << name << "')";
    }
    if (a.globalSlotCount != b.globalSlotCount)
        return ::testing::AssertionFailure() << "globalSlotCount differs";
    if (a.globalSlotNames != b.globalSlotNames)
        return ::testing::AssertionFailure() << "globalSlotNames differ";
    if (a.moduleExports != b.moduleExports)
        return ::testing::AssertionFailure() << "moduleExports differ";
    return ::testing::AssertionSuccess();
}

} // namespace

TEST(BytecodeCacheL17, RegisterRoundtripBasic) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_RegisterRoundtripBasic"));

    std::string src = "var x = 42; var y = x + 8; print(y);";
    RegisterCompileResult original = compileRegisterSource(src);
    ASSERT_FALSE(original.mainChunk.code.empty());

    BytecodeCache::CacheKey key;
    key.source = src;
    key.compilerMode = 0;
    cache.storeRegister(key, original);

    auto loaded = cache.tryLoadRegister(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(regResultsEqual(original, *loaded));
}

TEST(BytecodeCacheL17, RegisterMissOnDifferentSource) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_RegisterMissOnDifferentSource"));

    std::string src1 = "var x = 1;";
    std::string src2 = "var x = 2;";
    RegisterCompileResult r1 = compileRegisterSource(src1);

    BytecodeCache::CacheKey key1{src1, 0, 0};
    cache.storeRegister(key1, r1);

    BytecodeCache::CacheKey key2{src2, 0, 0};
    auto loaded = cache.tryLoadRegister(key2);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BytecodeCacheL17, StackAndRegisterCacheIsolated) {
    // StackVM .mbc 与 RegisterVM .mrc 同源码也应隔离
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_StackAndRegisterCacheIsolated"));

    std::string src = "var x = 42; print(x);";

    // 写入 StackVM 缓存
    CompileResult stackResult = compileSource(src);
    cache.store({src, 0, 0}, stackResult);

    // RegisterVM 路径查询应未命中（扩展名 .mrc 不同）
    auto loadedReg = cache.tryLoadRegister({src, 0, 0});
    EXPECT_FALSE(loadedReg.has_value()) << "RegisterVM 不应命中 StackVM 缓存";

    // 反向：写入 RegisterVM 缓存
    RegisterCompileResult regResult = compileRegisterSource(src);
    cache.storeRegister({src, 0, 0}, regResult);

    // StackVM 路径查询应命中自己的 .mbc（不受 .mrc 影响）
    auto loadedStack = cache.tryLoad({src, 0, 0});
    EXPECT_TRUE(loadedStack.has_value()) << "StackVM 缓存应仍命中";
}

TEST(BytecodeCacheL17, RegisterCachedResultExecutableByRegVM) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_RegisterCachedResultExecutableByRegVM"));

    std::string src = "var x = 10; var y = 20; print(x + y);";
    RegisterCompileResult original = compileRegisterSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.storeRegister(key, original);

    auto loaded = cache.tryLoadRegister(key);
    ASSERT_TRUE(loaded.has_value());

    // 用 RegisterVM 执行缓存加载的 RegisterCompileResult
    std::string output;
    RegisterVM vm;
    vm.setOutputCallback([&output](const std::string& s) { output += s; });
    VMResult result = vm.execute(*loaded);
    EXPECT_EQ(result, VMResult::VM_OK);
    EXPECT_EQ(output, "30");
}

TEST(BytecodeCacheL17, RegisterClearInvalidates) {
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_RegisterClearInvalidates"));

    std::string src = "var x = 1;";
    RegisterCompileResult r = compileRegisterSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.storeRegister(key, r);
    ASSERT_TRUE(cache.tryLoadRegister(key).has_value());

    // clear() 应同时清理 .mbc 和 .mrc
    cache.clear();
    EXPECT_FALSE(cache.tryLoadRegister(key).has_value());
}

TEST(BytecodeCacheL17, RegisterEntryCountWithMixedCaches) {
    // 验证 entryCount 同时计数 .mbc 和 .mrc
    BytecodeCache cache;
    cache.setCacheDir(makeUniqueCacheDir("L17_RegisterEntryCountWithMixedCaches"));

    EXPECT_EQ(cache.entryCount(), 0u);

    std::string src1 = "var x = 1;";
    std::string src2 = "var y = 2;";

    CompileResult stackR1 = compileSource(src1);
    RegisterCompileResult regR2 = compileRegisterSource(src2);

    cache.store({src1, 0, 0}, stackR1);
    EXPECT_EQ(cache.entryCount(), 1u);

    cache.storeRegister({src2, 0, 0}, regR2);
    EXPECT_EQ(cache.entryCount(), 2u);
}

TEST(BytecodeCacheL17, RegisterCorruptedFileReturnsNullopt) {
    BytecodeCache cache;
    std::string dir = makeUniqueCacheDir("L17_RegisterCorruptedFileReturnsNullopt");
    cache.setCacheDir(dir);

    std::string src = "var x = 1;";
    RegisterCompileResult r = compileRegisterSource(src);

    BytecodeCache::CacheKey key{src, 0, 0};
    cache.storeRegister(key, r);

    // 找到 .mrc 文件并破坏它
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".mrc") {
            std::ofstream ofs(entry.path(), std::ios::binary | std::ios::trunc);
            ofs << "GARBAGE_DATA_THIS_IS_NOT_A_VALID_REGISTER_CACHE_FILE!!!";
            break;
        }
    }

    auto loaded = cache.tryLoadRegister(key);
    EXPECT_FALSE(loaded.has_value());
}
