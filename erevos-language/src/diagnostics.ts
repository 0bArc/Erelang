import * as vscode from 'vscode';
import { isCoreBuiltin } from './manifest';
import { getDocumentIndex, isInStringLiteral } from './symbols';

const KEYWORD_CALLS = new Set([
  'if', 'while', 'for', 'switch', 'match', 'catch', 'sizeof', 'typeof',
  'decltype', 'alignof', 'offsetof', 'lambda', 'action',
  'dynamic_cast', 'reinterpret_cast', 'bit_cast', 'bitcast',
]);

const BUILTIN_SET = { has: (w: string) => isCoreBuiltin(w) };
const MAX_VALIDATION_LINES = 50_000;
const MAX_LINE_LENGTH = 20_000;

function safeRange(doc: vscode.TextDocument, line: number, start: number, end: number): vscode.Range | null {
  if (line < 0 || line >= doc.lineCount) return null;
  const lineLen = doc.lineAt(line).text.length;
  const s = Math.max(0, Math.min(start, lineLen));
  const e = Math.max(s, Math.min(end, lineLen));
  try {
    return new vscode.Range(line, s, line, e);
  } catch {
    return null;
  }
}

const MAX_DIAGNOSTICS = 500;

export function validateDocument(
  doc: vscode.TextDocument,
  coll: vscode.DiagnosticCollection,
  cancelled: () => boolean = () => false,
): void {
  try {
    if (doc.isClosed || doc.languageId !== 'erelang') {
      coll.delete(doc.uri);
      return;
    }
    if (doc.lineCount > MAX_VALIDATION_LINES) {
      coll.set(doc.uri, []);
      return;
    }

    const diags: vscode.Diagnostic[] = [];
    const symbols = getDocumentIndex(doc).symbols;
    const defined = symbols.actions;
    const userTypes = new Set([
      ...symbols.entities,
      ...symbols.structs,
      ...symbols.enums,
      ...symbols.typeAliases,
    ]);

    for (let i = 0; i < doc.lineCount; i++) {
      if ((i & 255) === 0 && cancelled()) return;
      const text = doc.lineAt(i).text;
      if (text.length > MAX_LINE_LENGTH) continue;

      if (/(?:^|\s)(?:public|private|export)?\s*(?:async\s+)?action\s+\w+/.test(text)) continue;
      if (/(?:^|\s)(?:public|private|export)?\s*(?:async\s+)?(?:void|int|double|float|string|str|bool|char|auto)\s+\w+\s*\(/.test(text)) continue;

      const callRe = /\b([A-Za-z_]\w*)\s*\(/g;
      let m: RegExpExecArray | null;
      while ((m = callRe.exec(text))) {
        const name = m[1];
        if (KEYWORD_CALLS.has(name)) continue;
        if (defined.has(name)) continue;
        if (BUILTIN_SET.has(name)) continue;
        if (m.index > 0 && text[m.index - 1] === '.') continue;

        const before = text.slice(0, m.index);
        if (before.includes('//')) continue;
        if (isInStringLiteral(text, m.index)) continue;

        if (/\bnew\s+$/.test(before)) {
          if (!userTypes.has(name)) {
            const range = safeRange(doc, i, m.index, m.index + name.length);
            if (!range) continue;
            const d = new vscode.Diagnostic(
              range,
              `Unknown entity: '${name}' - not defined in this file`,
              vscode.DiagnosticSeverity.Error,
            );
            d.source = 'erelang';
            diags.push(d);
            if (diags.length >= MAX_DIAGNOSTICS) break;
          }
          continue;
        }

        const range = safeRange(doc, i, m.index, m.index + name.length);
        if (!range) continue;
        const d = new vscode.Diagnostic(
          range,
          `Unknown action or function: '${name}' - not defined in this file`,
          vscode.DiagnosticSeverity.Error,
        );
        d.source = 'erelang';
        d.code = 'TC001';
        diags.push(d);
        if (diags.length >= MAX_DIAGNOSTICS) break;
      }
      if (diags.length >= MAX_DIAGNOSTICS) break;
    }

    if (!cancelled()) coll.set(doc.uri, diags);
  } catch {
    if (!cancelled()) {
      try { coll.set(doc.uri, []); } catch {}
    }
  }
}
