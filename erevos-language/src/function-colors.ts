// =============================================================================
// Erelang — Color defined action call sites and entity/type names
// Decorations are reliable in Cursor; semantic tokens alone often don't paint.
// =============================================================================

import * as vscode from 'vscode';
import { collectDefinedActions } from './semantic-tokens';
import { collectUserTypeNames, isInStringLiteral, scanWords } from './symbols';

const SKIP = new Set([
  'if', 'while', 'for', 'switch', 'match', 'catch', 'sizeof', 'typeof',
  'decltype', 'alignof', 'offsetof', 'lambda', 'action', 'print', 'PRINT',
  'static_cast', 'dynamic_cast', 'reinterpret_cast', 'bit_cast', 'bitcast',
  'new', 'entity', 'struct', 'enum',
]);

export function createFunctionCallDecoration(): vscode.TextEditorDecorationType {
  return vscode.window.createTextEditorDecorationType({
    color: new vscode.ThemeColor('symbolIcon.functionForeground'),
  });
}

export function createTypeNameDecoration(): vscode.TextEditorDecorationType {
  return vscode.window.createTextEditorDecorationType({
    color: new vscode.ThemeColor('symbolIcon.classForeground'),
  });
}

function safeRange(doc: vscode.TextDocument, line: number, start: number, end: number): vscode.Range | null {
  if (line < 0 || line >= doc.lineCount) return null;
  const lineLen = doc.lineAt(line).text.length;
  const s = Math.max(0, Math.min(start, lineLen));
  const e = Math.max(s, Math.min(end, lineLen));
  if (s === e && e === 0 && start > 0) return null;
  try {
    return new vscode.Range(line, s, line, e);
  } catch {
    return null;
  }
}

export function collectDefinedCallRanges(doc: vscode.TextDocument): vscode.Range[] {
  if (doc.isClosed) return [];
  const ranges: vscode.Range[] = [];
  try {
    const defined = collectDefinedActions(doc);
    const types   = collectUserTypeNames(doc);

    for (let line = 0; line < doc.lineCount; line++) {
      const raw = doc.lineAt(line).text;
      const trimmed = raw.trimStart();
      if (trimmed.startsWith('//') || trimmed.startsWith('#')) continue;

      if (/(?:^|\s)(?:public|private|export)?\s*(?:async\s+)?action\s+\w+/.test(raw)) continue;
      if (/(?:^|\s)(?:public|private|export)?\s*(?:async\s+)?(?:void|int|double|float|string|str|bool|char|auto)\s+\w+\s*\(/.test(raw)) continue;

      const re = /\b([A-Za-z_]\w*)\s*\(/g;
      let m: RegExpExecArray | null;
      while ((m = re.exec(raw))) {
        const name = m[1];
        if (SKIP.has(name)) continue;
        if (types.has(name)) continue;
        if (!defined.has(name)) continue;
        if (m.index > 0 && raw[m.index - 1] === '.') continue;

        const before = raw.slice(0, m.index);
        if (before.includes('//')) continue;
        if (/\bnew\s+$/.test(before)) continue;
        if (isInStringLiteral(raw, m.index)) continue;

        const r = safeRange(doc, line, m.index, m.index + name.length);
        if (r) ranges.push(r);
      }
    }
  } catch {
    return [];
  }
  return ranges;
}

export function collectTypeNameRanges(doc: vscode.TextDocument): vscode.Range[] {
  if (doc.isClosed) return [];
  const ranges: vscode.Range[] = [];
  try {
    const types = collectUserTypeNames(doc);
    if (types.size === 0) return ranges;

    for (let line = 0; line < doc.lineCount; line++) {
      const raw = doc.lineAt(line).text;
      const trimmed = raw.trimStart();
      if (trimmed.startsWith('//') || trimmed.startsWith('#')) continue;

      for (const w of scanWords(raw)) {
        if (!types.has(w.text)) continue;
        if (w.start > 0 && raw[w.start - 1] === '.') continue;
        if (isInStringLiteral(raw, w.start)) continue;
        const slash = raw.indexOf('//');
        if (slash >= 0 && w.start >= slash) continue;

        const r = safeRange(doc, line, w.start, w.start + w.length);
        if (r) ranges.push(r);
      }
    }
  } catch {
    return [];
  }
  return ranges;
}

export function refreshFunctionCallDecorations(
  editor: vscode.TextEditor | undefined,
  decoration: vscode.TextEditorDecorationType,
): void {
  try {
    if (!editor || editor.document.isClosed || editor.document.languageId !== 'erelang') {
      return;
    }
    editor.setDecorations(decoration, collectDefinedCallRanges(editor.document));
  } catch {
    try { editor?.setDecorations(decoration, []); } catch { /* ignore */ }
  }
}

export function refreshTypeNameDecorations(
  editor: vscode.TextEditor | undefined,
  decoration: vscode.TextEditorDecorationType,
): void {
  try {
    if (!editor || editor.document.isClosed || editor.document.languageId !== 'erelang') {
      return;
    }
    editor.setDecorations(decoration, collectTypeNameRanges(editor.document));
  } catch {
    try { editor?.setDecorations(decoration, []); } catch { /* ignore */ }
  }
}
