// =============================================================================
// Erelang — Symbol collection
// =============================================================================

import * as vscode from 'vscode';
import {
  ENTITY_RE, ACTION_RE, TYPED_FUNC_RE, FIELD_RE, STRUCT_RE, ENUM_RE,
  TYPE_ALIAS_RE, HOOK_RE, LET_RE, GLOBAL_RE, LANGUAGE_KEYWORDS,
} from './constants';
import { CollectedSymbols, WordToken, RangeToken } from './types';

const KEYWORD_SET = new Set<string>(LANGUAGE_KEYWORDS);

type Versioned<T> = { version: number; value: T; at: number };
const STICKY_MS = 250;
const _symbolCache   = new Map<string, Versioned<CollectedSymbols>>();
const _instanceCache = new Map<string, Versioned<Map<string, string>>>();
const _typeNameCache = new Map<string, Versioned<Set<string>>>();

function stickyGet<T>(
  cache: Map<string, Versioned<T>>,
  key: string,
  version: number,
  compute: () => T,
): T {
  const hit = cache.get(key);
  if (hit && (hit.version === version || Date.now() - hit.at < STICKY_MS)) return hit.value;
  const value = compute();
  cache.set(key, { version, value, at: Date.now() });
  return value;
}

// ─── Identifier Utilities ────────────────────────────────────────────────────

export function isIdentStart(ch: string): boolean { return /[A-Za-z_]/.test(ch); }
export function isIdentPart(ch: string):  boolean { return /[A-Za-z0-9_]/.test(ch); }

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

// ─── For-Each Header Parsing ─────────────────────────────────────────────────

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

// ─── Main Symbol Collection ──────────────────────────────────────────────────

export function collect(doc: vscode.TextDocument, uptoLine?: number): CollectedSymbols {
  const end = Math.min(uptoLine ?? doc.lineCount - 1, doc.lineCount - 1);
  const full = end >= doc.lineCount - 1;
  if (full) {
    return stickyGet(_symbolCache, doc.uri.toString(), doc.version, () => collectRange(doc, end));
  }
  return collectRange(doc, end);
}

function collectRange(doc: vscode.TextDocument, end: number): CollectedSymbols {
  const out: CollectedSymbols = {
    entities: new Set(), actions: new Set(), fields: new Set(),
    hooks: new Set(), globals: new Set(), locals: new Set(),
    arrays: new Set(), dictionaries: new Set(),
    structs: new Set(), enums: new Set(), typeAliases: new Set(),
    structFields: new Map(), enumMembers: new Map(),
  };
  let activeStruct: string | null = null;
  let activeEnum:   string | null = null;

  for (let i = 0; i <= end; i++) {
    const text = doc.lineAt(i).text;
    let m: RegExpExecArray | null;

    if ((m = ENTITY_RE.exec(text)))     out.entities.add(m[1]);
    if ((m = STRUCT_RE.exec(text)))     { out.structs.add(m[1]); activeStruct = m[1]; out.structFields.set(m[1], out.structFields.get(m[1]) ?? new Set()); }
    if ((m = ENUM_RE.exec(text)))       { out.enums.add(m[1]); activeEnum = m[1]; out.enumMembers.set(m[1], out.enumMembers.get(m[1]) ?? new Set()); }
    if ((m = TYPE_ALIAS_RE.exec(text))) out.typeAliases.add(m[1]);
    if ((m = ACTION_RE.exec(text)))     out.actions.add(m[1]);
    if ((m = TYPED_FUNC_RE.exec(text))) out.actions.add(m[1]);
    if ((m = FIELD_RE.exec(text)))      out.fields.add(m[1]);
    if ((m = HOOK_RE.exec(text)))       out.hooks.add(m[1]);
    if ((m = GLOBAL_RE.exec(text)))     out.globals.add(m[1]);
    if ((m = LET_RE.exec(text)))        out.locals.add(m[1]);

    const decl = /^\s*(let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap)\s+([A-Za-z_]\w*)\s*=\s*(.+)\s*$/.exec(text);
    if (decl) {
      const tw  = decl[1].toLowerCase();
      const vn  = decl[2];
      const rhs = decl[3].trim();
      if (tw === 'array' || rhs.startsWith('list_new(') || rhs.startsWith('[')) out.arrays.add(vn);
      if (tw === 'map' || tw === 'dictionary' || tw === 'hashmap' || rhs.startsWith('dict_new(') || rhs.startsWith('hashmap_new(') || rhs.startsWith('{')) out.dictionaries.add(vn);
    }

    const gen = /^\s*(?:constexpr\s+)?(?:static\s+)?(Array<[^>\n]{0,80}>|Map<[^>\n]{0,80}>|HashMap<[^>\n]{0,80}>)\s+([A-Za-z_]\w*)\s*=/.exec(text);
    if (gen) {
      const tw = gen[1];
      const vn = gen[2];
      if (tw.startsWith('Array<'))                           out.arrays.add(vn);
      if (tw.startsWith('Map<') || tw.startsWith('HashMap<')) out.dictionaries.add(vn);
      out.locals.add(vn);
    }

    // User type locals: `Counter c = new Counter();`
    const userTyped = /^\s*(?:public|private|export)?\s*([A-Za-z_]\w*(?:<[^;>\n]{0,80}>)?)\s+([A-Za-z_]\w*)\s*(?:=|;)/.exec(text);
    if (userTyped) {
      const typeName = userTyped[1].replace(/<.*$/, '');
      if (!KEYWORD_SET.has(typeName)) {
        out.locals.add(userTyped[2]);
      }
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
  }
  return out;
}

// ─── Entity Instance Tracking ────────────────────────────────────────────────

/** Returns varName → EntityTypeName for all `var = new EntityName(...)` up to uptoLine. */
export function collectEntityInstances(doc: vscode.TextDocument, uptoLine?: number): Map<string, string> {
  const end = Math.min(uptoLine ?? doc.lineCount - 1, doc.lineCount - 1);
  const full = end >= doc.lineCount - 1;
  if (full) {
    return stickyGet(_instanceCache, doc.uri.toString(), doc.version, () => scanEntityInstances(doc, end));
  }
  return scanEntityInstances(doc, end);
}

function scanEntityInstances(doc: vscode.TextDocument, end: number): Map<string, string> {
  const varToEntity = new Map<string, string>();
  for (let i = 0; i <= end; i++) {
    const text = doc.lineAt(i).text;
    const m = /\b([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*)/.exec(text);
    if (m) varToEntity.set(m[1], m[2]);
  }
  return varToEntity;
}

/** Entity, struct, enum, and type-alias names declared in this document. */
export function collectUserTypeNames(doc: vscode.TextDocument): Set<string> {
  return stickyGet(_typeNameCache, doc.uri.toString(), doc.version, () => {
    const col = collect(doc);
    return new Set([...col.entities, ...col.structs, ...col.enums, ...col.typeAliases]);
  });
}

/** True when `index` sits inside an unclosed `"..."` or `'...'` on this line. */
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

// ─── Combined Entity Member Scan (single pass for both actions + fields) ─────

export interface EntityMembers {
  actions: Map<string, Set<string>>;
  fields:  Map<string, Set<string>>;
}

const _entityMemberCache = new Map<string, Versioned<EntityMembers>>();

/** Drop versioned caches for a document (optional; version mismatch already misses). */
export function invalidateEntityMemberCache(docUri?: string): void {
  if (docUri) {
    _entityMemberCache.delete(docUri);
    _symbolCache.delete(docUri);
    _instanceCache.delete(docUri);
    _typeNameCache.delete(docUri);
  } else {
    _entityMemberCache.clear();
    _symbolCache.clear();
    _instanceCache.clear();
    _typeNameCache.clear();
  }
}

/** Returns EntityName → Set<actionName> and EntityName → Set<fieldName> in a single pass. */
export function collectEntityMembers(doc: vscode.TextDocument): EntityMembers {
  return stickyGet(_entityMemberCache, doc.uri.toString(), doc.version, () => {
    const actions = new Map<string, Set<string>>();
    const fields  = new Map<string, Set<string>>();
    let currentEntity: string | null = null;
    let braceDepth = 0;

    for (let i = 0; i < doc.lineCount; i++) {
      const text = doc.lineAt(i).text;
      const em = ENTITY_RE.exec(text);
      if (em) {
        currentEntity = em[1];
        if (!actions.has(currentEntity)) { actions.set(currentEntity, new Set()); fields.set(currentEntity, new Set()); }
        braceDepth = 0;
      }

      if (currentEntity !== null) {
        braceDepth += (text.match(/\{/g) ?? []).length;
        braceDepth -= (text.match(/\}/g) ?? []).length;

        const am = ACTION_RE.exec(text);
        if (am) actions.get(currentEntity)?.add(am[1]);

        const tm = TYPED_FUNC_RE.exec(text);
        if (tm) actions.get(currentEntity)?.add(tm[1]);

        const fm = FIELD_RE.exec(text);
        if (fm) fields.get(currentEntity)?.add(fm[1]);

        if (braceDepth <= 0 && text.includes('}')) currentEntity = null;
      }
    }

    return { actions, fields };
  });
}

/** Returns EntityName → Set<actionName> for all entity blocks in the document. */
export function collectEntityActions(doc: vscode.TextDocument): Map<string, Set<string>> {
  return collectEntityMembers(doc).actions;
}

/** Returns EntityName → Set<fieldName> for all entity blocks in the document. */
export function collectEntityFields(doc: vscode.TextDocument): Map<string, Set<string>> {
  return collectEntityMembers(doc).fields;
}
