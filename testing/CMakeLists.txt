# ============================================================
# testing/CMakeLists.txt — 自动化 bug 挖掘流水线（与主工程解耦）
# ------------------------------------------------------------
# 由顶层 CMakeLists.txt 在 MINILANG_BUILD_TESTING_PIPELINE=ON 时
# add_subdirectory 引入。默认 OFF，不影响主工程/CI 的默认构建。
#
# 交付物：
#   - mini_diff_runner: 单后端进程内执行内核（链接 minilang_core，
#     覆盖 interp/stackvm/stackvm-ir/regvm/jit 五种执行配置）。
#   - CTest 注册：将 samples/mini 下可独立运行的示例注册为差分自洽
#     回归用例（driver.py 驱动，四/五后端一致即通过）。
#
# 本目录仅新增测试与工具代码，不修改编译器任何实现逻辑。
# ============================================================

# ---- 差分执行内核 ----
add_executable(mini_diff_runner
    runner/mini_diff_runner.cpp
)
target_link_libraries(mini_diff_runner PRIVATE
    minilang_core
    minilang_compile_options
)
# 无 Q_OBJECT，关闭 AUTOMOC 省去 MOC 处理
set_target_properties(mini_diff_runner PROPERTIES AUTOMOC OFF)

# 输出到 <build>/testing/，便于 driver.py 稳定定位
set_target_properties(mini_diff_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testing
)

# ============================================================
# 阶段四：前端鲁棒性 Fuzzing —— standalone 回放回归 target
# ------------------------------------------------------------
# testing/fuzz_parser.cpp 为双模式 fuzz target：
#   - libFuzzer 模式（Clang）：由 testing/fuzz/{build,run}_fuzz.sh 构建运行，
#     本 CMake 不涉及（MSVC 无 libFuzzer）。
#   - standalone 回放模式（本 target）：定义 MINILANG_FUZZER_STANDALONE，
#     提供 main() 逐个回放 corpus/crash 文件走完整前端，进程存活到底即通过。
#     用于在无 Clang 的平台（如 Windows/MSVC）持续守护该 fuzz target 不腐化。
#
# 本 target 直接编译前端最小链接闭包（6 个既有编译器源文件，不修改），
# 与 testing/fuzz/build_fuzz.sh 的源清单严格一致——从而本 CMake 构建即可
# 验证该闭包在 MSVC 下链接完备（无未解析符号）、无 Qt 依赖。
# 强制包含 common/pch.h 以镜像主构建的标准库可见性。
# ============================================================
get_filename_component(_repo_root ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
add_executable(fuzz_parser_replay
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_parser.cpp
    ${_repo_root}/lexer/Lexer.cpp
    ${_repo_root}/parser/Parser.cpp
    ${_repo_root}/ast/ASTNode.cpp
    ${_repo_root}/ast/MacroExpander.cpp
    ${_repo_root}/interpreter/Value.cpp
    ${_repo_root}/interpreter/GcManager.cpp
)
target_compile_definitions(fuzz_parser_replay PRIVATE MINILANG_FUZZER_STANDALONE)
target_include_directories(fuzz_parser_replay PRIVATE ${_repo_root} ${_repo_root}/common)
target_precompile_headers(fuzz_parser_replay PRIVATE ${_repo_root}/common/pch.h)
target_link_libraries(fuzz_parser_replay PRIVATE minilang_compile_options)
# 无 Q_OBJECT / 无 Qt 依赖，关闭 AUTOMOC
set_target_properties(fuzz_parser_replay PROPERTIES AUTOMOC OFF)
set_target_properties(fuzz_parser_replay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testing
)

# CTest：回放种子语料 + 示例源程序，断言前端对全部输入不崩溃（进程存活即 PASS）。
# 不依赖 Python，不需要 Qt DLL（parse 闭包零 Qt 符号）。
add_test(
    NAME pipeline.fuzz.replay
    COMMAND fuzz_parser_replay
            ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus
            ${_repo_root}/samples/mini
)
set_tests_properties(pipeline.fuzz.replay PROPERTIES
    LABELS "pipeline;fuzz;robustness"
    TIMEOUT 120
)

# ============================================================
# CTest 注册：samples/mini 差分自洽回归
# ------------------------------------------------------------
# 说明：完整的四/五后端投票在阶段二 diff_test.py 实现；此处阶段一
# 先注册「driver.py 差分自洽」用例作为回归载体：对每个样例，比较
# 所有非降级后端的规范输出串是否逐字节一致，一致则 PASS。
# ============================================================
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
    set(_driver ${CMAKE_CURRENT_SOURCE_DIR}/driver.py)
    set(_samples_root ${CMAKE_CURRENT_SOURCE_DIR}/../samples/mini)

    # 可独立运行（不依赖文件模块导入 / 交互输入）的样例白名单。
    # 08-modules-and-backends 依赖真实模块解析（runner 注入空 loader），故不纳入。
    #
    # 分两类：
    #   _agreeing_samples       —— 四 strict 后端当前完全一致，正常 PASS。
    #   _known_divergence_samples —— 阶段一验证时发现的真实跨后端差异（见 README
    #     「已知发现」），标记 WILL_FAIL：当前 driver 退出非零（差异存在）→ CTest 判
    #     通过；一旦后端 bug 修复、四后端一致，driver 退出 0 → WILL_FAIL 用例转为
    #     FAILED，自动提醒「差异已消除，应移出本清单」。全程不改编译器实现。
    set(_agreeing_samples
        01-basics/hello.mini
        02-types-and-operators/types_and_operators.mini
        03-control-flow/control_flow.mini
        06-object-oriented/oop_shapes.mini
        07-error-handling/error_handling.mini
    )
    set(_known_divergence_samples
        # 命名函数在循环/块作用域内声明后存入数组：interp 可解析，
        # stackvm/stackvm-ir/regvm/jit 均报「未定义的变量: getter」。
        04-functions-and-closures/functions_and_closures.mini
        # 内建方法覆盖不一致：stackvm 缺 remove、regvm 缺 contains、jit 缺 array.len。
        05-data-structures/data_structures.mini
    )

    # Qt 运行期 DLL 目录（Debug 版 Qt6Cored.dll 等）：优先取 CMAKE_PREFIX_PATH/bin，
    # 再叠加构建根目录（windeployqt 拷贝的 DLL）与 runner 输出目录。
    set(_qt_bin "")
    if(CMAKE_PREFIX_PATH)
        list(GET CMAKE_PREFIX_PATH 0 _prefix0)
        set(_qt_bin "${_prefix0}/bin")
    endif()

    # 统一注册函数：add_test + 环境注入（Qt bin / 构建根解决 DLL 定位）。
    function(_register_diff_case _sample _will_fail)
        get_filename_component(_name ${_sample} NAME_WE)
        add_test(
            NAME pipeline.diff.${_name}
            COMMAND ${Python3_EXECUTABLE} ${_driver} diff
                    --file ${_samples_root}/${_sample}
                    --runner $<TARGET_FILE:mini_diff_runner>
                    --require-agreement
        )
        set_tests_properties(pipeline.diff.${_name} PROPERTIES
            LABELS "pipeline;differential"
            TIMEOUT 120
            ENVIRONMENT "MINI_QT_BIN=${_qt_bin};MINI_BUILD_ROOT=${CMAKE_BINARY_DIR}"
        )
        if(_will_fail)
            set_tests_properties(pipeline.diff.${_name} PROPERTIES
                WILL_FAIL TRUE
                LABELS "pipeline;differential;known_divergence"
            )
        endif()
    endfunction()

    foreach(_sample ${_agreeing_samples})
        _register_diff_case(${_sample} FALSE)
    endforeach()
    foreach(_sample ${_known_divergence_samples})
        _register_diff_case(${_sample} TRUE)
    endforeach()

    # 生成式差分冲烟：默认安全子集下连续 8 个确定性种子必须四后端一致。
    # 完全确定性（同种子同程序），不 flaky；产物写入构建目录避免污染源树。
    add_test(
        NAME pipeline.diff.batch_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/diff_test.py
                --n 8 --seed 1
                --runner $<TARGET_FILE:mini_diff_runner>
                --out-dir ${CMAKE_BINARY_DIR}/testing_artifacts
    )
    set_tests_properties(pipeline.diff.batch_smoke PROPERTIES
        LABELS "pipeline;differential;generative"
        TIMEOUT 300
        ENVIRONMENT "MINI_QT_BIN=${_qt_bin};MINI_BUILD_ROOT=${CMAKE_BINARY_DIR}"
    )

    # ============================================================
    # 阶段五：缩减后最小复现用例回归（regression/manifest.json 驱动）
    # ------------------------------------------------------------
    # reduce.py 缩减完成后把最小用例写入 regression/<name>.mini 并登记 manifest。
    # 此处读取 manifest，为每个用例注册 `reduce.py verify`：
    #   verify 退出 0 ⇔ bug 已消失（趣味性判据不再成立）。
    #   status=open  → 标 WILL_FAIL：bug 仍在(verify 退出 1) → CTest 判 PASS；
    #                  一旦后端修复(verify 退出 0) → WILL_FAIL 转 FAILED，自动提醒转正。
    #   status=fixed → 不标 WILL_FAIL：必须 verify 退出 0（回归防复发守卫）。
    # 全程不改编译器实现。string(JSON) 需 CMake ≥ 3.19（本工程要求 3.25）。
    # ============================================================
    set(_reduce ${CMAKE_CURRENT_SOURCE_DIR}/reduce.py)
    set(_manifest ${CMAKE_CURRENT_SOURCE_DIR}/regression/manifest.json)
    set(_nreg 0)
    if(EXISTS ${_manifest})
        file(READ ${_manifest} _mjson)
        set(_ncases 0)
        string(JSON _ncases ERROR_VARIABLE _jerr LENGTH "${_mjson}" cases)
        if(_jerr)
            set(_ncases 0)
            message(STATUS "[testing] regression manifest 解析失败，跳过：${_jerr}")
        endif()
        if(_ncases GREATER 0)
            math(EXPR _last "${_ncases} - 1")
            foreach(_i RANGE 0 ${_last})
                string(JSON _case GET "${_mjson}" cases ${_i})
                string(JSON _cname GET "${_case}" name)
                string(JSON _cstatus GET "${_case}" status)
                add_test(
                    NAME pipeline.regression.${_cname}
                    COMMAND ${Python3_EXECUTABLE} ${_reduce} verify
                            --manifest ${_manifest} --index ${_i}
                            --runner $<TARGET_FILE:mini_diff_runner>
                )
                set_tests_properties(pipeline.regression.${_cname} PROPERTIES
                    LABELS "pipeline;regression"
                    TIMEOUT 180
                    ENVIRONMENT "MINI_QT_BIN=${_qt_bin};MINI_BUILD_ROOT=${CMAKE_BINARY_DIR}"
                )
                # status=open：bug 尚未修复→标 WILL_FAIL（修复后自动转 FAILED 提醒转正）。
                if(_cstatus STREQUAL "open")
                    set_tests_properties(pipeline.regression.${_cname} PROPERTIES
                        WILL_FAIL TRUE
                        LABELS "pipeline;regression;known_bug"
                    )
                endif()
            endforeach()
            set(_nreg ${_ncases})
        endif()
    endif()

    message(STATUS "[testing] 已注册 differential 回归用例（前缀 pipeline.diff.*，含 known_divergence 标签的 WILL_FAIL 用例 + 生成式 batch_smoke）；缩减回归用例 ${_nreg} 个（前缀 pipeline.regression.*）")
else()
    message(STATUS "[testing] 未找到 Python3 解释器，跳过 CTest 用例注册（仅构建 mini_diff_runner）")
endif()
