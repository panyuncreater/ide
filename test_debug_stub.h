#pragma once
#include "ast/ASTNode.h"

class DebugController {
public:
    void checkBreak(ASTNode* node) { (void)node; }
    void reset() {}
};
