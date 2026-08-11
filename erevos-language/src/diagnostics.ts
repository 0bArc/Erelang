// =============================================================================
// Erelang — Semicolon diagnostics
// =============================================================================

import * as vscode from 'vscode';

/**
 * Returns true when the line is a statement that requires a trailing semicolon.
 *
 * Lines that do NOT need one:
 *  - empty / comment / preprocessor / decorator
 *  - already end with ; { } : , ( [
 *  - control-flow headers (if/else/while/for/switch/…)
 *  - declaration headers (action/entity/struct/enum/hook/field)
 *  - struct/dict field type annotations  (name: Type)
 */
export function needsSemicolon(line: string): boolean {
  const t = line.trim();
  if (!t) return false;

  // Comments, preprocessor, decorators
  if (t.startsWith('//') || t.startsWith('#') || t.startsWith('@')) return false;
  if (t.startsWith('/*') || t.startsWith('*') || t.endsWith('*/'))  return false;

  // Already terminated
  if (t.endsWith(';') || t.endsWith('{') || t.endsWith('}') || t.endsWith(':')) return false;
  if (t.endsWith(',') || t.endsWith('(') || t.endsWith('['))                    return false;

  // Control-flow headers — no semicolon on their own line
  if (/^(if|else|while|for|switch|match|try|catch|do|repeat|unsafe|parallel|namespace)\b/.test(t)) return false;

  // Declaration headers — entity/action/struct/enum/hook/field do not end with ;
  if (/^(?:public|private|export)?\s*(?:async\s+)?(?:action|entity|struct|enum|hook|field)\b/.test(t)) return false;

  // Struct / dict field annotations: identifier: Type[,]
  if (/^[A-Za-z_]\w*\s*:\s*[A-Za-z_][\w<>,\s]*[,]?$/.test(t))      return false;
  if (/^(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*.+[,]?$/.test(t))   return false;

  return true;
}

export function validateSemicolons(doc: vscode.TextDocument, coll: vscode.DiagnosticCollection): void {
  if (doc.languageId !== 'erelang') return;
  const diags: vscode.Diagnostic[] = [];
  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    if (!needsSemicolon(text)) continue;
    const len   = text.trimEnd().length;
    const start = Math.max(0, len - 1);
    const range = new vscode.Range(i, start, i, len);
    const d     = new vscode.Diagnostic(range, 'Missing semicolon (;)', vscode.DiagnosticSeverity.Error);
    d.source    = 'erelang';
    diags.push(d);
  }
  coll.set(doc.uri, diags);
}
