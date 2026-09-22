import * as vscode from 'vscode';
import { validateDocument } from './diagnostics';
import {
  ErelangCompletionProvider, setDebugChannel, isForeachColonCtx, isDictLiteralCtx,
} from './completions';
import {
  collect, parseForEachHeader, getDocumentIndex, invalidateDocumentIndex,
} from './symbols';
import { invalidateDefCache } from './semantic-tokens';
import { invalidateImportCache } from './imports';

const MAX_SYMBOL_LINES = 8_000;

const KIND_MAP = {
  entity: vscode.SymbolKind.Class,
  action: vscode.SymbolKind.Function,
  field: vscode.SymbolKind.Field,
  hook: vscode.SymbolKind.Event,
} as const;

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    if (doc.languageId !== 'erelang') return [];
    const index = getDocumentIndex(doc);
    const out: vscode.SymbolInformation[] = [];
    for (const sym of index.outline) {
      if (sym.line >= MAX_SYMBOL_LINES) break;
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

  const semiDiags = vscode.languages.createDiagnosticCollection('erelang');
  ctx.subscriptions.push(semiDiags);

  const diagnosticTimers = new Map<string, NodeJS.Timeout>();
  const diagnosticGeneration = new Map<string, number>();
  const scheduleDiags = (d: vscode.TextDocument, delay = 1200) => {
    if (d.isClosed || d.languageId !== 'erelang') return;
    const key = d.uri.toString();
    const generation = (diagnosticGeneration.get(key) ?? 0) + 1;
    diagnosticGeneration.set(key, generation);
    const pending = diagnosticTimers.get(key);
    if (pending) clearTimeout(pending);
    diagnosticTimers.set(key, setTimeout(() => {
      diagnosticTimers.delete(key);
      if (d.isClosed || d.version < 0) return;
      try {
        validateDocument(d, semiDiags, () =>
          diagnosticGeneration.get(key) !== generation || d.isClosed);
      } catch {}
    }, delay));
  };

  const forget = (d: vscode.TextDocument) => {
    if (d.languageId !== 'erelang') return;
    const key = d.uri.toString();
    invalidateImportCache(key);
    invalidateDefCache(key);
    invalidateDocumentIndex(key);
  };

  ctx.subscriptions.push(vscode.workspace.onDidSaveTextDocument(d => {
    if (d.languageId === 'erelang') scheduleDiags(d, 300);
  }));
  ctx.subscriptions.push(vscode.workspace.onDidCloseTextDocument(d => {
    if (d.languageId !== 'erelang') return;
    const key = d.uri.toString();
    const pending = diagnosticTimers.get(key);
    if (pending) clearTimeout(pending);
    diagnosticTimers.delete(key);
    diagnosticGeneration.delete(key);
    semiDiags.delete(d.uri);
    forget(d);
  }));
  ctx.subscriptions.push({ dispose: () => {
    for (const pending of diagnosticTimers.values()) clearTimeout(pending);
    diagnosticTimers.clear();
  }});

  const active = vscode.window.activeTextEditor?.document;
  if (active?.languageId === 'erelang') scheduleDiags(active, 2000);

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
    })
  );

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
}

export function deactivate() {}
