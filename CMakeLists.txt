# ============================================================
# MiniLang IDE 构建脚本（CMake）
# ------------------------------------------------------------
# 唯一构建系统。旧 .vcxproj 已移除。
# ============================================================

cmake_minimum_required(VERSION 3.20)
project(MiniLangIDE VERSION 1.0 LANGUAGES CXX)

# ============================================================
# CMake 模块路径
# 修复问题 1（CMAKE_MODULE_PATH 未声明）
# ============================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 构建选项
# ============================================================
# 修复问题 10：Warnings as errors 选项化
option(MINILANG_WERROR "Treat warnings as errors" OFF)
# 修复问题 17：ccache 支持
option(MINILANG_USE_CCACHE "Use ccache/sccache to accelerate builds" OFF)
if(MINILANG_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(NOT CCACHE_PROGRAM)
        find_program(CCACHE_PROGRAM sccache)
    endif()
    if(CCACHE_PROGRAM)
        message(STATUS "Using compiler cache: ${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
        message(WARNING "MINILANG_USE_CCACHE=ON but neither ccache nor sccache found")
    endif()
endif()

# 修复问题 5：Release LTO 开关
option(MINILANG_ENABLE_LTO "Enable Link Time Optimization in Release builds" ON)
if(MINILANG_ENABLE_LTO AND MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
endif()

# ============================================================
# 查找 Qt6（Core/Gui/Widgets）
# ============================================================
find_package(Qt6 6.0 COMPONENTS Core Gui Widgets Svg Xml REQUIRED)

# 自动处理 MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORC ON)

# ============================================================
# 第三方依赖：Qt Advanced Docking System (ADS)
# ------------------------------------------------------------
# ADS 以静态库方式编入项目，不构建其示例程序。
# ADS_VERSION 必须手动设置（作为子目录时 git describe 不可用）。
# ============================================================
set(ADS_VERSION "5.0.0" CACHE STRING "ADS version (override git describe)" FORCE)
set(BUILD_STATIC ON CACHE BOOL "Build ADS as static library" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build ADS examples" FORCE)
add_subdirectory(third_party/ads)

# ============================================================
# 第三方依赖：QFluentKit (Fluent Design Widgets)
# ------------------------------------------------------------
# QFluent 构建为 SHARED 库。
# 保存/恢复 CMAKE_INSTALL_PREFIX，防止 QFluent 的 FORCE 设置污染父项目。
# ============================================================
set(_saved_install_prefix "${CMAKE_INSTALL_PREFIX}")
add_subdirectory(third_party/QFluentKit)
set(CMAKE_INSTALL_PREFIX "${_saved_install_prefix}" CACHE PATH "" FORCE)
unset(_saved_install_prefix)

# ============================================================
# 包含共享模块：源文件列表 + 编译选项 + 配置函数
# 修复问题 2（源文件列表重复维护）
# ============================================================
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/minilang_core.cmake)

# ============================================================
# minilang_core 静态库（供 IDE 和测试复用）
# ============================================================
add_library(minilang_core STATIC
    ${MINILANG_CORE_SOURCES}
)

minilang_configure_core_target(minilang_core ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================
# minilang_ide 可执行目标（GUI 应用）
# P1 fix: 链接 minilang_core 而非重复包含源文件，减少 ~50% 编译时间
# ============================================================
add_executable(minilang_ide WIN32
    app/main.cpp
    app/ide.cpp
    app/IdeController.cpp
    app/InterpreterWorker.cpp
    app/PipelineRunner.cpp
    app/WorkerManager.cpp
    app/DebugCoordinator.cpp
    app/VmStepper.cpp
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
    Qt6::Svg
    Qt6::Xml
    minilang_compile_options
    qtadvanceddocking-qt6
    QFluent
)

# Windows: 链接 dwmapi 用于深色标题栏 (DWMWA_USE_IMMERSIVE_DARK_MODE)
if(WIN32)
    target_link_libraries(minilang_ide PRIVATE dwmapi)
endif()

# 部署 QFluent 动态库到 exe 同目录（QFluent 为 SHARED 库，运行时需 DLL）
add_custom_command(TARGET minilang_ide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:QFluent>"
            "$<TARGET_FILE_DIR:minilang_ide>"
    COMMENT "Deploying QFluent runtime for minilang_ide..."
)

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
# test_harness 子目录（独立审计/压力测试工具）
# 修复问题 3（test_harness 未集成 CMake）
# ============================================================
option(MINILANG_BUILD_TEST_HARNESS "Build MiniLang test harness (audit/stress tools)" OFF)
if(MINILANG_BUILD_TEST_HARNESS)
    add_subdirectory(test_harness)
endif()

# ============================================================
# 安装规则
# ------------------------------------------------------------
# IDE 应用分发：安装可执行文件
# SDK 分发：安装 minilang_core 静态库 + 公共头文件目录
# 修复问题 B-2（安装规则不完整，原仅安装 minilang_ide）
# ============================================================
install(TARGETS minilang_ide
    RUNTIME DESTINATION bin
)

# 安装 minilang_core 静态库（供 SDK 用途链接）
install(TARGETS minilang_core
    ARCHIVE DESTINATION lib
)

# 安装公共头文件目录（保留子目录结构）
install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/ast
    ${CMAKE_CURRENT_SOURCE_DIR}/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/compiler
    ${CMAKE_CURRENT_SOURCE_DIR}/debug
    ${CMAKE_CURRENT_SOURCE_DIR}/formatter
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    DESTINATION include/minilang
    FILES_MATCHING PATTERN "*.h"
)

# ============================================================
# Windows 平台自动部署 Qt 运行时依赖
# ------------------------------------------------------------
# 构建后调用 windeployqt 把 Qt6 DLL（Qt6Core/Qt6Gui/Qt6Widgets）
# 与 platforms/qwindows.dll 插件复制到可执行文件同目录，
# 使 minilang_ide.exe / minilang_tests.exe 可直接双击运行，
# 无需手动将 Qt bin 目录加入 PATH。
# 修复问题 E-1/E-2/B-1（exe 缺少 Qt DLL 与平台插件）。
# ============================================================
if(WIN32 AND TARGET Qt6::qmake)
    # 通过 qmake 推导 windeployqt 路径（同 bin 目录）
    get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_location}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin_dir}")
    if(WINDEPLOYQT_EXE)
        message(STATUS "Found windeployqt: ${WINDEPLOYQT_EXE}")
        # 为 GUI 主程序部署 Qt 依赖（含 platforms 插件）
        # 注：minilang_tests 的 windeployqt 在 tests/CMakeLists.txt 中调用，
        # 因为 add_custom_command(TARGET ...) 要求目标在同一目录定义。
        add_custom_command(TARGET minilang_ide POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:minilang_ide>"
            COMMENT "Deploying Qt runtime for minilang_ide..."
        )
        # 修复问题 7：安装时也部署 Qt DLL 到安装目录
        install(CODE "
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXE}\"
                        --no-translations
                        --no-system-d3d-compiler
                        --no-opengl-sw
                        \"\${CMAKE_INSTALL_PREFIX}/bin/$<TARGET_FILE_NAME:minilang_ide>\"
                RESULT_VARIABLE _deploy_result
            )
            if(NOT _deploy_result EQUAL 0)
                message(WARNING \"windeployqt failed during install (code \${_deploy_result})\")
            endif()
        ")
    else()
        message(WARNING "windeployqt not found near Qt6 bin (${_qt_bin_dir}). "
                        "Built executables will need Qt bin in PATH to run.")
    endif()
endif()

# ============================================================
# Sanitizer 支持
# 修复问题 8：Address/Undefined Behavior Sanitizer 预设
# 用法：cmake -DMINILANG_SANITIZE=address ...  (MSVC: /fsanitize=address)
# ============================================================
set(MINILANG_SANITIZE "" CACHE STRING "Enable sanitizer: address, undefined")
if(MINILANG_SANITIZE STREQUAL "address" AND MSVC)
    target_compile_options(minilang_compile_options INTERFACE /fsanitize=address)
    target_link_options(minilang_compile_options INTERFACE /fsanitize=address)
endif()

# ============================================================
# CPack 打包
# 修复问题 13：生成安装包（ZIP / NSIS）
# 用法：
#   cmake --build out/build/release
#   cd out/build/release && cpack -G ZIP
# ============================================================
include(CPack)
set(CPACK_PACKAGE_NAME "MiniLangIDE")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "MiniLang")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++20 / Qt6 MiniLang IDE")
# 仅打包已安装的文件（cmake --install 之后）
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
endif()
