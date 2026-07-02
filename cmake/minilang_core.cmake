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
)

# GUI 模块源文件
set(MINILANG_GUI_SOURCES
    gui/CodeEditor.cpp
    gui/SyntaxHighlighter.cpp
    gui/AstViewer.cpp
    gui/OutputPanel.cpp
    gui/DebugPanel.cpp
    gui/ReplPanel.cpp
    gui/VmStackPanel.cpp
    gui/IrViewer.cpp
    gui/FindReplacePanel.cpp
    gui/ActivityBar.cpp
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
