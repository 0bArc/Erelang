import * as path from 'path';
import { fileURLToPath, pathToFileURL } from 'url';
import { bindingIndex, tokenAt, Binding, SourceToken } from './scopeAnalysis';

export interface Definition { uri: string; binding: Binding; }
export type ReadSource = (uri: string) => Promise<string | undefined>;
export function sourceKey(uri: string): string {
  try {
    const native = path.resolve(fileURLToPath(uri));
    return process.platform === 'win32' ? native.toLowerCase() : native;
  } catch { return uri; }
}

/** Resolve through explicit imports only; an unrelated open file is never a definition. */
export async function projectDefinition(uri: string, text: string, pos: { line: number; character: number },
  read: ReadSource, visited = new Set<string>()): Promise<Definition | undefined> {
  if (visited.has(uri) || visited.size >= 32) return undefined;
  visited.add(uri);
  const index = bindingIndex(text);
  const token = tokenAt(index, pos);
  if (!token) return undefined;
  const at = index.tokens.indexOf(token);
  if (/^\s*(?:#|import\b)/.test(text.split('\n')[token.line])) return undefined;
  const local = index.resolve(token);
  if (local?.kind === 'action' && index.tokens[at + 1]?.text === '.') return undefined;
  if (local) return { uri, binding: local };
  if (index.tokens[at + 1]?.text === '.') return undefined;
  const qualifier = index.tokens[at - 1]?.text === '.' ? index.tokens[at - 2]?.text : undefined;
  if (qualifier && index.resolve(index.tokens[at - 2])) return undefined;
  const include = /^\s*(?:#\s*include\s*["<]([^">]+)[">]|import\s+["']([^"']+)["'])\s*(?:as\s+(\w+))?/gm;
  for (const match of text.matchAll(include)) {
    const spec = match[1] ?? match[2];
    if (spec.startsWith('builtin/') || spec.startsWith('std/')) continue;
    const alias = match[3] ?? path.basename(spec, path.extname(spec));
    if (qualifier && qualifier !== alias) continue;
    let base: string;
    try { base = path.resolve(path.dirname(fileURLToPath(uri)), spec); } catch { return undefined; }
    for (const candidate of path.extname(base) ? [base] : [base, base + '.elan']) {
      const targetUri = pathToFileURL(candidate).href;
      const source = await read(targetUri);
      if (source === undefined) continue;
      const target = bindingIndex(source);
      const binding = target.bindings.find(b => b.kind === 'action' && b.scope === 0 && b.token.text === token.text);
      if (binding) {
        const line = source.split('\n')[binding.token.line];
        if (!/\bprivate\b/.test(line)) return { uri: targetUri, binding };
      }
    }
  }
  return undefined;
}

export function definitionLocation(def: Definition) {
  const t = def.binding.token;
  return { uri: def.uri, range: tokenRange(t) };
}
export function tokenRange(t: SourceToken) {
  return { start: { line: t.line, character: t.start }, end: { line: t.line, character: t.start + t.text.length } };
}
