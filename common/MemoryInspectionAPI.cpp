// ============================================================
// MemoryInspectionAPI.cpp — 内存检视只读 API 实现（ARCH-10）
// ------------------------------------------------------------
// 封装 NaNBox / RefCounted / GcManager 内部访问，GUI 面板通过此 API
// 获取只读快照而无需直接依赖 interpreter/ 内部头文件。
// ============================================================

#include "common/MemoryInspectionAPI.h"

#include "interpreter/GcManager.h"
#include "interpreter/NaNBox.h"
#include "interpreter/RefCounted.h"
#include "interpreter/Value.h"

#include <iomanip>
#include <sstream>

// ============================================================
// 内部辅助
// ============================================================

namespace {

std::string bitsToHex(uint64_t bits) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
    return os.str();
}

std::string bitsToBinary(uint64_t bits) {
    std::string s(64, '0');
    for (int i = 0; i < 64; ++i) {
        if (bits & (1ULL << (63 - i)))
            s[i] = '1';
    }
    return s;
}

std::string ptrToHex(const void* p) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16) << reinterpret_cast<uintptr_t>(p);
    return os.str();
}

const char* tagName(int tag) {
    switch (tag) {
    case 0:
        return "FLOAT";
    case 1:
        return "INT";
    case 2:
        return "BOOL";
    case 3:
        return "NUL";
    case 4:
        return "POINTER";
    default:
        return "UNKNOWN";
    }
}

const char* valueTypeName(int t) {
    switch (t) {
    case 0:
        return "VAL_INT";
    case 1:
        return "VAL_FLOAT";
    case 2:
        return "VAL_BOOL";
    case 3:
        return "VAL_NULL";
    case 4:
        return "VAL_STRING";
    case 5:
        return "VAL_ARRAY";
    case 6:
        return "VAL_DICT";
    case 7:
        return "VAL_INSTANCE";
    case 8:
        return "VAL_CLOSURE";
    case 9:
        return "VAL_TUPLE";
    case 10:
        return "VAL_ENUM_VARIANT";
    case 11:
        return "VAL_CHANNEL";
    case 12:
        return "VAL_MUTEX";
    case 13:
        return "VAL_RWLOCK";
    case 14:
        return "VAL_THREAD";
    case 15:
        return "VAL_COROUTINE";
    default:
        return "<unknown>";
    }
}

const char* heapStructNameForType(int t) {
    switch (t) {
    case 0:
        return "BoxedIntData";
    case 4:
        return "StringData";
    case 5:
        return "ArrayData";
    case 6:
        return "DictData";
    case 7:
        return "InstanceData";
    case 8:
        return "ClosureData";
    case 9:
        return "TupleData";
    case 10:
        return "EnumVariantData";
    case 11:
        return "ChannelData";
    case 12:
        return "MutexData";
    case 13:
        return "RwLockData";
    case 14:
        return "ThreadData";
    case 15:
        return "CoroutineData";
    case 1:
    case 2:
    case 3:
        return "<scalar-in-heap>";
    default:
        return "<unknown>";
    }
}

} // namespace

// ============================================================
// NaN-box 检视
// ============================================================

NaNBoxSnapshot MemoryInspectionAPI::inspectValue(const Value& v) {
    NaNBoxSnapshot snap;
    uint64_t bits = 0;
    switch (v.getType()) {
    case ValueType::VAL_INT:
        // AUDIT-P1 fix: BoxedIntData（超大整数装箱为堆对象）的 type==VAL_INT
        // 但 isPointer()==true，此时 intVal() 返回原始 int64（可能超出 int48 范围），
        // 调用 NaNBox::fromInt 会触发 canEncodeInt 失败 → std::abort() 崩溃。
        // 对 BoxedIntData 显示堆指针占位符，与堆类型 default 分支一致。
        if (v.isPointer()) {
            bits = 0x7ffbULL << 48; // PTR_TAG_BASE 占位符
            snap.tagName = "POINTER";
            snap.tagValue = 4;
        } else {
            bits = NaNBox::fromInt(v.intVal()).rawBits();
            snap.tagName = "INT";
            snap.tagValue = 1;
        }
        break;
    case ValueType::VAL_FLOAT:
        bits = NaNBox::fromFloat(v.floatVal()).rawBits();
        snap.tagName = "FLOAT";
        snap.tagValue = 0;
        break;
    case ValueType::VAL_BOOL:
        bits = NaNBox::fromBool(v.boolVal()).rawBits();
        snap.tagName = "BOOL";
        snap.tagValue = 2;
        break;
    case ValueType::VAL_NULL:
        bits = NaNBox::null().rawBits();
        snap.tagName = "NUL";
        snap.tagValue = 3;
        break;
    default:
        // 堆指针类型：指针值不固定，教学展示用 PTR_TAG_BASE 占位符
        bits = 0x7ffbULL << 48;
        snap.tagName = "POINTER";
        snap.tagValue = 4;
        break;
    }
    snap.bits = bits;
    snap.bitsHex = bitsToHex(bits);
    snap.bitsBinary = bitsToBinary(bits);
    return snap;
}

NaNBoxSnapshot MemoryInspectionAPI::inspectBits(uint64_t bits) {
    NaNBoxSnapshot snap;
    snap.bits = bits;
    snap.bitsHex = bitsToHex(bits);
    snap.bitsBinary = bitsToBinary(bits);
    // 通过 NaNBox 解码获取 tag
    NaNBox box = NaNBox::fromBits(bits);
    int t = static_cast<int>(box.tag());
    snap.tagValue = t;
    snap.tagName = tagName(t);
    return snap;
}

// ============================================================
// NaN-box 编码（供 MemoryModelPanel 交互式编码器使用）
// ============================================================

bool MemoryInspectionAPI::canEncodeInt(int64_t v) {
    return NaNBox::canEncodeInt(v);
}

uint64_t MemoryInspectionAPI::encodeInt(int64_t v) {
    return NaNBox::fromInt(v).rawBits();
}

uint64_t MemoryInspectionAPI::encodeFloat(double v) {
    return NaNBox::fromFloat(v).rawBits();
}

uint64_t MemoryInspectionAPI::encodeBool(bool v) {
    return NaNBox::fromBool(v).rawBits();
}

uint64_t MemoryInspectionAPI::encodeNull() {
    return NaNBox::null().rawBits();
}

// ============================================================
// NaN-box 解码（供 MemoryModelPanel 自定义编码器 demotion 路径使用）
// ============================================================

double MemoryInspectionAPI::decodeFloat(uint64_t bits) {
    return NaNBox::fromBits(bits).asFloat();
}

int64_t MemoryInspectionAPI::decodeInt(uint64_t bits) {
    return NaNBox::fromBits(bits).asInt();
}

// ============================================================
// 堆对象检视
// ============================================================

HeapObjectSnapshot MemoryInspectionAPI::inspectHeap(const Value& v) {
    HeapObjectSnapshot snap;
    if (!v.isPointer()) {
        snap.isPointer = false;
        snap.valueType = static_cast<int>(v.getType());
        snap.valueTypeName = valueTypeName(snap.valueType);
        snap.structName = "<scalar>";
        return snap;
    }
    RefCounted* p = v.asPointer();
    snap.isPointer = true;
    snap.addressHex = ptrToHex(p);
    if (!p) {
        snap.structName = "<null-ptr>";
        return snap;
    }
    int t = static_cast<int>(p->type);
    snap.valueType = t;
    snap.valueTypeName = valueTypeName(t);
    snap.structName = heapStructNameForType(t);
    snap.refCount = p->useCount();
    // 计算字段数/元素数
    switch (p->type) {
    case ValueType::VAL_INT:
        snap.fieldCount = 1; // BoxedIntData.value
        break;
    case ValueType::VAL_STRING:
        snap.fieldCount = static_cast<size_t>(v.codepointCount());
        break;
    case ValueType::VAL_ARRAY:
        snap.fieldCount = v.arrayVal().size();
        break;
    case ValueType::VAL_DICT:
        snap.fieldCount = v.dictVal().size();
        break;
    case ValueType::VAL_INSTANCE:
        snap.fieldCount = v.fields().size();
        break;
    case ValueType::VAL_CLOSURE:
        snap.fieldCount = v.capturedVars().size();
        break;
    case ValueType::VAL_TUPLE:
        snap.fieldCount = v.tupleVal().size();
        break;
    case ValueType::VAL_ENUM_VARIANT:
        snap.fieldCount = v.enumVariantFields().size();
        break;
    case ValueType::VAL_CHANNEL:
    case ValueType::VAL_MUTEX:
    case ValueType::VAL_RWLOCK:
    case ValueType::VAL_THREAD:
    case ValueType::VAL_COROUTINE:
        // 并发原语/协程句柄：底层资源经 shared_ptr 封装，无可枚举字段
        snap.fieldCount = 0;
        break;
    case ValueType::VAL_NULL:
    case ValueType::VAL_FLOAT:
    case ValueType::VAL_BOOL:
        snap.fieldCount = 0;
        break;
    }
    return snap;
}

// ============================================================
// GC 统计与控制
// ============================================================

GcStatsSnapshot MemoryInspectionAPI::getGcStats() {
    auto& gc = GcManager::instance();
    GcStatsSnapshot snap;
    snap.trackedCount = gc.trackedCount();
    snap.totalGcCount = gc.totalGcCount();
    snap.lastMarkedCount = gc.lastMarkedCount();
    snap.lastCollectedCount = gc.lastCollectedCount();
    snap.mode = gc.gcMode();
    snap.phase = gc.currentPhase();
    snap.allocationsSinceLastGc = gc.allocationsSinceLastGc();
    snap.allocationThreshold = gc.gcAllocationThreshold();
    return snap;
}

void MemoryInspectionAPI::resetGc() {
    GcManager::instance().reset();
}

void MemoryInspectionAPI::setGcMode(GcMode mode) {
    GcManager::instance().setGcMode(mode);
}

void MemoryInspectionAPI::collectCycle(const std::vector<const void*>& roots) {
    GcManager::instance().collectCycle(roots);
}

void MemoryInspectionAPI::setGcAllocationThreshold(size_t threshold) {
    GcManager::instance().setGcAllocationThreshold(threshold);
}

void MemoryInspectionAPI::setGcTriggerCallback(std::function<void()> cb) {
    GcManager::instance().setGcTriggerCallback(std::move(cb));
}

// ============================================================
// 枚举转字符串工具
// ============================================================

std::string gcPhaseToString(GcPhase phase) {
    switch (phase) {
    case GcPhase::Idle:
        return "Idle";
    case GcPhase::Marking:
        return "Marking";
    case GcPhase::Sweeping:
        return "Sweeping";
    case GcPhase::Finalizing:
        return "Finalizing";
    }
    return "Unknown";
}

std::string gcModeToString(GcMode mode) {
    switch (mode) {
    case GcMode::RefCountOnly:
        return "RefCountOnly";
    case GcMode::RefCountWithCycleGc:
        return "RefCountWithCycleGc";
    case GcMode::GcOnly:
        return "GcOnly";
    }
    return "Unknown";
}
