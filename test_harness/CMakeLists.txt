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

# ---- 辅助：链接 gtest（需要 test_harness 的调试器桩文件） ----
# 超类排查测试使用 gtest，需链接 minilang_tests 相同的 gtest 库
# 注意：gtest 目标由父项目 tests/CMakeLists.txt 的 add_subdirectory 创建

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
target_link_libraries(debug_test PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# super_notfound_test：超类方法未找到测试（gtest）
# 修复：链接 gtest 库
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

# ================================================================
# 以下源文件未编译（缺少 main / API 不匹配）：
#   - fmt_minimal.cpp / fmt_super.cpp / formatter_test.cpp：
#     格式化器单元测试片段，无 main()，需合并入口
# ================================================================
