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
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # 查找 Qt6
    find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

    # 项目根目录
    set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # 核心静态库
    add_library(minilang_core STATIC
        ${MINILANG_ROOT_DIR}/common/TypeChecker.cpp
        ${MINILANG_ROOT_DIR}/lexer/Lexer.cpp
        ${MINILANG_ROOT_DIR}/ast/ASTNode.cpp
        ${MINILANG_ROOT_DIR}/parser/Parser.cpp
        ${MINILANG_ROOT_DIR}/interpreter/Environment.cpp
        ${MINILANG_ROOT_DIR}/interpreter/Interpreter.cpp
        ${MINILANG_ROOT_DIR}/interpreter/InterpreterCalls.cpp
        ${MINILANG_ROOT_DIR}/interpreter/InterpreterClasses.cpp
        ${MINILANG_ROOT_DIR}/interpreter/InterpreterModules.cpp
        ${MINILANG_ROOT_DIR}/interpreter/BuiltinMethods.cpp
        ${MINILANG_ROOT_DIR}/interpreter/Value.cpp
        ${MINILANG_ROOT_DIR}/interpreter/GcManager.cpp
        ${MINILANG_ROOT_DIR}/debug/DebugController.cpp
        ${MINILANG_ROOT_DIR}/compiler/Bytecode.cpp
        ${MINILANG_ROOT_DIR}/compiler/Compiler.cpp
        ${MINILANG_ROOT_DIR}/compiler/VM.cpp
        ${MINILANG_ROOT_DIR}/compiler/VMCalls.cpp
        ${MINILANG_ROOT_DIR}/compiler/VMContainers.cpp
        ${MINILANG_ROOT_DIR}/compiler/IR.cpp
        ${MINILANG_ROOT_DIR}/compiler/RegisterBytecode.cpp
        ${MINILANG_ROOT_DIR}/compiler/RegisterBytecodeBackend.cpp
        ${MINILANG_ROOT_DIR}/compiler/RegisterVM.cpp
        ${MINILANG_ROOT_DIR}/formatter/Formatter.cpp
    )

    target_include_directories(minilang_core PUBLIC
        ${MINILANG_ROOT_DIR}
        ${MINILANG_ROOT_DIR}/common
    )
    target_link_libraries(minilang_core PUBLIC Qt6::Core Qt6::Widgets)

    # S4 fix: 预编译头（PCH）—— 与父项目保持一致
    target_precompile_headers(minilang_core PRIVATE ${MINILANG_ROOT_DIR}/common/pch.h)

    if(MSVC)
        target_compile_options(minilang_core PRIVATE /W3 /utf-8 /EHsc /permissive- /wd4996)
    endif()
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
    message(STATUS "本地 GoogleTest 不存在，从 gitee 镜像下载")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
        GIT_TAG v1.14.0
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
    TestNaNBox.cpp
    TestIRShadowFix.cpp
    TestNestedLvalueProbe.cpp
    TestMethodCallProbe.cpp
    TestConsistencyDiff.cpp
    TestLexerParserAudit.cpp
)

target_link_libraries(minilang_tests PRIVATE
    minilang_core
    gtest_main
)

if(MSVC)
    target_compile_options(minilang_tests PRIVATE /W3 /utf-8 /EHsc /permissive- /wd4996)
    # AUDIT-P0 fix: 测试二进制栈扩至 8MB。Debug 模式下递归下降解析器的
    # 每个栈帧较大（~3KB），MAX_PARSE_DEPTH=256 对应 ~128 层括号 × 11 帧
    # × 3KB ≈ 4.2MB，需大于默认 1MB 栈才能让深度检查正常触发而非栈溢出。
    target_link_options(minilang_tests PRIVATE /STACK:8388608)
endif()

# ============================================================
# 注册 CTest 测试
# ============================================================
enable_testing()
add_test(NAME minilang_tests COMMAND minilang_tests)