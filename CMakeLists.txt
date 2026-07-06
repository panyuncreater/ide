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

# C1: 增量编译缓存优化 — Unity Build 批量编译（首次构建加速 30-50%）
# 启用后将 minilang_core 的多个 .cpp 合并为单个 TU 编译，减少 PCH 重复加载与
# 模板实例化开销。适用于 CI 与首次冷构建；增量构建时关闭以获得更细的依赖分析粒度。
# 选项声明：在 minilang_core 目标创建后应用 UNITY_BUILD 属性。
option(MINILANG_UNITY_BUILD "Batch-compile minilang_core as a single translation unit (cold build speedup)" OFF)

# C1: Ninja 响应文件 — 当源文件较多时通过 .rsp 文件传递编译参数，
# 避免 Windows 命令行 32K 限制与减少进程启动开销。
if(CMAKE_GENERATOR STREQUAL "Ninja" OR CMAKE_GENERATOR MATCHES "Ninja Multi-Config")
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Force Ninja to use response files" FORCE)
endif()

# C1: Doxygen API 文档生成（D1 配套）
option(MINILANG_BUILD_DOCS "Build API documentation via Doxygen" OFF)
if(MINILANG_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        message(STATUS "Doxygen found: API docs target 'docs' enabled")
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating MiniLang API documentation with Doxygen"
            VERBATIM
        )
    else()
        message(WARNING "MINILANG_BUILD_DOCS=ON but Doxygen not found; target 'docs' disabled")
    endif()
endif()

# D2: 国际化 i18n（Qt Linguist）—— 提取用户可见字符串到 .ts 翻译文件
option(MINILANG_ENABLE_I18N "Enable Qt Linguist i18n infrastructure (tr() wrapping + .ts catalog)" OFF)
if(MINILANG_ENABLE_I18N)
    add_compile_definitions(MINILANG_ENABLE_I18N)
    find_package(Qt6 COMPONENTS LinguistTools QUIET)
    if(Qt6LinguistTools_FOUND)
        message(STATUS "i18n enabled: Qt Linguist tools available")
        # 翻译源文件目录（占位，后续 lupdate 自动维护条目）
        set(MINILANG_TRANSLATION_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/app/translations/minilang_zh_CN.ts
        )
        # qt_create_translation 会在构建时运行 lupdate 同步 .ts 条目，
        # 并 lrelease 编译为 .qm（仅源码中 tr() 包裹的字符串会被提取）
        qt_create_translation(MINILANG_QM_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/app
            ${CMAKE_CURRENT_SOURCE_DIR}/gui
            ${MINILANG_TRANSLATION_SOURCES}
        )
        # 注册 .qm 文件到 IDE 目标的输出目录
        add_custom_target(minilang_translations ALL DEPENDS ${MINILANG_QM_FILES})
    else()
        message(WARNING "MINILANG_ENABLE_I18N=ON but Qt6::LinguistTools not found; i18n infrastructure disabled")
    endif()
endif()

# 修复问题 5：Release LTO 开关
option(MINILANG_ENABLE_LTO "Enable Link Time Optimization in Release builds" ON)
if(MINILANG_ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# C3: VM 解释循环热路径 profiling — 启用后 VM 记录每条 opcode 的执行计数，
# 用于识别热路径并指导手动内联/特殊化优化。默认 OFF（避免 ~1ns/指令的计数开销）。
# 用法：cmake -DMINILANG_VM_PROFILING=ON ...，运行后调用 vm.getOpCodeProfile() 取统计。
option(MINILANG_VM_PROFILING "Enable VM opcode execution profiling (per-instruction counters)" OFF)
if(MINILANG_VM_PROFILING)
    add_compile_definitions(MINILANG_VM_PROFILING)
    message(STATUS "VM opcode profiling enabled (MINILANG_VM_PROFILING)")
endif()

# ============================================================
# 查找 Qt6（Core/Gui/Widgets）
# ============================================================
find_package(Qt6 6.0 COMPONENTS Core Gui Widgets Svg Xml REQUIRED)

# C2: QtCharts 可选依赖 — 启用后 ProfileDashboardPanel 使用 QChart 替代 QPainter 自绘柱状图，
# 视觉更专业（自带 tooltip/legend/animation），但增加 Qt6::Charts 依赖。
# 默认 OFF 保持项目零额外依赖；用户可通过 -DMINILANG_USE_QTCHARTS=ON 启用。
option(MINILANG_USE_QTCHARTS "Use QtCharts for professional bar charts in ProfileDashboardPanel" OFF)
if(MINILANG_USE_QTCHARTS)
    find_package(Qt6 COMPONENTS Charts QUIET)
    if(Qt6Charts_FOUND)
        message(STATUS "QtCharts enabled: ProfileDashboardPanel will use QChart")
        add_compile_definitions(MINILANG_HAVE_QTCHARTS)
    else()
        message(WARNING "MINILANG_USE_QTCHARTS=ON but Qt6::Charts not found; falling back to QPainter")
        set(MINILANG_USE_QTCHARTS OFF CACHE BOOL "" FORCE)
    endif()
endif()

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

# C1: 应用 Unity Build 属性（选项在文件前部声明，此处目标已创建）
if(MINILANG_UNITY_BUILD)
    set_target_properties(minilang_core PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_CODE_BEFORE_INCLUDE "// Unity build block"
        UNITY_BUILD_BATCH_SIZE 8)
    message(STATUS "Unity build enabled for minilang_core (cold build optimization, batch=8)")
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
    app/PipelineRunner.cpp
    app/WorkerManager.cpp
    app/DebugCoordinator.cpp
    app/VmStepper.cpp
    app/ide.qrc
    ${MINILANG_GUI_SOURCES}
)

target_include_directories(minilang_ide PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # QFluentKit 组件头文件（PushButton.h / Label.h / CardWidget.h 等）位于 src/QFluent/
    # 子目录，而 QFluent 目标仅导出 src/。补充此路径以支持 #include "PushButton.h" 风格。
    # 注：QFluentKit 为 git submodule，不修改其内部 CMakeLists。
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/QFluentKit/QFluent/src/QFluent
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

# C2: 启用 QtCharts 时链接到 IDE 目标
if(MINILANG_USE_QTCHARTS)
    target_link_libraries(minilang_ide PRIVATE Qt6::Charts)
endif()

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

# 安装公共头文件目录
# 注：当前仅安装 common/ 下的公共 API 头文件。
# 内部模块（lexer/parser/interpreter/compiler 等）不作为 SDK 暴露。
# 如需完整 SDK，可在 include/minilang/public/ 下整理公共 API 头文件。
install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    DESTINATION include/minilang
    FILES_MATCHING PATTERN "*.h"
)

# ============================================================
# Windows 平台自动部署 Qt 运行时依赖
# ------------------------------------------------------------
# 使用共享模块 cmake/windeployqt.cmake，消除与 tests/ 的重复。
# ============================================================
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/windeployqt.cmake)
minilang_deploy_qt_runtime(minilang_ide)

# 安装时也部署 Qt DLL 到安装目录
if(WIN32 AND MINILANG_WINDEPLOYQT_EXE)
    install(CODE "
        execute_process(
            COMMAND \"${MINILANG_WINDEPLOYQT_EXE}\"
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
endif()

# ============================================================
# Sanitizer 支持（跨平台）
# 用法: cmake -DMINILANG_SANITIZE=address ...
# ============================================================
set(MINILANG_SANITIZE "" CACHE STRING "Enable sanitizer: address, undefined, both")
if(MINILANG_SANITIZE)
    if(MINILANG_SANITIZE STREQUAL "address")
        if(MSVC)
            target_compile_options(minilang_compile_options INTERFACE /fsanitize=address)
            target_link_options(minilang_compile_options INTERFACE /fsanitize=address)
        else()
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=address)
        endif()
    elseif(MINILANG_SANITIZE STREQUAL "undefined")
        if(NOT MSVC)
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=undefined -fno-omit-frame-pointer)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=undefined)
        else()
            message(WARNING "MINILANG_SANITIZE=undefined has limited support on MSVC")
        endif()
    elseif(MINILANG_SANITIZE STREQUAL "both")
        if(MSVC)
            target_compile_options(minilang_compile_options INTERFACE /fsanitize=address)
            target_link_options(minilang_compile_options INTERFACE /fsanitize=address)
        else()
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=address,undefined)
        endif()
    else()
        message(FATAL_ERROR "Unknown MINILANG_SANITIZE value: ${MINILANG_SANITIZE}"
                " Supported: address, undefined, both")
    endif()
endif()

# ============================================================
# CPack 打包
# 修复问题 13：生成安装包（ZIP / NSIS）
# 用法：
#   cmake --build out/build/release
#   cd out/build/release && cpack -G ZIP
# ============================================================
set(CPACK_PACKAGE_NAME "MiniLangIDE")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "MiniLang")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++20 / Qt6 MiniLang IDE")
# 仅打包已安装的文件（cmake --install 之后）
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()
include(CPack)
