#pragma once

#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include "compiler/Bytecode.h"
#include "interpreter/Value.h"
#include "Diagnostic.h"

// ============================================================
// VM 虚拟机（简单栈机）
// ============================================================

/// P13 fix: 小缓冲区优化的参数容器，避免方法调用时的堆分配
/// 对于 argCount <= N 使用栈上内联存储，超出时回退到 vector
template<typename T, size_t N = 8>
class SmallArgs {
    T inline_[N];
    std::vector<T> heap_;
    size_t sz_ = 0;
public:
    SmallArgs() = default;
    explicit SmallArgs(size_t count) : sz_(count) {
        if (count > N) heap_.resize(count);
    }
    size_t size() const { return sz_; }
    bool empty() const { return sz_ == 0; }
    T& operator[](size_t i) { return (sz_ <= N) ? inline_[i] : heap_[i]; }
    const T& operator[](size_t i) const { return (sz_ <= N) ? inline_[i] : heap_[i]; }
    T* begin() { return (sz_ <= N) ? inline_ : heap_.data(); }
    T* end() { return begin() + sz_; }
    const T* begin() const { return (sz_ <= N) ? inline_ : heap_.data(); }
    const T* end() const { return begin() + sz_; }
    void push_back(const T& v) {
        if (sz_ < N) { inline_[sz_++] = v; }
        else { if (sz_ == N) { heap_.assign(inline_, inline_ + N); } heap_.push_back(v); sz_++; }
    }
};

/// 虚拟机执行结果
enum class VMResult {
    VM_OK,
    VM_RUNTIME_ERROR,
    VM_STACK_OVERFLOW
};

/// VM 执行模式
enum class VMExecMode {
    VM_MODE_NONE,       // 未初始化
    VM_MODE_RUN,        // 全速运行
    VM_MODE_STEP        // 单步模式
};

/// 每条指令执行后的状态信息（用于调试/可视化）
struct VMStepInfo {
    size_t ip;                              // 当前指令指针
    OpCode opcode;                          // 当前操作码
};

/// VM 调用帧
struct VMCallFrame {
    const BytecodeChunk* chunk = nullptr;   // 当前执行的字节码块
    size_t ip = 0;                          // 当前帧的指令指针
    size_t returnIp = 0;                    // 返回后的 ip
    size_t basePointer = 0;                 // 帧基指针（栈中参数起始位置）
    std::string functionName;               // 函数名
    bool isMethodCall = false;              // 是否为方法调用（需要 writeBack）
    bool isInitCall = false;                // 是否为 init 构造函数调用（返回 this 而非 null）
    std::string receiverVarName;            // 方法调用时，接收者的全局变量名（用于 writeBack 到 globals_）
    int receiverLocalSlot = -1;             // 方法调用时，接收者在调用者帧中的局部变量槽号（-1=非局部变量）
    bool fieldsModified = false;            // VM fix: 方法内是否修改了字段（用于跳过只读方法的字段同步）
    // VM-05/06: 闭包 upvalue 列表
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
};

/// VM 类信息（用于构造函数调用）
struct VMClassInfo {
    std::string name;                                  // 类名
    std::string superClassName;                        // 父类名（空表示无父类）
    std::vector<std::string> fieldOrder;               // 字段声明顺序（含继承字段）
    std::unordered_map<std::string, Value> fieldDefaults; // 字段默认值（含继承字段）
    // P4 fix: 方法解析缓存（methodName -> chunk 指针），避免每次方法调用都拼接字符串+查继承链
    mutable std::unordered_map<std::string, const BytecodeChunk*> methodCache;
};

/// 简单栈式虚拟机
class VM {
public:
    VM();

    /// 执行编译结果（一次性全部执行，全速模式）
    VMResult execute(const CompileResult& result);

    /// 初始化执行环境但不运行（单步模式前置操作）
    void initExecution(const CompileResult& result);

    /// 单步执行一条指令（需先调用 initExecution）
    VMResult stepOnce();

    /// 是否执行完毕（无帧可执行）
    bool isFinished() const;

    /// 是否已初始化（initExecution 已调用）
    bool isInitialized() const;

    /// 重置 VM 状态（清理栈/帧/变量）
    void resetState();

    /// 设置输出回调
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 设置输入回调（用于 input() 函数）
    /// 回调接收提示字符串，返回用户输入的字符串
    void setInputCallback(std::function<std::string(const std::string&)> callback);

    /// 设置指令级步进回调（每条指令执行后调用）
    void setStepCallback(std::function<void(const VMStepInfo&)> callback);

    /// 设置是否启用步进回调（默认关闭，避免性能开销）
    void setStepCallbackEnabled(bool enabled);

    /// 是否发生过运行时错误
    bool hasError() const;

    /// 获取最后的运行时错误
    std::string getLastError() const;

    /// 获取最后错误的源码行号（1-based，0=无位置信息）
    int getLastErrorLine() const;

    /// 获取诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    /// 获取诊断信息（简短访问器）
    const DiagnosticBag& diagnostics() const { return diagnostics_; }

    /// 获取栈内容（用于调试，拷贝）
    std::vector<Value> getStack() const;

    /// 获取栈的常量引用（零拷贝，调试用）
    const std::vector<Value>& getStackRef() const { return stack_; }

    /// 获取全局变量表（合并 slot-based + map-based，调试/测试用）
    std::unordered_map<std::string, Value> getGlobals() const {
        std::unordered_map<std::string, Value> result;
        // Slot-based globals first
        for (size_t i = 0; i < globalSlots_.size() && i < globalSlotNames_.size(); ++i) {
            if (!globalSlots_[i].isNull()) {
                result[globalSlotNames_[i]] = globalSlots_[i];
            }
        }
        // Map-based globals overlay (runtime-defined, class markers, etc.)
        for (const auto& kv : globals_) {
            result[kv.first] = kv.second;
        }
        return result;
    }

    /// 向后兼容：旧版 getGlobalsRef 改为调用 getGlobals
    std::unordered_map<std::string, Value> getGlobalsRef() const { return getGlobals(); }

    /// 获取当前帧的 IP（用于单步调试 UI 高亮）
    size_t getCurrentIP() const;

    /// 获取当前帧的指令操作码（用于单步调试 UI 显示）
    OpCode getCurrentOpCode() const;

    /// 获取当前帧的源码行号（从当前 chunk 的 lines 数组获取）
    int getCurrentLine() const;

    /// 获取当前帧的 chunk 名称（"main" 或函数名）
    std::string getCurrentChunkName() const;

private:
    std::vector<Value> stack_;                     // 操作数栈
    std::unordered_map<std::string, Value> globals_; // 全局变量表（runtime-defined fallback）
    // A2: 全局变量整数槽位存储（编译期分配，vector 直接访问）
    std::vector<Value> globalSlots_;               // slot-indexed global storage
    std::vector<std::string> globalSlotNames_;     // parallel: slot -> name (debug)
    std::unordered_map<std::string, int> globalNameToSlot_; // name -> slot (runtime lookup)
    std::vector<VMCallFrame> frames_;              // 调用帧栈
    BytecodeChunk mainChunk_;                       // 主 chunk 副本（VM 自持，避免悬空指针）
    std::unordered_map<std::string, BytecodeChunk> functionChunks_; // 函数字节码

    // P3 fix: 函数调用内联缓存（按常量池字符串指针匹配，避免每次 hash 查找）
    static constexpr int CALL_CACHE_SIZE = 8;
    struct CallCacheEntry {
        const std::string* namePtr = nullptr;
        const BytecodeChunk* chunkPtr = nullptr;
    };
    CallCacheEntry callCache_[CALL_CACHE_SIZE] = {};
    int callCacheNextSlot_ = 0;  // P3: round-robin 替换指针

    // P2 fix: 全局变量内联缓存
    static constexpr int GLOBAL_CACHE_SIZE = 8;
    struct GlobalCacheEntry {
        const std::string* namePtr = nullptr;
        Value* valuePtr = nullptr;      // 指向 globals_ 中的 Value（rehash 后失效）
        size_t generation = 0;          // globals_.bucket_count() 快照（检测 rehash）
    };
    GlobalCacheEntry globalCache_[GLOBAL_CACHE_SIZE] = {};
    int globalCacheNextSlot_ = 0;
    // P7: ASCII 字符串索引缓存（记住上次检查过的字符串，避免循环中重复 O(n) 扫描）
    // P1 fix: 改为缓存字符串内容而非裸指针，消除 use-after-free 风险
    std::string lastAsciiStrContent_;
    bool lastAsciiStrIsAscii_ = false;
    std::unordered_map<std::string, VMClassInfo> classInfo_;        // 类信息注册表
    // VM-05/06: 闭包支持
    std::vector<std::shared_ptr<VMUpvalue>> openUpvalues_;   // 当前所有 open 的 upvalue（函数返回时关闭）
    std::unordered_map<std::string, Value> functionClosures_;       // 函数名→闭包值（含 upvalue 绑定）
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::function<std::string(const std::string&)> inputCallback_; // 输入回调（input() 函数）
    std::function<void(const VMStepInfo&)> stepCallback_;    // 步进回调
    bool stepCallbackEnabled_ = false;              // 是否启用步进回调
    bool initialized_ = false;                      // 是否已初始化执行环境
    std::string lastError_;                         // 最近一次运行时错误
    int lastErrorLine_ = 0;                          // 最近一次运行时错误的源码行号（1-based，0=无位置）
    // P1 fix: mutable 允许 const peek() 在栈下溢时设置错误标志
    mutable bool hasError_ = false;                 // 运行时错误标志（用于快速检测）
    DiagnosticBag diagnostics_;                     // 诊断收集器
    Value lastMutatedReceiver_;                     // 变异方法调用后暂存修改后的接收者对象（用于嵌套访问写回）
    std::vector<std::string> pendingFieldOrder_;    // M3: OP_INIT_FIELD 执行期间记录的字段声明顺序

    // F11: 异常处理
    struct TryHandler {
        size_t catchIp;       // catch 块的 IP
        size_t stackBase;     // try 开始时的栈大小（catch 时恢复）
        size_t frameIndex;    // 所属调用帧索引
    };
    std::vector<TryHandler> tryStack_;              // try 处理器栈
    static constexpr size_t MAX_STACK_SIZE = 1024;  // 栈最大深度
    static constexpr size_t MAX_FRAMES = 256;       // 调用帧最大深度
    // S-02 fix: 总指令执行预算，防止 while(true){} 字节码导致 DoS
    // P0-13 fix: 从 5 亿降至 5000 万，将单次执行 CPU 占用从 3-5 秒降至 ~0.5 秒
    // P1 fix: 从 5000 万降至 4000 万，确保紧凑循环（~4 指令/次）不超过 1000 万迭代上限
    static constexpr int64_t MAX_INSTRUCTIONS = 40000000;  // 4000 万条指令（≤1000 万次循环迭代）
    // P1 fix: stepOnce 累计指令计数器，防止通过循环调用 stepOnce 绕过 DoS 防护
    int64_t stepInstructionCount_ = 0;
    // 继承链最大深度（与 Interpreter 的 MAX_INHERITANCE_DEPTH 对齐）
    static constexpr int MAX_INHERITANCE_DEPTH = 64;

    /// 栈操作
    void push(const Value& val);
    void push(Value&& val);
    Value pop();
    const Value& peek(size_t distance = 0) const;

    /// 运行时错误
    VMResult runtimeError(const std::string& msg);

    /// F11: 抛出异常，搜索 try 处理器或跨帧传播
    VMResult throwException(Value thrownValue);

    /// F11-fix: 关闭指向 [fromSlot, stack_.size()) 范围内栈槽的 open upvalues
    /// 用于异常展开和帧弹出时防止悬垂指针
    void closeUpvaluesFrom(size_t fromSlot);

    /// F10-fix: 为 init 方法填充缺失的默认参数，返回 true 表示成功
    /// argCount 会被更新为填充后的参数数量，默认值追加到 defaults 向量
    bool fillDefaultArgs(const BytecodeChunk& chunk, uint8_t& argCount,
                         const std::string& funName, std::vector<Value>& defaults);

    /// 数值二元运算（枚举分发）
    VMResult numericOp(int opType);

    /// 比较运算结果写回（消除 6 个比较运算符的重复代码）
    /// ip 按引用传入，结果写入后自动 +1（所有比较指令均为 1 字节）
    VMResult pushCompareResult(bool result, size_t& ip, OpCode opcode);

    /// 写回变异方法调用后的接收者（统一数组/字典/实例三处写回逻辑）
    /// receiverVarIdx: 接收者的全局变量索引（0xFFFF 表示无）
    /// receiverLocalSlotByte: 接收者的本地槽字节（0xFF 表示无）
    /// mutatedObj: 被修改的对象引用（将被 std::move）
    /// fieldsModified: 是否修改了字段（影响实例字段同步）
    VMResult writeBackReceiver(uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                               Value& mutatedObj, bool fieldsModified);

    /// 有序比较：类型检查 + 比较 + 结果写回（<, >, <=, >= 共用模板）
    template<typename Cmp>
    VMResult orderedCompare(Cmp cmp, size_t& ip, OpCode opcode) {
        if (stack_.size() < 2) return runtimeError("栈下溢：比较运算需要两个操作数");
        const Value& right = stack_.back();
        const Value& left = stack_[stack_.size() - 2];
        // V2 fix: 支持字符串字典序比较，与解释器 M4 fix 一致
        if (left.isString() && right.isString()) {
            return pushCompareResult(cmp(left, right), ip, opcode);
        }
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值或字符串类型");
        return pushCompareResult(cmp(left, right), ip, opcode);
    }

    /// 内建方法名枚举（消除运行时字符串比较）
    enum class BuiltinMethod {
        // 数组方法
        ARR_PUSH, ARR_POP, ARR_LEN, ARR_REMOVE, ARR_CONTAINS, ARR_JOIN,
        // 字典方法
        DICT_LEN, DICT_KEYS, DICT_VALUES, DICT_HAS, DICT_REMOVE, DICT_GET,
        // 字符串方法
        STR_LEN, STR_UPPER, STR_LOWER, STR_CONTAINS, STR_STARTS_WITH,
        STR_ENDS_WITH, STR_REPLACE, STR_SUBSTR, STR_INDEX_OF,
        STR_SPLIT, STR_TRIM,
        // 未知
        UNKNOWN
    };

    /// 将方法名分类为枚举（单次哈希，后续 switch 分发）
    static BuiltinMethod classifyBuiltinMethod(const std::string& name);

    // ---- B7 fix: 内建方法分发（从 executeCallOps 提取，降低圈复杂度）----
    /// 数组内建方法分发。返回 VM_OK 表示已处理（caller 应 break），VM_RUNTIME_ERROR 表示出错。
    VMResult dispatchArrayBuiltin(const Value& obj, BuiltinMethod method,
                                   const std::string& methodName, uint8_t argCount,
                                   uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                                   size_t& ip, OpCode op, int instrLen);
    /// 字典内建方法分发。语义同上。
    VMResult dispatchDictBuiltin(const Value& obj, BuiltinMethod method,
                                  const std::string& methodName, uint8_t argCount,
                                  uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                                  size_t& ip, OpCode op, int instrLen);
    /// 字符串内建方法分发（全部非变异，无需 writeBack 参数）。语义同上。
    VMResult dispatchStringBuiltin(const Value& obj, BuiltinMethod method,
                                    const std::string& methodName, uint8_t argCount,
                                    size_t& ip, OpCode op, int instrLen);

    /// 通知步进回调（内联：禁用时直接返回，避免函数调用开销）
    void notifyStep(size_t ip, OpCode opcode) {
        if (!stepCallbackEnabled_) return;
        VMStepInfo info;
        info.ip = ip;
        info.opcode = opcode;
        stepCallback_(info);
    }

    /// 获取当前帧
    VMCallFrame& currentFrame();

    /// 获取当前 chunk
    const BytecodeChunk& currentChunk();

    /// 沿继承链查找方法 chunk（返回 nullptr 表示未找到）
    /// 先在 className 对应类查 methodName，未命中则查 superClass，递归到根。
    const BytecodeChunk* findMethodChunk(const std::string& className,
                                         const std::string& methodName) const;

    /// 按指令类别执行指令（executeOneInstruction 内部转发）
    VMResult executeConstantOps(OpCode op, size_t& ip);
    VMResult executeArithOps(OpCode op, size_t& ip);
    VMResult executeCompareOps(OpCode op, size_t& ip);
    VMResult executeVarOps(OpCode op, size_t& ip);
    VMResult executeCallOps(OpCode op, size_t& ip);
    VMResult executeContainerOps(OpCode op, size_t& ip);
    VMResult executeWritebackOps(OpCode op, size_t& ip);
    VMResult executeMiscOps(OpCode op, size_t& ip);

    /// 执行单条指令的内部实现（供 execute() 和 stepOnce() 共用）
    VMResult executeOneInstruction();
};
