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
    interpreter/Environment.cpp
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
)

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
    # 学习中心对话框——教学面板统一入口，依赖 QFluentKit（PushButton/CardWidget/Label），
    # 不依赖 IdeController / 引擎层，不加入测试目标。
    gui/LearningHubDialog.cpp
    # 教学面板统一标题栏组件——含帮助按钮与学习路径跳转，依赖 QFluentKit，
    # 不依赖 IdeController / 引擎层，不加入测试目标。
    gui/TeachingPanelHeader.cpp
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
