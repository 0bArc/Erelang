import * as vscode from 'vscode';
import * as fs     from 'fs';
import * as path   from 'path';
import { INCLUDE_ALIAS_RE, IMPORT_ALIAS_RE, MODULE_METHODS, ACTION_RE } from './constants';
import { ImportedSymbols } from './types';

const _importCache = new Map<string, { version: number; value: Promise<ImportedSymbols> }>();
const _actionCache = new Map<string, { mtimeMs: number; names: Set<string> }>();
const _dirCache = new Map<string, { mtimeMs: number; entries: DirEntry[] }>();
const _statCache = new Map<string, { mtimeMs: number; isFile: boolean } | null>();

export type DirEntry = { name: string; isDirectory: boolean; isFile: boolean };

export function invalidateImportCache(docUri?: string): void {
  if (docUri) _importCache.delete(docUri);
  else _importCache.clear();
}

export function invalidateDirCache(): void {
  _dirCache.clear();
  _statCache.clear();
}

export function normalizeSpec(spec: string): string {
  const t = spec.trim();
  if (t.startsWith('<') && t.endsWith('>')) return t.slice(1, -1).toLowerCase();
  if (t.startsWith('"') && t.endsWith('"')) return t.slice(1, -1).toLowerCase();
  return t.toLowerCase();
}

export function defaultAlias(spec: string): string {
  return normalizeSpec(spec).split('/').pop()!.replace(/[^A-Za-z0-9_]/g, '_');
}

function getPluginRoots(): string[] {
  const roots: string[] = [];
  const localAppData = process.env.LOCALAPPDATA || '';
  if (localAppData) roots.push(path.join(localAppData, 'Erelang', 'Plugins'));
  const appData = process.env.APPDATA || '';
  if (appData) roots.push(path.join(appData, 'Erelang', 'Plugins'));
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    roots.push(path.join(folder.uri.fsPath, 'plugins'));
  }
  return roots;
}

async function cachedIsFile(filePath: string): Promise<boolean> {
  const hit = _statCache.get(filePath);
  if (hit !== undefined) {
    if (hit === null) return false;
    try {
      const st = await fs.promises.stat(filePath);
      if (st.mtimeMs === hit.mtimeMs) return hit.isFile;
      const isFile = st.isFile();
      _statCache.set(filePath, { mtimeMs: st.mtimeMs, isFile });
      return isFile;
    } catch {
      _statCache.set(filePath, null);
      return false;
    }
  }
  try {
    const st = await fs.promises.stat(filePath);
    const isFile = st.isFile();
    _statCache.set(filePath, { mtimeMs: st.mtimeMs, isFile });
    return isFile;
  } catch {
    _statCache.set(filePath, null);
    return false;
  }
}

export async function listDirectoryCached(dir: string): Promise<DirEntry[]> {
  try {
    const st = await fs.promises.stat(dir);
    if (!st.isDirectory()) return [];
    const hit = _dirCache.get(dir);
    if (hit && hit.mtimeMs === st.mtimeMs) return hit.entries;
    const raw = await fs.promises.readdir(dir, { withFileTypes: true });
    const entries: DirEntry[] = raw.map(e => ({
      name: e.name,
      isDirectory: e.isDirectory(),
      isFile: e.isFile(),
    }));
    _dirCache.set(dir, { mtimeMs: st.mtimeMs, entries });
    return entries;
  } catch {
    _dirCache.delete(dir);
    return [];
  }
}

function candidatePaths(doc: vscode.TextDocument, norm: string): string[] {
  const dir = path.dirname(doc.uri.fsPath);
  const bases: string[] = [path.resolve(dir, norm)];
  if (!path.extname(norm)) {
    for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.resolve(dir, norm + e));
  }
  for (const f of vscode.workspace.workspaceFolders ?? []) {
    bases.push(path.resolve(f.uri.fsPath, norm));
    if (!path.extname(norm)) {
      for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.resolve(f.uri.fsPath, norm + e));
    }
    const wsPlugins = path.join(f.uri.fsPath, 'plugins');
    bases.push(path.join(wsPlugins, norm));
    if (!path.extname(norm)) {
      for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.join(wsPlugins, norm + e));
    }
  }
  for (const pluginRoot of getPluginRoots()) {
    bases.push(path.join(pluginRoot, norm));
    if (!path.extname(norm)) {
      for (const e of ['.elan', '.ere', '.0bs']) bases.push(path.join(pluginRoot, norm + e));
    }
  }
  return bases;
}

export async function resolveIncludeFile(doc: vscode.TextDocument, spec: string): Promise<string | null> {
  const norm = normalizeSpec(spec);
  if (!norm || norm.startsWith('builtin/')) return null;
  for (const c of candidatePaths(doc, norm)) {
    if (await cachedIsFile(c)) return c;
  }
  return null;
}

export async function extractActions(filePath: string): Promise<Set<string>> {
  try {
    const mtimeMs = (await fs.promises.stat(filePath)).mtimeMs;
    const cached = _actionCache.get(filePath);
    if (cached?.mtimeMs === mtimeMs) return cached.names;
    const names = new Set<string>();
    for (const line of (await fs.promises.readFile(filePath, 'utf8')).split(/\r?\n/)) {
      const m = ACTION_RE.exec(line);
      if (m) names.add(m[1]);
    }
    _actionCache.set(filePath, { mtimeMs, names });
    return names;
  } catch {
    _actionCache.delete(filePath);
    return new Set();
  }
}

export function collectImports(doc: vscode.TextDocument): Promise<ImportedSymbols> {
  const docUri = doc.uri.toString();
  const cached = _importCache.get(docUri);
  if (cached && cached.version === doc.version) return cached.value;

  const value = (async () => {
    const aliasToActions = new Map<string, Set<string>>();
    const allActions = new Set<string>();
    const scanLines = Math.min(doc.lineCount, 2_000);
    for (let i = 0; i < scanLines; i++) {
      const text = doc.lineAt(i).text;
      let spec: string | null = null;
      let alias: string | null = null;

      const inc = INCLUDE_ALIAS_RE.exec(text);
      if (inc) {
        spec = inc[1];
        alias = inc[2] ?? defaultAlias(inc[1]);
      } else {
        const imp = IMPORT_ALIAS_RE.exec(text);
        if (imp) {
          const rawPath = imp[1] ?? imp[2] ?? imp[3] ?? imp[4] ?? '';
          spec = rawPath || null;
          alias = imp[5] ?? (spec ? defaultAlias(spec) : null);
        }
      }
      if (!spec || !alias) continue;

      const builtinMethods = MODULE_METHODS[normalizeSpec(spec)];
      if (builtinMethods) {
        aliasToActions.set(alias, new Set(builtinMethods));
        continue;
      }

      const resolved = await resolveIncludeFile(doc, spec);
      if (!resolved) continue;
      const actions = await extractActions(resolved);
      if (actions.size === 0) continue;
      aliasToActions.set(alias, actions);
      for (const action of actions) allActions.add(action);
    }
    return { aliasToActions, allActions };
  })();

  _importCache.set(docUri, { version: doc.version, value });
  return value;
}
