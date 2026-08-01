# ============================================================
# MiniLang 单元测试构建脚本
# ------------------------------------------------------------
# 作为父项目 CMakeLists.txt 的子目录被添加。
# 也可独立构建（standalone）：cmake -B build && cmake --build build
# ============================================================

# BUILDFIX: 父项目未设置 MINILANG_ROOT_DIR，子目录需自行推导。
# 独立构建分支在下方再次设置（覆盖），此处仅作为 fallback 保证
# include(${MINILANG_ROOT_DIR}/cmake/windeployqt.cmake) 路径有效。
if(NOT DEFINED MINILANG_ROOT_DIR)
    set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

# 支持独立构建（不通过父项目 add_subdirectory 时）
if(NOT TARGET minilang_core)
    cmake_minimum_required(VERSION 3.25)
    project(MiniLangTests CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # 查找 Qt6
    find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

    # 项目根目录
    set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # 包含共享模块：源文件列表 + 编译选项 + 配置函数 + target 创建
    # P1-6: minilang_core.cmake 内部创建 4 个 OBJECT 子库 + STATIC 聚合，
    #        include 后 minilang_core target 即已就绪，无需再手动 add_library。
    include(${MINILANG_ROOT_DIR}/cmake/minilang_core.cmake)
endif()

# ============================================================
# 集成 GoogleTest v1.14.0
# 优先使用本地 third_party/googletest 源码（离线可用）
# 回退到 FetchContent 从 gitee 镜像下载（需网络）
# ============================================================
set(GOOGLETEST_LOCAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googletest)

if(EXISTS "${GOOGLETEST_LOCAL_DIR}/CMakeLists.txt")
    # 使用本地源码（离线可用）
    message(STATUS "使用本地 GoogleTest 源码: ${GOOGLETEST_LOCAL_DIR}")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${GOOGLETEST_LOCAL_DIR} ${CMAKE_BINARY_DIR}/_deps/googletest-build EXCLUDE_FROM_ALL)
else()
    # 回退：从 gitee 镜像下载（github.com 可能不可达）
    # 修复问题 6：GIT_SHALLOW 加速克隆
    message(STATUS "本地 GoogleTest 不存在，从 gitee 镜像下载（浅克隆）")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ============================================================
# minilang_tests 测试可执行目标
# ============================================================
add_executable(minilang_tests
    main.cpp
    TestLexer.cpp
    TestParser.cpp
    TestValue.cpp
    TestBuiltinMethods.cpp
    TestInterpreterE2E.cpp
    TestVME2E.cpp
    TestCompiler.cpp
    TestIR.cpp
    # P2-10 IR SSA 基础设施测试：CFG/支配树/支配边界/自然循环/PHI/GVN/LICM/内联
    TestIRSSA.cpp
    TestNaNBox.cpp
    TestBytecodeCache.cpp
    TestP1_4DebugUnification.cpp
    TestIRShadowFix.cpp
    TestNestedLvalueProbe.cpp
    TestMethodCallProbe.cpp
    TestThreeEnginesConsistency.cpp
    # OPT-2: 三后端随机程序差分回归测试（模板化生成 + 固定种子可复现）
    TestThreeEnginesFuzz.cpp
    TestLexerParserAudit.cpp
    # 多行字符串字面量回归锁（Lexer 跨行扫描/CRLF 规范化 + 四后端一致 + Formatter 往返）
    TestMultilineString.cpp
    # B1: Interpreter 尾调用蹦床（trampoline）回归锁（消除三后端最后一个已知设计差异）
    TestInterpreterTCO.cpp
    TestCompilerAudit.cpp
    TestCompilerIRIsolationAudit.cpp
    TestIRAudit.cpp
    TestThreeEnginesAudit.cpp
    TestDebugAudit.cpp
    TestFormatterAudit.cpp
    TestTeachingPanelsAudit.cpp
    TestTeachingPanelsAudit2.cpp
    TestTeachingPanelsAudit3.cpp
    TestTeachingPanelsAudit4.cpp
    TestTeachingPanelsAudit5.cpp
    # N5 fix: PanelCatalog + TeachingTreePanel navigation audit
    TestTeachingTreePanel.cpp
    TestAuditBatch1.cpp
    TestAuditBatch2.cpp
    TestAuditBatch3.cpp
    TestAuditBatch4.cpp
    TestAuditBatch5.cpp
    # P2-12 错误处理统一回归测试（IBackend 接口 + DiagCodes + Lexer/Parser 补全）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestErrorUnification.cpp
    # R98 runToCursor 临时断点机制回归测试（DebugController 一次性断点 API）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestRunToCursor.cpp
    # R104 调试器拓展回归测试（Logpoint / Function BP / Exception BP）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestR104DebuggerExtensions.cpp
    # R161 Watchpoint（数据断点）回归测试（VM/RegisterVM peekWriteTarget 单元测试）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestWatchpoint.cpp
    # R161 调试器 REPL 阶段 2 回归测试（Interpreter injectRegistriesFrom/hasRegistries）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestDebuggerReplStage2.cpp
    # R94 嵌套 try-finally + break/continue 续跳链测试（finallyIndices 索引 bug 回归）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestNestedTryFinallyBreak.cpp
    # R98 元组与解构：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestTuple.cpp
    # R99 枚举与 ADT + match：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestEnum.cpp
    # 七特性 MVP 阶段 1：? 传播运算符 × 泛型 enum Result/Option 四后端一致性
    ${CMAKE_CURRENT_SOURCE_DIR}/TestResultPropagation.cpp
    # 七特性 MVP 阶段 2：宏系统（parse 期展开）四后端一致性 + Formatter 往返
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMacro.cpp
    # 七特性 MVP 阶段 3：Trait/Mixin（parse 期方法合入）四后端一致性 + Formatter 往返
    ${CMAKE_CURRENT_SOURCE_DIR}/TestTrait.cpp
    # 七特性 MVP 阶段 4：async/await + 协作式调度器四后端一致性
    ${CMAKE_CURRENT_SOURCE_DIR}/TestAsyncAwait.cpp
    # R163 泛型/模板：四后端一致性 + Formatter/DocGenerator 输出测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestGenerics.cpp
    # R134 模式匹配扩展（VARIABLE/TUPLE/OR pattern + guard）：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMatch.cpp
    # R103 闭包与高阶函数增强：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestClosure.cpp
    # R98 W2 内建高阶函数 map/filter/reduce/forEach/find：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestHigherOrderFunctions.cpp
    # R98 W3 Lambda 表达式：四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestLambda.cpp
    # R109 代码格式化 CLI：核心逻辑测试（同时编译 fmt_core.cpp 以链接符号）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestFmtCli.cpp
    ${MINILANG_ROOT_DIR}/cli/fmt_core.cpp
    # R110 行级覆盖率 CLI：核心逻辑测试（同时编译 coverage_core.cpp 以链接符号）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCoverageCli.cpp
    ${MINILANG_ROOT_DIR}/cli/coverage_core.cpp
    # R162 静态分析 CLI：核心逻辑测试（同时编译 lint_core.cpp 以链接符号）
    # LintPass.cpp 已在 minilang_core 的 CORE_SOURCES 中，通过链接 minilang_core 获取符号
    ${CMAKE_CURRENT_SOURCE_DIR}/TestLintCli.cpp
    ${MINILANG_ROOT_DIR}/cli/lint_core.cpp
    # R162 B2 文档生成 CLI：核心逻辑测试（同时编译 doc_core.cpp 以链接符号）
    # DocGenerator.cpp 已在 minilang_core 的 CORE_SOURCES 中，通过链接 minilang_core 获取符号
    ${CMAKE_CURRENT_SOURCE_DIR}/TestDocCli.cpp
    ${MINILANG_ROOT_DIR}/cli/doc_core.cpp
    # R164 模糊测试 CLI：核心逻辑测试（同时编译 fuzz_core.cpp 以链接符号）
    # 引擎符号通过链接 minilang_core 获取
    ${CMAKE_CURRENT_SOURCE_DIR}/TestFuzzCli.cpp
    ${MINILANG_ROOT_DIR}/cli/fuzz_core.cpp
    # R164 LSP 语言服务器 CLI：核心逻辑测试（同时编译 lsp_core.cpp 以链接符号）
    # Lexer/Parser/Formatter/LintPass 已在 minilang_core 的 CORE_SOURCES 中
    ${CMAKE_CURRENT_SOURCE_DIR}/TestLspCli.cpp
    ${MINILANG_ROOT_DIR}/cli/lsp_core.cpp
    # R165 DAP 调试适配器 CLI：核心逻辑测试（同时编译 dap_core.cpp 以链接符号）
    # Lexer/Parser/Compiler/VM 已在 minilang_core 的 CORE_SOURCES 中
    ${CMAKE_CURRENT_SOURCE_DIR}/TestDapCli.cpp
    ${MINILANG_ROOT_DIR}/cli/dap_core.cpp
    # R165 包管理器 CLI：核心逻辑测试（同时编译 pkg_core.cpp 以链接符号）
    # Qt6::Core 已通过 minilang_core PUBLIC 链接传递
    ${CMAKE_CURRENT_SOURCE_DIR}/TestPkgCli.cpp
    ${MINILANG_ROOT_DIR}/cli/pkg_core.cpp
    # 教学面板数据类（位于 MINILANG_GUI_SOURCES，但仅含纯数据，
    # 通过显式编译进测试目标以避免 minilang_core 强依赖 Qt6::Widgets）
    ${MINILANG_ROOT_DIR}/gui/BugHuntLibrary.cpp
    ${MINILANG_ROOT_DIR}/gui/SyntaxProductionLibrary.cpp
    ${MINILANG_ROOT_DIR}/gui/LabManualContent.cpp
    # 第二波面板：MemoryModelLibrary/IRTransformLibrary/ProfileLibrary
    # P3-20 拆分：MemoryModelLibrary.cpp 已从 MemoryModelPanel.cpp 拆出，
    # IRTransformLibrary.cpp + IROptReplayLibrary.cpp 已从 IRTransformPanel.cpp 拆出
    # （纯静态教学场景数据），需独立加入测试目标以解析符号。Profile 仍内嵌于
    # 对应 Panel.cpp 中，整体加入测试目标。
    ${MINILANG_ROOT_DIR}/gui/MemoryModelLibrary.cpp
    ${MINILANG_ROOT_DIR}/gui/MemoryModelPanel.cpp
    ${MINILANG_ROOT_DIR}/gui/IRTransformPanel.cpp
    ${MINILANG_ROOT_DIR}/gui/IRTransformLibrary.cpp
    ${MINILANG_ROOT_DIR}/gui/IROptReplayLibrary.cpp
    ${MINILANG_ROOT_DIR}/gui/ProfileDashboardPanel.cpp
    # 第三波面板：CallStackLibrary/VariableInspectorLibrary/BytecodeTraceLibrary
    ${MINILANG_ROOT_DIR}/gui/CallStackPanel.cpp
    ${MINILANG_ROOT_DIR}/gui/VariableInspectorPanel.cpp
    ${MINILANG_ROOT_DIR}/gui/BytecodeTracePanel.cpp
    # UX-R fix: BytecodeTracePanel 等面板统一子页切换组件（仅依赖 Qt Widgets，无 IdeController 依赖）
    ${MINILANG_ROOT_DIR}/gui/TeachingSubPageBar.cpp
    # 第二档 P1-2：BreakpointConditionLibrary
    ${MINILANG_ROOT_DIR}/gui/BreakpointConditionPanel.cpp
    # R117: WatchExpressionLibrary（独立编译单元，不依赖 IdeController.cpp）
    # WatchPanel.cpp 调用 evaluateWatchExpression（非 inline，定义在 IdeController.cpp），
    # 而 minilang_tests 不链接 IdeController.cpp，故仅加入 Library 数据单元。
    ${MINILANG_ROOT_DIR}/gui/WatchExpressionLibrary.cpp
    # 新手引导组件——5 个面板的 createGuidedTour 方法调用 GuidedTour::addStep /
    # GuidedTour 构造函数，需加入测试目标以解析符号。依赖 Qt6::Widgets（QFrame/
    # QLabel/QPushButton），不依赖 IdeController / 引擎层。
    ${MINILANG_ROOT_DIR}/gui/GuidedTour.cpp
    # 第三档 P2-3：ExceptionFlowLibrary/ClosureInspectorLibrary
    ${MINILANG_ROOT_DIR}/gui/ExceptionFlowPanel.cpp
    ${MINILANG_ROOT_DIR}/gui/ClosureInspectorPanel.cpp
    # 拓展二期·教学：课程内容外部化——CourseSystemPanel.cpp 依赖 Qt6::Widgets
    # （与上方 MemoryModelPanel/IRTransformPanel 同模式），不依赖 IdeController.cpp
    # 符号（setController 内联），可安全加入测试目标以测 CourseSystemLibrary
    # 的文件 IO / 批量序列化。
    ${MINILANG_ROOT_DIR}/gui/CourseSystemPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCourseSystem.cpp
    # 拓展二期·教学：判题隐藏测试集——ExerciseGraderPanel.cpp 依赖 Qt6::Widgets +
    # BackendExecutionService（minilang_core），不依赖 IdeController.cpp 符号，
    # 加入测试目标以用三后端真实运行自校验题库全部用例（含隐藏用例）。
    ${MINILANG_ROOT_DIR}/gui/ExerciseGraderPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestExerciseGrader.cpp
    # 拓展二期·平台：libminilang C API——capi/minilang_capi.cpp 仅依赖
    # BackendExecutionService（minilang_core），纯 C ABI 层，直接编入测试目标
    # 验证全部导出符号（与 cli/*_core.cpp 同模式）。
    ${MINILANG_ROOT_DIR}/capi/minilang_capi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCApi.cpp
    # 七特性 MVP 阶段 6：沙箱模式（能力级限制）——复用已编入的 capi cpp 验证 C API 沙箱开关
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSandboxMode.cpp
    # 拓展二期·语言：运算符重载（instance 算术 dunder 分派）四后端一致性测试
    ${CMAKE_CURRENT_SOURCE_DIR}/TestOperatorOverload.cpp
    # 高价值改进方向 Top 3：拆分独立 Library.cpp 避免引入 IdeController 依赖
    ${MINILANG_ROOT_DIR}/gui/BugHuntVariantLibrary.cpp
    # 功能 6：学习路径地图——LearnerProgress.cpp 与 LearningPathData.cpp 为独立编译单元，
    # 不依赖 IdeController.h，可安全加入测试目标。LearningPathPanel.cpp 依赖 Qt Widgets +
    # PanelAnimator + QScrollArea 等，不加入测试目标。
    ${MINILANG_ROOT_DIR}/gui/LearnerProgress.cpp
    ${MINILANG_ROOT_DIR}/gui/LearningPathData.cpp
    # 功能 13：代码模板 / Snippets 系统——CodeSnippetEngine.cpp 为独立编译单元，
    # 仅依赖 Qt6::Core（QString），不依赖 CodeEditor / IdeController / Qt6::Widgets，
    # 可安全加入测试目标。CodeEditor.cpp 依赖 Qt6::Widgets 与多个 GUI 组件，不加入。
    ${MINILANG_ROOT_DIR}/gui/CodeSnippetEngine.cpp
    # 功能 5：VM 栈沙盒——SandboxLevels.cpp 为独立编译单元（仅依赖 Qt6::Core），
    # 不依赖 IdeController / Qt6::Widgets，可安全加入测试目标。
    # VmStackSandboxPanel.cpp 依赖 Qt6::Widgets（QComboBox/QListWidget 等），不加入。
    ${MINILANG_ROOT_DIR}/gui/SandboxLevels.cpp
    # 功能 3：交互式 Token 拼图游戏——TokenPuzzleData.cpp 为独立编译单元，
    # 仅依赖标准库（std::string / std::vector），不依赖 Qt Widgets / IdeController，
    # 可安全加入测试目标。TokenPuzzlePanel.cpp 依赖 Qt6::Widgets，不加入。
    ${MINILANG_ROOT_DIR}/gui/TokenPuzzleData.cpp
    # 拓展二期·平台：分享链接编解码——ShareCodec.cpp 为独立编译单元（仅依赖
    # Qt6::Core 的 QString/QByteArray），不依赖 IdeController / Qt6::Widgets，
    # 可安全加入测试目标。
    ${MINILANG_ROOT_DIR}/gui/ShareCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestShareCodec.cpp
    # N5 fix: PanelCatalog.cpp for TeachingTreePanel audit tests
    ${MINILANG_ROOT_DIR}/gui/PanelCatalog.cpp
    # 功能 4：AST 节点搭建玩具——AstToyLevels.cpp 为独立编译单元（仅依赖
    # Qt6::Core / 标准库），不依赖 IdeController / Qt6::Widgets，可安全加入测试目标。
    # AstBuilderToyPanel.cpp 依赖 Qt6::Widgets（QTreeWidget / QComboBox / QInputDialog 等），不加入。
    ${MINILANG_ROOT_DIR}/gui/AstToyLevels.cpp
    # 高价值改进方向 Top 3 测试文件
    ${CMAKE_CURRENT_SOURCE_DIR}/TestIROptReplayAudit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMemoryAnimAudit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestBugHuntVariantAudit.cpp
    # 功能 9 测试文件（BugHuntLibrary.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestBugHuntDifficultyAudit.cpp
    # 功能 6 测试文件
    ${CMAKE_CURRENT_SOURCE_DIR}/TestLearningPathAudit.cpp
    # 功能 13 测试文件（CodeSnippetEngine.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCodeSnippetAudit.cpp
    # 功能 12 测试文件（ErrorHintEngine.cpp 已在 minilang_core 的 CORE_SOURCES 中）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestErrorHintEngine.cpp
    # 功能 5 测试文件（SandboxLevels.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSandboxAudit.cpp
    # 功能 3 测试文件（TokenPuzzleData.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestTokenPuzzleAudit.cpp
    # 功能 4 测试文件（AstToyLevels.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestAstToyAudit.cpp
    # 教学面板端到端交互测试（验证 Panel 实例的子页切换/列表选择/QTimer 状态）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestTeachingPanelsE2E.cpp
    # 功能 11 测试文件（MagicCommands.cpp 已在 minilang_core 的 CORE_SOURCES 中，
    # 仅依赖 IdeController 内联方法 + minilang_core 符号，测试目标可安全链接）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMagicCommandsAudit.cpp
    # Markdown 渲染器测试（MarkdownRenderer.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMarkdownRendererAudit.cpp
    ${MINILANG_ROOT_DIR}/gui/MarkdownRenderer.cpp
    # R109 TCO 尾调用优化测试（覆盖基础尾递归 / 非尾递归 / 互递归 / try 块 / 类方法 / 闭包 / 默认参数 / 三后端一致性 / 类型检查）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestTCO.cpp
    # R117 Watch 表达式面板测试（WatchExpressionLibrary.cpp 已在上方显式加入测试目标）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestWatchPanel.cpp
    # R114 可回放执行时间轴：VmStepper.cpp 提供 Q_OBJECT MOC 元对象 + 非 inline 方法定义。
    # ExecutionTraceRecorder.cpp 已在 minilang_core 中（SKIP_AUTOMOC 避免跨目标 MOC 重复），
    # 测试目标通过链接 minilang_core 获取其符号。
    ${MINILANG_ROOT_DIR}/app/VmStepper.cpp
    # R114 可回放执行时间轴测试：核心 API + 环形缓冲 + JSON 序列化 +
    # Interpreter 集成 + PanelCatalog 注册。链接 minilang_core 获取全部符号。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestExecutionTraceRecorder.cpp
    # R121 调用栈帧切换测试：Interpreter::getCallStackSnapshot() 帧独立性 +
    # CallStackLibrary 场景完整性 + CallStackEntry 结构字段。
    # 上层 API（IdeController::setSelectedFrame / getSelectedFrameLocals）
    # 不在 minilang_core 中，通过代码审查 + IDE 手动测试验证（与 R98 VmStepper 模式一致）。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestFrameSwitching.cpp
    # R123 RegisterVM 闭包变量调用专项审计（边界场景测试）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestClosureAudit.cpp
    # R128 崩溃报告系统测试：CrashHandler 单例/安装/报告读取/清理/回调注入
    # 不进行真实崩溃触发，仅验证 API 行为与文件 IO（链接 minilang_core 获取符号）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCrashHandler.cpp
    # R133 Inline Cache 优化回归测试：覆盖 RegisterVM methodCache 正/负缓存路径、
    # 多态分发、深继承链、递归、父类重定义失效等 15 个场景，全部四后端一致性验证。
    # 依赖 common/ThreeBackends.h 共享基础设施（inline 函数 + EXPECT_ALL_BACKENDS 宏）。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestInlineCache.cpp
    # R134 SIMD 向量化回归测试：Layer 1 内核单元测试（simdSumInt64/simdSumDouble/
    # simdMinInt64/simdMaxInt64/decodeInt48Batch）+ Layer 2 端到端 sum() 四后端
    # 语义验证（覆盖 15/16/17/100/256 元素边界 + 负数 + 浮点 + 空数组）。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSimdKernels.cpp
    # R135 GC 替代引用计数控件：三模式（RefCountOnly/RefCountWithCycleGc/GcOnly）
    # 切换 API + 循环引用回收 + 三模式语义一致性验证（共 12 个测试）。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestGcModes.cpp
    # R136 线程与并发原语：channel/mutex/rwlock/spawn 四后端一致性测试
    # 覆盖 channel send/recv/tryRecv/close、mutex lock/unlock/tryLock、
    # rwlock read/write 锁、spawn + join/detach、闭包捕获、错误处理（共 30+ 测试）。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestConcurrency.cpp
    # R164 协程/生成器：fun* 生成器函数 + yield 表达式 + .next()/.done() 方法
    # 四后端一致性验证（Interpreter/StackVM/StackVM-IR/RegisterVM 重放模式）。
    # D.7 合并：原 TempStackVMCoroutine/TempRegVMCoroutine/TempStackVMIRCoroutine 临时测试
    # 已合并到 TestCoroutine.cpp，统一用 EXPECT_ALL_BACKENDS 宏验证四后端。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCoroutine.cpp
    # R138 JIT 后端阶段 1 PoC 测试：asmjit 字节码→本地代码编译执行
    # 验证整数算术子集（OP_INT/ADD/SUB/MUL/NEGATE/PRINT/POP/RETURN/NULL/TRUE/FALSE）。
    # 条件编译：MINILANG_USE_JIT=OFF 时仅注册 skip 占位测试，确保 CTest 一致性。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestJIT.cpp
        # testing.md 覆盖方向 #1：JIT 字符串方法/enum+match/并发原语/模块降级与 StackVM 一致性
        ${CMAKE_CURRENT_SOURCE_DIR}/TestJITCoverageGaps.cpp
    # P2-11 子项 4 内建标准库模块测试：BuiltinModuleRegistry API +
    # Compiler/Interpreter 路径对 `import "std/math|string|list"` 的拦截执行。
    # 链接 minilang_core 获取 BuiltinModules.cpp 符号。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestBuiltinModules.cpp
    # P2-10 IR SSA 基础设施测试：IRCFG/DominatorTree/DominanceFrontier/
    # NaturalLoopInfo + ssaConstruct/ssaDestruct + GVN/LICM/inline + 端到端语义验证。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestIRSSA.cpp
    # P2-11 预编译模块测试：compileModule + .minic 序列化 + 端到端导入 + 函数名隔离。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestPrecompiledModules.cpp
    # W3-2 覆盖率缺口定向补充测试：类型注解/UTF-8/类自动构造/闭包upvalue/enum错误路径四后端一致性。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCoverageGaps.cpp
    # P3-19 覆盖率缺口定向补充测试第二批：循环导入深层嵌套/并发原语边界/IR 优化 pass 验证。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCoverageGaps2.cpp
    # 覆盖率缺口定向补充测试第三批：rwlock 读写锁排斥边界/LICM 嵌套循环/GVN 副作用不消除/级联内联。
    ${CMAKE_CURRENT_SOURCE_DIR}/TestCoverageGaps3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestL14RuntimeErrorCatch.cpp
    # P3-IR-TEST IR 模块专用单元测试：AstIRBuilder / BytecodeIRBackend / RegisterBytecodeBackend
    # 各 AST 节点→IR 指令映射 + IR→StackVM/RegVM 字节码 lowering + 寄存器分配边界
    ${CMAKE_CURRENT_SOURCE_DIR}/TestAstIRBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestBytecodeIRBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestRegisterBytecodeBackend.cpp
    # 覆盖率审计补充：SpellChecker 拼写纠错单元测试（header-only，无额外链接依赖）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSpellChecker.cpp
    # 覆盖率审计补充：MemoryInspectionAPI 契约测试（NaNBox 编解码/GC 统计/堆检视）
    ${CMAKE_CURRENT_SOURCE_DIR}/TestMemoryInspectionAPI.cpp
    # 七特性 MVP 阶段 7：插件系统（宿主函数注册 + DLL 加载）——复用已编入的 capi cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestPlugin.cpp
)

target_link_libraries(minilang_tests PRIVATE
    minilang_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    gtest
    minilang_compile_options
)

# P1-6: 为 minilang_tests 启用 PCH（复用 common/pch.h 标准库头预编译）。
# 测试目标含 60+ .cpp，普遍包含 <string>/<vector>/<memory>，PCH 加速增量构建。
minilang_enable_pch(minilang_tests ${MINILANG_ROOT_DIR})

# ============================================================
# 七特性 MVP 阶段 7：测试插件动态库（minilang_test_plugin）
# ------------------------------------------------------------
# 导出 minilang_plugin_init，供 TestPlugin.cpp 的 minilang_load_plugin 加载测试。
# 仅依赖 capi/minilang_capi.h（纯头声明），不链接 minilang_core（插件自身不需）；
# 插件内调用的 minilang_register_function 符号由加载方（测试进程）提供。
# ============================================================
add_library(minilang_test_plugin SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/test_plugin.cpp
)
target_include_directories(minilang_test_plugin PRIVATE ${MINILANG_ROOT_DIR})
target_link_libraries(minilang_test_plugin PRIVATE minilang_compile_options)

# 将测试插件 DLL 的实际路径注入 TestPlugin.cpp（生成表达式，处理多配置路径）。
# 并确保插件先于测试构建（加载测试依赖 DLL 已存在）。
add_dependencies(minilang_tests minilang_test_plugin)
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/TestPlugin.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS "MINILANG_TEST_PLUGIN_PATH=\"$<TARGET_FILE:minilang_test_plugin>\"")

# QFluentKit 组件头文件（PushButton.h / Label.h / CardWidget.h 等）位于 src/QFluent/
# 子目录，而 QFluent 目标仅导出 src/。补充两个路径以支持 #include "PushButton.h"
# 与 #include "FluentGlobal.h" 风格。与 minilang_ide 目标保持一致（见父 CMakeLists.txt）。
target_include_directories(minilang_tests PRIVATE
    ${MINILANG_ROOT_DIR}/third_party/QFluentKit/QFluent/src
    ${MINILANG_ROOT_DIR}/third_party/QFluentKit/QFluent/src/QFluent
)

# Panel.cpp 文件（MemoryModelPanel / IRTransformPanel / CallStackPanel 等）包含
# QFluentKit 头文件并可能调用其内联函数，需链接 QFluent 共享库。
target_link_libraries(minilang_tests PRIVATE QFluent)

# 部署 QFluent 动态库到测试 exe 同目录（QFluent 为 SHARED 库，运行时需 DLL）
# 复用共享模块 cmake/deploy_qfluent.cmake（与根 CMakeLists 一致，修复审计问题 7）。
if(NOT COMMAND minilang_deploy_qfluent_runtime)
    include(${MINILANG_ROOT_DIR}/cmake/deploy_qfluent.cmake)
endif()
minilang_deploy_qfluent_runtime(minilang_tests)

if(MSVC)
    # AUDIT-P0 fix: 测试二进制栈扩至 8MB。Debug 模式下递归下降解析器的
    # 每个栈帧较大（~3KB），MAX_PARSE_DEPTH=256 对应 ~128 层括号 × 11 帧
    # × 3KB ≈ 4.2MB，需大于默认 1MB 栈才能让深度检查正常触发而非栈溢出。
    target_link_options(minilang_tests PRIVATE /STACK:8388608)
    # R128: CrashHandler.cpp 在 Windows 上使用 MiniDumpWriteDump，需链接 dbghelp
    target_link_libraries(minilang_tests PRIVATE dbghelp)
endif()

# ============================================================
# 注册 CTest 测试
# ------------------------------------------------------------
# 使用 gtest_discover_tests 在构建完成后运行测试二进制的
# --gtest_list_tests 接口发现所有测试用例，逐个注册到 CTest。
# 这样 ctest -R <name> 可筛选单个用例，CI 失败时定位精确。
# 修复问题 T-1（add_test 仅注册整体）/ T-3（无超时配置）。
# PRE_TEST 模式在运行时发现，确保二进制始终最新。
# ============================================================
enable_testing()
include(GoogleTest)
gtest_discover_tests(minilang_tests
    DISCOVERY_MODE PRE_TEST
    # 发现超时（--gtest_list_tests 启动耗时）：Release 大 exe 在 ctest -j 并行
    # 下多目标同时 discovery 易超默认 5s（Windows CI 实测超时失败）
    DISCOVERY_TIMEOUT 120
    PROPERTIES
        TIMEOUT 300
)


# ============================================================
# minilang_perf_test — CI 性能基准回归检测（独立可执行目标）
# ------------------------------------------------------------
# Phase 3 Task 3.5: 独立于 minilang_tests，避免基准测试拖慢常规单元测试。
# 输出 JSON 到 stdout，不依赖 pass/fail 断言。
# ============================================================
add_executable(minilang_perf_test
    TestPerformanceRegression.cpp
)

target_link_libraries(minilang_perf_test PRIVATE
    minilang_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    gtest
    minilang_compile_options
)

if(MSVC)
    target_link_options(minilang_perf_test PRIVATE /STACK:8388608)
    # R128: CrashHandler 通过 minilang_core 链接进来，需 dbghelp
    target_link_libraries(minilang_perf_test PRIVATE dbghelp)
endif()

# 注册到 CTest（独立于 minilang_tests 的测试发现）
gtest_discover_tests(minilang_perf_test
    DISCOVERY_MODE PRE_TEST
    # 同 minilang_tests：发现超时提升至 120s（并行 discovery 启动耗时）
    DISCOVERY_TIMEOUT 120
    PROPERTIES
        TIMEOUT 600
)

# ============================================================
# Windows 平台自动部署 Qt 运行时依赖到测试可执行文件同目录
# 使用共享模块 cmake/windeployqt.cmake（与 minilang_ide 一致）
# ============================================================
if(NOT COMMAND minilang_deploy_qt_runtime)
    include(${MINILANG_ROOT_DIR}/cmake/windeployqt.cmake)
endif()
minilang_deploy_qt_runtime(minilang_tests)
minilang_deploy_qt_runtime(minilang_perf_test)

# ============================================================
# minilang_app_tests — App 编排层线程安全测试（独立可执行目标）
# ------------------------------------------------------------
# 覆盖率审计 P1：DebugCoordinator / WorkerManager / PipelineRunner /
# InterpreterWorker 此前零测试。这些类依赖 QThread + QObject 信号槽，
# 且 IdeController.cpp 不被 minilang_tests 链接（避免拉入完整 GUI 依赖链），
# 故单独建目标，仅链接 app/ 编排层 + minilang_core。
# ============================================================
find_package(Qt6 COMPONENTS Test QUIET)

add_executable(minilang_app_tests
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestAppOrchestration.cpp
    ${MINILANG_ROOT_DIR}/app/DebugCoordinator.cpp
    ${MINILANG_ROOT_DIR}/app/WorkerManager.cpp
    ${MINILANG_ROOT_DIR}/app/InterpreterWorker.cpp
    ${MINILANG_ROOT_DIR}/app/PipelineRunner.cpp
)

target_link_libraries(minilang_app_tests PRIVATE
    minilang_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
    gtest
    minilang_compile_options
)

if(MSVC)
    target_link_options(minilang_app_tests PRIVATE /STACK:8388608)
    target_link_libraries(minilang_app_tests PRIVATE dbghelp)
endif()

gtest_discover_tests(minilang_app_tests
    DISCOVERY_MODE PRE_TEST
    # 同 minilang_tests：发现超时提升至 120s（并行 discovery 启动耗时）
    DISCOVERY_TIMEOUT 120
    PROPERTIES
        TIMEOUT 300
)
minilang_deploy_qt_runtime(minilang_app_tests)
# LNK2005 fix（fresh 重编译暴露）：启用与 minilang_tests 一致的 PCH。
# 根因：tests/main.cpp 调用内联 GcManager::instance()（含 static GcManager gc 局部静态），
# 无 PCH 编译时 MSVC 将 GcManager 构造/析构作为本地定义发射到 main.cpp.obj，
# 与 minilang_core.lib 的 unity obj 重复定义→ LNK2005/LNK1169；启用 PCH 后该符号
# 回到 UNDEF 外部引用（由 core.lib 解析），与 minilang_tests 行为一致。
minilang_enable_pch(minilang_app_tests ${MINILANG_ROOT_DIR})

# ============================================================
# minilang_gui_smoke — GUI 控件冒烟测试（可选目标）
# ------------------------------------------------------------
# 覆盖率审计 P2：CodeEditor 等 GUI 控件此前无 QTest 冒烟。
# 依赖 CodeEditor.cpp 及其 GUI 组件链，单独建目标避免污染主测试。
# ============================================================
add_executable(minilang_gui_smoke
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TestGuiSmoke.cpp
    ${MINILANG_ROOT_DIR}/gui/CodeEditor.cpp
    # LNK2019 fix: CodeEditor.cpp calls CodeSnippetEngine::matchTrigger/expand/snippets
    ${MINILANG_ROOT_DIR}/gui/CodeSnippetEngine.cpp
)

target_include_directories(minilang_gui_smoke PRIVATE
    ${MINILANG_ROOT_DIR}/third_party/QFluentKit/QFluent/src
    ${MINILANG_ROOT_DIR}/third_party/QFluentKit/QFluent/src/QFluent
)

target_link_libraries(minilang_gui_smoke PRIVATE
    minilang_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    QFluent
    gtest
    minilang_compile_options
)

if(MSVC)
    target_link_options(minilang_gui_smoke PRIVATE /STACK:8388608)
    target_link_libraries(minilang_gui_smoke PRIVATE dbghelp)
endif()

gtest_discover_tests(minilang_gui_smoke
    DISCOVERY_MODE PRE_TEST
    # 同 minilang_tests：发现超时提升至 120s（并行 discovery 启动耗时）
    DISCOVERY_TIMEOUT 120
    PROPERTIES
        TIMEOUT 120
)
minilang_deploy_qt_runtime(minilang_gui_smoke)
minilang_deploy_qfluent_runtime(minilang_gui_smoke)
# LNK2005 fix（同 minilang_app_tests）：tests/main.cpp 无 PCH 时 GcManager 单例构造/析构
# 被本地定义发射，与 core.lib 重复。启用与 minilang_tests 一致的 PCH 修复。
minilang_enable_pch(minilang_gui_smoke ${MINILANG_ROOT_DIR})
