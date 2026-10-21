import { Diagnostic, DiagnosticSeverity } from 'vscode-languageserver/node';

// Matches typechecker output (see format_diagnostic in src/typechecker.cpp):
//   [error] TC001: message (context) at 12:5
//   [warn] TC120: message (context)          <- position is optional
const TAGGED_RE = /^\[(error|warn|hint)\]\s+(\w+):\s+(.*?)(?:\s+at\s+(\d+):(\d+))?\s*$/;

/** Codes that are noise for library files checked on their own. */
const DOWNGRADE_TO_INFO = new Set(['TC111']);

export function parseCheckOutput(stderr: string): Diagnostic[] {
  const out: Diagnostic[] = [];
  for (const raw of stderr.split(/\r?\n/)) {
    const t = raw.trim();
    if (!t) continue;
    const m = TAGGED_RE.exec(t);
    if (!m) {
      // Loader / internal errors ("Check error: ...") have no tag.
      out.push({
        severity: DiagnosticSeverity.Error,
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
        message: t,
        source: 'erelang',
      });
      continue;
    }
    let sev = m[1] === 'warn' ? DiagnosticSeverity.Warning
      : m[1] === 'hint' ? DiagnosticSeverity.Information
      : DiagnosticSeverity.Error;
    if (DOWNGRADE_TO_INFO.has(m[2])) sev = DiagnosticSeverity.Information;
    const line = m[4] ? Math.max(0, parseInt(m[4], 10) - 1) : 0;
    const col = m[5] ? Math.max(0, parseInt(m[5], 10) - 1) : 0;
    out.push({
      severity: sev,
      range: { start: { line, character: col }, end: { line, character: col + 1 } },
      message: `${m[2]}: ${m[3]}`,
      code: m[2],
      source: 'erelang',
    });
  }
  return out;
}
