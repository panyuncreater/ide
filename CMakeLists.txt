# ============================================================
# MiniLang IDE 构建脚本（CMake）
# ------------------------------------------------------------
# 唯一构建系统。旧 .vcxproj 已移除。
# ============================================================

cmake_minimum_required(VERSION 3.25)
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

# P1-6: 导出 compile_commands.json 供 clangd/clang-tidy 工具链使用。
# Ninja/Makefile 生成器支持；VS 生成器不支持（IDE 内置 IntelliSense）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 构建选项
# ============================================================
# 修复问题 10：Warnings as errors 选项化
# P1-8 (W1 收尾): /W4 警告清零已完成，windows-msvc-debug/release preset 默认启用 /W4 + /WX。
# C2 (2026-07-25 P3-A2 配套): GCC/Clang 警告清零已完成，linux-gcc-debug/release 与
# macos-clang-debug/release preset 全部启用 -Wall -Wextra -Wpedantic -Werror 零警告基线
# （原仅 release preset 启用 -Werror，现 debug preset 一致启用，三平台 CI 矩阵
# 在 Debug/Release 两种配置下均强制零警告）。
option(MINILANG_WERROR "Treat warnings as errors" OFF)
option(MINILANG_W4 "Enable MSVC /W4 warning level and release /wd4996 (requires warning cleanup)" OFF)
# 修复问题 17：ccache 支持
option(MINILANG_USE_CCACHE "Use ccache/sccache to accelerate builds" ON)
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
option(MINILANG_UNITY_BUILD "Batch-compile minilang_core as a single translation unit (cold build speedup)" ON)

# C1: Ninja 响应文件 — 当源文件较多时通过 .rsp 文件传递编译参数，
# 避免 Windows 命令行 32K 限制与减少进程启动开销。
# 注：macOS 系统 ar（来自 Xcode/Command Line Tools）不支持 @response_file 语法，
# 强制开启会导致 "ar: @CMakeFiles/xxx.rsp: No such file or directory" 错误，
# 故仅在非 Apple 平台启用。macOS preset 已通过 CMAKE_C/CXX_USE_RESPONSE_FILE_FOR_OBJECTS=OFF
# 单独控制编译器响应文件行为。
if((CMAKE_GENERATOR STREQUAL "Ninja" OR CMAKE_GENERATOR MATCHES "Ninja Multi-Config") AND NOT APPLE)
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

# D2: 国际化 i18n（Qt Linguist）—— 将已提交的 .ts 翻译目录编译为运行时 .qm
# 默认 ON：运行时加载 zh_CN 目录，整条 i18n 管线默认可用、可验证。
# 仅用 qt_add_translation（lrelease）编译已填充的 .ts，不自动运行 lupdate，
# 以免每次构建都改写受版本控制的 .ts 源文件；翻译条目更新请手动执行 lupdate。
option(MINILANG_ENABLE_I18N "Enable Qt Linguist i18n infrastructure (.ts -> .qm + runtime loading)" ON)
if(MINILANG_ENABLE_I18N)
    add_compile_definitions(MINILANG_ENABLE_I18N)
    set(MINILANG_TRANSLATION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/app/translations/minilang_zh_CN.ts
        ${CMAKE_CURRENT_SOURCE_DIR}/app/translations/minilang_en_US.ts
    )
    # 优先路径：Qt6 LinguistTools CMake 配置（提供 qt_add_translation 函数）
    find_package(Qt6 COMPONENTS LinguistTools QUIET)
    if(Qt6LinguistTools_FOUND)
        message(STATUS "i18n enabled: Qt Linguist tools available (qt_add_translation)")
        # 仅 lrelease：把已填充的 .ts 编译为 .qm（不重写 .ts 源文件）
        qt_add_translation(MINILANG_QM_FILES
            ${MINILANG_TRANSLATION_SOURCES}
        )
    else()
        # L30 回退路径：Qt6LinguistTools CMake 配置不可用（aqtinstall 未提供 qttools 模块），
        # 但系统中存在 lrelease 可执行文件（如 apt 包 qt6-l10n-tools）。直接调用 lrelease
        # 编译 .ts → .qm，等效于 qt_add_translation 的底层实现。
        find_program(LRELEASE_EXECUTABLE
            NAMES lrelease lrelease6 lrelease-qt6
            DOC "Qt Linguist lrelease tool (.ts -> .qm compiler)")
        if(LRELEASE_EXECUTABLE)
            message(STATUS "i18n enabled: lrelease found at ${LRELEASE_EXECUTABLE} (fallback path)")
            foreach(_ts ${MINILANG_TRANSLATION_SOURCES})
                get_filename_component(_qm_name "${_ts}" NAME_WE)
                set(_qm "${CMAKE_CURRENT_BINARY_DIR}/${_qm_name}.qm")
                add_custom_command(
                    OUTPUT "${_qm}"
                    COMMAND ${LRELEASE_EXECUTABLE} -compress -idbased "${_ts}" -qm "${_qm}"
                    DEPENDS "${_ts}"
                    COMMENT "Compiling ${_qm_name}.qm (lrelease fallback)"
                    VERBATIM
                )
                list(APPEND MINILANG_QM_FILES "${_qm}")
            endforeach()
        else()
            message(WARNING "MINILANG_ENABLE_I18N=ON but neither Qt6::LinguistTools nor lrelease found; .qm not built (runtime falls back to source strings)")
        endif()
    endif()
    # 注册 .qm 生成目标（默认构建）与安装
    if(MINILANG_QM_FILES)
        add_custom_target(minilang_translations ALL DEPENDS ${MINILANG_QM_FILES})
        install(FILES ${MINILANG_QM_FILES}
            DESTINATION bin/translations
        )
    endif()
endif()

# 修复问题 5：Release LTO 开关
option(MINILANG_ENABLE_LTO "Enable Link Time Optimization in Release builds" ON)
if(MINILANG_ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# PERF: Profile-Guided Optimization (PGO) 两趟构建支持。
# 第一趟：-DMINILANG_PGO_GENERATE=ON 构建并运行代表性工作负载生成 profile 数据。
# 第二趟：-DMINILANG_PGO_USE=ON 利用 profile 数据指导优化（典型收益 10-30%）。
# 仅对 MSVC 和 GCC/Clang 生效；Release 构建专用。
option(MINILANG_PGO_GENERATE "Enable PGO instrumentation (pass 1: generate profile data)" OFF)
option(MINILANG_PGO_USE "Enable PGO optimization (pass 2: use profile data)" OFF)
if(MINILANG_PGO_GENERATE)
    if(MSVC)
        add_compile_options(/GL)
        add_link_options(/LTCG /GENPROFILE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fprofile-generate)
        add_link_options(-fprofile-generate)
    endif()
    message(STATUS "PGO instrumentation enabled (MINILANG_PGO_GENERATE)")
elseif(MINILANG_PGO_USE)
    if(MSVC)
        add_compile_options(/GL)
        add_link_options(/LTCG /USEPROFILE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fprofile-use -fprofile-correction)
        add_link_options(-fprofile-use)
    endif()
    message(STATUS "PGO optimization enabled (MINILANG_PGO_USE)")
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
# ADS_VERSION 必须手动固定：作为 git submodule 引入时，third_party/ads 目录内
# 没有独立 .git（仅有 gitlink），其 CMakeLists 的 git_describe 自动检测会失败；
# 当前 pin 为 5.0.0（与 ads 子模块 checkout commit 对应，见 .gitmodules 记录）。
# 移除 FORCE 以便需要时可用 -DADS_VERSION=x.y.z 覆盖；更新 ads 子模块需同步此值。
#
# vcpkg 替代路径：若通过 vcpkg 提供 ADS，将下方 add_subdirectory 替换为：
#   find_package(ads REQUIRED)
# 并将 target_link_libraries 中的 qtadvanceddocking-qt6 改为 ads::qtadvanceddocking。
# 注：vcpkg port 名为 "qtadvanceddocking"，参见 vcpkg.json 中的 feature 配置。
# ============================================================
set(ADS_VERSION "5.0.0" CACHE STRING "Pinned ADS version (submodule: git describe unavailable)")
set(BUILD_STATIC ON CACHE BOOL "Build ADS as static library" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build ADS examples" FORCE)
add_subdirectory(third_party/ads)

# ============================================================
# 第三方依赖：QFluentKit (Fluent Design Widgets)
# ------------------------------------------------------------
# QFluent 构建为 SHARED 库。
# 保存/恢复 CMAKE_INSTALL_PREFIX，防止 QFluent 的 FORCE 设置污染父项目。
# QFLUENTKIT_VERSION 固定：作为 git submodule 引入，third_party/QFluentKit 内
# git describe 返回 1.0-4-g8503e72（基线 tag 1.0）。当前 pin 为 1.0
# （见 .gitmodules 记录）。更新 QFluentKit 子模块需同步此值。
# 注：QFluentKit 未收录 vcpkg，仅以 submodule 方式提供。
#
# TODO(上游追踪): macOS arm64 链接问题通过 patches/qfluentkit-local-fixes.patch 临时解决。
# 补丁内容：(1) using Base::Base 改为显式构造函数（arm64 clang 不生成继承构造符号）
#              (2) InlineSpinBoxBase 方法内联化
#              (3) QFluentExample 子目录注释掉
#              (4) 安装条件增加顶层源码目录判断
# 待上游修复后可移除补丁。相关讨论：https://github.com/user/QFluentKit/issues/TBD
# ============================================================
set(QFLUENTKIT_VERSION "1.0" CACHE STRING "Pinned QFluentKit version (submodule: tag 1.0)")
set(_saved_install_prefix "${CMAKE_INSTALL_PREFIX}")
add_subdirectory(third_party/QFluentKit)
set(CMAKE_INSTALL_PREFIX "${_saved_install_prefix}" CACHE PATH "" FORCE)
unset(_saved_install_prefix)

# ============================================================
# 第三方依赖：asmjit（JIT 后端，可选）
# ------------------------------------------------------------
# asmjit 是轻量级 C++17 JIT 编译库，支持 x86/x64/ARM64。
# 启用 MINILANG_USE_JIT=ON 时作为静态库编入项目，供 JITBackend 使用。
# 教学价值：展示 JIT 编译原理（字节码→本地代码），与 StackVM 解释执行对比。
# ASMJIT_VERSION 固定：asmjit 上游采用 rolling 发布（无正式版本 tag），
# 子模块跟随 master 分支，当前 pin commit 0bd5787（见 .gitmodules 记录）。
# vcpkg 中的 asmjit 端口以日期版本发布，可作替代来源。
#
# vcpkg 替代路径：若通过 vcpkg 提供 asmjit，将下方 add_subdirectory 替换为：
#   find_package(asmjit CONFIG REQUIRED)
# 并将 target_link_libraries 中的 asmjit 改为 asmjit::asmjit。
#
# 平台限制：当前 JIT 代码生成仅支持 x86-64 架构。
# 非 x86-64 平台（如 Apple Silicon Mac）自动禁用 JIT，回退到 StackVM 解释执行。
# 教学平台 ARM64 支持尚未实现，待 asmjit ARM64 后端成熟后追加。
# ============================================================
set(ASMJIT_VERSION "master-0bd5787" CACHE STRING "Pinned asmjit version (submodule: master, rolling)")
option(MINILANG_USE_JIT "Enable JIT backend (asmjit)" ON)
# 平台检测：JIT 代码生成仅支持 x86-64，非 x64 平台自动禁用
if(MINILANG_USE_JIT)
    if(NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64"))
        message(STATUS "JIT disabled: current platform '${CMAKE_SYSTEM_PROCESSOR}' is not x86-64 (JIT code generation requires x86-64)")
        set(MINILANG_USE_JIT OFF CACHE BOOL "" FORCE)
    endif()
endif()
if(MINILANG_USE_JIT)
    set(ASMJIT_STATIC ON CACHE BOOL "Build asmjit as static library" FORCE)
    set(ASMJIT_TEST OFF CACHE BOOL "Build asmjit tests" FORCE)
    set(ASMJIT_NO_INSTALL ON CACHE BOOL "Disable asmjit install" FORCE)
    # SYSTEM：将 asmjit 导出的头文件目录标记为系统包含路径，
    # 抑制其头文件在本项目 TU 中触发的 -Wpedantic 警告
    # （如匿名 struct）——第三方代码不受本项目 -Werror 门禁约束。
    add_subdirectory(third_party/asmjit SYSTEM)
    add_compile_definitions(MINILANG_USE_JIT)
    message(STATUS "JIT backend enabled (asmjit ${ASMJIT_VERSION})")
endif()

# ARM64 JIT 后端（实验性 PoC，默认 OFF）
# 仅实现 Phase 1 PoC：基本算术 + 控制流 + 局部/全局变量（对齐 x86-64 R138-R140），
# 无函数调用/类/闭包/异常/GC 集成，无单元测试覆盖。不建议生产使用，
# 仅供后续 ARM64 教学平台移植参考。详见 docs/adr/ADR-006-jit-backend.md。
option(MINILANG_USE_JIT_A64 "Enable ARM64 JIT backend (asmjit a64, EXPERIMENTAL PoC)" OFF)
if(MINILANG_USE_JIT_A64)
    add_compile_definitions(MINILANG_USE_JIT_A64)
    message(WARNING "MINILANG_USE_JIT_A64 is an EXPERIMENTAL PoC: limited opcode coverage, no tests, no GC/closure/exception support. Not for production use.")
endif()

# ============================================================
# 包含共享模块：源文件列表 + 编译选项 + 配置函数 + target 创建
# 修复问题 2（源文件列表重复维护）
# P1-6: minilang_core.cmake 内部创建 4 个 OBJECT 子库 + STATIC 聚合，
#        此处 include 后 minilang_core target 即已就绪。
# ============================================================
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/minilang_core.cmake)

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
    app/app.rc
    ${MINILANG_GUI_SOURCES}
    # R164+ 模糊测试教学面板复用 fuzz_core 逻辑（cli/fuzz_core.cpp 无 Qt 依赖，安全包含）
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/fuzz_core.cpp
    # R162+ 静态分析教学面板复用 lint_core 逻辑（cli/lint_core.cpp 无 Qt 依赖，安全包含）
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/lint_core.cpp
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

# P1-6: 为 minilang_ide 启用 PCH（复用 common/pch.h 标准库头预编译）。
# ide 的 65 个 .cpp 普遍包含 <string>/<vector>/<memory> 等标准库头，
# PCH 可减少重复解析开销（增量构建 -10~20%）。
minilang_enable_pch(minilang_ide ${CMAKE_CURRENT_SOURCE_DIR})

# C2 (P3-A2): GUI 源文件预存警告豁免。
# gui/ 目录 146 个文件含大量历史代码，存在 C4456/C4189/C4100 等 W4 警告，
# 待后续面板注册表迁移时逐步清理。不影响 minilang_core 的 /W4 /WX 基线。
if(MSVC)
    set_source_files_properties(${MINILANG_GUI_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4456;/wd4189;/wd4100;/wd4458;/wd4996")
endif()

# C2: 启用 QtCharts 时链接到 IDE 目标
if(MINILANG_USE_QTCHARTS)
    target_link_libraries(minilang_ide PRIVATE Qt6::Charts)
endif()

# Windows: 链接 dwmapi 用于深色标题栏 (DWMWA_USE_IMMERSIVE_DARK_MODE)
# R128: 链接 dbghelp 用于 CrashHandler 的 MiniDumpWriteDump
if(WIN32)
    target_link_libraries(minilang_ide PRIVATE dwmapi dbghelp)
endif()

# 部署 QFluent 动态库到 exe 同目录（QFluent 为 SHARED 库，运行时需 DLL）
# 复用共享模块 cmake/deploy_qfluent.cmake，消除与 tests/ 的重复（修复审计问题 7）。
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy_qfluent.cmake)
minilang_deploy_qfluent_runtime(minilang_ide)

# D2: 部署翻译 .qm 到 exe 旁的 translations/，使 loadMiniLangTranslations() 运行时能加载
# （仅当 i18n 启用且 LinguistTools 可用；否则 .qm 未生成，运行时回退到源字符串）
# 注：必须检查 MINILANG_QM_FILES 非空，否则 copy_if_different 只收到目标路径一个参数，
# cmake -E 会打印 usage 并退出 1（曾导致 Windows CI 构建失败）。
if(MINILANG_ENABLE_I18N AND Qt6LinguistTools_FOUND AND MINILANG_QM_FILES)
    add_custom_command(TARGET minilang_ide POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:minilang_ide>/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MINILANG_QM_FILES}
                "$<TARGET_FILE_DIR:minilang_ide>/translations/"
        DEPENDS ${MINILANG_QM_FILES}
        COMMENT "Deploying MiniLang translations (.qm) next to executable..."
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
# test_harness 子目录（独立审计/压力测试工具）
# 修复问题 3（test_harness 未集成 CMake）
# ============================================================
option(MINILANG_BUILD_TEST_HARNESS "Build MiniLang test harness (audit/stress tools)" ON)
if(MINILANG_BUILD_TEST_HARNESS)
    add_subdirectory(test_harness)
endif()

# ============================================================
# cli 子目录（独立命令行工具）
# R109: minilang-fmt 代码格式化 CLI
# 默认 ON（轻量工具，无额外依赖），CI 覆盖率构建可显式关闭
# ============================================================
option(MINILANG_BUILD_CLI "Build MiniLang CLI tools (minilang-fmt)" ON)
if(MINILANG_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ============================================================
# 性能基准回归测试（独立可执行目标 minilang_perf_test）
# ------------------------------------------------------------
# 默认 ON 以便常规构建仍能跑性能测试；CI 覆盖率构建显式关闭，
# 因为覆盖率 job 仅构建 --target minilang_tests，未构建
# minilang_perf_test，但 gtest_discover_tests(DISCOVERY_MODE PRE_TEST)
# 仍会注册占位测试 minilang_perf_test_NOT_BUILT 导致 CTest 失败。
# 选项同时包裹 add_executable 与 gtest_discover_tests，保证
# "未构建即不注册"的不变量。
# ============================================================
option(MINILANG_BUILD_PERF_TESTS "Build MiniLang performance regression test target (minilang_perf_test)" ON)

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
set(MINILANG_SANITIZE "" CACHE STRING "Enable sanitizer: address, undefined, thread, both")
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
    elseif(MINILANG_SANITIZE STREQUAL "thread")
        if(NOT MSVC)
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=thread)
        else()
            message(WARNING "MINILANG_SANITIZE=thread is not supported on MSVC")
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
                " Supported: address, undefined, thread, both")
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
