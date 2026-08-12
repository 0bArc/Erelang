// =============================================================================
// Erelang — Diagnostics (semicolons, entity constructors, undefined action calls)
// =============================================================================

import * as vscode from 'vscode';
import { BUILT_INS } from './constants';
import { collectDefinedActions } from './semantic-tokens';
import { collectUserTypeNames, isInStringLiteral } from './symbols';

export function needsSemicolon(line: string): boolean {
  const t = line.trim();
  if (!t) return false;

  if (t.startsWith('//') || t.startsWith('#') || t.startsWith('@')) return false;
  if (t.startsWith('/*') || t.startsWith('*') || t.endsWith('*/'))  return false;

  if (t.endsWith(';') || t.endsWith('{') || t.endsWith('}') || t.endsWith(':')) return false;
  if (t.endsWith(',') || t.endsWith('(') || t.endsWith('['))                    return false;

  if (/^(if|else|while|for|switch|match|try|catch|do|repeat|unsafe|parallel|namespace)\b/.test(t)) return false;
  if (/^(?:public|private|export)?\s*(?:async\s+)?(?:action|entity|struct|enum|hook|field)\b/.test(t)) return false;

  if (/^[A-Za-z_]\w*\s*:\s*[A-Za-z_][\w<>,\s]*[,]?$/.test(t))      return false;
  if (/^(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*.+[,]?$/.test(t))   return false;

  return true;
}

const KEYWORD_CALLS = new Set([
  'if', 'while', 'for', 'switch', 'match', 'catch', 'sizeof', 'typeof',
  'decltype', 'alignof', 'offsetof', 'lambda', 'action', 'static_cast',
  'dynamic_cast', 'reinterpret_cast', 'bit_cast', 'bitcast',
]);

const BUILTIN_SET = new Set(BUILT_INS);

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

export function validateDocument(doc: vscode.TextDocument, coll: vscode.DiagnosticCollection): void {
  try {
    if (doc.isClosed || doc.languageId !== 'erelang') {
      coll.delete(doc.uri);
      return;
    }

    const diags: vscode.Diagnostic[] = [];
    // Use cached version — it was already invalidated by the done handler
    const defined = collectDefinedActions(doc);
    const userTypes = collectUserTypeNames(doc);

    for (let i = 0; i < doc.lineCount; i++) {
      const text = doc.lineAt(i).text;

      if (needsSemicolon(text)) {
        const len = text.trimEnd().length;
        const start = Math.max(0, len - 1);
        const range = safeRange(doc, i, start, len);
        if (range) {
          const d = new vscode.Diagnostic(range, 'Missing semicolon (;)', vscode.DiagnosticSeverity.Error);
          d.source = 'erelang';
          diags.push(d);
        }
      }

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

        // `new Counter()` is entity construction, not an action call.
        if (/\bnew\s+$/.test(before)) {
          if (!userTypes.has(name)) {
            const range = safeRange(doc, i, m.index, m.index + name.length);
            if (!range) continue;
            const d = new vscode.Diagnostic(
              range,
              `Unknown entity: '${name}' — not defined in this file`,
              vscode.DiagnosticSeverity.Error,
            );
            d.source = 'erelang';
            diags.push(d);
          }
          continue;
        }

        const range = safeRange(doc, i, m.index, m.index + name.length);
        if (!range) continue;
        const d = new vscode.Diagnostic(
          range,
          `Unknown action or function: '${name}' — not defined in this file`,
          vscode.DiagnosticSeverity.Error,
        );
        d.source = 'erelang';
        d.code = 'TC001';
        diags.push(d);
      }
    }

    coll.set(doc.uri, diags);
  } catch {
    try { coll.set(doc.uri, []); } catch { /* ignore */ }
  }
}

/** @deprecated use validateDocument */
export function validateSemicolons(doc: vscode.TextDocument, coll: vscode.DiagnosticCollection): void {
  validateDocument(doc, coll);
}
