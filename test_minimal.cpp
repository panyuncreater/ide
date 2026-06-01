#include <iostream>
#include <string>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <functional>
#include <vector>

// Include only the core interpreter files (no Qt, no DebugController)
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "lexer/Token.h"
#include "lexer/Lexer.h"
#include "ast/ASTNode.h"
#include "parser/Parser.h"

// Stub DebugController
class DebugController {
public:
    void checkBreak(ASTNode*) {}
    void reset() {}
};

// Now include Interpreter (it will find our stub DebugController)
// But Interpreter.h includes debug/DebugController.h which needs Qt.
// So we just copy the Interpreter class definition here.

class RuntimeError : public std::runtime_error {
public:
    int line; int column;
    RuntimeError(const std::string& msg, int ln = 0, int col = 0)
        : std::runtime_error(msg), line(ln), column(col) {}
};

class ReturnException : public std::runtime_error {
public:
    Value returnValue;
    ReturnException(const Value& val) : std::runtime_error("return"), returnValue(val) {}
};

struct CallFrame {
    std::string functionName;
    std::shared_ptr<Environment> env = nullptr;
    int line = 0, depth = 0;
    CallFrame() = default;
    CallFrame(const std::string& n, std::shared_ptr<Environment> e, int ln, int d)
        : functionName(n), env(e), line(ln), depth(d) {}
};

struct ClassInfo {
    std::string name, superClassName;
    std::unordered_map<std::string, FunDecl*> methods;
    std::unordered_map<std::string, Value> fields;
    ClassInfo* superClass = nullptr;
};

class Interpreter : public Visitor {
public:
    Interpreter() : globalEnv_(std::make_shared<Environment>()), currentEnv_(globalEnv_), debugger_(nullptr), recursionDepth_(0) { outputCallback_ = [](const std::string&) {}; }
    ~Interpreter() {}
    Value execute(Block& program) {
        globalEnv_ = std::make_shared<Environment>(); currentEnv_ = globalEnv_;
        callStack_.clear(); funRegistry_.clear(); classRegistry_.clear();
        typeAnnotations_.clear(); currentFunctionReturnType_.clear(); recursionDepth_ = 0;
        Value result = Value::nullValue();
        for (auto& stmt : program.statements) result = evaluate(stmt.get());
        return result;
    }
    void setOutputCallback(std::function<void(const std::string&)> cb) { outputCallback_ = cb; }
    void setDebugger(DebugController* d) { debugger_ = d; }
    void setDebugMode(bool) {}
    Environment* currentEnvironment() const { return currentEnv_.get(); }
    // Visit methods - include the .cpp implementations
