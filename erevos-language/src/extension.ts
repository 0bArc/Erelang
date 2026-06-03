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
import { validateSemicolons } from './diagnostics';
import {
  ErelangCompletionProvider, setDebugChannel, isForeachColonCtx, isDictLiteralCtx,
  isControlFlowAwaitingBody, markAutoSuggestFromEdit,
} from './completions';
import { collect } from './symbols';
import { parseForEachHeader } from './symbols';

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
    const uris = await vscode.workspace.findFiles('**/*.{0bs,ere,elan}');
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

function cursorAfterChange(change: vscode.TextDocumentContentChangeEvent): vscode.Position {
  return new vscode.Position(
    change.range.start.line,
    change.range.start.character + change.text.length,
  );
}

// ─── Auto-Retrigger Logic ────────────────────────────────────────────────────

function shouldRetrigger(
  change:   vscode.TextDocumentContentChangeEvent,
  prefix:   string,
  fullLine: string,
  cursor:   vscode.Position,
): boolean {
  const typed     = change.text.length === 1 && /[A-Za-z0-9_]/.test(change.text);
  const openBrace = change.text.includes('{');
  const deleted   = change.text.length === 0 && change.rangeLength > 0;
  if (!typed && !openBrace && !deleted) return false;
  if (/^\s*\/\//.test(prefix)) return false;

  if (isControlFlowAwaitingBody(prefix.replace(/\{\s*$/, ''))) return false;

  if (
    /^\s*#\s*[A-Za-z]*\s*(?:[<"].*)?$/i.test(prefix) ||
    /^\s*#\s*include\s+[A-Za-z0-9_./\\-]*$/i.test(prefix)
  ) {
    if (deleted) return true;
    return change.text.length === 1 && /[#A-Za-z0-9_./\\"<>\- ]/.test(change.text);
  }

  if (openBrace) {
    const beforeBrace = prefix.replace(/\{\s*$/, '').trimEnd();
    if (isControlFlowAwaitingBody(beforeBrace)) return false;
    return true;
  }
  if (deleted)   return /[A-Za-z_]\w+$/.test(prefix);
  if (/[A-Za-z_]\w+$/.test(prefix)) return true;
  return false;
}

// ─── Activation ─────────────────────────────────────────────────────────────

export function activate(ctx: vscode.ExtensionContext) {
  console.log('Erelang language extension active (v3)');

  // Debug channel
  const debugCh = vscode.window.createOutputChannel('Erelang Language Debug');
  ctx.subscriptions.push(debugCh);
  setDebugChannel(debugCh);

  // Semicolon diagnostics
  const semiDiags = vscode.languages.createDiagnosticCollection('erelang-semicolons');
  ctx.subscriptions.push(semiDiags);
  const refreshSemi = (d: vscode.TextDocument) => validateSemicolons(d, semiDiags);
  ctx.subscriptions.push(vscode.workspace.onDidOpenTextDocument(refreshSemi));
  ctx.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => refreshSemi(e.document)));
  ctx.subscriptions.push(vscode.workspace.onDidSaveTextDocument(refreshSemi));
  for (const d of vscode.workspace.textDocuments) refreshSemi(d);

  // Auto-retrigger backup when quickSuggestions disabled in user settings
  let retriggerTimer: NodeJS.Timeout | undefined;
  ctx.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument(event => {
      if (!vscode.workspace.getConfiguration('erelang').get<boolean>('autoSuggestIdentifiers', true)) return;
      if (event.document.languageId !== 'erelang') return;
      if (event.contentChanges.length === 0) return;
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.toString() !== event.document.uri.toString()) return;

      const shouldTrigger = event.contentChanges.some(change => {
        const cursor   = cursorAfterChange(change);
        const lineText = event.document.lineAt(cursor.line).text;
        const prefix   = lineText.slice(0, cursor.character);
        return shouldRetrigger(change, prefix, lineText, cursor);
      });
      if (!shouldTrigger) return;

      if (retriggerTimer) clearTimeout(retriggerTimer);
      retriggerTimer = setTimeout(() => {
        markAutoSuggestFromEdit();
        void vscode.commands.executeCommand('editor.action.triggerSuggest');
      }, 15);
    })
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

  // Register providers
  ctx.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      { language: 'erelang' },
      new ErelangCompletionProvider(),
      '#', '.', ':', '"', '<', '/', '\\',
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
