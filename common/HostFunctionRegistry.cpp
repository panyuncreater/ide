/**
 * @file common/HostFunctionRegistry.cpp
 * @brief 七特性 MVP 阶段 7：宿主函数注册表实现。
 */
#include "common/HostFunctionRegistry.h"

namespace minilang {

HostFunctionRegistry& HostFunctionRegistry::instance() {
    static HostFunctionRegistry registry;
    return registry;
}

void HostFunctionRegistry::registerFunction(const std::string& name, HostFn fn, void* userdata) {
    functions_[name] = Entry{fn, userdata};
}

bool HostFunctionRegistry::has(const std::string& name) const {
    return functions_.find(name) != functions_.end();
}

HostValue HostFunctionRegistry::call(const std::string& name, const HostValue* args, int argc, bool* found) const {
    auto it = functions_.find(name);
    if (it == functions_.end() || it->second.fn == nullptr) {
        if (found)
            *found = false;
        return HostValue::makeNull();
    }
    if (found)
        *found = true;
    return it->second.fn(args, argc, it->second.userdata);
}

void HostFunctionRegistry::clear() {
    functions_.clear();
}

} // namespace minilang
