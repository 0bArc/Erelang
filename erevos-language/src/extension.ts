import * as vscode from 'vscode';
import * as path from 'path';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';
import {
  ErelangCompletionProvider, setDebugChannel, isForeachColonCtx, isDictLiteralCtx,
  noteDocumentEdit,
} from './completions';
import {
  collect, parseForEachHeader, getDocumentIndex, invalidateDocumentIndex,
  MAX_INDEX_LINES,
} from './symbols';
import { invalidateDefCache } from './semantic-tokens';
import { invalidateImportCache } from './imports';

const KIND_MAP = {
  entity: vscode.SymbolKind.Class,
  action: vscode.SymbolKind.Function,
  field: vscode.SymbolKind.Field,
  hook: vscode.SymbolKind.Event,
  namespace: vscode.SymbolKind.Namespace,
} as const;

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    if (doc.languageId !== 'erelang') return [];
    if (doc.lineCount > MAX_INDEX_LINES) return [];
    const index = getDocumentIndex(doc);
    const out: vscode.SymbolInformation[] = [];
    for (const sym of index.outline) {
      out.push(new vscode.SymbolInformation(
        sym.name,
        KIND_MAP[sym.kind],
        '',
        new vscode.Location(doc.uri, new vscode.Position(sym.line, 0)),
      ));
    }
    return out;
  }
}

class ErelangWorkspaceSymbolProvider implements vscode.WorkspaceSymbolProvider {
  provideWorkspaceSymbols(
    query: string,
    token: vscode.CancellationToken,
  ): vscode.SymbolInformation[] {
    const q = query.trim();
    if (q.length < 2) return [];

    const out: vscode.SymbolInformation[] = [];
    for (const doc of vscode.workspace.textDocuments) {
      if (token.isCancellationRequested) return out;
      if (doc.languageId !== 'erelang') continue;
      if (doc.lineCount > MAX_INDEX_LINES) continue;
      const index = getDocumentIndex(doc);
      for (const sym of index.outline) {
        if (!sym.name.includes(q)) continue;
        out.push(new vscode.SymbolInformation(
          sym.name,
          KIND_MAP[sym.kind],
          '',
          new vscode.Location(doc.uri, new vscode.Position(sym.line, 0)),
        ));
      }
    }
    return out;
  }
}

let client: LanguageClient | undefined;

export function activate(ctx: vscode.ExtensionContext) {
  let debugCh: vscode.OutputChannel | undefined;
  const getDebugCh = () => {
    if (!debugCh) {
      debugCh = vscode.window.createOutputChannel('Erelang Language Debug');
      ctx.subscriptions.push(debugCh);
      setDebugChannel(debugCh);
    }
    return debugCh;
  };

  const serverModule = ctx.asAbsolutePath(path.join('out', 'server.js'));
  const exePath = vscode.workspace.getConfiguration('erelang').get<string>('executablePath', '');
  const serverOptions: ServerOptions = {
    run: { module: serverModule, transport: TransportKind.stdio },
    debug: { module: serverModule, transport: TransportKind.stdio },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'erelang' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{elan,ere}'),
    },
    initializationOptions: {
      erelangPath: exePath,
    },
  };
  client = new LanguageClient('erelang', 'Erelang Language Server', serverOptions, clientOptions);
  ctx.subscriptions.push({ dispose: () => { void client?.stop(); } });
  void client.start();

  ctx.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => {
    noteDocumentEdit(e);
  }));
  ctx.subscriptions.push(vscode.workspace.onDidCloseTextDocument(d => {
    if (d.languageId !== 'erelang') return;
    const key = d.uri.toString();
    invalidateImportCache(key);
    invalidateDefCache(key);
    invalidateDocumentIndex(key);
  }));

  ctx.subscriptions.push(
    vscode.commands.registerCommand('erelang.debugCompletionContext', () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'erelang') {
        vscode.window.showWarningMessage('Open an Erelang file first.');
        return;
      }
      const ch = getDebugCh();
      const cur    = editor.selection.active;
      const line   = editor.document.lineAt(cur.line).text;
      const prefix = line.slice(0, cur.character);
      const col    = collect(editor.document);

      ch.appendLine('Erelang Completion Context');
      ch.appendLine(`cursor:       ${cur.line + 1}:${cur.character + 1}`);
      ch.appendLine(`line:         ${line}`);
      ch.appendLine(`prefix:       ${prefix}`);
      ch.appendLine(`foreach:      ${JSON.stringify(parseForEachHeader(line))}`);
      ch.appendLine(`locals:       ${[...col.locals].join(', ')      || 'none'}`);
      ch.appendLine(`arrays:       ${[...col.arrays].join(', ')      || 'none'}`);
      ch.appendLine(`dictionaries: ${[...col.dictionaries].join(', ') || 'none'}`);
      ch.appendLine(`foreachCtx:   ${isForeachColonCtx(prefix)}`);
      ch.appendLine(`dictLitCtx:   ${isDictLiteralCtx(prefix)}`);
      ch.appendLine('');
      ch.show(true);
      vscode.window.showInformationMessage('Context dumped → Output > Erelang Language Debug');
    }),
  );

  // Rich client-side completions keep include-path / member heuristics; LSP also offers basics.
  ctx.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      { language: 'erelang' },
      new ErelangCompletionProvider(),
      '#', '.', '<', '"', '/',
    ),
  );
  ctx.subscriptions.push(
    vscode.languages.registerDocumentSymbolProvider({ language: 'erelang' }, new ErelangDocumentSymbolProvider()),
  );
  ctx.subscriptions.push(
    vscode.languages.registerWorkspaceSymbolProvider(new ErelangWorkspaceSymbolProvider()),
  );

  ctx.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory('erelang', {
      createDebugAdapterDescriptor() {
        return new vscode.DebugAdapterExecutable(
          process.execPath,
          [ctx.asAbsolutePath(path.join('out', 'debugAdapter.js'))],
        );
      },
    }),
  );
}

export async function deactivate() {
  if (client) await client.stop();
}
