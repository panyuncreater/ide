# ============================================================
# MiniLang CLI 工具构建脚本
# ------------------------------------------------------------
# 默认构建（MINILANG_BUILD_CLI=ON），CI 覆盖率构建可显式关闭。
# 当前工具：
#   - minilang_fmt: 代码格式化 CLI（R109）
#     将 Formatter 抽出为独立命令，支持 --check/--write/--dry-run
#     和 --indent/--style/--tabs 选项。
#   - minilang_coverage: 行级覆盖率 CLI（R110）
#     基于 VM 指令行号信息（BytecodeChunk.lines）与 stepCallback
#     收集行级覆盖率，支持 text/lcov 输出和 --fail-under 阈值。
#   - minilang_lint: 静态分析 CLI（R162）
#     将 LintPass 抽出为独立命令，支持 --quiet/--rule/--max-complexity
#     和 --format text|json 选项。
#   - minilang_doc: API 文档生成 CLI（R162 B2）
#     将 DocGenerator 抽出为独立命令，支持 --format markdown|html|json
#     和 --output 选项，从 /// 文档注释生成 API 文档。
#   - minilang_fuzz: 模糊测试 CLI（R164）
#     模糊测试器，支持生成/变异/文件三种模式，三后端差分，
#     发现崩溃时保存用例到 crash-*.ml 文件。
#   - minilang_lsp: 语言服务器 CLI（R164）
#     Language Server Protocol 实现，支持 VS Code/Vim/Emacs 接入。
#     提供 didOpen/didChange/didClose、publishDiagnostics、completion、
#     hover、definition、documentSymbol、formatting 等功能。
#   - minilang_dap: 调试适配器 CLI（R165）
#     Debug Adapter Protocol 实现，支持 VS Code 等编辑器调试 MiniLang 程序。
#     直接使用 StackVM 的 stepOnce + getCallStack + getFrameLocalsAt，
#     提供 launch/setBreakpoints/continue/next/stepIn/stepOut/
#     stackTrace/scopes/variables/evaluate 等调试能力。
# ============================================================

# ================================================================
# minilang_fmt: 代码格式化 CLI 工具
# 仅依赖 Lexer + Parser + Formatter（通过 minilang_core 链接）
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_fmt
    fmt.cpp
    fmt_core.cpp
)
target_link_libraries(minilang_fmt PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_coverage: 行级覆盖率 CLI 工具
# 依赖 Lexer + Parser + Compiler + VM + RegisterVM（通过 minilang_core 链接）
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_coverage
    coverage.cpp
    coverage_core.cpp
)
target_link_libraries(minilang_coverage PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_lint: 静态分析 CLI 工具
# 依赖 Lexer + Parser + LintPass（通过 minilang_core 链接）
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_lint
    lint.cpp
    lint_core.cpp
)
target_link_libraries(minilang_lint PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_doc: API 文档生成 CLI 工具
# 依赖 Lexer + Parser + DocGenerator（通过 minilang_core 链接）
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_doc
    doc.cpp
    doc_core.cpp
)
target_link_libraries(minilang_doc PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_fuzz: 模糊测试 CLI 工具
# 依赖 Lexer + Parser + Compiler + VM + RegisterVM + Interpreter
# （通过 minilang_core 链接），不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_fuzz
    fuzz.cpp
    fuzz_core.cpp
)
target_link_libraries(minilang_fuzz PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_lsp: 语言服务器 CLI 工具
# 依赖 Lexer + Parser + Formatter + LintPass（通过 minilang_core 链接）
# 复用 Qt6::Core 的 QJsonDocument 处理 JSON-RPC 消息
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_lsp
    lsp.cpp
    lsp_core.cpp
)
target_link_libraries(minilang_lsp PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_dap: 调试适配器 CLI 工具
# 依赖 Lexer + Parser + Compiler + StackVM（通过 minilang_core 链接）
# 复用 Qt6::Core 的 QJsonDocument 处理 JSON-RPC 消息
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_dap
    dap.cpp
    dap_core.cpp
)
target_link_libraries(minilang_dap PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_pkg: 包管理器 CLI 工具
# 依赖 Qt6::Core 的 QJsonDocument/QFile/QDir 处理 JSON 和文件操作
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_pkg
    pkg.cpp
    pkg_core.cpp
)
target_link_libraries(minilang_pkg PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_compile: 预编译模块 CLI 工具（P2-11）
# 依赖 Lexer + Parser + Compiler + BytecodeCache（通过 minilang_core 链接）
# 将 MiniLang 源码编译为 .minic 预编译模块文件
# 不依赖 Qt6::Widgets / gtest
# ================================================================
add_executable(minilang_compile
    compile.cpp
    compile_core.cpp
)
target_link_libraries(minilang_compile PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang: 统一命令行入口（单二进制子命令分发）
# 通过 minilang <subcommand> 调用各 CLI 工具，类似 cargo/git 体验。
# 原独立可执行文件仍保留（向后兼容）。
# 包含所有 *_core.cpp + main.cpp + lsp_entry.cpp + dap_entry.cpp。
# ================================================================
add_executable(minilang
    main.cpp
    fmt_core.cpp
    coverage_core.cpp
    lint_core.cpp
    doc_core.cpp
    fuzz_core.cpp
    pkg_core.cpp
    compile_core.cpp
    lsp_core.cpp
    lsp_entry.cpp
    dap_core.cpp
    dap_entry.cpp
)
target_link_libraries(minilang PRIVATE
    minilang_core
    minilang_compile_options
)

# ================================================================
# minilang_capi: libminilang 嵌入式 C API 静态库（拓展二期·平台）
# 稳定 C ABI（capi/minilang_capi.h）：create/eval/get_output/get_error/
# set_backend/destroy。嵌入方链接 minilang_capi + minilang_core
#（及 Qt6::Core 传递依赖）。接口层由 tests/TestCApi.cpp 全覆盖。
# ================================================================
add_library(minilang_capi STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../capi/minilang_capi.cpp
)
target_include_directories(minilang_capi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(minilang_capi
    PUBLIC minilang_core
    PRIVATE minilang_compile_options
)
