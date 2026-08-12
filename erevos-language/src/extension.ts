// =============================================================================
// Erelang Language Extension — VS Code
// =============================================================================
//
// Entry point: registers providers and hooks defined in the sibling modules.
//
//   constants.ts   — regex patterns, keyword/builtin lists
//   types.ts       — shared TypeScript interfaces
//   symbols.ts     — AST-free symbol collection + entity instance tracking
//   imports.ts     — #include / import resolution
//   diagnostics.ts — semicolon validation
//   completions.ts — ErelangCompletionProvider (dot, print, include, global)
//
// =============================================================================

import * as vscode from 'vscode';
import { ENTITY_RE, ACTION_RE, FIELD_RE, HOOK_RE } from './constants';
import { validateDocument } from './diagnostics';
import {
  ErelangCompletionProvider, setDebugChannel, isForeachColonCtx, isDictLiteralCtx,
} from './completions';
import { collect, parseForEachHeader, invalidateEntityMemberCache } from './symbols';
import { invalidateDefCache } from './semantic-tokens';
import { invalidateImportCache } from './imports';

// ─── Symbol Providers ───────────────────────────────────────────────────────

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    const out: vscode.SymbolInformation[] = [];
    for (let i = 0; i < doc.lineCount; i++) {
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
  async provideWorkspaceSymbols(query: string): Promise<vscode.SymbolInformation[]> {
    const uris = await vscode.workspace.findFiles('**/*.{0bs,ere,elan}', '**/node_modules/**', 40);
    const out: vscode.SymbolInformation[] = [];
    for (const uri of uris) {
      const doc = await vscode.workspace.openTextDocument(uri);
      for (let i = 0; i < doc.lineCount; i++) {
        const line = doc.lineAt(i).text;
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

// ─── Activation ─────────────────────────────────────────────────────────────

export function activate(ctx: vscode.ExtensionContext) {
  console.log('Erelang language extension active (v3)');

  // Debug channel
  const debugCh = vscode.window.createOutputChannel('Erelang Language Debug');
  ctx.subscriptions.push(debugCh);
  setDebugChannel(debugCh);

  // Diagnostics — semicolons, entity constructors, undefined action calls
  const semiDiags = vscode.languages.createDiagnosticCollection('erelang');
  ctx.subscriptions.push(semiDiags);
  const refreshDiags = (d: vscode.TextDocument) => {
    if (d.isClosed || d.languageId !== 'erelang') return;
    try { validateDocument(d, semiDiags); } catch { /* never crash the host */ }
  };
  ctx.subscriptions.push(vscode.workspace.onDidOpenTextDocument(refreshDiags));
  ctx.subscriptions.push(vscode.workspace.onDidSaveTextDocument(d => {
    invalidateImportCache(d.uri.toString());
    invalidateDefCache(d.uri.toString());
    invalidateEntityMemberCache(d.uri.toString());
    refreshDiags(d);
  }));
  for (const d of vscode.workspace.textDocuments) refreshDiags(d);

  // No-op code action provider — prevents VS Code from stalling searching for one.
  // The only diagnostic we emit is "Missing semicolon", whose fix is adding a semicolon.
  // VS Code already handles this via the built-in quick-fix (insert `;`) without us.
  ctx.subscriptions.push(
    vscode.languages.registerCodeActionsProvider(
      { language: 'erelang' },
      {
        provideCodeActions(_doc, _range, _ctx, _token): vscode.CodeAction[] {
          return [];
        },
      },
    ),
  );

  // No-op document formatter — prevents VS Code from stalling searching for one on save.
  ctx.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider(
      { language: 'erelang' },
      {
        provideDocumentFormattingEdits(_doc, _opts, _token): vscode.TextEdit[] {
          return [];
        },
      },
    ),
  );

  // Debug command: dump completion context to output panel
  ctx.subscriptions.push(
    vscode.commands.registerCommand('erelang.debugCompletionContext', () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'erelang') {
        vscode.window.showWarningMessage('Open an Erelang file first.');
        return;
      }
      const cur    = editor.selection.active;
      const line   = editor.document.lineAt(cur.line).text;
      const prefix = line.slice(0, cur.character);
      const col    = collect(editor.document, cur.line);

      debugCh.appendLine('═══ Erelang Completion Context ═══');
      debugCh.appendLine(`cursor:       ${cur.line + 1}:${cur.character + 1}`);
      debugCh.appendLine(`line:         ${line}`);
      debugCh.appendLine(`prefix:       ${prefix}`);
      debugCh.appendLine(`foreach:      ${JSON.stringify(parseForEachHeader(line))}`);
      debugCh.appendLine(`locals:       ${[...col.locals].join(', ')      || 'none'}`);
      debugCh.appendLine(`arrays:       ${[...col.arrays].join(', ')      || 'none'}`);
      debugCh.appendLine(`dictionaries: ${[...col.dictionaries].join(', ') || 'none'}`);
      debugCh.appendLine(`foreachCtx:   ${isForeachColonCtx(prefix)}`);
      debugCh.appendLine(`dictLitCtx:   ${isDictLiteralCtx(prefix)}`);
      debugCh.appendLine('');
      debugCh.show(true);
      vscode.window.showInformationMessage('Context dumped → Output > Erelang Language Debug');
    })
  );

  // Completions — `.` for members, `#` for includes. No work on backspace.
  ctx.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      { language: 'erelang' },
      new ErelangCompletionProvider(),
      '#', '.', '<', '"', '/', ' ',
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
