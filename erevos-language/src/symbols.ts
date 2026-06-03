// =============================================================================
// Erelang — Symbol collection
// =============================================================================

import * as vscode from 'vscode';
import {
  ENTITY_RE, ACTION_RE, FIELD_RE, STRUCT_RE, ENUM_RE,
  TYPE_ALIAS_RE, HOOK_RE, LET_RE, GLOBAL_RE,
} from './constants';
import { CollectedSymbols, WordToken, RangeToken } from './types';

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
  const out: CollectedSymbols = {
    entities: new Set(), actions: new Set(), fields: new Set(),
    hooks: new Set(), globals: new Set(), locals: new Set(),
    arrays: new Set(), dictionaries: new Set(),
    structs: new Set(), enums: new Set(), typeAliases: new Set(),
    structFields: new Map(), enumMembers: new Map(),
  };
  let activeStruct: string | null = null;
  let activeEnum:   string | null = null;
  const end = Math.min(uptoLine ?? doc.lineCount - 1, doc.lineCount - 1);

  for (let i = 0; i <= end; i++) {
    const text = doc.lineAt(i).text;
    let m: RegExpExecArray | null;

    if ((m = ENTITY_RE.exec(text)))     out.entities.add(m[1]);
    if ((m = STRUCT_RE.exec(text)))     { out.structs.add(m[1]); activeStruct = m[1]; out.structFields.set(m[1], out.structFields.get(m[1]) ?? new Set()); }
    if ((m = ENUM_RE.exec(text)))       { out.enums.add(m[1]); activeEnum = m[1]; out.enumMembers.set(m[1], out.enumMembers.get(m[1]) ?? new Set()); }
    if ((m = TYPE_ALIAS_RE.exec(text))) out.typeAliases.add(m[1]);
    if ((m = ACTION_RE.exec(text)))     out.actions.add(m[1]);
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

    const gen = /^\s*(?:constexpr\s+)?(?:static\s+)?(Array<[^>]+>|Map<[^>]+>|HashMap<[^>]+>)\s+([A-Za-z_]\w*)\s*=/.exec(text);
    if (gen) {
      const tw = gen[1];
      const vn = gen[2];
      if (tw.startsWith('Array<'))                           out.arrays.add(vn);
      if (tw.startsWith('Map<') || tw.startsWith('HashMap<')) out.dictionaries.add(vn);
      out.locals.add(vn);
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

/** Returns varName → EntityTypeName for all `let varName = new EntityName(...)` up to uptoLine. */
export function collectEntityInstances(doc: vscode.TextDocument, uptoLine?: number): Map<string, string> {
  const varToEntity = new Map<string, string>();
  const end = Math.min(uptoLine ?? doc.lineCount - 1, doc.lineCount - 1);
  for (let i = 0; i <= end; i++) {
    const text = doc.lineAt(i).text;
    const m = /\blet\s+([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*)/.exec(text);
    if (m) varToEntity.set(m[1], m[2]);
  }
  return varToEntity;
}

/** Returns EntityName → Set<actionName> for all entity blocks in the document. */
export function collectEntityActions(doc: vscode.TextDocument): Map<string, Set<string>> {
  const entityActions = new Map<string, Set<string>>();
  const entityFields  = new Map<string, Set<string>>();
  let currentEntity: string | null = null;
  let braceDepth = 0;

  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    const em = ENTITY_RE.exec(text);
    if (em) {
      currentEntity = em[1];
      if (!entityActions.has(currentEntity)) entityActions.set(currentEntity, new Set());
      if (!entityFields.has(currentEntity))  entityFields.set(currentEntity, new Set());
      braceDepth = 0;
    }

    if (currentEntity !== null) {
      braceDepth += (text.match(/\{/g) ?? []).length;
      braceDepth -= (text.match(/\}/g) ?? []).length;

      const am = ACTION_RE.exec(text);
      if (am) entityActions.get(currentEntity)?.add(am[1]);

      const fm = FIELD_RE.exec(text);
      if (fm) entityFields.get(currentEntity)?.add(fm[1]);

      if (braceDepth <= 0 && text.includes('}')) currentEntity = null;
    }
  }
  return entityActions;
}

/** Returns EntityName → Set<fieldName> for all entity blocks in the document. */
export function collectEntityFields(doc: vscode.TextDocument): Map<string, Set<string>> {
  const entityFields = new Map<string, Set<string>>();
  let currentEntity: string | null = null;
  let braceDepth = 0;

  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    const em = ENTITY_RE.exec(text);
    if (em) {
      currentEntity = em[1];
      if (!entityFields.has(currentEntity)) entityFields.set(currentEntity, new Set());
      braceDepth = 0;
    }

    if (currentEntity !== null) {
      braceDepth += (text.match(/\{/g) ?? []).length;
      braceDepth -= (text.match(/\}/g) ?? []).length;

      const fm = FIELD_RE.exec(text);
      if (fm) entityFields.get(currentEntity)?.add(fm[1]);

      if (braceDepth <= 0 && text.includes('}')) currentEntity = null;
    }
  }
  return entityFields;
}
