#include "ast/ASTNode.h"
#include "interpreter/Value.h" // A1 fix: getValue()/nodeName() 实现需要 Value 完整定义
#include "interpreter/Visitor.h"

// ============================================================
// AST 节点的 accept 实现
// ============================================================
// A1 fix: accept 返回 void，结果通过 Visitor 子类成员变量传递。

// A1 fix: 以下 getValue() / nodeName() 实现原本 inline 定义于 ASTNode.h，
// 但 ASTNode.h 现仅前向声明 Value（不再 include Value.h），故实现移至此处。

Value NumberLiteral::getValue() const {
    // 根据字面量标记选择构造浮点 Value 或整数 Value（A1 fix: 标量按需封装）。
    // 之所以同时保留 intValue_ 与 floatValue_ 两个字段，是因为 MiniLang 区分
    // 整数与浮点类型；getValue() 在读取时才决定封装成哪种 Value，
    // 这样 AST 节点保持轻量且可被多次遍历而不会产生副作用（非破坏性）。
    return isFloat_ ? Value(floatValue_) : Value(intValue_);
}

std::string NumberLiteral::nodeName() const {
    return "Number(" + getValue().toString() + ")";
}

Value StringLiteral::getValue() const {
    // 字符串字面量直接封装为字符串语义的 Value（A1 fix: 按需构造，非破坏性）。
    return Value(value);
}

Value BoolLiteral::getValue() const {
    // 布尔字面量封装为布尔语义的 Value（A1 fix: 标量按需封装）。
    // bool 本身已是标量，getValue() 仅做一次薄封装，供统一的值语义通路使用。
    return Value(value);
}

// 二元运算节点（加减乘除、比较、逻辑 and/or）。accept 派发至 visitBinaryOp，
// 由 Visitor（解释器/编译器）递归求值 left/right 后按 opType 计算结果。
void BinaryOp::accept(Visitor& visitor) {
    visitor.visitBinaryOp(*this);
}

// 一元运算节点（负号 -、逻辑非 not、一元加 +）。派发至 visitUnaryOp。
void UnaryOp::accept(Visitor& visitor) {
    visitor.visitUnaryOp(*this);
}

// 数字字面量节点（整数或浮点，由 isFloat_ 区分）。派发至 visitNumberLiteral；
// 实际数值在 getValue() 中按需封装为 Value，便于多次遍历 AST 而不破坏值。
void NumberLiteral::accept(Visitor& visitor) {
    visitor.visitNumberLiteral(*this);
}

// 字符串字面量节点。派发至 visitStringLiteral；值由 getValue() 按需构造为字符串 Value。
void StringLiteral::accept(Visitor& visitor) {
    visitor.visitStringLiteral(*this);
}

// 布尔字面量节点（true/false）。派发至 visitBoolLiteral；getValue() 按需构造布尔 Value。
void BoolLiteral::accept(Visitor& visitor) {
    visitor.visitBoolLiteral(*this);
}

// 变量声明节点（含可选类型注解 typeAnnotation 与可选初始化表达式）。
// 派发至 visitVarDecl；访问者负责将变量登记进环境并（若有）求值初始值。
void VarDecl::accept(Visitor& visitor) {
    visitor.visitVarDecl(*this);
}

// 变量赋值语句（name = value）。派发至 visitAssignment；
// 访问者先求值右部，再将结果绑定到左侧变量名所在环境。
void Assignment::accept(Visitor& visitor) {
    visitor.visitAssignment(*this);
}

// 变量引用节点（读取标识符的值）。派发至 visitVarRef；
// 访问者按名字在当前作用域链中查找变量并取回其 Value。
void VarRef::accept(Visitor& visitor) {
    visitor.visitVarRef(*this);
}

// 条件分支语句（if / 可选 else）。派发至 visitIfStmt；
// 访问者先求 condition，再仅对命中的 then/else 分支递归 visit 求值。
void IfStmt::accept(Visitor& visitor) {
    visitor.visitIfStmt(*this);
}

// while 循环语句。派发至 visitWhileStmt；
// 访问者在 condition 为真时反复 visit body，直到条件为假或遇到 break。
void WhileStmt::accept(Visitor& visitor) {
    visitor.visitWhileStmt(*this);
}

// for 循环语句（initializer; condition; update）。派发至 visitForStmt；
// 访问者先执行 initializer，再在 condition 为真时 visit body，每次迭代后执行 update。
void ForStmt::accept(Visitor& visitor) {
    visitor.visitForStmt(*this);
}

// 函数声明节点（含参数名/类型、返回类型、默认参数值）。派发至 visitFunDecl；
// 访问者将其登记为可调用对象（闭包持有定义时环境），但不立即执行函数体。
void FunDecl::accept(Visitor& visitor) {
    visitor.visitFunDecl(*this);
}

// 函数调用节点（命名调用或链式/表达式调用 callee）。派发至 visitFunCall；
// 访问者按名字或 callee 值解析函数，求值实参后执行调用。
void FunCall::accept(Visitor& visitor) {
    visitor.visitFunCall(*this);
}

// return 语句（可选返回值）。派发至 visitReturnStmt；
// 访问者通过控制流机制（异常/信号）将 value 的求值结果作为当前函数返回值。
void ReturnStmt::accept(Visitor& visitor) {
    visitor.visitReturnStmt(*this);
}

// print 语句（输出若干表达式值）。派发至 visitPrintStmt；
// 访问者依次求值 values 并写入标准输出。
void PrintStmt::accept(Visitor& visitor) {
    visitor.visitPrintStmt(*this);
}

// 语句块节点（大括号包裹的语句序列，构成独立作用域）。派发至 visitBlock；
// 访问者建立新作用域后顺序 visit 各语句，块结束恢复外层作用域。
void Block::accept(Visitor& visitor) {
    visitor.visitBlock(*this);
}

// 新增节点的 accept 实现

// 数组字面量节点 [e1, e2, ...]。派发至 visitArrayLiteral；
// 访问者依次求值各元素并组装为数组 Value。
void ArrayLiteral::accept(Visitor& visitor) {
    visitor.visitArrayLiteral(*this);
}

// 字典字面量节点 {"key": value, ...}。派发至 visitDictLiteral；
// 访问者依次求值键与值，组装为字典（映射）Value。
void DictLiteral::accept(Visitor& visitor) {
    visitor.visitDictLiteral(*this);
}

// 下标访问节点 arr[index] 或 dict[key]。派发至 visitIndexAccess；
// 访问者先求 object 与 index，再取下标位置的 Value。
void IndexAccess::accept(Visitor& visitor) {
    visitor.visitIndexAccess(*this);
}

// 下标赋值节点 arr[index] = value。派发至 visitIndexAssign；
// 访问者求 value 后将结果写回 object 的对应下标位置。
void IndexAssign::accept(Visitor& visitor) {
    visitor.visitIndexAssign(*this);
}

// 类声明节点（含类名、父类名、成员列表）。派发至 visitClassDecl；
// 访问者建立类定义（收集字段与方法）并登记到当前作用域，不实例化对象。
void ClassDecl::accept(Visitor& visitor) {
    visitor.visitClassDecl(*this);
}

// 成员访问节点 obj.field。派发至 visitMemberAccess；
// 访问者求值 object 后在其实例字段表中读取 fieldName 对应 Value。
void MemberAccess::accept(Visitor& visitor) {
    visitor.visitMemberAccess(*this);
}

// 成员赋值节点 obj.field = value。派发至 visitMemberAssign；
// 访问者求 value 后写回 object 实例的 fieldName 字段。
void MemberAssign::accept(Visitor& visitor) {
    visitor.visitMemberAssign(*this);
}

// 方法调用节点 obj.method(args)。派发至 visitMethodCall；
// 访问者求值 object 并以其为 this 绑定，按 methodName 解析方法后调用。
void MethodCall::accept(Visitor& visitor) {
    visitor.visitMethodCall(*this);
}

// null 字面量节点。派发至 visitNullLiteral；
// 访问者生成表示"空值"的 Value（无类型、不参与运算）。
void NullLiteral::accept(Visitor& visitor) {
    visitor.visitNullLiteral(*this);
}

// super 表达式节点（在方法体内引用父类实现）。派发至 visitSuperExpr；
// 访问者将其解析为当前实例绑定父类方法链的状态。
void SuperExpr::accept(Visitor& visitor) {
    visitor.visitSuperExpr(*this);
}

// break 语句（跳出最近循环）。派发至 visitBreakStmt；
// 访问者通过控制流信号中断当前 while/for 循环。
void BreakStmt::accept(Visitor& visitor) {
    visitor.visitBreakStmt(*this);
}

// continue 语句（跳到最近循环的下一次迭代）。派发至 visitContinueStmt；
// 访问者通过控制流信号跳过当前循环体剩余部分。
void ContinueStmt::accept(Visitor& visitor) {
    visitor.visitContinueStmt(*this);
}

// try-catch-finally 异常处理语句。派发至 visitTryStmt；
// 访问者先执行 tryBlock，异常时跳转 catchBlock（绑定异常变量），无论是否异常均执行 finallyBlock。
void TryStmt::accept(Visitor& visitor) {
    visitor.visitTryStmt(*this);
}

// throw 语句（抛出一个值作为异常）。派发至 visitThrowStmt；
// 访问者求 expression 后触发异常传播，由最近的 try 的 catch/finally 接管。
void ThrowStmt::accept(Visitor& visitor) {
    visitor.visitThrowStmt(*this);
}

// import 模块语句（导入其他模块的导出符号）。派发至 visitImportStmt；
// 访问者按 modulePath 加载目标模块并引入其导出名到当前作用域。
void ImportStmt::accept(Visitor& visitor) {
    visitor.visitImportStmt(*this);
}

// export 导出语句（将内部声明暴露给模块外部）。派发至 visitExportStmt；
// 访问者先 visit 内部 declaration，再将其标记为对外可见。
void ExportStmt::accept(Visitor& visitor) {
    visitor.visitExportStmt(*this);
}

// C5 fix: 插值字符串节点的 accept 实现
void InterpolatedString::accept(Visitor& visitor) {
    visitor.visitInterpolatedString(*this);
}

// R98 元组与解构：元组字面量节点的 accept 实现
// 派发至 visitTupleLiteral；访问者依次求值各 elements 并组装为 immutable 元组 Value。
void TupleLiteral::accept(Visitor& visitor) {
    visitor.visitTupleLiteral(*this);
}

// R98 元组与解构：解构绑定节点的 accept 实现
// 派发至 visitDestructureBinding；访问者求值 initializer（必须为 tuple）后
// 按位置拆分到 names 中的各变量名，可选类型注解 tupleTypeAnnotation 用于运行时校验。
void DestructureBinding::accept(Visitor& visitor) {
    visitor.visitDestructureBinding(*this);
}

// R99 枚举与 ADT + match：AST 节点 accept 实现
// EnumDecl 派发至 visitEnumDecl；访问者注册 enum 到 enumRegistry_，供后续
// EnumVariantExpr 构造时验证 enum/variant 存在性 + 参数数量。
void EnumDecl::accept(Visitor& visitor) {
    visitor.visitEnumDecl(*this);
}

// EnumVariantExpr 派发至 visitEnumVariantExpr；访问者查找 enum + variant，
// 校验参数数量后构造 EnumVariantData Value（immutable）。
void EnumVariantExpr::accept(Visitor& visitor) {
    visitor.visitEnumVariantExpr(*this);
}

// MatchPattern 不直接 accept（由 MatchExpr::accept 内联处理），但保留实现以满足
// ASTNode 抽象基类要求。MatchPattern 不应被独立访问——Compiler/Interpreter 直接读取
// pattern 字段而非通过 visitor 分派。此处 no-op 保证若误调用不会污染栈/状态。
void MatchPattern::accept(Visitor& visitor) {
    // 故意空实现：MatchPattern 不通过 visitor 分派
    (void)visitor;
}

// MatchExpr 派发至 visitMatchExpr；访问者求值 scrutinee，按顺序匹配 cases，
// 第一个匹配的 case 执行 body 并返回值。穷尽性检查在 TypeChecker 编译期完成。
void MatchExpr::accept(Visitor& visitor) {
    visitor.visitMatchExpr(*this);
}

// R164 协程/生成器：YieldExpr 派发至 visitYieldExpr。
// 访问者求值 value 表达式，挂起当前生成器并向调用者返回值。
void YieldExpr::accept(Visitor& visitor) {
    visitor.visitYieldExpr(*this);
}

// 七特性 MVP 阶段 2：宏声明派发至 visitMacroDecl（运行期 no-op）。
void MacroDecl::accept(Visitor& visitor) {
    visitor.visitMacroDecl(*this);
}

// 七特性 MVP 阶段 2：宏调用派发至 visitMacroCallExpr。
// 访问者直接求值/编译 expanded 子树（parse 期已展开）。
void MacroCallExpr::accept(Visitor& visitor) {
    visitor.visitMacroCallExpr(*this);
}

// 七特性 MVP 阶段 3：trait 声明派发至 visitTraitDecl（运行期 no-op，
// 方法已在 parse 期合入混入类的 members）。
void TraitDecl::accept(Visitor& visitor) {
    visitor.visitTraitDecl(*this);
}

// 七特性 MVP 阶段 4：await 表达式派发至 visitAwaitExpr。
// 访问者求值 operand，若为协程则驱动到完成并返回最终值。
void AwaitExpr::accept(Visitor& visitor) {
    visitor.visitAwaitExpr(*this);
}
