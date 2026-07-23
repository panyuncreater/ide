# ============================================================
# minilang_core.cmake — 共享的源文件列表与编译配置
# ------------------------------------------------------------
# 由根 CMakeLists.txt 和 tests/CMakeLists.txt（独立构建路径）
# 共同 include，消除源文件列表重复维护问题。
# 修复问题 2（独立构建路径重复维护源文件列表）。
# ============================================================

# 核心模块源文件（不含 GUI 和 main）
set(MINILANG_CORE_SOURCES
    common/TypeChecker.cpp
    lexer/Lexer.cpp
    ast/ASTNode.cpp
    ast/ModuleIsolation.cpp
    parser/Parser.cpp
    interpreter/Interpreter.cpp
    interpreter/InterpreterCalls.cpp
    interpreter/InterpreterClasses.cpp
    interpreter/InterpreterModules.cpp
    interpreter/BuiltinMethods.cpp
    interpreter/Value.cpp
    interpreter/GcManager.cpp
    debug/DebugController.cpp
    compiler/Bytecode.cpp
    compiler/Compiler.cpp
    compiler/VM.cpp
    compiler/VMCalls.cpp
    compiler/VMContainers.cpp
    compiler/IR.cpp
    compiler/RegisterBytecode.cpp
    compiler/RegisterBytecodeBackend.cpp
    compiler/RegisterVM.cpp
    formatter/Formatter.cpp
    # 功能 12：错误信息友好化增强引擎（core 层使用，Interpreter.cpp 集成需要）
    gui/ErrorHintEngine.cpp
    # 功能 11：REPL %magic 命令系统（仅依赖 IdeController 内联方法 + minilang_core 符号，
    # 可被测试目标安全链接。ReplPanel.cpp 在 GUI_SOURCES 中调用 MagicCommands::handle）
    gui/MagicCommands.cpp
    # R114 可回放执行时间轴：执行轨迹记录器（VmStepper / Interpreter 调用）
    debug/ExecutionTraceRecorder.cpp
    # R128 崩溃报告：CrashHandler（MiniDumpWriteDump + 崩溃报告持久化）
    common/CrashHandler.cpp
    # R162 静态分析器：LintPass（基于 AST 的 8 项检查规则）
    # 仅依赖 ast + common + interpreter/Visitor.h（DefaultVisitor 基类），不依赖 Qt / 引擎层
    lint/LintPass.cpp
    # R162 B2 文档生成器：DocGenerator（基于 AST 遍历 + Lexer 注释流的 API 文档生成）
    # 仅依赖 ast + common + lexer + interpreter/Visitor.h（DefaultVisitor 基类），不依赖 Qt / 引擎层
    doc/DocGenerator.cpp
)

# JIT 后端源文件（条件编译，MINILANG_USE_JIT = ON 时包含）
if(MINILANG_USE_JIT)
    list(APPEND MINILANG_CORE_SOURCES compiler/JIT.cpp)
endif()

# GUI 模块源文件
set(MINILANG_GUI_SOURCES
    gui/CodeEditor.cpp
    gui/CodeSnippetEngine.cpp
    gui/SyntaxHighlighter.cpp
    gui/AstViewer.cpp
    gui/DebugPanel.cpp
    gui/ReplPanel.cpp
    gui/VmStackPanel.cpp
    gui/IrViewer.cpp
    gui/FindReplacePanel.cpp
    gui/ActivityBar.cpp
    # 教学增强面板（第一波 + 第三波）
    gui/PipelineViewer.cpp
    gui/BackendComparePanel.cpp
    gui/BugHuntPanel.cpp
    gui/BugHuntLibrary.cpp
    gui/BugHuntVariantLibrary.cpp
    gui/SyntaxExplorerPanel.cpp
    gui/SyntaxProductionLibrary.cpp
    gui/LabManualPanel.cpp
    gui/LabManualContent.cpp
    gui/MarkdownRenderer.cpp
    gui/MemoryModelPanel.cpp
    gui/IRTransformPanel.cpp
    gui/ProfileDashboardPanel.cpp
    # 教学面板拓展：JIT 可视化 + 执行步骤讲解 + 三后端并行 + AST 可视化 + 内联缓存
    gui/JitRunner.cpp
    gui/JitVisualizerPanel.cpp
    gui/StepExplainerPanel.cpp
    gui/BackendParallelPanel.cpp
    gui/AstVisualizerPanel.cpp
    gui/InlineCachePanel.cpp
    # 教学面板拓展（第五波）：循环展开 + 逃逸分析 + 寄存器分配 + 三后端性能竞赛
    gui/LoopUnrollingPanel.cpp
    gui/EscapeAnalysisPanel.cpp
    gui/RegisterAllocatorPanel.cpp
    gui/PerformanceRacePanel.cpp
    # 教学面板拓展（第六波）：内存布局 + 教学课程 + 练习评分
    gui/MemoryLayoutPanel.cpp
    gui/CourseSystemPanel.cpp
    gui/ExerciseGraderPanel.cpp
    # 教学面板拓展（第七波）：协程 + GC + Lint + Fuzz + 模块系统
    gui/CoroutineVisualizerPanel.cpp
    gui/GcVisualizerPanel.cpp
    gui/LintExplorerPanel.cpp
    gui/FuzzPlaygroundPanel.cpp
    gui/ModuleSystemVisualizerPanel.cpp
    # 教学面板子页切换栏统一组件（R165 体验优化）
    gui/TeachingSubPageBar.cpp
    # 教学增强面板（第三波）
    gui/CallStackPanel.cpp
    gui/VariableInspectorPanel.cpp
    gui/BytecodeTracePanel.cpp
    gui/BreakpointConditionPanel.cpp
    # 教学增强面板（第三档）
    gui/ExceptionFlowPanel.cpp
    gui/ClosureInspectorPanel.cpp
    # 教学增强面板（功能 6：学习路径地图）
    gui/LearningPathPanel.cpp
    gui/LearningPathData.cpp
    gui/LearnerProgress.cpp
    # 功能 3：交互式 Token 拼图游戏
    # TokenPuzzleData.cpp 为独立编译单元（仅依赖标准库，不依赖 Qt Widgets / IdeController）
    # TokenPuzzlePanel.cpp 依赖 Qt Widgets
    gui/TokenPuzzleData.cpp
    gui/TokenPuzzlePanel.cpp
    # 教学增强面板（功能 5：VM 栈沙盒）
    # SandboxLevels.cpp 为独立编译单元（仅依赖 Qt6::Core），可被测试目标链接；
    # VmStackSandboxPanel.cpp 依赖 Qt6::Widgets，仅在主 IDE 中链接。
    gui/SandboxLevels.cpp
    gui/VmStackSandboxPanel.cpp
    # 功能 4：AST 节点搭建玩具——AstToyLevels.cpp 为独立编译单元
    # （仅依赖 Qt6::Core，不依赖 IdeController.h），可被测试目标安全链接。
    # AstBuilderToyPanel.cpp 依赖 Qt6::Widgets（QTreeWidget/QComboBox 等），
    # 不依赖 IdeController / 引擎层，不加入测试目标。
    gui/AstToyLevels.cpp
    gui/AstBuilderToyPanel.cpp
    # 功能 1：Welcome 向导 / 首次启动导览——依赖 Qt6::Widgets（QDialog /
    # QStackedWidget），不依赖 IdeController / 引擎层，不加入测试目标。
    gui/WelcomeWizard.cpp
    # 功能 2（降级）：代码生命旅程静态信息图——依赖 Qt6::Widgets（QTextBrowser），
    # 不依赖 IdeController / 引擎层，不加入测试目标。
    gui/CodeJourneyInfoPanel.cpp
    # P2-1 fix: 抽取统一面板导航目录（替代原 LearningHubDialog 卡片设计，
    # 并作为 TeachingTreePanel 与 onActivityRequested 的单一数据源）。
    # 依赖 Qt6::Core（仅因 PCH 拉入 QString，无 Widgets/QFluentKit 依赖），
    # 不依赖 IdeController / 引擎层。不加入测试目标（纯静态数据，可被测试目标链接）。
    gui/PanelCatalog.cpp
    # 教学面板统一标题栏组件——含帮助按钮与学习路径跳转，依赖 QFluentKit，
    # 不依赖 IdeController / 引擎层，不加入测试目标。
    gui/TeachingPanelHeader.cpp
    # 新手引导组件（GuidedTour）——4 步高亮关键控件的"3 分钟 Hello World"闭环。
    # 依赖 Qt6::Widgets（QFrame/QLabel/QPushButton），使用 TeachingTheme 语义色，
    # 不依赖 QFluentKit / IdeController / 引擎层，不加入测试目标（GUI 组件难以单元测试）。
    gui/GuidedTour.cpp
    # 术语表面板——集中展示核心术语（NaN-boxing/COW/upvalue/SSA 等 30 条），
    # 左侧字母序列表 + 顶部搜索框 + 右侧 Markdown 详情，支持 term: 链接跳转。
    # 依赖 Qt6::Widgets（QLineEdit/QListWidget/QTextBrowser）+ MarkdownRenderer +
    # TeachingTheme + PanelAnimator，不依赖 IdeController / 引擎层，纯静态数据，不加入测试目标。
    gui/GlossaryPanel.cpp
    # 教学面板树形导航——替代 LearningHubDialog 弹窗，左侧停靠区的可折叠树形导航，
    # 4 大分类 + 顶部「代码编辑器」入口，点击叶子节点切换中央 centerStack_。
    # P2-1 fix: 4 大分类数据迁移到 PanelCatalog，本文件仅负责树构建与交互。
    # 依赖 Qt6::Widgets（QTreeWidget/QTreeWidgetItem），不依赖 IdeController / 引擎层，
    # 不加入测试目标（GUI 组件难以单元测试）。
    gui/TeachingTreePanel.cpp
    # 教学面板拓展（第四波）：观察表达式 + 执行时间轴 + 崩溃报告
    gui/WatchExpressionLibrary.cpp
    gui/WatchPanel.cpp
    # R161: 数据断点面板（Watchpoint，监视变量 / 字段被修改时暂停）
    gui/WatchpointPanel.cpp
    gui/ExecutionTimelinePanel.cpp
    gui/CrashReportDialog.cpp
)

# ============================================================
# INTERFACE 目标：统一的 MSVC 编译选项
# 修复问题 8（编译选项重复未抽取）
# ============================================================
add_library(minilang_compile_options INTERFACE)
if(MSVC)
    target_compile_options(minilang_compile_options INTERFACE
        /W3
        /utf-8
        /EHsc
        /permissive-
        /wd4996
        /MP
        # C1: /Zf — MSVC 19.20+ 支持"更快 PCH"，允许多个 CL 进程共享同一 PCH 缓存，
        # 显著降低 minilang_core + minilang_ide 联合编译时的 PCH 重复生成开销。
        # 配合 Ninja 多任务并行编译时收益最大（典型场景冷构建 -15%）。
        /Zf
    )
    if(MINILANG_WERROR)
        target_compile_options(minilang_compile_options INTERFACE /WX)
    endif()
endif()

# ============================================================
# 辅助函数：配置 minilang_core 目标的公共属性
# 调用方负责先创建 minilang_core 目标，传入目标名即可。
# ============================================================
function(minilang_configure_core_target target_name root_dir)
    target_include_directories(${target_name} PUBLIC
        ${root_dir}
        ${root_dir}/common
    )
    target_link_libraries(${target_name} PUBLIC Qt6::Core)
    target_precompile_headers(${target_name} PRIVATE ${root_dir}/common/pch.h)
    target_link_libraries(${target_name} PRIVATE minilang_compile_options)
endfunction()
