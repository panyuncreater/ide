# ============================================================
# minilang_core.cmake — 共享的源文件列表与编译配置
# ------------------------------------------------------------
# 由根 CMakeLists.txt 和 tests/CMakeLists.txt（独立构建路径）
# 共同 include，消除源文件列表重复维护问题。
# 修复问题 2（独立构建路径重复维护源文件列表）。
#
# P1-6 模块化 target 拆分（2026-07-24）：
# 原单一 minilang_core STATIC 库（25+ .cpp）拆分为 4 个 OBJECT
# 子库 + 1 个 STATIC 聚合，加速增量编译与并行编译：
#   minilang_core_base     — 强连通核心（common+ast+interpreter+debug/DebugController）
#   minilang_frontend      — 前端工具集（lexer+parser+formatter+lint+doc）
#   minilang_backend       — 编译器/三后端（compiler/*.cpp，变更最频繁）
#   minilang_guibridge     — app 桥接层（MagicCommands+ExecutionTraceRecorder，含 SKIP_AUTOMOC）
#   minilang_core (STATIC) — 聚合上述 4 个 OBJECT 库的对象，对外接口不变
#
# 拆分依据（依赖分析）：
#   - {common, ast, interpreter, debug/DebugController} 构成头文件级强连通分量
#     （TypeChecker.h→Value.h、IBackend.h→Value.h、Value.h→RuntimeLimits.h 等双向循环），
#     不可再拆分，合并为 minilang_core_base。
#   - compiler/ 单向依赖 core_base + frontend，独立成 minilang_backend。
#   - lexer/parser/formatter/lint/doc 仅依赖 base，独立成 minilang_frontend。
#   - gui/MagicCommands.cpp + debug/ExecutionTraceRecorder.cpp 传递包含 app/VmStepper.h
#     （Q_OBJECT），需 SKIP_AUTOMOC 避免 LNK2005，独立成 minilang_guibridge。
# ============================================================

# 推导项目根目录（兼容父项目 include 与 tests/ 独立构建）
if(NOT DEFINED MINILANG_ROOT_DIR)
    set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ============================================================
# 源文件列表按子库分组（相对 MINILANG_ROOT_DIR 的路径）
# ============================================================

# --- minilang_core_base：强连通核心（不可拆分） ---
# common/ + ast/ + interpreter/ + debug/DebugController + gui/ErrorHintEngine（纯 STL 叶子）
# 这些目录间存在头文件级双向循环依赖：
#   common/TypeChecker.h → interpreter/Value.h → common/RuntimeLimits.h
#   common/IBackend.h → interpreter/Value.h
#   ast/ASTNode.cpp → interpreter/Value.h；interpreter/Visitor.h → ast/ASTNode.h
#   debug/DebugController.h → interpreter/Value.h；interpreter/*.cpp → debug/DebugController.h
set(MINILANG_CORE_BASE_SOURCES
    ${MINILANG_ROOT_DIR}/common/BuiltinModules.cpp
    ${MINILANG_ROOT_DIR}/common/TypeChecker.cpp
    ${MINILANG_ROOT_DIR}/common/CrashHandler.cpp
    # ARCH-10: 内存检视只读 API（GUI 面板通过此 API 访问 NaNBox/RefCounted/GcManager，
    # 消除对 interpreter/ 内部头文件的直接依赖）
    # 仅依赖 interpreter/* 内部头文件（与 base 已有强连通核心同层），保留在 base。
    ${MINILANG_ROOT_DIR}/common/MemoryInspectionAPI.cpp
    ${MINILANG_ROOT_DIR}/ast/ASTNode.cpp
    ${MINILANG_ROOT_DIR}/ast/MacroExpander.cpp
    ${MINILANG_ROOT_DIR}/ast/ModuleIsolation.cpp
    ${MINILANG_ROOT_DIR}/interpreter/Interpreter.cpp
    ${MINILANG_ROOT_DIR}/interpreter/InterpreterCalls.cpp
    ${MINILANG_ROOT_DIR}/interpreter/InterpreterClasses.cpp
    ${MINILANG_ROOT_DIR}/interpreter/InterpreterModules.cpp
    ${MINILANG_ROOT_DIR}/interpreter/InterpreterCoroutine.cpp
    ${MINILANG_ROOT_DIR}/interpreter/BuiltinMethods.cpp
    ${MINILANG_ROOT_DIR}/interpreter/Value.cpp
    ${MINILANG_ROOT_DIR}/interpreter/GcManager.cpp
    ${MINILANG_ROOT_DIR}/debug/DebugController.cpp
    # gui/ErrorHintEngine.cpp 纯 STL 叶子（无项目内依赖），放 base 避免单独成库
    ${MINILANG_ROOT_DIR}/gui/ErrorHintEngine.cpp
)

# --- minilang_frontend：前端工具集（单向依赖 base） ---
# lexer/parser/formatter/lint/doc 互无耦合，仅共享 base 的 Value/ASTNode
set(MINILANG_FRONTEND_SOURCES
    ${MINILANG_ROOT_DIR}/lexer/Lexer.cpp
    ${MINILANG_ROOT_DIR}/parser/Parser.cpp
    ${MINILANG_ROOT_DIR}/formatter/Formatter.cpp
    ${MINILANG_ROOT_DIR}/lint/LintPass.cpp
    ${MINILANG_ROOT_DIR}/doc/DocGenerator.cpp
)

# --- minilang_backend：编译器/三后端（单向依赖 base + frontend） ---
# compiler/*.cpp 变更最频繁（IR/VM/RegisterVM/JIT），独立成 target 加速增量编译
# P1-2 拆分规划：待巨型文件拆分后新增的编译单元在此添加：
#   JIT.cpp → JIT.cpp + JITCodeGen.cpp + JITTiering.cpp + JITRuntime.cpp + JITClosure.cpp
#   IR.cpp  → IR.cpp + AstIRBuilder.cpp + BytecodeIRBackend.cpp + IROptPasses.cpp
#   Compiler.cpp → Compiler.cpp + CompilerExpr.cpp + CompilerStmt.cpp + CompilerClass.cpp
#   RegisterVM.cpp → RegisterVM.cpp + RegisterVMExec.cpp + RegisterVMCalls.cpp
set(MINILANG_BACKEND_SOURCES
    ${MINILANG_ROOT_DIR}/compiler/Bytecode.cpp
    ${MINILANG_ROOT_DIR}/compiler/Compiler.cpp
    ${MINILANG_ROOT_DIR}/compiler/CompilerExpr.cpp
    ${MINILANG_ROOT_DIR}/compiler/CompilerStmt.cpp
    ${MINILANG_ROOT_DIR}/compiler/CompilerClass.cpp
    ${MINILANG_ROOT_DIR}/compiler/VM.cpp
    ${MINILANG_ROOT_DIR}/compiler/VMCalls.cpp
    ${MINILANG_ROOT_DIR}/compiler/VMContainers.cpp
    ${MINILANG_ROOT_DIR}/compiler/IR.cpp
    ${MINILANG_ROOT_DIR}/compiler/AstIRBuilder.cpp
    ${MINILANG_ROOT_DIR}/compiler/BytecodeIRBackend.cpp
    ${MINILANG_ROOT_DIR}/compiler/IROptPasses.cpp
    ${MINILANG_ROOT_DIR}/compiler/IRSSA.cpp
    ${MINILANG_ROOT_DIR}/compiler/RegisterBytecode.cpp
    ${MINILANG_ROOT_DIR}/compiler/RegisterBytecodeBackend.cpp
    ${MINILANG_ROOT_DIR}/compiler/RegisterVM.cpp
    ${MINILANG_ROOT_DIR}/compiler/RegisterVMExec.cpp
    ${MINILANG_ROOT_DIR}/compiler/RegisterVMCalls.cpp
    # P1-7: 字节码磁盘缓存（CompileResult 序列化，跳过重复编译）
    ${MINILANG_ROOT_DIR}/compiler/BytecodeCache.cpp
    # ARCH-10: 后端执行服务中间层（GUI 面板通过此服务触发编译执行，
    # 消除对 compiler/VM/RegisterVM/Interpreter/Lexer/Parser 内部头文件的直接依赖）
    # 放在 backend 子库而非 base 的原因：依赖 lexer/parser/compiler/VM/RegisterVM，
    # 这些均位于 frontend/backend 层，base 不应反向依赖；同时避免与 base 中
    # CrashHandler.cpp（含 <windows.h>）在 Unity Build 同一 TU 中冲突——
    # windows.h 经某些 SDK 链路引入的宏会污染 lexer/Token.h 的 TokenType/type 字段。
    ${MINILANG_ROOT_DIR}/common/BackendExecutionService.cpp
)

# JIT 后端源文件（条件编译，MINILANG_USE_JIT = ON 时包含）
if(MINILANG_USE_JIT)
    list(APPEND MINILANG_BACKEND_SOURCES
        ${MINILANG_ROOT_DIR}/compiler/JIT.cpp
        ${MINILANG_ROOT_DIR}/compiler/JITCodeGen.cpp
        ${MINILANG_ROOT_DIR}/compiler/JITCodeGenHelpers.cpp
        ${MINILANG_ROOT_DIR}/compiler/JITRuntime.cpp
        ${MINILANG_ROOT_DIR}/compiler/JITTiering.cpp
        ${MINILANG_ROOT_DIR}/compiler/JITClosure.cpp
    )
endif()

# ARM64 JIT 后端源文件（条件编译，MINILANG_USE_JIT_A64 = ON 时包含）
if(MINILANG_USE_JIT_A64)
    list(APPEND MINILANG_BACKEND_SOURCES
        ${MINILANG_ROOT_DIR}/compiler/JITA64CodeGen.cpp
    )
endif()

# --- minilang_guibridge：app 桥接层（含 SKIP_AUTOMOC） ---
# gui/MagicCommands.cpp + debug/ExecutionTraceRecorder.cpp 传递包含 app/VmStepper.h
# （Q_OBJECT 派生类），AUTOMOC 会为这些 TU 生成 moc_VmStepper.cpp，与 minilang_ide/tests
# 中的 VmStepper.cpp 重复（LNK2005）。SKIP_AUTOMOC 让本 target 跳过 MOC 扫描，
# MOC 元对象符号由消费方（minilang_ide/tests）的 VmStepper.cpp 提供。
set(MINILANG_GUIBRIDGE_SOURCES
    ${MINILANG_ROOT_DIR}/gui/MagicCommands.cpp
    ${MINILANG_ROOT_DIR}/debug/ExecutionTraceRecorder.cpp
)

# 完整源文件列表（向后兼容，供需要全量源文件的场景使用）
set(MINILANG_CORE_SOURCES
    ${MINILANG_CORE_BASE_SOURCES}
    ${MINILANG_FRONTEND_SOURCES}
    ${MINILANG_BACKEND_SOURCES}
    ${MINILANG_GUIBRIDGE_SOURCES}
)

# GUI 模块源文件（仅 minilang_ide 使用，不属于 core）
# 新增教学面板时需手动添加 .cpp 到此列表（CMake 不使用 file(GLOB) 以遵循最佳实践）。
# 面板注册流程参见 docs/development.md「新增教学面板指南」：
#   1. 创建 gui/XxxPanel.cpp/.h
#   2. 添加 .cpp 到此 MINILANG_GUI_SOURCES 列表
#   3. 在 gui/PanelCatalog.cpp 注册面板元数据（id/label/emoji/category）
#   4. 在 app/ide.cpp registerLazyTeachingPanels() 的对应 register*Panels helper 中
#      调用 registrar("panel-id", mlTr("标题"), [this]() { ... return panel; })
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
    gui/MemoryModelLibrary.cpp
    gui/IRTransformPanel.cpp
    # P3-20: IRTransformPanel 拆分 — 静态教学场景库独立成库
    gui/IRTransformLibrary.cpp
    gui/IROptReplayLibrary.cpp
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
    # 拓展二期·平台：代码片段分享链接编解码（纯 Qt6::Core，可被测试目标链接）
    gui/ShareCodec.cpp
)

# ============================================================
# INTERFACE 目标：统一的编译选项
# 修复问题 8（编译选项重复未抽取）
# P1-6 fix: GCC/Clang 添加 -Wall -Wextra -Wpedantic（原仅 MSVC /W3）
# P1-8 fix: MSVC 警告级别选项化（MINILANG_W4 升级到 /W4 + 移除 /wd4996）
# ============================================================
add_library(minilang_compile_options INTERFACE)
if(MSVC)
    target_compile_options(minilang_compile_options INTERFACE
        /utf-8
        /EHsc
        /permissive-
        /MP
        # C1: /Zf — MSVC 19.20+ 支持"更快 PCH"，允许多个 CL 进程共享同一 PCH 缓存，
        # 显著降低 minilang_core + minilang_ide 联合编译时的 PCH 重复生成开销。
        # 配合 Ninja 多任务并行编译时收益最大（典型场景冷构建 -15%）。
        /Zf
    )
    # P1-8 (W1 收尾): 警告级别选项化。
    # 默认 /W3 + /wd4996（抑制 snprintf 等 C4996 弃用警告）。
    # MINILANG_W4=ON 升级到 /W4 并释放 /wd4996——windows-msvc-debug/release
    # preset 已默认启用 /W4 + /WX（零警告基线），CMakeLists.txt option 默认 OFF
    # 供非 preset 构建（如独立 tests/ 路径）回退到 /W3。
    if(MINILANG_W4)
        target_compile_options(minilang_compile_options INTERFACE /W4)
    else()
        target_compile_options(minilang_compile_options INTERFACE /W3 /wd4996)
    endif()
    if(MINILANG_WERROR)
        target_compile_options(minilang_compile_options INTERFACE /WX)
    endif()
    # 拓展计划·基建：Release 构建生成 PDB 调试符号（崩溃报告符号化链路）。
    # CrashHandler (R128) 崩溃时写 minidump (.dmp)，若 Release 无 PDB 则 .dmp
    # 无法解析出源码级栈回溯。选 /Z7（调试信息嵌入 obj）而非 /Zi：
    #   - /Zi 的 mspdbsrv 共享写 PDB 与 ccache 不兼容（CI 缓存失效）
    #   - /Z7 下链接器仍由 /DEBUG 产出最终 PDB，效果等价
    # /OPT:REF,ICF 恢复 /DEBUG 默认关闭的链接优化，二进制大小/性能不变。
    # 配套：release.yml 上传符号包 + scripts/analyze_minidump.ps1 本地解析。
    option(MINILANG_RELEASE_SYMBOLS "Generate PDB symbols in Release builds (crash report symbolication)" ON)
    if(MINILANG_RELEASE_SYMBOLS)
        target_compile_options(minilang_compile_options INTERFACE $<$<CONFIG:Release>:/Z7>)
        target_link_options(minilang_compile_options INTERFACE
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    endif()
else()
    # P1-6: GCC/Clang 警告级别。原实现非 MSVC 平台无任何警告标志，
    # 导致 Linux/macOS 构建几乎无静态检查。-Wall -Wextra -Wpedantic
    # 对齐 MSVC /W3 的覆盖面，捕获隐式转换 / 未用变量 / 可疑转换等问题。
    target_compile_options(minilang_compile_options INTERFACE -Wall -Wextra -Wpedantic)
    if(MINILANG_WERROR)
        target_compile_options(minilang_compile_options INTERFACE -Werror)
    endif()
endif()

# ============================================================
# 辅助函数：为指定目标启用预编译头（PCH）
# P1-6 fix: 抽取独立函数，使 minilang_ide / minilang_tests 也能复用 pch.h。
# pch.h 仅含标准库头（稳定），跨目标复用安全。各目标生成独立 PCH 缓存，
# /Zf 允许同目标内多个 CL 进程共享缓存。
# ============================================================
function(minilang_enable_pch target_name root_dir)
    target_precompile_headers(${target_name} PRIVATE ${root_dir}/common/pch.h)
endfunction()

# ============================================================
# 辅助函数：配置 OBJECT 子库的公共属性
# 每个 OBJECT 子库共享 root_dir + common/ include 目录、PCH、编译选项。
# ============================================================
function(minilang_configure_object_target target_name root_dir)
    target_include_directories(${target_name} PUBLIC
        ${root_dir}
        ${root_dir}/common
    )
    minilang_enable_pch(${target_name} ${root_dir})
    target_link_libraries(${target_name} PRIVATE minilang_compile_options)
endfunction()

# ============================================================
# 辅助函数：配置 minilang_core STATIC 聚合目标的公共属性
# 调用方负责先创建 minilang_core 目标，传入目标名即可。
# 保留原 minilang_configure_core_target 签名向后兼容。
# ============================================================
function(minilang_configure_core_target target_name root_dir)
    target_include_directories(${target_name} PUBLIC
        ${root_dir}
        ${root_dir}/common
    )
    target_link_libraries(${target_name} PUBLIC Qt6::Core)
    minilang_enable_pch(${target_name} ${root_dir})
    target_link_libraries(${target_name} PRIVATE minilang_compile_options)
endfunction()

# ============================================================
# 创建 4 个 OBJECT 子库 + minilang_core STATIC 聚合
# 用 if(NOT TARGET ...) 守卫，确保父项目与 tests/ 独立构建不会重复创建。
# 依赖方向（无环）：
#   base ──► frontend ──► backend
#     ▲          ▲          ▲
#     └──────────┴──────────┘
#                │
#          guibridge (需 base + frontend + backend 符号 + app/ 头)
#                │
#         minilang_core (STATIC 聚合)
# ============================================================
if(NOT TARGET minilang_core)

    # --- minilang_core_base：强连通核心 ---
    add_library(minilang_core_base OBJECT ${MINILANG_CORE_BASE_SOURCES})
    minilang_configure_object_target(minilang_core_base ${MINILANG_ROOT_DIR})
    target_link_libraries(minilang_core_base PUBLIC Qt6::Core)
    # ARCH-10 Unity Build 修复：minilang_core_base 内 common/CrashHandler.cpp
    # 包含 <windows.h>，经 SDK 链路引入的宏会污染 lexer/Token.h 的 TokenType
    # 字段（Token::type 与 windows.h 中 #define type 冲突 → C3646 未知重写说明符）。
    # 用 SKIP_UNITY_BUILD_INCLUSION 让 CrashHandler.cpp 独立编译（不进入 Unity batch），
    # 避免 windows.h 宏污染 batch 内其他 TU。AUTOMOC 必须保持开启，因为
    # debug/DebugController.h 包含 Q_OBJECT 宏，需要 moc 生成 metaObject/qt_metacall
    # 等元对象实现（关闭 AUTOMOC 会导致 LNK2001: pausedAt/logpointLogged/staticMetaObject）。
    set_source_files_properties(
        ${MINILANG_ROOT_DIR}/common/CrashHandler.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # 拓展二期修复：debug/DebugController.cpp 必须独立编译（排除出 Unity batch）。
    # test_harness/debug_test 依赖 MSVC「命令行 .obj 优先于 .lib 成员」规则用桩
    # DebugController.cpp.obj 压制 lib 中的真实实现——链接器仅在符号未解析时才
    # 拉取 lib 成员。若真实 DebugController 被 Unity 批入 unity_N_cxx.obj，与
    # Interpreter 等必需符号同 obj，链接器为解析后者必然拉入该 obj → LNK2005。
    # 独立编译后 lib 中的 DebugController.cpp.obj 不含其他必需符号，永不被拉取。
    # 注：正确属性是 SKIP_UNITY_BUILD_INCLUSION（源文件级），UNITY_BUILD 是 target 级属性。
    set_source_files_properties(
        ${MINILANG_ROOT_DIR}/debug/DebugController.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

    # --- minilang_frontend：前端工具集（依赖 base） ---
    add_library(minilang_frontend OBJECT ${MINILANG_FRONTEND_SOURCES})
    minilang_configure_object_target(minilang_frontend ${MINILANG_ROOT_DIR})
    target_link_libraries(minilang_frontend PUBLIC minilang_core_base)
    # ARCH-10 Unity Build 修复：lexer/parser/formatter/lint/doc 均无 Q_OBJECT，
    # 关闭 AUTOMOC 避免 mocs_compilation.cpp 污染 Unity batch（同 minilang_core_base）。
    set_target_properties(minilang_frontend PROPERTIES AUTOMOC OFF)

    # --- minilang_backend：编译器/三后端（依赖 base + frontend） ---
    add_library(minilang_backend OBJECT ${MINILANG_BACKEND_SOURCES})
    minilang_configure_object_target(minilang_backend ${MINILANG_ROOT_DIR})
    target_link_libraries(minilang_backend PUBLIC minilang_core_base minilang_frontend)
    # ARCH-10 Unity Build 修复：compiler/*.cpp 均无 Q_OBJECT，关闭 AUTOMOC
    # 避免 mocs_compilation.cpp 污染 Unity batch（同 minilang_core_base）。
    set_target_properties(minilang_backend PROPERTIES AUTOMOC OFF)
    # JIT 后端：链接 asmjit（条件依赖）
    if(MINILANG_USE_JIT)
        target_link_libraries(minilang_backend PUBLIC asmjit::asmjit)
    endif()

    # --- minilang_guibridge：app 桥接层（依赖 base + frontend + backend） ---
    add_library(minilang_guibridge OBJECT ${MINILANG_GUIBRIDGE_SOURCES})
    minilang_configure_object_target(minilang_guibridge ${MINILANG_ROOT_DIR})
    target_link_libraries(minilang_guibridge PUBLIC minilang_core_base minilang_frontend minilang_backend)
    # guibridge 的 cpp 传递包含 app/VmStepper.h（Q_OBJECT），需 app/ 在 include path
    target_include_directories(minilang_guibridge PRIVATE ${MINILANG_ROOT_DIR}/app)
    # SKIP_AUTOMOC：避免 AUTOMOC 为 MagicCommands.cpp / ExecutionTraceRecorder.cpp
    # 生成 moc_VmStepper.cpp，与 minilang_ide/tests 的 VmStepper.cpp 重复（LNK2005）。
    # P1-6 fix: 补全 MagicCommands.cpp 遗漏的 SKIP_AUTOMOC（原仅 ExecutionTraceRecorder.cpp 设置）。
    set_source_files_properties(
        ${MINILANG_ROOT_DIR}/gui/MagicCommands.cpp
        ${MINILANG_ROOT_DIR}/debug/ExecutionTraceRecorder.cpp
        PROPERTIES SKIP_AUTOMOC ON)

    # --- minilang_core：STATIC 聚合（对外接口不变） ---
    add_library(minilang_core STATIC
        $<TARGET_OBJECTS:minilang_core_base>
        $<TARGET_OBJECTS:minilang_frontend>
        $<TARGET_OBJECTS:minilang_backend>
        $<TARGET_OBJECTS:minilang_guibridge>
    )
    minilang_configure_core_target(minilang_core ${MINILANG_ROOT_DIR})

    # OBJECT 库的 PUBLIC 链接关系不会通过 $<TARGET_OBJECTS:...> 传递到 STATIC 聚合，
    # 需在 minilang_core 上显式声明外部依赖，确保消费方（minilang_ide/tests/cli）能链接到。
    # JIT 后端：链接 asmjit 静态库（条件依赖）
    if(MINILANG_USE_JIT)
        target_link_libraries(minilang_core PUBLIC asmjit::asmjit)
    endif()

    # C1: 应用 Unity Build 属性（选项在文件前部声明，此处目标已创建）
    # Unity Build 设置在各 OBJECT 子库上（STATIC 聚合不直接编译源文件）
    if(MINILANG_UNITY_BUILD)
        foreach(_sublib minilang_core_base minilang_frontend minilang_backend minilang_guibridge)
            set_target_properties(${_sublib} PROPERTIES
                UNITY_BUILD ON
                UNITY_BUILD_CODE_BEFORE_INCLUDE "// Unity build block"
                UNITY_BUILD_BATCH_SIZE 8)
        endforeach()
        # ARCH-10 Unity Build 修复：CrashHandler.cpp 包含 <windows.h>，经 SDK 链路
        # 拉入的宏（min/max/Polygon 等）会污染后续 #include "lexer/Token.h" 的
        # TokenType / Token::type 字段（C3646 未知重写说明符）。即使已定义
        # WIN32_LEAN_AND_MEAN + NOMINMAX + NOGDI 仍无法完全消除 windows.h 在
        # Unity batch 内的副作用（windows.h 会引入其他 #define 与 using 声明）。
        # 解法：将 CrashHandler.cpp 排除出 Unity batch，独立编译，使其副作用
        # 不外溢到同 batch 的其他 .cpp。set_source_files_properties 在 target
        # 级 UNITY_BUILD 之上对单文件覆盖。
        set_source_files_properties(
            ${MINILANG_ROOT_DIR}/common/CrashHandler.cpp
            PROPERTIES UNITY_BUILD OFF)
        message(STATUS "Unity build enabled for minilang_core sub-libraries (cold build optimization, batch=8)")
    endif()

endif()
