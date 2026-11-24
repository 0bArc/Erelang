import * as vscode from 'vscode';
import {
  ENTITY_RE, ACTION_RE, TYPED_FUNC_RE, FIELD_RE, STRUCT_RE, ENUM_RE,
  TYPE_ALIAS_RE, TRAIT_RE, HOOK_RE, LET_RE, GLOBAL_RE,
  NAMESPACE_RE,
} from './constants';
import {
  CollectedSymbols, Declaration, DocumentIndex, EntityMembers, OutlineSymbol, WordToken, RangeToken,
} from './types';
import { isKeyword, isTypeWord } from './manifest';

const KEYWORD_SET = { has: (w: string) => isKeyword(w) && !isTypeWord(w) };

// `Type name`, `Type<...> name {N}`, optionally after a modifier, followed by = ; or end of line.
const TYPED_DECL_RE = /^\s*(?:(?:public|private|export|global|const|static|constexpr|let)\s+)*([A-Za-z_]\w*(?:\s*<[^;=\n]{0,80}>)?)\s+([A-Za-z_]\w*)\s*(?:\{[^}\n]{0,40}\}\s*)?(?:=\s*(.*?))?\s*;?\s*$/;
// `let name: Type = ...`
const LET_TYPED_RE = /^\s*(?:let|const|global)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>, ]*)\s*(?:=\s*(.*?))?\s*;?\s*$/;
// Parameters inside an action header.
const ACTION_PARAMS_RE = /\baction\s+[A-Za-z_]\w*\s*(?:<[^>]*>)?\s*\(([^)]*)\)/;
// `for (Type name : xs)` and `for (Type k, Type v : m)`
const FOREACH_TYPED_RE = /\bfor\s*\(([^:)]*?)(?::| in )/;

/** Above this, skip full-file symbol scan — keeps AI bulk edits from freezing the host. */
export const MAX_INDEX_LINES = 8_000;
export const MAX_INDEX_CHARS = 500_000;
export const MAX_INDEX_LINE_CHARS = 20_000;

const _indexCache = new Map<string, DocumentIndex>();

function emptySymbols(): CollectedSymbols {
  return {
    entities: new Set(), actions: new Set(), fields: new Set(),
    hooks: new Set(), globals: new Set(), locals: new Set(),
    arrays: new Set(), dictionaries: new Set(),
    structs: new Set(), enums: new Set(), typeAliases: new Set(),
    structFields: new Map(), enumMembers: new Map(),
    namespaces: new Set(), namespaceMembers: new Map(),
  };
}

function emptyIndex(version: number): DocumentIndex {
  return {
    version,
    declarations: new Map(),
    symbols: emptySymbols(),
    entityInstances: new Map(),
    entityMembers: { actions: new Map(), fields: new Map() },
    outline: [],
  };
}

export function isIdentStart(ch: string): boolean {
  const c = ch.charCodeAt(0);
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95;
}

export function isIdentPart(ch: string): boolean {
  const c = ch.charCodeAt(0);
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95;
}

export function scanWords(line: string): WordToken[] {
  const words: WordToken[] = [];
  let i = 0;
  while (i < line.length) {
    if (!isIdentStart(line[i])) { i++; continue; }
    const start = i++;
    while (i < line.length && isIdentPart(line[i])) i++;
    words.push({ text: line.slice(start, i), start, length: i - start });
  }
  return words;
}

export function parseForEachHeader(line: string): { loopVars: RangeToken[]; iterable: RangeToken | null } | null {
  const fi = line.indexOf('for');
  if (fi < 0) return null;
  if (fi > 0 && isIdentPart(line[fi - 1])) return null;
  if (fi + 3 < line.length && isIdentPart(line[fi + 3])) return null;

  const lp = line.indexOf('(', fi + 3);
  if (lp < 0) return null;
  const rp = line.indexOf(')', lp + 1);
  if (rp < 0) return null;

  const inside = line.slice(lp + 1, rp);
  const colon  = inside.indexOf(':');
  const inKw   = colon >= 0 ? -1 : inside.indexOf(' in ');
  if (colon < 0 && inKw < 0) return null;

  const split = colon >= 0 ? colon : inKw;
  const left  = inside.slice(0, split).trim();
  const right = colon >= 0 ? inside.slice(split + 1).trim() : inside.slice(split + 4).trim();
  if (!left || !right) return null;

  const loopVars: RangeToken[] = [];
  const leftOff = lp + 1 + inside.indexOf(left);
  let cursor = leftOff;
  for (const group of left.split(',').map(s => s.trim()).filter(Boolean)) {
    const pos   = line.indexOf(group, cursor);
    cursor      = pos + group.length;
    const words = scanWords(group);
    if (words.length === 0) continue;
    const last  = words[words.length - 1];
    loopVars.push({ start: pos + last.start, length: last.length });
  }

  const rightOff   = lp + 1 + inside.indexOf(right);
  const rightWords = scanWords(right);
  const iterable   = rightWords.length > 0
    ? { start: rightOff + rightWords[0].start, length: rightWords[0].length }
    : null;

  return loopVars.length > 0 ? { loopVars, iterable } : null;
}

export function foreachLocalNames(line: string): string[] {
  const parsed = parseForEachHeader(line);
  if (!parsed) return [];
  return parsed.loopVars
    .map(r => line.slice(r.start, r.start + r.length))
    .filter(n => n.length > 0);
}

function buildDocumentIndex(doc: vscode.TextDocument): DocumentIndex {
  const out = emptySymbols();
  const entityInstances = new Map<string, string>();
  const entityActions = new Map<string, Set<string>>();
  const entityFields = new Map<string, Set<string>>();
  const outline: OutlineSymbol[] = [];
  const declarations = new Map<string, Declaration[]>();
  const declare = (name: string, type: string, line: number, init?: string) => {
    const t = type.replace(/\s+/g, '');
    if (!t || isKeyword(t) && !isTypeWord(t)) return;
    let list = declarations.get(name);
    if (!list) { list = []; declarations.set(name, list); }
    list.push({ type: t, line, init });
  };

  let activeStruct: string | null = null;
  let activeEnum: string | null = null;
  let currentEntity: string | null = null;
  let currentNamespace: string | null = null;
  let braceDepth = 0;
  let namespaceDepth = 0;

  const end = doc.lineCount - 1;
  let scannedChars = 0;
  for (let i = 0; i <= end; i++) {
    const text = doc.lineAt(i).text;
    scannedChars += text.length;
    // A line-count cap alone does not protect against generated/minified sources.
    if (text.length > MAX_INDEX_LINE_CHARS || scannedChars > MAX_INDEX_CHARS) {
      return emptyIndex(doc.version);
    }
    let m: RegExpExecArray | null;

    if ((m = NAMESPACE_RE.exec(text))) {
      out.namespaces.add(m[1]);
      currentNamespace = m[1];
      if (!out.namespaceMembers.has(m[1])) out.namespaceMembers.set(m[1], new Set());
      namespaceDepth = 0;
      outline.push({ name: m[1], kind: 'namespace', line: i });
    }

    if ((m = ENTITY_RE.exec(text))) {
      out.entities.add(m[1]);
      currentEntity = m[1];
      if (!entityActions.has(currentEntity)) {
        entityActions.set(currentEntity, new Set());
        entityFields.set(currentEntity, new Set());
      }
      braceDepth = 0;
      outline.push({ name: m[1], kind: 'entity', line: i });
    }

    if ((m = STRUCT_RE.exec(text))) {
      out.structs.add(m[1]);
      activeStruct = m[1];
      out.structFields.set(m[1], out.structFields.get(m[1]) ?? new Set());
    }
    if ((m = ENUM_RE.exec(text))) {
      out.enums.add(m[1]);
      activeEnum = m[1];
      out.enumMembers.set(m[1], out.enumMembers.get(m[1]) ?? new Set());
    }
    if ((m = TYPE_ALIAS_RE.exec(text))) out.typeAliases.add(m[1]);
    if ((m = TRAIT_RE.exec(text))) out.typeAliases.add(m[1]);

    if ((m = ACTION_RE.exec(text))) {
      out.actions.add(m[1]);
      outline.push({ name: m[1], kind: 'action', line: i });
      if (currentEntity) entityActions.get(currentEntity)?.add(m[1]);
      if (currentNamespace) out.namespaceMembers.get(currentNamespace)?.add(m[1]);
    }
    if ((m = TYPED_FUNC_RE.exec(text))) {
      out.actions.add(m[1]);
      if (currentEntity) entityActions.get(currentEntity)?.add(m[1]);
      if (currentNamespace) out.namespaceMembers.get(currentNamespace)?.add(m[1]);
    }
    if ((m = FIELD_RE.exec(text))) {
      out.fields.add(m[1]);
      outline.push({ name: m[1], kind: 'field', line: i });
      if (currentEntity) entityFields.get(currentEntity)?.add(m[1]);
    }
    if ((m = HOOK_RE.exec(text))) {
      out.hooks.add(m[1]);
      outline.push({ name: m[1], kind: 'hook', line: i });
    }
    if ((m = GLOBAL_RE.exec(text))) out.globals.add(m[1]);
    if ((m = LET_RE.exec(text))) out.locals.add(m[1]);

    const decl = /^\s*(let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap)\s+([A-Za-z_]\w*)\s*=\s*(.+)\s*$/.exec(text);
    if (decl) {
      const tw  = decl[1].toLowerCase();
      const vn  = decl[2];
      const rhs = decl[3].trim();
      if (tw === 'array' || rhs.startsWith('list_new(') || rhs.startsWith('[')) out.arrays.add(vn);
      if (tw === 'map' || tw === 'dictionary' || tw === 'hashmap' || rhs.startsWith('dict_new(') || rhs.startsWith('hashmap_new(') || rhs.startsWith('{')) out.dictionaries.add(vn);
    }

    const gen = /^\s*(?:constexpr\s+)?(?:static\s+)?(Array<[^>\n]{0,80}>|Map<[^>\n]{0,80}>|HashMap<[^>\n]{0,80}>)\s+([A-Za-z_]\w*)\s*(?:\{[^}\n]{0,40}\}\s*)?(?:=|;)/.exec(text);
    if (gen) {
      const tw = gen[1];
      const vn = gen[2];
      if (tw.startsWith('Array<'))                           out.arrays.add(vn);
      if (tw.startsWith('Map<') || tw.startsWith('HashMap<')) out.dictionaries.add(vn);
      out.locals.add(vn);
    }

    const userTyped = /^\s*(?:public|private|export)?\s*([A-Za-z_]\w*(?:<[^;>\n]{0,80}>)?)\s+([A-Za-z_]\w*)\s*(?:=|;)/.exec(text);
    if (userTyped) {
      const typeName = userTyped[1].replace(/<.*$/, '');
      if (!KEYWORD_SET.has(typeName)) {
        out.locals.add(userTyped[2]);
      }
    }

    const typed = /^\s*(?:public|private|export)?\s*([A-Z][A-Za-z0-9_]*)\s+([A-Za-z_]\w*)\b/.exec(text);
    if (typed) entityInstances.set(typed[2], typed[1]);
    const neu = /\b([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*)/.exec(text);
    if (neu) entityInstances.set(neu[1], neu[2]);

    if (currentEntity !== null) {
      braceDepth += (text.match(/\{/g) ?? []).length;
      braceDepth -= (text.match(/\}/g) ?? []).length;
      if (braceDepth <= 0 && text.includes('}')) currentEntity = null;
    }

    if (currentNamespace !== null) {
      namespaceDepth += (text.match(/\{/g) ?? []).length;
      namespaceDepth -= (text.match(/\}/g) ?? []).length;
      if (namespaceDepth <= 0 && text.includes('}')) currentNamespace = null;
    }

    if (activeStruct) {
      const sf = /^\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>,]*)/.exec(text);
      if (sf) out.structFields.get(activeStruct)?.add(sf[1]);
      if (/\}/.test(text)) activeStruct = null;
    }

    if (activeEnum) {
      const em = /^\s*([A-Za-z_]\w*)\s*(?:,|;|$)/.exec(text);
      if (em && !/^\s*\}/.test(text)) out.enumMembers.get(activeEnum)?.add(em[1]);
      if (/\}/.test(text)) activeEnum = null;
    }

    for (const name of foreachLocalNames(text)) out.locals.add(name);

    // Typed declarations for member completion.
    const trimmed = text.replace(/\/\/.*$/, '');
    let d: RegExpExecArray | null;
    if ((d = LET_TYPED_RE.exec(trimmed))) {
      declare(d[1], d[2], i, d[3]);
    } else if ((d = TYPED_DECL_RE.exec(trimmed)) && !/^\s*(?:return|print|run|if|while|for|else)\b/.test(trimmed)) {
      const typeHead = d[1].replace(/<.*$/, '').trim();
      if (!isKeyword(typeHead) || isTypeWord(typeHead)) declare(d[2], d[1], i, d[3]);
    }
    if ((d = ACTION_PARAMS_RE.exec(trimmed))) {
      for (const part of d[1].split(',')) {
        const pm = /^\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>, ]*?)\s*$/.exec(part)
          ?? /^\s*([A-Za-z_][\w<>]*)\s+([A-Za-z_]\w*)\s*$/.exec(part);
        if (!pm) continue;
        if (/:/.test(part)) declare(pm[1], pm[2], i); else declare(pm[2], pm[1], i);
      }
    }
    if ((d = FOREACH_TYPED_RE.exec(trimmed))) {
      for (const part of d[1].split(',')) {
        const fm = /^\s*([A-Za-z_][\w<>]*)\s+([A-Za-z_]\w*)\s*$/.exec(part);
        if (fm) declare(fm[2], fm[1], i);
      }
    }
  }

  return {
    version: doc.version,
    declarations,
    symbols: out,
    entityInstances,
    entityMembers: { actions: entityActions, fields: entityFields },
    outline,
  };
}

export function getDocumentIndex(doc: vscode.TextDocument): DocumentIndex {
  const key = doc.uri.toString();
  const hit = _indexCache.get(key);
  if (hit && hit.version === doc.version) return hit;
  if (doc.lineCount > MAX_INDEX_LINES) {
    const empty = emptyIndex(doc.version);
    _indexCache.set(key, empty);
    return empty;
  }
  const built = buildDocumentIndex(doc);
  _indexCache.set(key, built);
  return built;
}

/** The declaration of `name` in effect at `line`: the nearest one at or above it. */
export function declarationAt(index: DocumentIndex, name: string, line: number): Declaration | undefined {
  const list = index.declarations.get(name);
  if (!list) return undefined;
  let best: Declaration | undefined;
  for (const d of list) {
    if (d.line <= line) best = d;
  }
  return best;
}

export function invalidateDocumentIndex(docUri?: string): void {
  if (docUri) _indexCache.delete(docUri);
  else _indexCache.clear();
}

/** @deprecated alias — clears the unified document index */
export function invalidateEntityMemberCache(docUri?: string): void {
  invalidateDocumentIndex(docUri);
}

export function collect(doc: vscode.TextDocument, _uptoLine?: number): CollectedSymbols {
  return getDocumentIndex(doc).symbols;
}

export function collectEntityInstances(doc: vscode.TextDocument, _uptoLine?: number): Map<string, string> {
  return getDocumentIndex(doc).entityInstances;
}

export function collectEntityMembers(doc: vscode.TextDocument, _uptoLine?: number): EntityMembers {
  return getDocumentIndex(doc).entityMembers;
}

export function collectUserTypeNames(doc: vscode.TextDocument): Set<string> {
  const col = getDocumentIndex(doc).symbols;
  return new Set([...col.entities, ...col.structs, ...col.enums, ...col.typeAliases]);
}

export function collectEntityActions(doc: vscode.TextDocument): Map<string, Set<string>> {
  return getDocumentIndex(doc).entityMembers.actions;
}

export function collectEntityFields(doc: vscode.TextDocument): Map<string, Set<string>> {
  return getDocumentIndex(doc).entityMembers.fields;
}

export function isInStringLiteral(line: string, index: number): boolean {
  let inDq = false;
  let inSq = false;
  const n = Math.min(index, line.length);
  for (let i = 0; i < n; i++) {
    const ch = line.charCodeAt(i);
    if (ch === 92 /* \\ */) { i++; continue; }
    if (ch === 34 /* " */ && !inSq) inDq = !inDq;
    else if (ch === 39 /* ' */ && !inDq) inSq = !inSq;
  }
  return inDq || inSq;
}
