// =============================================================================
// Erelang — Import / #include resolution
// =============================================================================

import * as vscode from 'vscode';
import * as fs     from 'fs';
import * as path   from 'path';
import { INCLUDE_ALIAS_RE, IMPORT_ALIAS_RE, MODULE_METHODS } from './constants';
import { ImportedSymbols } from './types';
import { ACTION_RE } from './constants';

// ─── Path Normalization ──────────────────────────────────────────────────────

export function normalizeSpec(spec: string): string {
  const t = spec.trim();
  if (t.startsWith('<') && t.endsWith('>')) return t.slice(1, -1).toLowerCase();
  if (t.startsWith('"') && t.endsWith('"')) return t.slice(1, -1).toLowerCase();
  return t.toLowerCase();
}

export function defaultAlias(spec: string): string {
  return normalizeSpec(spec).split('/').pop()!.replace(/[^A-Za-z0-9_]/g, '_');
}

// ─── File Resolution ─────────────────────────────────────────────────────────

/** Resolve an include spec to a concrete file path, or null if not found. */
export function resolveIncludeFile(doc: vscode.TextDocument, spec: string): string | null {
  const norm = normalizeSpec(spec);
  if (!norm || norm.startsWith('builtin/')) return null;
  const dir  = path.dirname(doc.uri.fsPath);
  const bases: string[] = [path.resolve(dir, norm)];
  if (!path.extname(norm)) {
    for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.resolve(dir, norm + e));
  }
  for (const f of vscode.workspace.workspaceFolders ?? []) {
    bases.push(path.resolve(f.uri.fsPath, norm));
    if (!path.extname(norm)) {
      for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.resolve(f.uri.fsPath, norm + e));
    }
  }
  for (const c of bases) {
    try { if (fs.existsSync(c) && fs.statSync(c).isFile()) return c; } catch { /* skip */ }
  }
  return null;
}

// ─── Action Extraction ───────────────────────────────────────────────────────

export function extractActions(filePath: string): Set<string> {
  const names = new Set<string>();
  try {
    for (const line of fs.readFileSync(filePath, 'utf8').split(/\r?\n/)) {
      const m = ACTION_RE.exec(line);
      if (m) names.add(m[1]);
    }
  } catch { /* skip */ }
  return names;
}

// ─── Import Collection ───────────────────────────────────────────────────────

export function collectImports(doc: vscode.TextDocument): ImportedSymbols {
  const aliasToActions = new Map<string, Set<string>>();
  const allActions     = new Set<string>();

  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    let spec:  string | null = null;
    let alias: string | null = null;

    const inc = INCLUDE_ALIAS_RE.exec(text);
    if (inc) {
      spec  = inc[1];
      alias = inc[2] ?? defaultAlias(inc[1]);
    } else {
      const imp = IMPORT_ALIAS_RE.exec(text);
      if (imp) {
        // Groups: 1=<angle>, 2="double", 3='single', 4=bare  5=alias
        const rawPath = imp[1] ?? imp[2] ?? imp[3] ?? imp[4] ?? '';
        spec  = rawPath || null;
        alias = imp[5] ?? (spec ? defaultAlias(spec) : null);
      }
    }
    if (!spec || !alias) continue;

    const builtinMethods = MODULE_METHODS[normalizeSpec(spec)];
    if (builtinMethods) { aliasToActions.set(alias, new Set(builtinMethods)); continue; }

    const resolved = resolveIncludeFile(doc, spec);
    if (!resolved) continue;
    const acts = extractActions(resolved);
    if (acts.size === 0) continue;
    aliasToActions.set(alias, acts);
    for (const a of acts) allActions.add(a);
  }
  return { aliasToActions, allActions };
}
