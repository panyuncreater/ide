# ============================================================
# MiniLang IDE 构建脚本（CMake）
# ------------------------------------------------------------
# 唯一构建系统。旧 .vcxproj 已移除。
# ============================================================

cmake_minimum_required(VERSION 3.20)
project(MiniLangIDE VERSION 1.0 LANGUAGES CXX)

# C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 查找 Qt6（Core/Gui/Widgets）
# ============================================================
find_package(Qt6 6.0 COMPONENTS Core Gui Widgets REQUIRED)

# 自动处理 MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ============================================================
# 核心模块源文件（不含 GUI 和 main）
# ============================================================
set(MINILANG_CORE_SOURCES
    lexer/Lexer.cpp
    ast/ASTNode.cpp
    parser/Parser.cpp
    interpreter/Environment.cpp
    interpreter/Interpreter.cpp
    interpreter/BuiltinMethods.cpp
    debug/DebugController.cpp
    compiler/Compiler.cpp
    compiler/VM.cpp
    formatter/Formatter.cpp
)

# 核心模块需要 MOC 的头文件
set(MINILANG_CORE_MOC_HEADERS
    debug/DebugController.h
)

# ============================================================
# GUI 模块源文件
# ============================================================
set(MINILANG_GUI_SOURCES
    gui/CodeEditor.cpp
    gui/SyntaxHighlighter.cpp
    gui/AstViewer.cpp
    gui/OutputPanel.cpp
    gui/DebugPanel.cpp
    gui/ReplPanel.cpp
    gui/VmStackPanel.cpp
    gui/FindReplacePanel.cpp
)

# GUI 模块需要 MOC 的头文件
set(MINILANG_GUI_MOC_HEADERS
    app/ide.h
    app/IdeController.h
    app/InterpreterWorker.h
    gui/CodeEditor.h
    gui/AstViewer.h
    gui/OutputPanel.h
    gui/DebugPanel.h
    gui/SyntaxHighlighter.h
    gui/ReplPanel.h
    gui/VmStackPanel.h
    gui/FindReplacePanel.h
)

# ============================================================
# minilang_core 静态库（供 IDE 和测试复用）
# ============================================================
add_library(minilang_core STATIC
    ${MINILANG_CORE_SOURCES}
)

target_include_directories(minilang_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(minilang_core PUBLIC Qt6::Core)

if(MSVC)
    target_compile_options(minilang_core PRIVATE
        /W3
        /utf-8
        /EHsc
        /permissive-
        /wd4996
        /MP
    )
endif()

# ============================================================
# minilang_ide 可执行目标（GUI 应用）
# P1 fix: 链接 minilang_core 而非重复包含源文件，减少 ~50% 编译时间
# ============================================================
add_executable(minilang_ide WIN32
    app/main.cpp
    app/ide.cpp
    app/IdeController.cpp
    app/InterpreterWorker.cpp
    app/ide.qrc
    ${MINILANG_GUI_SOURCES}
)

target_include_directories(minilang_ide PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(minilang_ide PRIVATE
    minilang_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if(MSVC)
    target_compile_options(minilang_ide PRIVATE
        /W3
        /utf-8
        /EHsc
        /permissive-
        /wd4996
        /MP
    )
endif()

# Debug 配置生成调试信息
target_compile_definitions(minilang_ide PRIVATE $<$<CONFIG:Debug>:QT_DEBUG>)

# ============================================================
# 测试子目录（GoogleTest 单元测试）
# ============================================================
option(MINILANG_BUILD_TESTS "Build MiniLang unit tests" ON)
if(MINILANG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
# 安装规则（可选）
# ============================================================
install(TARGETS minilang_ide
    RUNTIME DESTINATION bin
)