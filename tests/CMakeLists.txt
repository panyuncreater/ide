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

    # 包含共享模块：源文件列表 + 编译选项 + 配置函数
    # 修复问题 2（源文件列表重复维护）
    include(${MINILANG_ROOT_DIR}/cmake/minilang_core.cmake)

    # 将源文件路径前缀为绝对路径（独立构建时 CMAKE_CURRENT_SOURCE_DIR 是 tests/）
    set(ABS_CORE_SOURCES "")
    foreach(src ${MINILANG_CORE_SOURCES})
        list(APPEND ABS_CORE_SOURCES "${MINILANG_ROOT_DIR}/${src}")
    endforeach()

    # 核心静态库
    add_library(minilang_core STATIC ${ABS_CORE_SOURCES})

    # 使用共享配置函数（统一 include dirs / link / PCH / 编译选项）
    minilang_configure_core_target(minilang_core ${MINILANG_ROOT_DIR})
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
    TestNaNBox.cpp
    TestIRShadowFix.cpp
    TestNestedLvalueProbe.cpp
    TestMethodCallProbe.cpp
    TestConsistencyDiff.cpp
    TestLexerParserAudit.cpp
)

target_link_libraries(minilang_tests PRIVATE
    minilang_core
    gtest
    minilang_compile_options
)

if(MSVC)
    # AUDIT-P0 fix: 测试二进制栈扩至 8MB。Debug 模式下递归下降解析器的
    # 每个栈帧较大（~3KB），MAX_PARSE_DEPTH=256 对应 ~128 层括号 × 11 帧
    # × 3KB ≈ 4.2MB，需大于默认 1MB 栈才能让深度检查正常触发而非栈溢出。
    target_link_options(minilang_tests PRIVATE /STACK:8388608)
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
    PROPERTIES
        TIMEOUT 300
)

# ============================================================
# Windows 平台自动部署 Qt 运行时依赖到测试可执行文件同目录
# ------------------------------------------------------------
# minilang_tests 链接了 Qt6::Widgets，运行时需要 Qt6 DLL。
# 此处与父项目 CMakeLists.txt 中 minilang_ide 的部署逻辑呼应，
# 保证测试二进制也可直接运行无需手动配置 PATH。
# 修复问题 E-1/E-2（测试 exe 缺少 Qt DLL）。
# ============================================================
if(WIN32 AND TARGET Qt6::qmake)
    get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_location}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin_dir}")
    if(WINDEPLOYQT_EXE)
        add_custom_command(TARGET minilang_tests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:minilang_tests>"
            COMMENT "Deploying Qt runtime for minilang_tests..."
        )
    endif()
endif()
