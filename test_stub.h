// Minimal stub for DebugController (no Qt dependency)
#pragma once
#include <set>
#include <functional>
#include <vector>
#include <string>
#include "interpreter/Value.h"

struct VariableSnapshot {
    std::string name;
    Value value;
};

class DebugController {
public:
    void checkBreak(ASTNode* node) {}
    void reset() {}
    void setBreakpoints(const std::set<int>& bps) {}
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb) {}
};
