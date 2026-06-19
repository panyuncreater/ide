#pragma once

#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include "compiler/Bytecode.h"
#include "interpreter/Value.h"

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
};

/// VM 类信息（用于构造函数调用）
struct VMClassInfo {
    std::string name;                                  // 类名
    std::string superClassName;                        // 父类名（空表示无父类）
    std::vector<std::string> fieldOrder;               // 字段声明顺序（含继承字段）
    std::unordered_map<std::string, Value> fieldDefaults; // 字段默认值（含继承字段）
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

    /// 获取栈内容（用于调试，拷贝）
    std::vector<Value> getStack() const;

    /// 获取全局变量（用于调试，拷贝）
    std::unordered_map<std::string, Value> getGlobals() const;

    /// 获取栈的常量引用（零拷贝，调试用）
    const std::vector<Value>& getStackRef() const { return stack_; }

    /// 获取全局变量表的常量引用（零拷贝，调试用）
    const std::unordered_map<std::string, Value>& getGlobalsRef() const { return globals_; }

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
    std::unordered_map<std::string, Value> globals_; // 全局变量表
    std::vector<VMCallFrame> frames_;              // 调用帧栈
    BytecodeChunk mainChunk_;                       // 主 chunk 副本（VM 自持，避免悬空指针）
    std::unordered_map<std::string, BytecodeChunk> functionChunks_; // 函数字节码
    std::unordered_map<std::string, VMClassInfo> classInfo_;        // 类信息注册表
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::function<void(const VMStepInfo&)> stepCallback_;    // 步进回调
    bool stepCallbackEnabled_ = false;              // 是否启用步进回调
    bool initialized_ = false;                      // 是否已初始化执行环境
    std::string lastError_;                         // 最近一次运行时错误
    int lastErrorLine_ = 0;                          // 最近一次运行时错误的源码行号（1-based，0=无位置）
    bool hasError_ = false;                         // 运行时错误标志（用于快速检测）
    Value lastMutatedReceiver_;                     // 变异方法调用后暂存修改后的接收者对象（用于嵌套访问写回）
    std::vector<std::string> pendingFieldOrder_;    // M3: OP_INIT_FIELD 执行期间记录的字段声明顺序
    static constexpr size_t MAX_STACK_SIZE = 1024;  // 栈最大深度
    static constexpr size_t MAX_FRAMES = 256;       // 调用帧最大深度

    /// 栈操作
    void push(const Value& val);
    void push(Value&& val);
    Value pop();
    const Value& peek(size_t distance = 0) const;

    /// 运行时错误
    VMResult runtimeError(const std::string& msg);

    /// 数值二元运算（枚举分发）
    VMResult numericOp(int opType);

    /// 比较运算结果写回（消除 6 个比较运算符的重复代码）
    /// ip 按引用传入，结果写入后自动 +1（所有比较指令均为 1 字节）
    VMResult pushCompareResult(bool result, size_t& ip, OpCode opcode);

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

    /// 执行单条指令的内部实现（供 execute() 和 stepOnce() 共用）
    VMResult executeOneInstruction();
};
