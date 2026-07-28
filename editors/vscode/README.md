# MiniLang VS Code 扩展

MiniLang 语言的 VS Code 支持：语法高亮 + LSP 语言服务 + DAP 调试。

## 功能

| 能力 | 后端 | 说明 |
|---|---|---|
| 语法高亮 | tmLanguage | 关键字/字符串（含插值）/注释/数字/类型 |
| 诊断 | minilang_lsp | 词法/语法错误 + lint 警告实时下划线 |
| 补全/悬停/跳转定义 | minilang_lsp | 关键字 + 文档符号 |
| 查找引用 / 重命名 | minilang_lsp | LSP 二期 |
| 签名帮助 / 语义着色 | minilang_lsp | LSP 二期 |
| 格式化 | minilang_lsp | 复用 minilang-fmt 管线 |
| 调试 | minilang_dap | 断点/条件断点/命中条件/单步/变量查看与修改（setVariable）|

## 构建 .vsix

需要 Node.js ≥ 18：

```bash
cd editors/vscode
npm install
npx vsce package        # 产出 minilang-0.1.0.vsix
```

安装：VS Code → 扩展面板 → `...` → 从 VSIX 安装。

CI：release 工作流的 `vsix` 作业在发布时自动打包并作为 Release 附件上传。

## 配置

扩展依赖两个 CLI 可执行文件（随 MiniLang IDE 构建产出）：

```jsonc
{
  // 构建产物位于 out/build/<preset>/cli/ 下
  "minilang.lspPath": "C:/path/to/out/build/release/cli/minilang_lsp.exe",
  "minilang.dapPath": "C:/path/to/out/build/release/cli/minilang_dap.exe"
}
```

不配置时默认使用 PATH 中的 `minilang_lsp` / `minilang_dap`。

## 调试配置示例（launch.json）

```jsonc
{
  "type": "minilang",
  "request": "launch",
  "name": "调试当前 MiniLang 文件",
  "program": "${file}"
}
```
