// Builtin knowledge for the extension, from `erelang --dump-builtins --json`.
//
// A copy generated at package time ships in resources/builtins.json (npm run gen). At startup the
// extension also asks the installed erelang for a fresh manifest, so a newer erelang build shows up
// in completion without repackaging. No vscode import here so the unit tests can load this file.
import * as fs from 'fs';
import * as path from 'path';
import { execFile } from 'child_process';

export interface ManifestMember {
  name: string;
  builtin?: string;
  min: number;
  max: number;
  returns: string;
  doc: string;
  hidden?: boolean;
}

export interface ManifestModule {
  id: string;
  paths: string[];
  alias: string;
  summary: string;
  experimental?: boolean;
  members: ManifestMember[];
}

export interface ManifestType {
  kind: string;
  summary: string;
  members: ManifestMember[];
}

export interface Manifest {
  version: number;
  modules: ManifestModule[];
  core: ManifestMember[];
  types: ManifestType[];
  keywords: string[];
  typeWords: string[];
}

const EMPTY: Manifest = { version: 0, modules: [], core: [], types: [], keywords: [], typeWords: [] };

export const BUNDLED_MANIFEST_PATH = path.join(__dirname, '..', 'resources', 'builtins.json');

function parse(text: string): Manifest | undefined {
  try {
    const m = JSON.parse(text) as Manifest;
    if (!m || !Array.isArray(m.modules) || !Array.isArray(m.core)) return undefined;
    return m;
  } catch {
    return undefined;
  }
}

function loadBundled(): Manifest {
  try {
    return parse(fs.readFileSync(BUNDLED_MANIFEST_PATH, 'utf8')) ?? EMPTY;
  } catch {
    return EMPTY;
  }
}

let current: Manifest = loadBundled();
let generation = 0;
const listeners: Array<() => void> = [];

export function manifest(): Manifest { return current; }
/** Increases whenever the manifest is replaced, so caches can key on it. */
export function manifestGeneration(): number { return generation; }
export function onManifestChanged(fn: () => void): void { listeners.push(fn); }

export function setManifest(m: Manifest): void {
  current = m;
  generation++;
  rebuildIndexes();
  for (const fn of listeners) {
    try { fn(); } catch { /* listener errors must not break refresh */ }
  }
}

/**
 * Ask an erelang executable for its manifest and use it when it parses. The result is cached next
 * to the bundled copy keyed by exe path and modification time, so startup stays fast.
 */
export function refreshFromExecutable(exe: string, cacheDir?: string, timeoutMs = 5000): Promise<boolean> {
  return new Promise(resolve => {
    let stamp = '';
    try { stamp = `${exe}|${fs.statSync(exe).mtimeMs}`; } catch { resolve(false); return; }
    const cacheFile = cacheDir ? path.join(cacheDir, 'builtins.cache.json') : undefined;
    if (cacheFile) {
      try {
        const cached = JSON.parse(fs.readFileSync(cacheFile, 'utf8')) as { stamp: string; manifest: Manifest };
        if (cached.stamp === stamp && cached.manifest && Array.isArray(cached.manifest.modules)) {
          setManifest(cached.manifest);
          resolve(true);
          return;
        }
      } catch { /* no cache yet */ }
    }
    execFile(exe, ['--dump-builtins', '--json'], { timeout: timeoutMs, windowsHide: true, maxBuffer: 8 * 1024 * 1024 },
      (err, stdout) => {
        if (err) { resolve(false); return; }
        const m = parse(String(stdout));
        if (!m) { resolve(false); return; }
        setManifest(m);
        if (cacheFile) {
          try {
            fs.mkdirSync(path.dirname(cacheFile), { recursive: true });
            fs.writeFileSync(cacheFile, JSON.stringify({ stamp, manifest: m }));
          } catch { /* cache is optional */ }
        }
        resolve(true);
      });
  });
}

// ---- lookups ----------------------------------------------------------------------------------

let byPath = new Map<string, ManifestModule>();
let byKind = new Map<string, ManifestType>();
let coreVisible: ManifestMember[] = [];
let keywordSet = new Set<string>();
let typeWordSet = new Set<string>();
let builtinNameSet = new Set<string>();

function rebuildIndexes(): void {
  byPath = new Map();
  for (const m of current.modules) for (const p of m.paths) byPath.set(p.toLowerCase(), m);
  byKind = new Map(current.types.map(t => [t.kind, t]));
  coreVisible = current.core.filter(c => !c.hidden);
  keywordSet = new Set(current.keywords);
  typeWordSet = new Set(current.typeWords);
  builtinNameSet = new Set(current.core.map(c => c.name));
}
rebuildIndexes();

export function normalizeModulePath(spec: string): string {
  let s = spec.trim();
  if ((s.startsWith('<') && s.endsWith('>')) || (s.startsWith('"') && s.endsWith('"'))) s = s.slice(1, -1);
  return s.replace(/\\/g, '/').toLowerCase();
}

export function moduleForPath(spec: string): ManifestModule | undefined {
  return byPath.get(normalizeModulePath(spec));
}

export function allModulePaths(): string[] {
  const out: string[] = [];
  for (const m of current.modules) for (const p of m.paths) out.push(p);
  return out;
}

export function visibleMembers(m: ManifestModule): ManifestMember[] {
  return m.members.filter(x => !x.hidden);
}

export function typeSpec(kind: string): ManifestType | undefined { return byKind.get(kind); }
export function coreBuiltins(): ManifestMember[] { return coreVisible; }
export function isKeyword(word: string): boolean { return keywordSet.has(word); }
export function isTypeWord(word: string): boolean { return typeWordSet.has(word); }
export function isCoreBuiltin(name: string): boolean { return builtinNameSet.has(name); }
export function keywords(): string[] { return current.keywords; }

let lastSegmentGen = -1;
let lastSegmentSet = new Set<string>();
/** Core builtin names plus the last segment of dotted ones (string.split gives split). */
export function isBuiltinWord(word: string): boolean {
  if (lastSegmentGen !== generation) {
    lastSegmentGen = generation;
    lastSegmentSet = new Set();
    for (const c of current.core) {
      lastSegmentSet.add(c.name);
      const dot = c.name.lastIndexOf('.');
      if (dot >= 0) lastSegmentSet.add(c.name.slice(dot + 1));
    }
  }
  return lastSegmentSet.has(word);
}
export function typeWords(): string[] { return current.typeWords; }

export function signature(m: ManifestMember, prefix = ''): string {
  const args = m.max < 0 ? `${m.min}+ args` : m.min === m.max ? `${m.min} arg${m.min === 1 ? '' : 's'}` : `${m.min}..${m.max} args`;
  return `${prefix}${m.name}(${args}) -> ${m.returns}`;
}

/**
 * Member type for a receiver type name as written in source: Array<int>, Map<string,int>, string,
 * or a handle kind. Returns the manifest type kind, or undefined when nothing is known.
 */
export function typeKindFor(typeName: string): string | undefined {
  const t = typeName.replace(/\s+/g, '');
  const lower = t.toLowerCase();
  if (/^(array|list|vec)</.test(lower) || lower === 'array' || lower.endsWith('[]')) return 'array';
  if (/^(map|hashmap|dict|dictionary)</.test(lower) || lower === 'map' || lower === 'dict' || lower === 'dictionary') return 'map';
  if (lower === 'string' || lower === 'str') return 'string';
  if (byKind.has(lower)) return lower;
  return undefined;
}
