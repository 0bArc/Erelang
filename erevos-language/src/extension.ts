import * as vscode from 'vscode';
import { ENTITY_RE, ACTION_RE, FIELD_RE, HOOK_RE } from './constants';
import { validateDocument } from './diagnostics';
import {
  ErelangCompletionProvider, setDebugChannel, isForeachColonCtx, isDictLiteralCtx,
} from './completions';
import { collect, parseForEachHeader, invalidateEntityMemberCache } from './symbols';
import { invalidateDefCache } from './semantic-tokens';
import { invalidateImportCache } from './imports';

const MAX_SYMBOL_LINES = 8_000;

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    const out: vscode.SymbolInformation[] = [];
    const limit = Math.min(doc.lineCount, MAX_SYMBOL_LINES);
    for (let i = 0; i < limit; i++) {
      const line = doc.lineAt(i).text;
      let m: RegExpExecArray | null;
      if      ((m = ENTITY_RE.exec(line))) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = ACTION_RE.exec(line))) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = FIELD_RE.exec(line)))  out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = HOOK_RE.exec(line)))   out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
    }
    return out;
  }
}

class ErelangWorkspaceSymbolProvider implements vscode.WorkspaceSymbolProvider {
  async provideWorkspaceSymbols(
    query: string,
    token: vscode.CancellationToken,
  ): Promise<vscode.SymbolInformation[]> {
    const uris = await vscode.workspace.findFiles('**/*.{0bs,ere,elan}', '**/node_modules/**', 40);
    const out: vscode.SymbolInformation[] = [];
    for (const uri of uris) {
      if (token.isCancellationRequested) break;
      const bytes = await vscode.workspace.fs.readFile(uri);
      if (bytes.byteLength > 2 * 1024 * 1024) continue;
      const lines = Buffer.from(bytes).toString('utf8').split(/\r?\n/);
      for (let i = 0; i < lines.length; i++) {
        if ((i & 511) === 0 && token.isCancellationRequested) break;
        const line = lines[i];
        let m: RegExpExecArray | null;
        if      ((m = ENTITY_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = ACTION_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = FIELD_RE.exec(line))  && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = HOOK_RE.exec(line))   && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
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
    invalidateEntityMemberCache(key);
  };

  ctx.subscriptions.push(vscode.workspace.onDidSaveTextDocument(d => {
    if (d.languageId === 'erelang') scheduleDiags(d, 300);
  }));
  ctx.subscriptions.push(vscode.workspace.onDidCloseTextDocument(d => {
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
      const col    = collect(editor.document, cur.line);

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
