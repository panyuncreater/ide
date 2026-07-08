# ============================================================
# deploy_qfluent.cmake — QFluentKit 运行时依赖自动部署（共享模块）
# ------------------------------------------------------------
# 由根 CMakeLists.txt 和 tests/CMakeLists.txt 共同 include，
# 消除 QFluent 动态库（SHARED）POST_BUILD 拷贝逻辑的重复维护。
# 用法：minilang_deploy_qfluent_runtime(<target>)
# 作用：将 QFluent 共享库复制到目标可执行文件同目录（构建树 POST_BUILD），
#       确保开发/调试时运行时可加载。
# 职责边界（重要，避免重复安装）：
#   - 构建树部署：由本函数通过 POST_BUILD 负责（开发/IDE 直接运行用）。
#   - 安装包部署：由 QFluentKit 自身的 install(TARGETS QFluent ...) 负责。
#     其安装规则仅在「独立构建」时把 prefix 指向 Qt 目录（见 QFluent/CMakeLists.txt
#     的 `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` 判断）；本项目把 QFluent
#     作为子目录引入，该判断为假，故 QFluent 会随项目前缀装到 bin/，cpack 包已含
#     QFluent 运行时，本函数无需再重复处理安装包部署。
# ============================================================

if(NOT COMMAND minilang_deploy_qfluent_runtime)
    function(minilang_deploy_qfluent_runtime target_name)
        if(TARGET QFluent)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:QFluent>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                COMMENT "Deploying QFluent runtime for ${target_name}..."
            )
        else()
            message(WARNING "minilang_deploy_qfluent_runtime(${target_name}): "
                    "QFluent target not found; skipping runtime deploy")
        endif()
    endfunction()
endif()
