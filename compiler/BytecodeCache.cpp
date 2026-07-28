#include "compiler/BytecodeCache.h"

#include "interpreter/Value.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

// ============================================================
// 内部: 二进制写入器
// ============================================================
namespace {

class BufferWriter {
    std::vector<uint8_t> buf_;

public:
    void writeBytes(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + len);
    }
    void writeU8(uint8_t v) { buf_.push_back(v); }
    void writeU16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void writeU32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    }
    void writeU64(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    }
    void writeI32(int32_t v) { writeU32(static_cast<uint32_t>(v)); }
    void writeI64(int64_t v) { writeU64(static_cast<uint64_t>(v)); }
    void writeF64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU64(bits);
    }
    void writeBool(bool v) { writeU8(v ? 1 : 0); }
    void writeString(const std::string& s) {
        writeU32(static_cast<uint32_t>(s.size()));
        writeBytes(s.data(), s.size());
    }
    const std::vector<uint8_t>& data() const { return buf_; }
};

// ============================================================
// 内部: 二进制读取器
// ============================================================

class BufferReader {
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;

public:
    BufferReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool ok() const { return ok_; }
    size_t remaining() const { return pos_ <= size_ ? size_ - pos_ : 0; }

    uint8_t readU8() {
        if (!ok_ || pos_ + 1 > size_) {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }
    uint16_t readU16() {
        if (!ok_ || pos_ + 2 > size_) {
            ok_ = false;
            return 0;
        }
        uint16_t v = static_cast<uint16_t>(data_[pos_]) | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return v;
    }
    uint32_t readU32() {
        if (!ok_ || pos_ + 4 > size_) {
            ok_ = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(data_[pos_ + i]) << (i * 8);
        }
        pos_ += 4;
        return v;
    }
    uint64_t readU64() {
        if (!ok_ || pos_ + 8 > size_) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
        }
        pos_ += 8;
        return v;
    }
    int32_t readI32() { return static_cast<int32_t>(readU32()); }
    int64_t readI64() { return static_cast<int64_t>(readU64()); }
    double readF64() {
        uint64_t bits = readU64();
        if (!ok_)
            return 0.0;
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    bool readBool() { return readU8() != 0; }
    std::string readString() {
        uint32_t len = readU32();
        if (!ok_ || pos_ + len > size_) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }
    bool readBytes(void* out, size_t len) {
        if (!ok_ || pos_ + len > size_) {
            ok_ = false;
            return false;
        }
        std::memcpy(out, data_ + pos_, len);
        pos_ += len;
        return true;
    }
};

// ============================================================
// FNV-1a hash
// ============================================================

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

// ============================================================
// L12: 源码 mtime + content hash 计算（用于 .minic 失效校验）
// ------------------------------------------------------------
// 返回 {mtime_ms, content_hash}；文件不存在或读取失败返回 {0, 0}。
// mtime 用 int64_t 毫秒表示（自 epoch 起），0 表示无效/不校验。
// content hash 复用 fnv1a64（与 P1-7 缓存 key 算法一致）。
// ============================================================

struct SourceMeta {
    int64_t mtimeMs = 0;
    uint64_t contentHash = 0;
};

SourceMeta computeSourceMeta(const std::string& sourcePath) {
    namespace fs = std::filesystem;
    SourceMeta meta{0, 0};

    std::error_code ec;
    auto ftime = fs::last_write_time(sourcePath, ec);
    if (ec)
        return meta;

    // C++20: file_time_type 可通过 clock_cast 转换到 system_clock
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    meta.mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    // 读取源码内容计算 hash
    std::ifstream ifs(sourcePath, std::ios::binary);
    if (!ifs)
        return {0, 0};
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    meta.contentHash = fnv1a64(content);
    return meta;
}

// ============================================================
// 常量
// ============================================================

constexpr char kMagic[4] = {'M', 'L', 'B', 'C'};
// L11: RegisterVM 预编译模块使用独立 magic "MLRC"（MiniLang Register Cache），
// 与 StackVM "MLBC"（MiniLang Bytecode Cache）隔离，避免误加载错误格式。
constexpr char kRegisterMagic[4] = {'M', 'L', 'R', 'C'};
// AUDIT-R3 P2-4/P2-5 fix: 版本 1→2——(1) 头部 reserved 字节改为承载 optFlags
// （优化开关入键）；(2) moduleExports 段改为无条件读写（移除 remaining()>=4
// 启发式）。旧版本缓存由版本校验自然失效。
constexpr uint16_t kFormatVersion = 3; // AUDIT-R6 F3: VMEnumVariantInfo.paramTypes + VMEnumInfo.typeParams
constexpr size_t kHeaderSize = 32;
constexpr const char* kCacheSubDir = "minilang_bytecache";

// ============================================================
// 序列化: Value (标量 + 容器堆类型)
// ------------------------------------------------------------
// L13 扩展（2026-07-24）：支持 Array/Dict/Tuple/EnumVariant 序列化。
//   - BoxedInt（超 int48 范围）自动通过 isInt() → tag 1 路径处理，
//     Value(int64_t) 构造函数会自动装箱，无需独立 tag。
//   - Closure/Instance/Channel/Mutex/RwLock/Thread/Coroutine 永不进入
//     常量池（编译器 addConstant 仅放标量），但仍标记 kNonSerializableTag 作防御。
//   - 环检测：thread_local visited 集合记录已访问的堆对象指针，
//     遇到环时写 kNonSerializableTag 失败，调用方根据返回值回滚或降级。
//
// tag 编码:
//   0   = null
//   1   = int (含 BoxedInt，统一写 int64)
//   2   = float
//   3   = bool
//   4   = string
//   5   = array  (u32 count + count 个 Value)
//   6   = dict   (u32 count + count 个 (DictKey, Value) 对)
//   7   = tuple  (u32 count + count 个 Value)
//   8   = enum_variant (string enumName + string variantName + u32 fieldCount + fields)
//   kNonSerializableTag = 不可序列化（Closure/Instance/同步对象/Coroutine/环引用）
// ============================================================

// P3-14: 不可序列化 Value 的 tag 标记（协议级哨兵，与 NO_INDEX/NO_SLOT 索引哨兵不同）
constexpr uint8_t kNonSerializableTag = 0xFF;

// L13: 环检测 visited 集合
// thread_local 保证多线程安全；每次 writeValueRecursive 入口清理。
// 仅记录堆类型对象指针（标量不入集合）。
thread_local std::unordered_set<const void*> g_serializeVisited;

// 前向声明（互相递归）
bool writeValueRecursive(BufferWriter& w, const Value& v);
bool readValueRecursive(BufferReader& r, Value& v);

// DictKey 序列化: variant index 0=string, 1=int64, 2=bool, 3=double
bool writeDictKey(BufferWriter& w, const Value::DictKey& k) {
    switch (k.index()) {
    case 0:
        w.writeU8(0);
        w.writeString(std::get<std::string>(k));
        return true;
    case 1:
        w.writeU8(1);
        w.writeI64(std::get<int64_t>(k));
        return true;
    case 2:
        w.writeU8(2);
        w.writeBool(std::get<bool>(k));
        return true;
    case 3:
        w.writeU8(3);
        w.writeF64(std::get<double>(k));
        return true;
    default:
        return false;
    }
}

bool readDictKey(BufferReader& r, Value::DictKey& k) {
    uint8_t tag = r.readU8();
    if (!r.ok())
        return false;
    switch (tag) {
    case 0: {
        std::string s = r.readString();
        if (!r.ok())
            return false;
        k = std::move(s);
        return true;
    }
    case 1: {
        int64_t x = r.readI64();
        if (!r.ok())
            return false;
        k = x;
        return true;
    }
    case 2: {
        bool x = r.readBool();
        if (!r.ok())
            return false;
        k = x;
        return true;
    }
    case 3: {
        double x = r.readF64();
        if (!r.ok())
            return false;
        k = x;
        return true;
    }
    default:
        return false;
    }
}

bool writeValueRecursive(BufferWriter& w, const Value& v) {
    // 标量路径（不入 visited 集合）
    if (v.isNull()) {
        w.writeU8(0);
        return true;
    }
    if (v.isInt()) {
        // L13: BoxedInt 也走此路径，intVal() 统一返回 int64
        w.writeU8(1);
        w.writeI64(v.intVal());
        return true;
    }
    if (v.isFloat()) {
        w.writeU8(2);
        w.writeF64(v.floatVal());
        return true;
    }
    if (v.isBool()) {
        w.writeU8(3);
        w.writeBool(v.boolVal());
        return true;
    }
    if (v.isString()) {
        w.writeU8(4);
        w.writeString(v.stringVal());
        return true;
    }

    // 堆类型路径：环检测
    // asPointer() 返回 RefCounted*（仅当 isPointer()）
    if (!v.isPointer()) {
        w.writeU8(kNonSerializableTag);
        return false;
    }
    const void* ptr = v.asPointer();
    if (g_serializeVisited.count(ptr)) {
        // 环引用：写 kNonSerializableTag 失败，调用方回滚
        w.writeU8(kNonSerializableTag);
        return false;
    }
    g_serializeVisited.insert(ptr);

    bool ok = false;
    if (v.isArray()) {
        w.writeU8(5);
        const auto& elems = v.arrayVal();
        w.writeU32(static_cast<uint32_t>(elems.size()));
        ok = true;
        for (const auto& e : elems) {
            if (!writeValueRecursive(w, e)) {
                ok = false;
                break;
            }
        }
    } else if (v.isDict()) {
        w.writeU8(6);
        const auto& entries = v.dictVal();
        w.writeU32(static_cast<uint32_t>(entries.size()));
        ok = true;
        for (const auto& [k, val] : entries) {
            if (!writeDictKey(w, k) || !writeValueRecursive(w, val)) {
                ok = false;
                break;
            }
        }
    } else if (v.isTuple()) {
        w.writeU8(7);
        const auto& elems = v.tupleVal();
        w.writeU32(static_cast<uint32_t>(elems.size()));
        ok = true;
        for (const auto& e : elems) {
            if (!writeValueRecursive(w, e)) {
                ok = false;
                break;
            }
        }
    } else if (v.isEnumVariant()) {
        w.writeU8(8);
        w.writeString(v.enumVariantEnumName());
        w.writeString(v.enumVariantName());
        const auto& fields = v.enumVariantFields();
        w.writeU32(static_cast<uint32_t>(fields.size()));
        ok = true;
        for (const auto& f : fields) {
            if (!writeValueRecursive(w, f)) {
                ok = false;
                break;
            }
        }
    } else {
        // Closure/Instance/Channel/Mutex/RwLock/Thread/Coroutine: 不可序列化
        w.writeU8(kNonSerializableTag);
        ok = false;
    }

    g_serializeVisited.erase(ptr);
    return ok;
}

bool readValueRecursive(BufferReader& r, Value& v) {
    uint8_t tag = r.readU8();
    if (!r.ok())
        return false;
    switch (tag) {
    case 0:
        v = Value::nullValue();
        return true;
    case 1: {
        int64_t x = r.readI64();
        if (!r.ok())
            return false;
        // Value(int64_t) 自动处理 int48 内联 vs BoxedInt 装箱
        v = Value(x);
        return true;
    }
    case 2: {
        double x = r.readF64();
        if (!r.ok())
            return false;
        v = Value(x);
        return true;
    }
    case 3: {
        bool x = r.readBool();
        if (!r.ok())
            return false;
        v = Value(x);
        return true;
    }
    case 4: {
        std::string s = r.readString();
        if (!r.ok())
            return false;
        v = Value(s);
        return true;
    }
    case 5: { // array
        uint32_t cnt = r.readU32();
        if (!r.ok())
            return false;
        std::vector<Value> elems;
        elems.reserve(cnt);
        for (uint32_t i = 0; i < cnt; ++i) {
            Value e;
            if (!readValueRecursive(r, e))
                return false;
            elems.push_back(std::move(e));
        }
        v = Value(std::move(elems));
        return true;
    }
    case 6: { // dict
        uint32_t cnt = r.readU32();
        if (!r.ok())
            return false;
        Value::DictMap entries;
        for (uint32_t i = 0; i < cnt; ++i) {
            Value::DictKey k;
            if (!readDictKey(r, k))
                return false;
            Value val;
            if (!readValueRecursive(r, val))
                return false;
            entries.emplace(std::move(k), std::move(val));
        }
        v = Value(std::move(entries));
        return true;
    }
    case 7: { // tuple
        uint32_t cnt = r.readU32();
        if (!r.ok())
            return false;
        std::vector<Value> elems;
        elems.reserve(cnt);
        for (uint32_t i = 0; i < cnt; ++i) {
            Value e;
            if (!readValueRecursive(r, e))
                return false;
            elems.push_back(std::move(e));
        }
        v = Value::makeTuple(std::move(elems));
        return true;
    }
    case 8: { // enum_variant
        std::string enumName = r.readString();
        if (!r.ok())
            return false;
        std::string variantName = r.readString();
        if (!r.ok())
            return false;
        uint32_t fieldCnt = r.readU32();
        if (!r.ok())
            return false;
        std::vector<Value> fields;
        fields.reserve(fieldCnt);
        for (uint32_t i = 0; i < fieldCnt; ++i) {
            Value f;
            if (!readValueRecursive(r, f))
                return false;
            fields.push_back(std::move(f));
        }
        v = Value::makeEnumVariant(enumName, variantName, std::move(fields));
        return true;
    }
    default:
        // kNonSerializableTag 或未知 tag → 不可序列化或损坏
        return false;
    }
}

void writeValue(BufferWriter& w, const Value& v) {
    writeValueRecursive(w, v);
}

bool readValue(BufferReader& r, Value& v) {
    return readValueRecursive(r, v);
}

// ============================================================
// 序列化: UpvalueDesc
// ============================================================

void writeUpvalueDesc(BufferWriter& w, const UpvalueDesc& uv) {
    w.writeI32(uv.index);
    w.writeBool(uv.isLocal);
    w.writeString(uv.name);
}

bool readUpvalueDesc(BufferReader& r, UpvalueDesc& uv) {
    uv.index = r.readI32();
    uv.isLocal = r.readBool();
    uv.name = r.readString();
    return r.ok();
}

// ============================================================
// 序列化: SlotNameRange（模板，兼容 BytecodeChunk/RegBytecodeChunk/IRFunction）
// 三者 SlotNameRange 结构体字段一致（slot/name/startIp/endIp），模板避免重复。
// ============================================================

template <typename SlotNameRangeT> void writeSlotNameRange(BufferWriter& w, const SlotNameRangeT& sr) {
    w.writeU8(sr.slot);
    w.writeString(sr.name);
    w.writeU64(static_cast<uint64_t>(sr.startIp));
    w.writeU64(static_cast<uint64_t>(sr.endIp));
}

template <typename SlotNameRangeT> bool readSlotNameRange(BufferReader& r, SlotNameRangeT& sr) {
    sr.slot = r.readU8();
    sr.name = r.readString();
    sr.startIp = static_cast<size_t>(r.readU64());
    sr.endIp = static_cast<size_t>(r.readU64());
    return r.ok();
}

// ============================================================
// 序列化: BytecodeChunk
// ============================================================

bool writeBytecodeChunk(BufferWriter& w, const BytecodeChunk& chunk) {
    // code
    w.writeU32(static_cast<uint32_t>(chunk.code.size()));
    w.writeBytes(chunk.code.data(), chunk.code.size());

    // constants (vector<Value>) — L13: 标量 + 容器堆类型,isSerializable 已递归预检查
    w.writeU32(static_cast<uint32_t>(chunk.constants.size()));
    for (const auto& v : chunk.constants) {
        writeValue(w, v);
    }

    // lines
    w.writeU32(static_cast<uint32_t>(chunk.lines.size()));
    for (int l : chunk.lines) {
        w.writeI32(l);
    }

    // columns
    w.writeU32(static_cast<uint32_t>(chunk.columns.size()));
    for (int c : chunk.columns) {
        w.writeI32(c);
    }

    // name
    w.writeString(chunk.name);

    // arity, requiredArity
    w.writeI32(chunk.arity);
    w.writeI32(chunk.requiredArity);

    // defaultConstIndices (vector<uint16_t>)
    w.writeU32(static_cast<uint32_t>(chunk.defaultConstIndices.size()));
    for (uint16_t idx : chunk.defaultConstIndices) {
        w.writeU16(idx);
    }

    // ipToInstrIndex (vector<int>)
    w.writeU32(static_cast<uint32_t>(chunk.ipToInstrIndex.size()));
    for (int idx : chunk.ipToInstrIndex) {
        w.writeI32(idx);
    }

    // fieldOrder (vector<string>)
    w.writeU32(static_cast<uint32_t>(chunk.fieldOrder.size()));
    for (const auto& f : chunk.fieldOrder) {
        w.writeString(f);
    }

    // localCount
    w.writeI32(chunk.localCount);

    // upvalues (vector<UpvalueDesc>)
    w.writeU32(static_cast<uint32_t>(chunk.upvalues.size()));
    for (const auto& uv : chunk.upvalues) {
        writeUpvalueDesc(w, uv);
    }

    // localSlotNames (vector<string>)
    w.writeU32(static_cast<uint32_t>(chunk.localSlotNames.size()));
    for (const auto& n : chunk.localSlotNames) {
        w.writeString(n);
    }

    // slotNameRanges (vector<SlotNameRange>)
    w.writeU32(static_cast<uint32_t>(chunk.slotNameRanges.size()));
    for (const auto& sr : chunk.slotNameRanges) {
        writeSlotNameRange(w, sr);
    }

    // isGenerator, yieldCount
    w.writeBool(chunk.isGenerator);
    w.writeI32(chunk.yieldCount);

    return true;
}

bool readBytecodeChunk(BufferReader& r, BytecodeChunk& chunk) {
    // code
    uint32_t codeLen = r.readU32();
    if (!r.ok())
        return false;
    chunk.code.resize(codeLen);
    if (codeLen > 0 && !r.readBytes(chunk.code.data(), codeLen)) {
        return false;
    }

    // constants
    uint32_t constCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.constants.clear();
    chunk.constants.reserve(constCount);
    for (uint32_t i = 0; i < constCount; ++i) {
        Value v;
        if (!readValue(r, v))
            return false;
        chunk.constants.push_back(v);
    }

    // lines
    uint32_t lineCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.lines.resize(lineCount);
    for (uint32_t i = 0; i < lineCount; ++i) {
        chunk.lines[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // columns
    uint32_t colCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.columns.resize(colCount);
    for (uint32_t i = 0; i < colCount; ++i) {
        chunk.columns[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // name
    chunk.name = r.readString();
    if (!r.ok())
        return false;

    // arity, requiredArity
    chunk.arity = r.readI32();
    chunk.requiredArity = r.readI32();
    if (!r.ok())
        return false;

    // defaultConstIndices
    uint32_t dciCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.defaultConstIndices.resize(dciCount);
    for (uint32_t i = 0; i < dciCount; ++i) {
        chunk.defaultConstIndices[i] = r.readU16();
    }
    if (!r.ok())
        return false;

    // ipToInstrIndex
    uint32_t ipiCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.ipToInstrIndex.resize(ipiCount);
    for (uint32_t i = 0; i < ipiCount; ++i) {
        chunk.ipToInstrIndex[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // fieldOrder
    uint32_t foCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.fieldOrder.resize(foCount);
    for (uint32_t i = 0; i < foCount; ++i) {
        chunk.fieldOrder[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // localCount
    chunk.localCount = r.readI32();
    if (!r.ok())
        return false;

    // upvalues
    uint32_t uvCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.upvalues.resize(uvCount);
    for (uint32_t i = 0; i < uvCount; ++i) {
        if (!readUpvalueDesc(r, chunk.upvalues[i]))
            return false;
    }

    // localSlotNames
    uint32_t lsnCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.localSlotNames.resize(lsnCount);
    for (uint32_t i = 0; i < lsnCount; ++i) {
        chunk.localSlotNames[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // slotNameRanges
    uint32_t snrCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.slotNameRanges.resize(snrCount);
    for (uint32_t i = 0; i < snrCount; ++i) {
        if (!readSlotNameRange(r, chunk.slotNameRanges[i]))
            return false;
    }

    // isGenerator, yieldCount
    chunk.isGenerator = r.readBool();
    chunk.yieldCount = r.readI32();
    if (!r.ok())
        return false;

    // 重置懒缓存字段(运行时重新计算)
    chunk.fieldIndexCacheBuilt_ = false;
    chunk.fieldIndexCache_.clear();

    return true;
}

// ============================================================
// 序列化: VMEnumVariantInfo / VMEnumInfo
// ============================================================

void writeEnumVariant(BufferWriter& w, const VMEnumVariantInfo& v) {
    w.writeString(v.name);
    w.writeI32(v.arity);
    // AUDIT-R6 F3 fix: 序列化字段类型注解（kFormatVersion 2→3）
    w.writeU32(static_cast<uint32_t>(v.paramTypes.size()));
    for (const auto& t : v.paramTypes) {
        w.writeString(t);
    }
}

bool readEnumVariant(BufferReader& r, VMEnumVariantInfo& v) {
    v.name = r.readString();
    v.arity = r.readI32();
    // AUDIT-R6 F3 fix: 反序列化字段类型注解
    uint32_t tcnt = r.readU32();
    if (!r.ok())
        return false;
    v.paramTypes.resize(tcnt);
    for (uint32_t i = 0; i < tcnt; ++i) {
        v.paramTypes[i] = r.readString();
    }
    return r.ok();
}

void writeEnumInfo(BufferWriter& w, const VMEnumInfo& e) {
    w.writeString(e.name);
    w.writeU32(static_cast<uint32_t>(e.variants.size()));
    for (const auto& v : e.variants) {
        writeEnumVariant(w, v);
    }
    // AUDIT-R6 F3 fix: 序列化泛型类型参数
    w.writeU32(static_cast<uint32_t>(e.typeParams.size()));
    for (const auto& t : e.typeParams) {
        w.writeString(t);
    }
}

bool readEnumInfo(BufferReader& r, VMEnumInfo& e) {
    e.name = r.readString();
    uint32_t cnt = r.readU32();
    if (!r.ok())
        return false;
    e.variants.resize(cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        if (!readEnumVariant(r, e.variants[i]))
            return false;
    }
    // AUDIT-R6 F3 fix: 反序列化泛型类型参数
    uint32_t tpCnt = r.readU32();
    if (!r.ok())
        return false;
    e.typeParams.resize(tpCnt);
    for (uint32_t i = 0; i < tpCnt; ++i) {
        e.typeParams[i] = r.readString();
    }
    return r.ok();
}

// ============================================================
// 序列化: CompileResult
// ============================================================

/// L13: 递归检查单个 Value 是否可序列化。
/// 标量（int/float/bool/null/string）总是可序列化；
/// Array/Dict/Tuple/EnumVariant 递归检查元素（含环检测）；
/// Closure/Instance/Channel/Mutex/RwLock/Thread/Coroutine 不可序列化。
/// 复用 g_serializeVisited 集合做环检测（与 writeValueRecursive 共享）。
bool isValueSerializable(const Value& v) {
    if (v.isNull() || v.isInt() || v.isFloat() || v.isBool() || v.isString())
        return true;
    if (!v.isPointer())
        return false;
    const void* ptr = v.asPointer();
    if (g_serializeVisited.count(ptr))
        return false; // 环引用
    g_serializeVisited.insert(ptr);
    bool ok = false;
    if (v.isArray()) {
        ok = true;
        for (const auto& e : v.arrayVal()) {
            if (!isValueSerializable(e)) {
                ok = false;
                break;
            }
        }
    } else if (v.isDict()) {
        ok = true;
        for (const auto& [k, val] : v.dictVal()) {
            // DictKey 是 string/int64/bool/double，总是可序列化
            if (!isValueSerializable(val)) {
                ok = false;
                break;
            }
        }
    } else if (v.isTuple()) {
        ok = true;
        for (const auto& e : v.tupleVal()) {
            if (!isValueSerializable(e)) {
                ok = false;
                break;
            }
        }
    } else if (v.isEnumVariant()) {
        ok = true;
        for (const auto& f : v.enumVariantFields()) {
            if (!isValueSerializable(f)) {
                ok = false;
                break;
            }
        }
    }
    // Closure/Instance/Channel/Mutex/RwLock/Thread/Coroutine: ok 保持 false
    g_serializeVisited.erase(ptr);
    return ok;
}

/// 预检查:常量池所有值是否可序列化（递归 + 环检测）。
/// L13 扩展：允许 Array/Dict/Tuple/EnumVariant（递归检查元素），
/// 仍拒绝 Closure/Instance/同步对象/Coroutine。
bool isSerializable(const BytecodeChunk& chunk) {
    for (const auto& v : chunk.constants) {
        if (!isValueSerializable(v))
            return false;
    }
    return true;
}

bool writeCompileResult(BufferWriter& w, const CompileResult& cr) {
    // 预检查:所有 chunk 的常量池必须可序列化（递归）
    // 清理环检测集合（防御性，正常情况下上次调用已清空）
    g_serializeVisited.clear();
    if (!isSerializable(cr.mainChunk))
        return false;
    for (const auto& [name, chunk] : cr.functionChunks) {
        if (!isSerializable(chunk))
            return false;
    }
    g_serializeVisited.clear();

    // mainChunk
    if (!writeBytecodeChunk(w, cr.mainChunk))
        return false;

    // functionChunks (map<string, BytecodeChunk>)
    w.writeU32(static_cast<uint32_t>(cr.functionChunks.size()));
    for (const auto& [name, chunk] : cr.functionChunks) {
        w.writeString(name);
        if (!writeBytecodeChunk(w, chunk))
            return false;
    }

    // globalSlotCount
    w.writeI32(cr.globalSlotCount);

    // globalSlotNames (vector<string>)
    w.writeU32(static_cast<uint32_t>(cr.globalSlotNames.size()));
    for (const auto& n : cr.globalSlotNames) {
        w.writeString(n);
    }

    // enumInfos (vector<VMEnumInfo>)
    w.writeU32(static_cast<uint32_t>(cr.enumInfos.size()));
    for (const auto& e : cr.enumInfos) {
        writeEnumInfo(w, e);
    }

    // P2-11 预编译模块: moduleExports (vector<string>)
    w.writeU32(static_cast<uint32_t>(cr.moduleExports.size()));
    for (const auto& name : cr.moduleExports) {
        w.writeString(name);
    }

    return true;
}

bool readCompileResult(BufferReader& r, CompileResult& cr) {
    // mainChunk
    if (!readBytecodeChunk(r, cr.mainChunk))
        return false;

    // functionChunks
    uint32_t fcCount = r.readU32();
    if (!r.ok())
        return false;
    cr.functionChunks.clear();
    for (uint32_t i = 0; i < fcCount; ++i) {
        std::string name = r.readString();
        if (!r.ok())
            return false;
        BytecodeChunk chunk;
        if (!readBytecodeChunk(r, chunk))
            return false;
        cr.functionChunks.emplace(std::move(name), std::move(chunk));
    }

    // globalSlotCount
    cr.globalSlotCount = r.readI32();
    if (!r.ok())
        return false;

    // globalSlotNames
    uint32_t gsnCount = r.readU32();
    if (!r.ok())
        return false;
    cr.globalSlotNames.resize(gsnCount);
    for (uint32_t i = 0; i < gsnCount; ++i) {
        cr.globalSlotNames[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // enumInfos
    uint32_t eiCount = r.readU32();
    if (!r.ok())
        return false;
    cr.enumInfos.resize(eiCount);
    for (uint32_t i = 0; i < eiCount; ++i) {
        if (!readEnumInfo(r, cr.enumInfos[i]))
            return false;
    }

    // P2-11 预编译模块: moduleExports
    // AUDIT-R3 P2-5 fix: kFormatVersion 2 起无条件读取（writer 恒写入本段）——
    // 原 remaining()>=4 启发式在未来追加尾部段时会把其他数据误读为导出表；
    // 旧格式（无本段）缓存已被版本校验拒绝，无需兼容分支。
    {
        uint32_t meCount = r.readU32();
        if (!r.ok())
            return false;
        cr.moduleExports.reserve(meCount);
        for (uint32_t i = 0; i < meCount; ++i) {
            cr.moduleExports.push_back(r.readString());
            if (!r.ok())
                return false;
        }
    }

    return true;
}

// ============================================================
// L11 序列化: RegBytecodeChunk / RegisterCompileResult
// ------------------------------------------------------------
// 与 BytecodeChunk 序列化的字段差异：
//   - 多 registerCount（在 localCount 之后写入）
//   - localRegNames 对应 BytecodeChunk::localSlotNames
//   - 其余字段（code/constants/lines/columns/name/arity/requiredArity/
//     defaultConstIndices/ipToInstrIndex/fieldOrder/localCount/upvalues/
//     slotNameRanges/isGenerator/yieldCount）顺序与 BytecodeChunk 一致
// ============================================================

bool isRegSerializable(const RegBytecodeChunk& chunk) {
    // 复用 g_serializeVisited 集合（已由调用方清理）
    for (const auto& v : chunk.constants) {
        if (!isValueSerializable(v))
            return false;
    }
    return true;
}

bool writeRegBytecodeChunk(BufferWriter& w, const RegBytecodeChunk& chunk) {
    // code
    w.writeU32(static_cast<uint32_t>(chunk.code.size()));
    w.writeBytes(chunk.code.data(), chunk.code.size());

    // constants (vector<Value>)
    w.writeU32(static_cast<uint32_t>(chunk.constants.size()));
    for (const auto& v : chunk.constants) {
        writeValue(w, v);
    }

    // lines
    w.writeU32(static_cast<uint32_t>(chunk.lines.size()));
    for (int l : chunk.lines) {
        w.writeI32(l);
    }

    // columns
    w.writeU32(static_cast<uint32_t>(chunk.columns.size()));
    for (int c : chunk.columns) {
        w.writeI32(c);
    }

    // name
    w.writeString(chunk.name);

    // arity, requiredArity
    w.writeI32(chunk.arity);
    w.writeI32(chunk.requiredArity);

    // defaultConstIndices (vector<uint16_t>)
    w.writeU32(static_cast<uint32_t>(chunk.defaultConstIndices.size()));
    for (uint16_t idx : chunk.defaultConstIndices) {
        w.writeU16(idx);
    }

    // ipToInstrIndex (vector<int>)
    w.writeU32(static_cast<uint32_t>(chunk.ipToInstrIndex.size()));
    for (int idx : chunk.ipToInstrIndex) {
        w.writeI32(idx);
    }

    // fieldOrder (vector<string>)
    w.writeU32(static_cast<uint32_t>(chunk.fieldOrder.size()));
    for (const auto& f : chunk.fieldOrder) {
        w.writeString(f);
    }

    // localCount
    w.writeI32(chunk.localCount);

    // L11: registerCount（RegBytecodeChunk 独有）
    w.writeI32(chunk.registerCount);

    // upvalues (vector<UpvalueDesc>)
    w.writeU32(static_cast<uint32_t>(chunk.upvalues.size()));
    for (const auto& uv : chunk.upvalues) {
        writeUpvalueDesc(w, uv);
    }

    // localRegNames (vector<string>) — 对应 BytecodeChunk::localSlotNames
    w.writeU32(static_cast<uint32_t>(chunk.localRegNames.size()));
    for (const auto& n : chunk.localRegNames) {
        w.writeString(n);
    }

    // slotNameRanges (vector<SlotNameRange>) — 与 BytecodeChunk 结构一致
    w.writeU32(static_cast<uint32_t>(chunk.slotNameRanges.size()));
    for (const auto& sr : chunk.slotNameRanges) {
        writeSlotNameRange(w, sr);
    }

    // isGenerator, yieldCount
    w.writeBool(chunk.isGenerator);
    w.writeI32(chunk.yieldCount);

    return true;
}

bool readRegBytecodeChunk(BufferReader& r, RegBytecodeChunk& chunk) {
    // code
    uint32_t codeLen = r.readU32();
    if (!r.ok())
        return false;
    chunk.code.resize(codeLen);
    if (codeLen > 0 && !r.readBytes(chunk.code.data(), codeLen)) {
        return false;
    }

    // constants
    uint32_t constCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.constants.clear();
    chunk.constants.reserve(constCount);
    for (uint32_t i = 0; i < constCount; ++i) {
        Value v;
        if (!readValue(r, v))
            return false;
        chunk.constants.push_back(v);
    }

    // lines
    uint32_t lineCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.lines.resize(lineCount);
    for (uint32_t i = 0; i < lineCount; ++i) {
        chunk.lines[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // columns
    uint32_t colCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.columns.resize(colCount);
    for (uint32_t i = 0; i < colCount; ++i) {
        chunk.columns[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // name
    chunk.name = r.readString();
    if (!r.ok())
        return false;

    // arity, requiredArity
    chunk.arity = r.readI32();
    chunk.requiredArity = r.readI32();
    if (!r.ok())
        return false;

    // defaultConstIndices
    uint32_t dciCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.defaultConstIndices.resize(dciCount);
    for (uint32_t i = 0; i < dciCount; ++i) {
        chunk.defaultConstIndices[i] = r.readU16();
    }
    if (!r.ok())
        return false;

    // ipToInstrIndex
    uint32_t ipiCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.ipToInstrIndex.resize(ipiCount);
    for (uint32_t i = 0; i < ipiCount; ++i) {
        chunk.ipToInstrIndex[i] = r.readI32();
    }
    if (!r.ok())
        return false;

    // fieldOrder
    uint32_t foCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.fieldOrder.resize(foCount);
    for (uint32_t i = 0; i < foCount; ++i) {
        chunk.fieldOrder[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // localCount
    chunk.localCount = r.readI32();
    if (!r.ok())
        return false;

    // L11: registerCount
    chunk.registerCount = r.readI32();
    if (!r.ok())
        return false;

    // upvalues
    uint32_t uvCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.upvalues.resize(uvCount);
    for (uint32_t i = 0; i < uvCount; ++i) {
        if (!readUpvalueDesc(r, chunk.upvalues[i]))
            return false;
    }

    // localRegNames
    uint32_t lrnCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.localRegNames.resize(lrnCount);
    for (uint32_t i = 0; i < lrnCount; ++i) {
        chunk.localRegNames[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // slotNameRanges
    uint32_t snrCount = r.readU32();
    if (!r.ok())
        return false;
    chunk.slotNameRanges.resize(snrCount);
    for (uint32_t i = 0; i < snrCount; ++i) {
        if (!readSlotNameRange(r, chunk.slotNameRanges[i]))
            return false;
    }

    // isGenerator, yieldCount
    chunk.isGenerator = r.readBool();
    chunk.yieldCount = r.readI32();
    if (!r.ok())
        return false;

    return true;
}

bool writeRegisterCompileResult(BufferWriter& w, const RegisterCompileResult& cr) {
    // 预检查:所有 chunk 的常量池必须可序列化（递归）
    g_serializeVisited.clear();
    if (!isRegSerializable(cr.mainChunk))
        return false;
    for (const auto& [name, chunk] : cr.functionChunks) {
        if (!isRegSerializable(chunk))
            return false;
    }
    g_serializeVisited.clear();

    // mainChunk
    if (!writeRegBytecodeChunk(w, cr.mainChunk))
        return false;

    // functionChunks (map<string, RegBytecodeChunk>)
    w.writeU32(static_cast<uint32_t>(cr.functionChunks.size()));
    for (const auto& [name, chunk] : cr.functionChunks) {
        w.writeString(name);
        if (!writeRegBytecodeChunk(w, chunk))
            return false;
    }

    // globalSlotCount
    w.writeI32(cr.globalSlotCount);

    // globalSlotNames (vector<string>)
    w.writeU32(static_cast<uint32_t>(cr.globalSlotNames.size()));
    for (const auto& n : cr.globalSlotNames) {
        w.writeString(n);
    }

    // enumInfos (vector<VMEnumInfo>)
    w.writeU32(static_cast<uint32_t>(cr.enumInfos.size()));
    for (const auto& e : cr.enumInfos) {
        writeEnumInfo(w, e);
    }

    // L11: moduleExports (vector<string>)
    w.writeU32(static_cast<uint32_t>(cr.moduleExports.size()));
    for (const auto& name : cr.moduleExports) {
        w.writeString(name);
    }

    return true;
}

bool readRegisterCompileResult(BufferReader& r, RegisterCompileResult& cr) {
    // mainChunk
    if (!readRegBytecodeChunk(r, cr.mainChunk))
        return false;

    // functionChunks
    uint32_t fcCount = r.readU32();
    if (!r.ok())
        return false;
    cr.functionChunks.clear();
    for (uint32_t i = 0; i < fcCount; ++i) {
        std::string name = r.readString();
        if (!r.ok())
            return false;
        RegBytecodeChunk chunk;
        if (!readRegBytecodeChunk(r, chunk))
            return false;
        cr.functionChunks.emplace(std::move(name), std::move(chunk));
    }

    // globalSlotCount
    cr.globalSlotCount = r.readI32();
    if (!r.ok())
        return false;

    // globalSlotNames
    uint32_t gsnCount = r.readU32();
    if (!r.ok())
        return false;
    cr.globalSlotNames.resize(gsnCount);
    for (uint32_t i = 0; i < gsnCount; ++i) {
        cr.globalSlotNames[i] = r.readString();
    }
    if (!r.ok())
        return false;

    // enumInfos
    uint32_t eiCount = r.readU32();
    if (!r.ok())
        return false;
    cr.enumInfos.resize(eiCount);
    for (uint32_t i = 0; i < eiCount; ++i) {
        if (!readEnumInfo(r, cr.enumInfos[i]))
            return false;
    }

    // L11: moduleExports
    // AUDIT-R3 P2-5 fix: 同 StackVM 路径，kFormatVersion 2 起无条件读取
    {
        uint32_t meCount = r.readU32();
        if (!r.ok())
            return false;
        cr.moduleExports.reserve(meCount);
        for (uint32_t i = 0; i < meCount; ++i) {
            cr.moduleExports.push_back(r.readString());
            if (!r.ok())
                return false;
        }
    }

    return true;
}

// ============================================================
// 缓存文件路径
// ============================================================

std::string hashToHex(uint64_t h) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

} // namespace

// ============================================================
// BytecodeCache 公开实现
// ============================================================

BytecodeCache::BytecodeCache() {
    namespace fs = std::filesystem;
    try {
        fs::path tmp = fs::temp_directory_path();
        cacheDir_ = (tmp / kCacheSubDir).string();
    } catch (...) {
        cacheDir_ = "minilang_bytecache";
    }
}

BytecodeCache::~BytecodeCache() = default;

std::optional<CompileResult> BytecodeCache::tryLoad(const CacheKey& key) {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return std::nullopt;

    uint64_t hash = fnv1a64(key.source);
    fs::path filePath = fs::path(cacheDir_) / (hashToHex(hash) + ".mbc");

    std::error_code ec;
    if (!fs::exists(filePath, ec))
        return std::nullopt;

    // 读取整个文件
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs)
        return std::nullopt;
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (fileData.size() < kHeaderSize)
        return std::nullopt;

    // 解析头部
    BufferReader hr(fileData.data(), kHeaderSize);
    char magic[4];
    hr.readBytes(magic, 4);
    uint16_t version = hr.readU16();
    uint8_t mode = hr.readU8();
    uint8_t optFlags = hr.readU8(); // AUDIT-R3 P2-4 fix: reserved 字节现承载优化开关
    uint64_t srcHash = hr.readU64();
    int64_t srcMtime = hr.readI64();
    uint32_t payloadChecksum = hr.readU32();
    uint32_t payloadSize = hr.readU32();
    if (!hr.ok())
        return std::nullopt;

    // 五重校验
    if (std::memcmp(magic, kMagic, 4) != 0)
        return std::nullopt;
    if (version != kFormatVersion)
        return std::nullopt;
    if (mode != key.compilerMode)
        return std::nullopt;
    // AUDIT-R3 P2-4 fix: 优化开关不匹配即失效（切换优化配置后旧缓存不可复用）
    if (optFlags != key.optFlags)
        return std::nullopt;
    if (srcHash != hash)
        return std::nullopt;
    if (key.mtime != 0 && srcMtime != 0 && srcMtime != key.mtime)
        return std::nullopt;

    // 校验 payload 大小
    if (fileData.size() < kHeaderSize + payloadSize)
        return std::nullopt;

    // 校验 payload checksum
    const uint8_t* payload = fileData.data() + kHeaderSize;
    uint32_t actualChecksum = fnv1a32(payload, payloadSize);
    if (actualChecksum != payloadChecksum)
        return std::nullopt;

    // 反序列化 payload
    BufferReader pr(payload, payloadSize);
    CompileResult result;
    if (!readCompileResult(pr, result))
        return std::nullopt;

    // 确保所有数据已消费(允许尾部有少量保留字节,但不允许大量未读)
    // 不强制 pos == payloadSize,以兼容未来向后兼容扩展

    return result;
}

void BytecodeCache::store(const CacheKey& key, const CompileResult& result) {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return;

    // 序列化 payload
    BufferWriter w;
    if (!writeCompileResult(w, result))
        return; // 常量池含非标量,不缓存

    const auto& payload = w.data();
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    uint32_t payloadChecksum = fnv1a32(payload.data(), payload.size());
    uint64_t srcHash = fnv1a64(key.source);

    // 构造头部
    BufferWriter header;
    header.writeBytes(kMagic, 4);
    header.writeU16(kFormatVersion);
    header.writeU8(key.compilerMode);
    header.writeU8(key.optFlags); // AUDIT-R3 P2-4 fix: reserved 字节承载优化开关
    header.writeU64(srcHash);
    header.writeI64(key.mtime);
    header.writeU32(payloadChecksum);
    header.writeU32(payloadSize);

    // 确保缓存目录存在
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);
    if (ec)
        return;

    // 写入临时文件再 rename(原子写入,避免崩溃产生半写文件)
    uint64_t hash = srcHash;
    fs::path finalPath = fs::path(cacheDir_) / (hashToHex(hash) + ".mbc");
    fs::path tmpPath = fs::path(cacheDir_) / (hashToHex(hash) + ".tmp");

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return;
        ofs.write(reinterpret_cast<const char*>(header.data().data()), header.data().size());
        ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (!ofs)
            return; // 写入失败(磁盘满/权限),静默
    }

    fs::rename(tmpPath, finalPath, ec);
    // rename 失败则删除临时文件
    if (ec) {
        std::error_code dec;
        fs::remove(tmpPath, dec);
    }
}

// ============================================================
// L17: PipelineRunner 内存级 RegisterVM 缓存
// ------------------------------------------------------------
// 与 tryLoad/store 对称，区别：
//   - magic: kRegisterMagic "MLRC"（与 StackVM "MLBC" 隔离）
//   - 扩展名: .mrc（与 .mbc 隔离）
//   - payload: writeRegisterCompileResult/readRegisterCompileResult
//   - compiler_mode 字段约定: 2=RegisterVM 路径（与文件版 storeRegisterToFile 一致）
// ============================================================

std::optional<RegisterCompileResult> BytecodeCache::tryLoadRegister(const CacheKey& key) {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return std::nullopt;

    uint64_t hash = fnv1a64(key.source);
    fs::path filePath = fs::path(cacheDir_) / (hashToHex(hash) + ".mrc");

    std::error_code ec;
    if (!fs::exists(filePath, ec))
        return std::nullopt;

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs)
        return std::nullopt;
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (fileData.size() < kHeaderSize)
        return std::nullopt;

    // 解析头部
    BufferReader hr(fileData.data(), kHeaderSize);
    char magic[4];
    hr.readBytes(magic, 4);
    uint16_t version = hr.readU16();
    uint8_t mode = hr.readU8();
    uint8_t optFlags = hr.readU8(); // AUDIT-R3 P2-4 fix
    uint64_t srcHash = hr.readU64();
    int64_t srcMtime = hr.readI64();
    uint32_t payloadChecksum = hr.readU32();
    uint32_t payloadSize = hr.readU32();
    if (!hr.ok())
        return std::nullopt;

    // 五重校验（magic 改为 kRegisterMagic）
    if (std::memcmp(magic, kRegisterMagic, 4) != 0)
        return std::nullopt;
    if (version != kFormatVersion)
        return std::nullopt;
    // L17: compilerMode 在 RegisterVM 路径下恒为 2（storeRegisterToFile 约定），
    // 但为兼容历史调用方传入 0/1，此处放宽校验——只要 source hash 匹配即可。
    // 真正隔离由 magic + 扩展名保证。
    (void)mode;
    // AUDIT-R3 P2-4 fix: 优化开关不匹配即失效
    if (optFlags != key.optFlags)
        return std::nullopt;
    if (srcHash != hash)
        return std::nullopt;
    if (key.mtime != 0 && srcMtime != 0 && srcMtime != key.mtime)
        return std::nullopt;

    if (fileData.size() < kHeaderSize + payloadSize)
        return std::nullopt;

    const uint8_t* payload = fileData.data() + kHeaderSize;
    uint32_t actualChecksum = fnv1a32(payload, payloadSize);
    if (actualChecksum != payloadChecksum)
        return std::nullopt;

    BufferReader pr(payload, payloadSize);
    RegisterCompileResult result;
    if (!readRegisterCompileResult(pr, result))
        return std::nullopt;

    return result;
}

void BytecodeCache::storeRegister(const CacheKey& key, const RegisterCompileResult& result) {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return;

    // 序列化 payload
    BufferWriter w;
    if (!writeRegisterCompileResult(w, result))
        return; // 常量池含非标量,不缓存

    const auto& payload = w.data();
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    uint32_t payloadChecksum = fnv1a32(payload.data(), payload.size());
    uint64_t srcHash = fnv1a64(key.source);

    // 构造头部（使用 kRegisterMagic "MLRC"）
    BufferWriter header;
    header.writeBytes(kRegisterMagic, 4);
    header.writeU16(kFormatVersion);
    // compiler_mode: RegisterVM 路径固定 2（与 storeRegisterToFile 一致，供调试识别）
    header.writeU8(2);
    header.writeU8(key.optFlags); // AUDIT-R3 P2-4 fix
    header.writeU64(srcHash);
    header.writeI64(key.mtime);
    header.writeU32(payloadChecksum);
    header.writeU32(payloadSize);

    // 确保缓存目录存在
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);
    if (ec)
        return;

    // 原子写入：先写 .tmp 再 rename
    uint64_t hash = srcHash;
    fs::path finalPath = fs::path(cacheDir_) / (hashToHex(hash) + ".mrc");
    fs::path tmpPath = fs::path(cacheDir_) / (hashToHex(hash) + ".tmp");

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return;
        ofs.write(reinterpret_cast<const char*>(header.data().data()), header.data().size());
        ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (!ofs)
            return;
    }

    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        std::error_code dec;
        fs::remove(tmpPath, dec);
    }
}

void BytecodeCache::clear() {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return;
    std::error_code ec;
    if (!fs::exists(cacheDir_, ec))
        return;
    for (const auto& entry : fs::directory_iterator(cacheDir_, ec)) {
        if (!entry.is_regular_file())
            continue;
        // L17: 同时清理 StackVM .mbc 与 RegisterVM .mrc
        const auto ext = entry.path().extension();
        if (ext == ".mbc" || ext == ".mrc") {
            std::error_code rec;
            fs::remove(entry.path(), rec);
        }
    }
}

size_t BytecodeCache::entryCount() const {
    namespace fs = std::filesystem;
    if (cacheDir_.empty())
        return 0;
    std::error_code ec;
    if (!fs::exists(cacheDir_, ec))
        return 0;
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(cacheDir_, ec)) {
        if (!entry.is_regular_file())
            continue;
        // L17: 同时计数 StackVM .mbc 与 RegisterVM .mrc
        const auto ext = entry.path().extension();
        if (ext == ".mbc" || ext == ".mrc") {
            ++count;
        }
    }
    return count;
}

// ============================================================
// P2-11 预编译模块：文件级 API（.minic 文件读写）
// ============================================================

std::optional<CompileResult> BytecodeCache::tryLoadFromFile(const std::string& filePath,
                                                            const std::string& sourcePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(filePath, ec))
        return std::nullopt;

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs)
        return std::nullopt;
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (fileData.size() < kHeaderSize)
        return std::nullopt;

    // 解析头部（与 tryLoad 相同的 32B 头部格式）
    BufferReader hr(fileData.data(), kHeaderSize);
    char magic[4];
    hr.readBytes(magic, 4);
    uint16_t version = hr.readU16();
    /*uint8_t mode =*/hr.readU8();
    /*uint8_t reserved =*/hr.readU8();
    uint64_t embeddedSrcHash = hr.readU64();
    int64_t embeddedSrcMtime = hr.readI64();
    uint32_t payloadChecksum = hr.readU32();
    uint32_t payloadSize = hr.readU32();
    if (!hr.ok())
        return std::nullopt;

    // 三重校验（magic/version/checksum）；不校验 source hash（文件路径即 key）
    if (std::memcmp(magic, kMagic, 4) != 0)
        return std::nullopt;
    if (version != kFormatVersion)
        return std::nullopt;
    if (fileData.size() < kHeaderSize + payloadSize)
        return std::nullopt;

    const uint8_t* payload = fileData.data() + kHeaderSize;
    uint32_t actualChecksum = fnv1a32(payload, payloadSize);
    if (actualChecksum != payloadChecksum)
        return std::nullopt;

    // L12: 源码失效校验
    // 仅当 .minic 头部嵌入了 source mtime（!=0）且调用方提供 sourcePath 时校验。
    // 校验 mtime + content hash 任一不匹配 → 返回 nullopt（.minic 失效）
    if (embeddedSrcMtime != 0 && !sourcePath.empty()) {
        SourceMeta current = computeSourceMeta(sourcePath);
        if (current.mtimeMs == 0)
            return std::nullopt; // 源码文件不存在或读取失败 → 视为失效
        if (current.mtimeMs != embeddedSrcMtime)
            return std::nullopt; // mtime 不匹配 → 源码已修改
        if (current.contentHash != embeddedSrcHash)
            return std::nullopt; // content hash 不匹配 → 内容已变更（防御性双校验）
    }

    // 反序列化 payload
    BufferReader pr(payload, payloadSize);
    CompileResult result;
    if (!readCompileResult(pr, result))
        return std::nullopt;

    return result;
}

bool BytecodeCache::storeToFile(const std::string& filePath, const CompileResult& result, const std::string& modulePath,
                                const std::string& sourcePath) {
    namespace fs = std::filesystem;

    // 序列化 payload
    BufferWriter w;
    if (!writeCompileResult(w, result))
        return false; // 常量池含非标量

    const auto& payload = w.data();
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    uint32_t payloadChecksum = fnv1a32(payload.data(), payload.size());

    // L12: 计算 source_hash 与 source_mtime
    // - sourcePath 非空且存在: 写入源码 mtime + content hash,供 tryLoadFromFile 校验
    // - sourcePath 为空或不存在: 退化到模块路径 hash（供调试识别，mtime=0 跳过校验）
    int64_t embeddedMtime = 0;
    uint64_t embeddedHash = 0;
    if (!sourcePath.empty()) {
        SourceMeta meta = computeSourceMeta(sourcePath);
        if (meta.mtimeMs != 0) {
            embeddedMtime = meta.mtimeMs;
            embeddedHash = meta.contentHash;
        }
    }
    if (embeddedMtime == 0) {
        // 无 sourcePath 或源码读取失败：用 modulePath hash 作调试标识，mtime=0 跳过校验
        embeddedHash = modulePath.empty() ? 0 : fnv1a64(modulePath);
    }

    // 构造头部
    BufferWriter header;
    header.writeBytes(kMagic, 4);
    header.writeU16(kFormatVersion);
    header.writeU8(0); // compiler_mode（模块预编译固定 0）
    header.writeU8(0); // reserved
    header.writeU64(embeddedHash);
    header.writeI64(embeddedMtime);
    header.writeU32(payloadChecksum);
    header.writeU32(payloadSize);

    // 确保目标目录存在
    fs::path destPath(filePath);
    if (destPath.has_parent_path()) {
        std::error_code mkEc;
        fs::create_directories(destPath.parent_path(), mkEc);
    }

    // 原子写入：先写 .tmp 再 rename
    fs::path tmpPath = destPath;
    tmpPath += ".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        ofs.write(reinterpret_cast<const char*>(header.data().data()), header.data().size());
        ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (!ofs)
            return false;
    }

    std::error_code ec;
    fs::rename(tmpPath, destPath, ec);
    if (ec) {
        std::error_code dec;
        fs::remove(tmpPath, dec);
        return false;
    }
    return true;
}

// ============================================================
// L11 RegisterVM 预编译模块：文件级 API（.minic 文件读写，magic "MLRC"）
// ============================================================

std::optional<RegisterCompileResult> BytecodeCache::tryLoadRegisterFromFile(const std::string& filePath,
                                                                            const std::string& sourcePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(filePath, ec))
        return std::nullopt;

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs)
        return std::nullopt;
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (fileData.size() < kHeaderSize)
        return std::nullopt;

    // 解析头部（与 StackVM 相同的 32B 头部布局，仅 magic 不同）
    BufferReader hr(fileData.data(), kHeaderSize);
    char magic[4];
    hr.readBytes(magic, 4);
    uint16_t version = hr.readU16();
    /*uint8_t mode =*/hr.readU8();
    /*uint8_t reserved =*/hr.readU8();
    uint64_t embeddedSrcHash = hr.readU64();
    int64_t embeddedSrcMtime = hr.readI64();
    uint32_t payloadChecksum = hr.readU32();
    uint32_t payloadSize = hr.readU32();
    if (!hr.ok())
        return std::nullopt;

    // 三重校验（magic/version/checksum）；magic 必须是 "MLRC"
    if (std::memcmp(magic, kRegisterMagic, 4) != 0)
        return std::nullopt;
    if (version != kFormatVersion)
        return std::nullopt;
    if (fileData.size() < kHeaderSize + payloadSize)
        return std::nullopt;

    const uint8_t* payload = fileData.data() + kHeaderSize;
    uint32_t actualChecksum = fnv1a32(payload, payloadSize);
    if (actualChecksum != payloadChecksum)
        return std::nullopt;

    // L12: 源码失效校验（与 StackVM 路径一致）
    if (embeddedSrcMtime != 0 && !sourcePath.empty()) {
        SourceMeta current = computeSourceMeta(sourcePath);
        if (current.mtimeMs == 0)
            return std::nullopt;
        if (current.mtimeMs != embeddedSrcMtime)
            return std::nullopt;
        if (current.contentHash != embeddedSrcHash)
            return std::nullopt;
    }

    // 反序列化 payload
    BufferReader pr(payload, payloadSize);
    RegisterCompileResult result;
    if (!readRegisterCompileResult(pr, result))
        return std::nullopt;

    return result;
}

bool BytecodeCache::storeRegisterToFile(const std::string& filePath, const RegisterCompileResult& result,
                                        const std::string& modulePath, const std::string& sourcePath) {
    namespace fs = std::filesystem;

    // 序列化 payload
    BufferWriter w;
    if (!writeRegisterCompileResult(w, result))
        return false; // 常量池含非标量

    const auto& payload = w.data();
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    uint32_t payloadChecksum = fnv1a32(payload.data(), payload.size());

    // L12: 计算 source_hash 与 source_mtime（与 StackVM 路径一致）
    int64_t embeddedMtime = 0;
    uint64_t embeddedHash = 0;
    if (!sourcePath.empty()) {
        SourceMeta meta = computeSourceMeta(sourcePath);
        if (meta.mtimeMs != 0) {
            embeddedMtime = meta.mtimeMs;
            embeddedHash = meta.contentHash;
        }
    }
    if (embeddedMtime == 0) {
        embeddedHash = modulePath.empty() ? 0 : fnv1a64(modulePath);
    }

    // 构造头部（使用 kRegisterMagic "MLRC"）
    BufferWriter header;
    header.writeBytes(kRegisterMagic, 4);
    header.writeU16(kFormatVersion);
    header.writeU8(2); // compiler_mode（2=RegisterVM 路径，供调试识别）
    header.writeU8(0); // reserved
    header.writeU64(embeddedHash);
    header.writeI64(embeddedMtime);
    header.writeU32(payloadChecksum);
    header.writeU32(payloadSize);

    // 确保目标目录存在
    fs::path destPath(filePath);
    if (destPath.has_parent_path()) {
        std::error_code mkEc;
        fs::create_directories(destPath.parent_path(), mkEc);
    }

    // 原子写入：先写 .tmp 再 rename
    fs::path tmpPath = destPath;
    tmpPath += ".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        ofs.write(reinterpret_cast<const char*>(header.data().data()), header.data().size());
        ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (!ofs)
            return false;
    }

    std::error_code ec;
    fs::rename(tmpPath, destPath, ec);
    if (ec) {
        std::error_code dec;
        fs::remove(tmpPath, dec);
        return false;
    }
    return true;
}
