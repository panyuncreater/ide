# ============================================================
# windeployqt.cmake — Qt 运行时依赖自动部署（共享模块）
# ------------------------------------------------------------
# 由根 CMakeLists.txt 和 tests/CMakeLists.txt 共同 include，
# 消除 windeployqt 检测与部署逻辑的重复维护问题。
# 用法：minilang_deploy_qt_runtime(<target>)
# ============================================================

if(NOT COMMAND minilang_deploy_qt_runtime)
    function(minilang_deploy_qt_runtime target_name)
        if(WIN32 AND TARGET Qt6::qmake)
            # 优先使用缓存路径（根 CMakeLists 已发现时子项目直接复用）
            if(NOT MINILANG_WINDEPLOYQT_EXE)
                get_target_property(_qmake_loc Qt6::qmake IMPORTED_LOCATION)
                get_filename_component(_qt_bin "${_qmake_loc}" DIRECTORY)
                find_program(MINILANG_WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin}")
            endif()

            if(MINILANG_WINDEPLOYQT_EXE)
                # 缓存路径供子项目复用，避免重复 find_program
                set(MINILANG_WINDEPLOYQT_EXE "${MINILANG_WINDEPLOYQT_EXE}"
                    CACHE FILEPATH "Path to windeployqt")

                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND "${MINILANG_WINDEPLOYQT_EXE}"
                            --no-translations
                            --no-system-d3d-compiler
                            --no-opengl-sw
                            "$<TARGET_FILE:${target_name}>"
                    COMMENT "Deploying Qt runtime for ${target_name}..."
                )
            endif()
        endif()
    endfunction()
endif()
