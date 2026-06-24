## 专项4：代码格式化双向一致性与幂等性校验报告

### 审计目标

验证 MiniLang 格式化器（Formatter）的语义正确性与幂等性：原始代码与格式化后代码运行结果完全一致（一致性），对格式化后代码再次格式化结果不变（幂等性）。

### 测试设计

共 70 个测试用例（60 语法覆盖 + 10 边界场景），每个用例执行四步验证：

1. **格式化**：Source → Lex → Parse → Formatter → Formatted
2. **幂等性**：Formatted → Lex → Parse → Formatter → Formatted2，比较 Formatted == Formatted2
3. **运行原始**：Source → Interpreter → Output A
4. **运行格式化**：Formatted → Interpreter → Output B，比较 A == B

### 测试分类（70 例）

| 类别 | ID | 数量 | 覆盖内容 |
|------|-----|------|----------|
| 算术表达式 | AR1–AR8 | 8 | 整数/浮点/加减乘除/优先级/取模/一元负号/复合运算 |
| 一元与比较 | UN1–UN6 | 6 | 逻辑非/比较链/逻辑 and-or/字符串/布尔-null |
| 变量 | VR1–VR6 | 6 | 声明/赋值/多变量/字符串拼接/作用域遮蔽/复合表达式 |
| 分支 | BR1–BR8 | 8 | if/else/else-if/嵌套/dangling-else/空 if/复杂嵌套 |
| 循环 | LP1–LP6 | 6 | while 基础/求和/for 基础/求和/嵌套 for/倒计时 |
| 函数 | FN1–FN8 | 8 | 基础/无返回/递归/多参/嵌套调用/void 副作用/类型参数/互递归 |
| 数组 | AY1–AY6 | 6 | 字面量/len/push/遍历/拼接/嵌套数组 |
| 字典 | DC1–DC4 | 4 | 字面量/方法/赋值/嵌套字典 |
| 类 | CL1–CL4 | 4 | 基础类/继承/字段/方法调用 |
| 复合表达式 | CM1–CM4 | 4 | 嵌套括号/数组方法链/字符串方法/混合表达式 |
| 边界场景 | BD1–BD10 | 10 | 10层嵌套/多余空格/超长表达式/空程序/单语句/多语句/5层函数链/20元素数组/3层类继承/混乱格式类 |

### 测试结果

```
RESULTS: 70 passed, 0 failed, 0 skipped, 70 total
```

所有 70 个测试用例的一致性校验和幂等性校验全部通过。

### 发现的非 Formatter 问题

**BUG-INTP-SUPER：解释器三层 super.init() 调用链崩溃**

- 位置：`interpreter/Interpreter.cpp`，`visitMethodCall` 中 `super` 表达式解析
- 触发条件：三层类继承（A→B→C），B 和 C 的 `init` 方法均调用 `super.init()`。C 的 `super.init()` 调用 B 的 init，B 内的 `super.init()` 应调用 A 的 init，但解释器在此场景下崩溃（退出码 3）。
- 影响：非格式化器 Bug，是解释器的 `super` 方法解析在多层继承链中的缺陷。
- 两层继承（A→B + super.init()）工作正常。

### 结论

Formatter 模块在所有测试场景下表现正确：格式化后的代码与原始代码语义完全一致（一致性），连续两次格式化产生完全相同的输出（幂等性）。格式化器对 10 层嵌套块、多余空格、超长表达式、混乱格式等边界场景均能正确规范化。测试执行器构建在 `test_harness.vcxproj`，可通过 MSBuild 编译运行。
