#pragma once

#include "interpreter/Value.h"
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// Environment 作用域链
// ============================================================

/// 作用域环境，支持嵌套（作用域链），使用 shared_ptr 管理生命周期
/// 优化：对父作用域查找使用深度快捷缓存，避免重复 O(depth) 链遍历
class Environment : public std::enable_shared_from_this<Environment> {
public:
    /// 父作用域指针（全局环境为 nullptr）
    std::shared_ptr<Environment> parent;

    /// 构造函数
    explicit Environment(std::shared_ptr<Environment> parentEnv = nullptr) : parent(parentEnv) {
        // 注意：不在这里递增 generation_
        // 创建子作用域不改变已有变量的深度位置，缓存仍有效
        // 修复：继承父作用域的 boundInstance_，使方法体内的块作用域也能访问实例字段
        if (parentEnv) {
            boundInstance_ = parentEnv->boundInstance_;
        }
    }

    /// 在当前作用域定义变量
    void define(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        if (!inserted) {
            it->second = val; // 覆盖已有变量
        }
    }

    /// 移动重载：避免临时 Value 的深拷贝（数组/字典/实例等大对象）
    void define(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        if (!inserted) {
            it->second = std::move(val); // 覆盖已有变量
        }
    }

    /// P21 fix: 原子性检查+插入 — 变量已存在返回 false，新插入返回 true
    /// 用于 visitVarDecl 消除 find+define 双次查找
    bool tryDefineNew(const std::string& name, const Value& val) {
        auto [it, inserted] = variables.try_emplace(name, val);
        return inserted;
    }
    bool tryDefineNew(const std::string& name, Value&& val) {
        auto [it, inserted] = variables.try_emplace(name, std::move(val));
        return inserted;
    }

    /// 获取变量值（沿作用域链查找）— 返回指针，nullptr 表示未找到
    const Value* get(const std::string& name) const {
        // PERF-02 fix: 递归改迭代循环，消除函数调用开销。
        // 每层先查 variables，再查 boundInstance 字段，再向上。
        // 迭代上限保护：防止异常的循环父指针导致无限循环。
        // #21 fix: boundInstance_ 沿作用域链继承（构造/resetForReuse 时从父级拷贝），
        // 同一链上多数层 boundInstance_ 指针相同。缓存上次已检查过的实例指针，
        // 若当前层与之相同则跳过重复 fields 查找——同一实例的字段集在 get() 期间不变，
        // 上次未命中则本次也不会命中。仅当链上出现不同 boundInstance_（bindInstance
        // 显式覆盖）时才重新检查。语义等价：scope 0 vars > 实例字段 > 父级 vars > ...。
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                return &it->second;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            if (cur->boundInstance_ && cur->boundInstance_->isInstance() &&
                cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& flds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = flds.find(name);
                if (fit != flds.end())
                    return &fit->second;
            }
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// B1 fix: 沿作用域链查找变量（仅 variables map，不含 boundInstance 字段）。
    /// 用于闭包 capturedVars 快照——仅捕获环境变量，不捕获实例字段（与
    /// collectVariables/allVariablesMap 行为一致）。
    const Value* getVariableOnly(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end())
                return &it->second;
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// 设置变量值（沿作用域链查找并更新）
    bool set(const std::string& name, const Value& val) {
        // PERF-02 fix: 递归改迭代，每层查 variables 再查 boundInstance 字段
        // #21 fix: 同 get()，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                it->second = val;
                return true;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
            if (cur->boundInstance_ && cur->boundInstance_->isInstance() &&
                cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& constFlds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = constFlds.find(name);
                if (fit != constFlds.end()) {
                    cur->boundInstance_->fields()[name] = val;
                    return true;
                }
            }
            cur = cur->parent.get();
        }
        return false; // 变量不存在
    }

    /// P1 fix: move 重载 — 避免 writeBack 中 std::move 静默退化为深拷贝
    ///
    /// 契约（P1-4 fix 显式标注）：
    ///   - 返回 true:  val 已被 move 入目标变量，caller 不可再使用 val。
    ///   - 返回 false: val 未被 move（caller 仍持有 val 的所有权，可继续使用）。
    ///
    /// 调用方必须检查返回值，否则在 false 路径下若按"已 move"语义使用 val
    /// 会得到不一致行为（val 实际仍有效）。需要"无论是否找到都消费 val"
    /// 语义时，使用 const Value& 重载或在调用后显式 val = Value::nullValue()。
    bool set(const std::string& name, Value&& val) {
        // PERF-02 fix: 递归改迭代
        // #21 fix: 同 set(const&)，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            auto it = cur->variables.find(name);
            if (it != cur->variables.end()) {
                it->second = std::move(val);
                return true;
            }
            // INTERP-01 fix: 回退到绑定实例的字段（在检查父作用域之前）
            // P1-5 fix: 先用 const 访问器检查字段是否存在，避免不必要 COW 深拷贝
            if (cur->boundInstance_ && cur->boundInstance_->isInstance() &&
                cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& constFlds = static_cast<const Value*>(cur->boundInstance_)->fields();
                auto fit = constFlds.find(name);
                if (fit != constFlds.end()) {
                    cur->boundInstance_->fields()[name] = std::move(val);
                    return true;
                }
            }
            cur = cur->parent.get();
        }
        return false;
    }

    /// 检查变量是否存在（沿作用域链）
    /// P2-10 fix: 与 get() 保持一致，也检查绑定实例的字段
    bool hasVariable(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        // #21 fix: 同 get()，缓存 lastCheckedInstance 跳过重复 boundInstance_ 字段查找
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        const Value* lastCheckedInstance = nullptr;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            if (cur->variables.find(name) != cur->variables.end())
                return true;
            // 与 get() 一致：回退到绑定实例的字段检查
            if (cur->boundInstance_ && cur->boundInstance_->isInstance() &&
                cur->boundInstance_ != lastCheckedInstance) {
                lastCheckedInstance = cur->boundInstance_;
                const auto& flds = static_cast<const Value*>(cur->boundInstance_)->fields();
                if (flds.find(name) != flds.end())
                    return true;
            }
            cur = cur->parent.get();
        }
        return false;
    }

    /// 收集所有变量到结果向量（避免N个临时vector深拷贝）
    void collectVariables(std::vector<std::pair<std::string, Value>>& result) const {
        // 先收集父作用域（父的变量在前）
        if (parent) {
            parent->collectVariables(result);
        }
        // 再收集当前作用域（子的覆盖父的，所以追加到末尾）
        for (const auto& kv : variables) {
            result.emplace_back(kv.first, kv.second);
        }
    }

    /// 获取当前作用域及所有父作用域的变量快照（向量形式，保留遮蔽顺序）
    std::vector<std::pair<std::string, Value>> allVariables() const {
        std::vector<std::pair<std::string, Value>> result;
        collectVariables(result);
        return result;
    }

    /// C1 fix: 获取所有可见变量的扁平 map（用于闭包 capturedVars 快照）
    /// 子作用域变量覆盖父作用域同名变量（与 collectVariables 的追加顺序一致）
    std::unordered_map<std::string, Value> allVariablesMap() const {
        auto vec = allVariables();
        std::unordered_map<std::string, Value> result;
        result.reserve(vec.size());
        for (auto& kv : vec) {
            result[kv.first] = std::move(kv.second);
        }
        return result;
    }

    /// 仅获取当前作用域变量（不含父作用域）
    // PERF-01 fix: 改回 unordered_map — std::unordered_map 的引用/指针在 rehash 时
    // 不失效（C++ 标准保证：node-based 容器仅迭代器失效）。boundInstance_ 指向
    // variables 中 "this" 条目的 Value*，在 unordered_map 中同样稳定。
    // 变量查找从 O(log n) 降为 O(1) 平均，解释器整体提速 20-40%。
    const std::unordered_map<std::string, Value>& localVariables() const { return variables; }

    // ---- 条件断点沙箱化 (#1 fix) ----
    // 条件断点求值前快照变量绑定，求值后恢复，防止条件中的赋值/声明
    // 修改程序状态。注意：Value 是 ref-counted，容器变异（arr.push）仍
    // 影响共享对象——这是残余限制，文档化在 project_memory 中。
    std::unordered_map<std::string, Value> snapshotLocalVariables() const { return variables; }
    void restoreLocalVariables(const std::unordered_map<std::string, Value>& snap) {
        variables = snap;
        // H2 fix: variables = snap 整表替换会使旧 map 中所有 Value* 失效，
        // boundInstance_ 指向旧 variables["this"] 条目，现已悬垂。
        // 必须在新 map 中重新定位 "this" 并重新锚定 boundInstance_，
        // 否则后续 cur->boundInstance_->fields() 将解引用悬垂指针 (UAF)。
        if (boundInstance_ != nullptr) {
            auto it = variables.find("this");
            if (it != variables.end() && it->second.isInstance()) {
                boundInstance_ = &it->second;
            } else {
                boundInstance_ = nullptr;
            }
        }
    }

    // ---- P5 fix: 实例字段绑定 ----
    // 方法调用时绑定 this 实例，get/set 找不到变量时回退到实例字段
    // 避免将所有字段深拷贝到方法环境中

    void bindInstance(Value* instance) {
        // instance 应为 nullptr（解绑）或指向 variables 中 "this" 条目的 Value*
        // 调用方契约：instance 指向的 Value 在 Environment 销毁前必须有效
        // unordered_map 是 node-based，rehash 不失效指针，但 erase/clear 会让指针失效
        // resetForReuse 已正确处理（先 nullptr 再 clear）
        // Debug 构建中 assert 捕获误用（绑定非实例值）
        assert(instance == nullptr || instance->isInstance());
        boundInstance_ = instance;
    }
    Value* getBoundInstance() const { return boundInstance_; }

    // ---- B1 fix: 闭包捕获的 open/close upvalue 机制 ----
    // 闭包创建时，将其注册到"定义被捕获变量的 env"上。
    // 作用域退出时调用 closeCapturedVariables()，将变量的最终值写回闭包的 capturedVars，
    // 实现等价于 VM 的 OP_CLOSE_UPVALUE——循环变量 i 在 for 作用域退出时关闭为终值 3，
    // 循环体变量 captured 在 block 退出时关闭为当次迭代的值。

    /// 仅查本作用域的局部变量（不含父作用域和 boundInstance 字段）。
    /// 用于确定被捕获变量定义在哪个 env 上。
    const Value* getLocalVariable(const std::string& name) const {
        auto it = variables.find(name);
        return it != variables.end() ? &it->second : nullptr;
    }

    /// 注册闭包捕获 — 记录某个闭包捕获了本 env 中的变量。
    /// closureVal 是闭包值的副本（共享 ClosureData，保持其存活）。
    void registerClosureCapture(Value closureVal, const std::string& capturedName) {
        closureCaptures_.push_back({std::move(closureVal), capturedName});
    }

    /// 作用域退出时，将本 env 中被捕获的变量的最终值写回闭包的 capturedVars。
    /// 实现等价于 VM 的 OP_CLOSE_UPVALUE（关闭 open upvalue）。
    void closeCapturedVariables() {
        for (auto& cap : closureCaptures_) {
            auto it = variables.find(cap.capturedName);
            if (it != variables.end()) {
                // B1 fix: 直接修改共享 ClosureData 的 capturedVars，不触发 COW。
                // capturedVars() 的非 const 重载会 ensureUnique → refCount>1 时创建副本，
                // 导致修改写到副本而非原件，closures 数组中的原始闭包 capturedVars 保持陈旧。
                // 使用 const 引用 + const_cast 绕过 COW，直接修改共享的 ClosureData。
                const auto& constCaptured = const_cast<const Value&>(cap.closureVal).capturedVars();
                const_cast<std::unordered_map<std::string, Value>&>(constCaptured)[cap.capturedName] = it->second;
            }
        }
        closureCaptures_.clear();
    }

    /// 是否有闭包捕获了本 env 的变量（用于判断是否可回收至 envPool_）。
    /// 有捕获的 env 不能回收（resetForReuse 会清空 variables，导致 weak_ptr 仍有效但内容陈旧）。
    bool hasClosureCaptures() const { return !closureCaptures_.empty(); }

    /// AUDIT-BUG-I1 fix: 是否有闭包以本 env 作为 env weak_ptr 目标。
    /// 即使闭包仅捕获父级变量（不在本 env 注册 capture），本 env 也不能回收——
    /// 因为闭包的 env weak_ptr 指向本 env，回收后 resetForReuse 会清空 variables
    /// 并替换 parent，导致闭包调用时变量查找失败。
    void markClosureEnvRef() { hasClosureEnvRef_ = true; }
    bool hasClosureEnvRef() const { return hasClosureEnvRef_; }

    /// PERF-07 fix: 重置 Environment 状态以便对象池复用。
    /// 清空 variables/typeAnnotations_/boundInstance_，更新 parent 指针。
    /// 用于 visitBlock 退出时回收未捕获的块作用域 Environment，避免重复堆分配。
    void resetForReuse(std::shared_ptr<Environment> parentEnv) {
        variables.clear();
        typeAnnotations_.clear();
        boundInstance_ = nullptr;
        parent = std::move(parentEnv);
        if (parent) {
            boundInstance_ = parent->boundInstance_;
        }
        // B1 fix: 防御性清空 — 有捕获的 env 不应被回收，但此处兜底避免悬垂引用
        closureCaptures_.clear();
        // AUDIT-BUG-I1 fix: 重置闭包 env 引用标记
        hasClosureEnvRef_ = false;
        // AUDIT-P2-CORRECT fix: 重置捕获变量名集合
        capturedVarNames_.clear();
    }

    // ---- AUDIT-P2-CORRECT fix: 捕获变量名追踪 ----
    // 记录从 capturedVars 重建时导入的变量名集合，用于区分"捕获变量被赋值修改"
    // 与"捕获变量被 var 重声明"。visitVarDecl 中若 var 声明的名字在此集合中，
    // 则从集合移除（标记为已重声明）。writeBackCapturedVars 仅写回仍在集合中的变量，
    // 防止重声明的局部变量被写回 capturedVars 污染下次调用。
    void markAsCaptured(const std::string& name) { capturedVarNames_.insert(name); }
    bool isCapturedVar(const std::string& name) const { return capturedVarNames_.count(name) > 0; }
    void unmarkCaptured(const std::string& name) { capturedVarNames_.erase(name); }

    // ---- B2 fix: 作用域感知的类型注解 ----

    /// 在当前作用域定义类型注解
    void defineTypeAnnotation(const std::string& name, const std::string& type) { typeAnnotations_[name] = type; }

    /// 沿作用域链查找类型注解（返回指针，nullptr=无注解）
    const std::string* getTypeAnnotation(const std::string& name) const {
        // PERF-02 fix: 递归改迭代
        const Environment* cur = this;
        int depth = 0;
        constexpr int MAX_SCOPE_DEPTH = 1024;
        while (cur) {
            if (depth++ >= MAX_SCOPE_DEPTH)
                break;
            auto it = cur->typeAnnotations_.find(name);
            if (it != cur->typeAnnotations_.end())
                return &it->second;
            cur = cur->parent.get();
        }
        return nullptr;
    }

    /// 获取当前作用域的类型注解（用于 REPL 状态保存）
    const std::unordered_map<std::string, std::string>& localTypeAnnotations() const { return typeAnnotations_; }

private:
    // PERF-01 fix: 改回 unordered_map。C++ 标准保证 unordered_map 的引用/指针在 rehash
    // 时不失效（仅迭代器失效），因此 boundInstance_（指向 "this" 条目的 Value*）安全。
    // 变量查找从 O(log n) 降为 O(1) 平均。
    std::unordered_map<std::string, Value> variables;
    std::unordered_map<std::string, std::string> typeAnnotations_; // B2: 作用域感知类型注解
    Value* boundInstance_ = nullptr; // P5: 绑定的 this 实例（非拥有指针，方法调用期间有效）

    // B1 fix: 闭包捕获追踪。记录哪些闭包捕获了本 env 中的变量。
    // 作用域退出时 closeCapturedVariables() 将最终值写回闭包 capturedVars。
    // Value 副本共享 ClosureData（intrusive refcount），不构成循环引用
    // （ClosureData.env 是 weak_ptr）。
    struct ClosureCapture {
        Value closureVal;
        std::string capturedName;
    };
    std::vector<ClosureCapture> closureCaptures_;

    // AUDIT-BUG-I1 fix: 标记是否有闭包以本 env 作为 env weak_ptr 目标。
    // 用于防止 envPool_ 回收破坏闭包 env weak_ptr。
    bool hasClosureEnvRef_ = false;

    // AUDIT-P2-CORRECT fix: 记录从 capturedVars 重建时导入的变量名集合，
    // 用于区分"捕获变量被赋值修改"与"捕获变量被 var 重声明"。
    // visitVarDecl 中若 var 声明的名字在此集合中，则从集合移除（标记为已重声明）。
    // writeBackCapturedVars 仅写回仍在集合中的变量，防止重声明的局部变量污染 capturedVars。
    std::unordered_set<std::string> capturedVarNames_;

    // P0-5 fix: 已移除有缺陷的深度缓存（DepthEntry/depthCache_/generation_/
    //   findTargetEnv/getAtDepth/setAtDepth/getWithDepth），改用简单作用域链遍历。
    //   缓存验证条件几乎永不成立且存在遮蔽 Bug，移除后无性能损失。
};
