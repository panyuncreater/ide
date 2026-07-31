# ============================================================
# MiniLang test_harness 构建脚本
# ------------------------------------------------------------
# 独立审计/压力/一致性测试工具，原通过硬编码 .bat 编译，
# 现集成到 CMake 统一构建。修复已知限制：
#   - debug_test.cpp: include 路径优先 stub DebugController（避免 LNK2005）
#   - super_notfound_test.cpp: 链接 gtest
#   - 全部文件完成 Parser::getErrors() → getDiagnostics() API 迁移
#
# 默认不构建（MINILANG_BUILD_TEST_HARNESS=OFF），
# 需显式启用：cmake -DMINILANG_BUILD_TEST_HARNESS=ON ...
# ============================================================

# 项目根目录（test_harness 是子目录，根是上一级）
set(MINILANG_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---- 自主获取 gtest（修复审计问题 3：解耦对父项目 tests/ 的隐式依赖）----
# 若父项目 tests/ 已创建 gtest 目标（MINILANG_BUILD_TESTS=ON），直接复用；
# 若仅启用 test_harness 而禁用 tests，则自行从本地 submodule 或 gitee 镜像构建，
# 使 test_harness 不再因 add_subdirectory 顺序或 tests 禁用而构建失败。
if(NOT TARGET gtest)
    set(GOOGLETEST_LOCAL_DIR ${MINILANG_ROOT_DIR}/third_party/googletest)
    if(EXISTS "${GOOGLETEST_LOCAL_DIR}/CMakeLists.txt")
        message(STATUS "[test_harness] 使用本地 GoogleTest 源码: ${GOOGLETEST_LOCAL_DIR}")
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory(${GOOGLETEST_LOCAL_DIR}
            ${CMAKE_BINARY_DIR}/_deps/googletest-build EXCLUDE_FROM_ALL)
    else()
        message(STATUS "[test_harness] 本地 GoogleTest 不存在，从 gitee 镜像下载（浅克隆）")
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
            GIT_TAG v1.14.0
            GIT_SHALLOW TRUE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# ================================================================
# formatter_audit：格式化器深度审计（100+ 用例）
# 仅依赖 Lexer + Parser + Formatter
# ================================================================
add_executable(formatter_audit
    formatter_audit.cpp
)
target_link_libraries(formatter_audit PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# audit_verify：格式化器验证补充
# ================================================================
add_executable(audit_verify
    audit_verify.cpp
)
target_link_libraries(audit_verify PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# stress_test：压力测试
# ================================================================
add_executable(stress_test
    stress_test.cpp
)
target_link_libraries(stress_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# ================================================================
# debug_test：调试一致性审计
# 修复 LNK2005：桩实现已从头文件移到 debug/DebugController.cpp，
# 作为 .obj 直接链接到 debug_test.exe。MSVC 链接器优先使用
# .obj 中的符号（先于 minilang_core.lib 中的同名符号），
# 无多重定义冲突。AUTOMOC OFF：桩无 Q_OBJECT，不需 MOC。
# ================================================================
add_executable(debug_test
    debug_test.cpp
    debug/DebugController.cpp
)
target_include_directories(debug_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(debug_test PROPERTIES AUTOMOC OFF)
# GNU ld 默认对桩/真实实现的同名符号报 multiple definition（MSVC 则 .obj 优先）；
# --allow-multiple-definition 保留“命令行目标文件（桩）优先于库”语义，与 MSVC 行为对齐。
if(NOT MSVC)
    target_link_options(debug_test PRIVATE -Wl,--allow-multiple-definition)
endif()
target_link_libraries(debug_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# super_notfound_test：超类方法未找到测试（gtest）
# gtest 目标由上方“自主获取 gtest”逻辑保证存在（复用 tests/ 或自建），
# 不再隐式依赖父项目的 add_subdirectory 顺序。
# ================================================================
add_executable(super_notfound_test
    super_notfound_test.cpp
)
target_link_libraries(super_notfound_test PRIVATE
    minilang_core
    gtest
    minilang_compile_options
)

# ================================================================
# super_init_test：超类 init 测试
# ================================================================
add_executable(super_init_test
    super_init_test.cpp
)
target_link_libraries(super_init_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# diff_test：三后端差分测试（独立版）
# ================================================================
add_executable(diff_test
    diff_test.cpp
)
target_link_libraries(diff_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# ast_test：AST 双向解析一致性校验
# ================================================================
add_executable(ast_test
    ast_test.cpp
)
target_link_libraries(ast_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# test_harness_runner：通用测试运行器
# ================================================================
add_executable(test_harness_runner
    test_main.cpp
)
target_link_libraries(test_harness_runner PRIVATE
    minilang_core
    minilang_compile_options
)
