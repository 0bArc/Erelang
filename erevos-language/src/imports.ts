// =============================================================================
// Erelang -- Import / #include resolution
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

// ─── Plugin Root Resolution ──────────────────────────────────────────────────

function getPluginRoots(): string[] {
  const roots: string[] = [];
  const localAppData = process.env.LOCALAPPDATA || '';
  if (localAppData) {
    roots.push(path.join(localAppData, 'Erelang', 'Plugins'));
  }
  const appData = process.env.APPDATA || '';
  if (appData) {
    roots.push(path.join(appData, 'Erelang', 'Plugins'));
  }
  // Also search workspace folders for plugin directories
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    roots.push(path.join(folder.uri.fsPath, 'plugins'));
  }
  return roots;
}

function parsePluginScripts(elpPath: string): string[] {
  const scripts: string[] = [];
  try {
    const content = fs.readFileSync(elpPath, 'utf8');
    const includeRe = /<include>([^<]+)<\/include>/g;
    let m: RegExpExecArray | null;
    while ((m = includeRe.exec(content)) !== null) {
      const rel = m[1].trim();
      const dir = path.dirname(elpPath);
      const full = path.resolve(dir, rel);
      if (fs.existsSync(full)) {
        scripts.push(full);
      }
    }
  } catch { /* skip */ }
  return scripts;
}

function findPluginScripts(): string[] {
  const scripts: string[] = [];
  for (const root of getPluginRoots()) {
    try {
      if (!fs.existsSync(root)) continue;
      const entries = fs.readdirSync(root, { withFileTypes: true });
      for (const entry of entries) {
        if (!entry.isDirectory()) continue;
        const elpFile = path.join(root, entry.name, 'project.elp');
        if (fs.existsSync(elpFile)) {
          const pluginScripts = parsePluginScripts(elpFile);
          scripts.push(...pluginScripts);
        }
      }
    } catch { /* skip */ }
  }
  return scripts;
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
    // Also search workspace plugins/
    const wsPlugins = path.join(f.uri.fsPath, 'plugins');
    if (fs.existsSync(wsPlugins)) {
      bases.push(path.join(wsPlugins, norm));
      if (!path.extname(norm)) {
        for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.join(wsPlugins, norm + e));
      }
    }
  }
  // Search plugin roots
  for (const pluginRoot of getPluginRoots()) {
    const full = path.join(pluginRoot, norm);
    if (fs.existsSync(full)) { bases.push(full); continue; }
    if (!path.extname(norm)) {
      for (const e of ['.elan', '.ere', '.0bs']) {
        const withExt = path.join(pluginRoot, norm + e);
        if (fs.existsSync(withExt)) bases.push(withExt);
      }
    }
    // Also crawl subdirectories for plugin files
    try {
      if (fs.existsSync(pluginRoot)) {
        const entries = fs.readdirSync(pluginRoot, { withFileTypes: true });
        for (const entry of entries) {
          if (!entry.isDirectory()) continue;
          const subFile = path.join(pluginRoot, entry.name, norm);
          if (fs.existsSync(subFile)) bases.push(subFile);
          if (!path.extname(norm)) {
            for (const e of ['.elan', '.ere', '.0bs']) {
              const withExt = path.join(pluginRoot, entry.name, norm + e);
              if (fs.existsSync(withExt)) bases.push(withExt);
            }
          }
        }
      }
    } catch { /* skip */ }
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

  // Also collect from plugin directories so plugin actions are available
  const pluginScripts = findPluginScripts();
  for (const scriptPath of pluginScripts) {
    const acts = extractActions(scriptPath);
    if (acts.size === 0) continue;
    for (const a of acts) allActions.add(a);
  }

  return { aliasToActions, allActions };
}
