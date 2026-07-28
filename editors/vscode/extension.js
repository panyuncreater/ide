// ============================================================
// MiniLang VS Code 扩展入口（拓展计划·工具链）
// ------------------------------------------------------------
// 职责：
//   1. 启动 minilang_lsp 语言服务器（stdio 传输），获得补全/悬停/定义/
//      引用/重命名/签名帮助/语义着色/格式化/诊断能力
//   2. 注册 MiniLang 调试适配器工厂：把 "minilang" 调试类型接到
//      minilang_dap 可执行文件（DAP over stdio）
//
// 可执行文件定位：
//   settings 的 minilang.lspPath / minilang.dapPath；默认裸名
//   （要求在 PATH 中，或用户配置绝对路径指向构建目录/安装目录）。
// ============================================================
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    const config = vscode.workspace.getConfiguration('minilang');

    // ---- 1. LSP 客户端 ----
    const lspPath = config.get('lspPath', 'minilang_lsp');
    const serverOptions = {
        command: lspPath,
        transport: TransportKind.stdio,
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'minilang' }],
    };
    client = new LanguageClient('minilang', 'MiniLang Language Server', serverOptions, clientOptions);
    client.start().catch((err) => {
        vscode.window.showWarningMessage(
            `MiniLang LSP 启动失败（${lspPath}）：${err.message}。` +
            '请在设置 minilang.lspPath 中配置 minilang_lsp 可执行文件路径。');
    });

    // ---- 2. DAP 调试适配器 ----
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('minilang', {
            createDebugAdapterDescriptor() {
                const dapPath = vscode.workspace.getConfiguration('minilang').get('dapPath', 'minilang_dap');
                return new vscode.DebugAdapterExecutable(dapPath, []);
            },
        })
    );
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
