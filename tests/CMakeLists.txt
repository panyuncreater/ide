# ============================================================
# MiniLang 单元测试构建脚本
# ------------------------------------------------------------
# 作为父项目 CMakeLists.txt 的子目录被添加。
# 也可独立构建（standalone）：cmake -B build && cmake --build build
# ============================================================

# 支持独立构建（不通过父项目 add_subdirectory 时）
if(NOT TARGET minilang_core)
    cmake_minimum_required(VERSION 3.20)
    project(MiniLangTests CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # 查找 Qt6
    find_package(Qt6 COMPONENTS Core REQUIRED)

    # 项目根目录
    set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # 核心静态库
    add_library(minilang_core STATIC
        ${MINILANG_ROOT_DIR}/lexer/Lexer.cpp
        ${MINILANG_ROOT_DIR}/ast/ASTNode.cpp
        ${MINILANG_ROOT_DIR}/parser/Parser.cpp
        ${MINILANG_ROOT_DIR}/interpreter/Environment.cpp
        ${MINILANG_ROOT_DIR}/interpreter/Interpreter.cpp
        ${MINILANG_ROOT_DIR}/interpreter/BuiltinMethods.cpp
        ${MINILANG_ROOT_DIR}/debug/DebugController.cpp
        ${MINILANG_ROOT_DIR}/compiler/Compiler.cpp
        ${MINILANG_ROOT_DIR}/compiler/VM.cpp
        ${MINILANG_ROOT_DIR}/formatter/Formatter.cpp
    )

    target_include_directories(minilang_core PUBLIC ${MINILANG_ROOT_DIR})
    target_link_libraries(minilang_core PUBLIC Qt6::Core)

    if(MSVC)
        target_compile_options(minilang_core PRIVATE /W3 /utf-8 /EHsc /permissive- /wd4996)
    endif()
endif()

# ============================================================
# 通过 FetchContent 拉取 GoogleTest v1.14.0
# ============================================================
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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
)

target_link_libraries(minilang_tests PRIVATE
    minilang_core
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(minilang_tests PRIVATE /W3 /utf-8 /EHsc /permissive- /wd4996)
endif()

# ============================================================
# 注册 CTest 测试
# ============================================================
enable_testing()
add_test(NAME minilang_tests COMMAND minilang_tests)