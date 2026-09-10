import * as vscode from 'vscode';
import {
  ACTION_RE, TYPED_FUNC_RE, ENTITY_RE, STRUCT_RE, ENUM_RE, TYPE_ALIAS_RE,
  FIELD_RE, LET_RE, GLOBAL_RE, BUILT_INS, LANGUAGE_KEYWORDS,
} from './constants';
import { collect, isInStringLiteral, scanWords } from './symbols';

const TOKEN_TYPES = [
  'function',
  'method',
  'class',
  'parameter',
  'variable',
  'property',
  'enumMember',
  'number',
  'string',
  'keyword',
] as const;

const TOKEN_MODIFIERS = ['declaration', 'defaultLibrary', 'readonly'] as const;

const legend = new vscode.SemanticTokensLegend(
  [...TOKEN_TYPES],
  [...TOKEN_MODIFIERS],
);

const T = {
  function: 0,
  method: 1,
  class: 2,
  parameter: 3,
  variable: 4,
  property: 5,
  enumMember: 6,
  number: 7,
  string: 8,
  keyword: 9,
} as const;

const M = {
  declaration: 1 << 0,
  defaultLibrary: 1 << 1,
  readonly: 1 << 2,
} as const;

/** Above this, TextMate-only — semantic pass is skipped to keep the host responsive. */
const MAX_SEMANTIC_LINES = 8_000;
const MAX_LINE_LENGTH = 4_000;
const MAX_TOKENS = 12_000;

const KEYWORD_SET = new Set(LANGUAGE_KEYWORDS);
const BUILTIN_SET = new Set(BUILT_INS.map(n => n.includes('.') ? n.slice(n.lastIndexOf('.') + 1) : n));
const CONTROL_CALLS = new Set([
  'if', 'while', 'for', 'switch', 'match', 'catch', 'sizeof', 'typeof',
  'decltype', 'alignof', 'offsetof', 'lambda', 'action', 'new',
  'entity', 'struct', 'enum', 'static_cast', 'dynamic_cast',
  'reinterpret_cast', 'bit_cast', 'bitcast',
]);
const TYPE_WORDS = new Set([
  'void', 'int', 'double', 'float', 'string', 'str', 'bool', 'char', 'auto',
  'any', 'pointer', 'Array', 'Map', 'HashMap', 'array', 'map', 'hashmap',
  'dictionary', 'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'uint',
  'unsigned',
]);

const _defCache = new Map<string, { version: number; value: Set<string> }>();

export function collectDefinedActions(doc: vscode.TextDocument): Set<string> {
  if (doc.isClosed) return new Set();
  const key = doc.uri.toString();
  const cached = _defCache.get(key);
  if (cached && cached.version === doc.version) return cached.value;
  return collectDefinedActionsUncached(doc);
}

export function collectDefinedActionsUncached(doc: vscode.TextDocument): Set<string> {
  const names = new Set<string>();
  try {
    if (doc.isClosed) return names;
    const limit = Math.min(doc.lineCount, MAX_SEMANTIC_LINES);
    for (let i = 0; i < limit; i++) {
      const text = doc.lineAt(i).text;
      const am = ACTION_RE.exec(text);
      if (am) names.add(am[1]);
      const tm = TYPED_FUNC_RE.exec(text);
      if (tm) names.add(tm[1]);
    }
  } catch {}
  _defCache.set(doc.uri.toString(), { version: doc.version, value: names });
  return names;
}

export function invalidateDefCache(docUri?: string): void {
  if (docUri) _defCache.delete(docUri); else _defCache.clear();
}

export interface ClassifiedToken {
  line: number;
  start: number;
  length: number;
  type: number;
  modifiers: number;
}

function pushToken(
  out: ClassifiedToken[],
  line: number,
  col: number,
  len: number,
  type: number,
  mods: number,
  lineLen: number,
): boolean {
  if (out.length >= MAX_TOKENS) return false;
  if (col < 0 || len <= 0 || col >= lineLen) return true;
  const clamped = Math.min(len, lineLen - col);
  if (clamped <= 0) return true;
  out.push({ line, start: col, length: clamped, type, modifiers: mods });
  return out.length < MAX_TOKENS;
}

function classifyParamList(text: string, line: number, out: ClassifiedToken[]): void {
  const open = text.indexOf('(');
  const close = text.lastIndexOf(')');
  if (open < 0 || close <= open) return;
  const inside = text.slice(open + 1, close);
  if (!inside.trim()) return;
  let cursor = open + 1;
  for (const part of inside.split(',')) {
    const trimmed = part.trim();
    if (!trimmed) {
      cursor += part.length + 1;
      continue;
    }
    const pos = text.indexOf(trimmed, cursor);
    if (pos < 0) continue;
    cursor = pos + trimmed.length + 1;
    const colon = trimmed.indexOf(':');
    if (colon >= 0) {
      const left = trimmed.slice(0, colon).trim();
      const leftWords = scanWords(left);
      if (leftWords.length > 0) {
        const name = leftWords[leftWords.length - 1];
        if (!KEYWORD_SET.has(name.text) && !TYPE_WORDS.has(name.text)) {
          if (!pushToken(out, line, pos + left.indexOf(name.text), name.length, T.parameter, M.declaration, text.length)) return;
        }
      }
      continue;
    }
    const words = scanWords(trimmed);
    if (words.length === 0) continue;
    const last = words[words.length - 1];
    if (KEYWORD_SET.has(last.text) || TYPE_WORDS.has(last.text)) continue;
    if (!pushToken(out, line, pos + last.start, last.length, T.parameter, M.declaration, text.length)) return;
  }
}

function lineHasQuotes(text: string): boolean {
  return text.includes('"') || text.includes("'");
}

/**
 * Lightweight single-pass classifier. Literals (strings/numbers/true/false) stay
 * on TextMate — semantic tokens only cover declarations, calls, types, and members.
 */
export function classifyDocumentTokens(
  doc: vscode.TextDocument,
  token?: vscode.CancellationToken,
): ClassifiedToken[] {
  const out: ClassifiedToken[] = [];
  if (doc.isClosed) return out;
  if (doc.lineCount > MAX_SEMANTIC_LINES) return out;

  // One symbol scan; actions come from the same pass.
  const symbols = collect(doc);
  const types = new Set([
    ...symbols.entities,
    ...symbols.structs,
    ...symbols.enums,
    ...symbols.typeAliases,
  ]);
  const defined = symbols.actions.size > 0 ? symbols.actions : collectDefinedActions(doc);
  _defCache.set(doc.uri.toString(), { version: doc.version, value: defined });

  const limit = Math.min(doc.lineCount, MAX_SEMANTIC_LINES);

  for (let i = 0; i < limit; i++) {
    if (token?.isCancellationRequested) break;
    if ((i & 63) === 0 && token?.isCancellationRequested) break;
    if (out.length >= MAX_TOKENS) break;

    let text = doc.lineAt(i).text;
    if (text.length > MAX_LINE_LENGTH) text = text.slice(0, MAX_LINE_LENGTH);
    const trimmed = text.trimStart();
    if (!trimmed || trimmed.startsWith('//') || trimmed.startsWith('#')) continue;

    const lineLen = text.length;
    const quoted = lineHasQuotes(text);
    const inStr = (col: number) => quoted && isInStringLiteral(text, col);

    const entity = ENTITY_RE.exec(text) || STRUCT_RE.exec(text) || TYPE_ALIAS_RE.exec(text);
    if (entity) {
      const col = text.indexOf(entity[1], entity.index);
      if (!pushToken(out, i, col, entity[1].length, T.class, M.declaration, lineLen)) break;
    }
    const enm = ENUM_RE.exec(text);
    if (enm) {
      const col = text.indexOf(enm[1], enm.index);
      if (!pushToken(out, i, col, enm[1].length, T.class, M.declaration, lineLen)) break;
      const brace = text.indexOf('{');
      if (brace >= 0) {
        const close = text.indexOf('}', brace + 1);
        const body = close > brace ? text.slice(brace + 1, close) : text.slice(brace + 1);
        let cursor = brace + 1;
        for (const part of body.split(',')) {
          const name = part.trim();
          if (!name) { cursor += part.length + 1; continue; }
          const pos = text.indexOf(name, cursor);
          if (pos < 0) continue;
          cursor = pos + name.length + 1;
          const ident = /^[A-Za-z_]\w*/.exec(name);
          if (ident && !pushToken(out, i, pos, ident[0].length, T.enumMember, M.declaration, lineLen)) break;
        }
      }
    }

    let declName: string | null = null;
    const am = ACTION_RE.exec(text);
    if (am) {
      declName = am[1];
      const col = text.indexOf(am[1], am.index);
      if (!pushToken(out, i, col, am[1].length, T.function, M.declaration, lineLen)) break;
      classifyParamList(text, i, out);
    }
    const tm = TYPED_FUNC_RE.exec(text);
    if (tm) {
      declName = tm[1];
      const col = text.indexOf(tm[1], tm.index);
      if (!pushToken(out, i, col, tm[1].length, T.function, M.declaration, lineLen)) break;
      classifyParamList(text, i, out);
    }

    const field = FIELD_RE.exec(text);
    if (field) {
      const col = text.indexOf(field[1], field.index);
      if (!pushToken(out, i, col, field[1].length, T.property, M.declaration, lineLen)) break;
    }

    const letm = LET_RE.exec(text);
    if (letm && !am && !tm) {
      const col = text.indexOf(letm[1], letm.index);
      const readonly = /^\s*const\b/.test(text) ? M.readonly : 0;
      if (!pushToken(out, i, col, letm[1].length, T.variable, M.declaration | readonly, lineLen)) break;
    } else if (!am && !tm) {
      const typedVar = /^\s*(?:public|private|export)?\s*([A-Z][A-Za-z0-9_]*)\s+([A-Za-z_]\w*)\b/.exec(text);
      if (typedVar && (types.has(typedVar[1]) || TYPE_WORDS.has(typedVar[1]))) {
        if (!pushToken(out, i, text.indexOf(typedVar[1]), typedVar[1].length, T.class, 0, lineLen)) break;
        if (!pushToken(out, i, text.indexOf(typedVar[2], typedVar.index), typedVar[2].length, T.variable, M.declaration, lineLen)) break;
      }
    }
    const glob = GLOBAL_RE.exec(text);
    if (glob) {
      const col = text.indexOf(glob[1], glob.index);
      if (!pushToken(out, i, col, glob[1].length, T.variable, M.declaration, lineLen)) break;
    }

    if (text.includes('for') && text.includes('(')) {
      const foreach = /\bfor\s*\(([^)]*)\)/.exec(text);
      if (foreach) {
        const inside = foreach[1];
        const colon = inside.indexOf(':');
        const inKw = colon >= 0 ? -1 : inside.indexOf(' in ');
        if (colon >= 0 || inKw >= 0) {
          const left = (colon >= 0 ? inside.slice(0, colon) : inside.slice(0, inKw)).trim();
          const leftOff = text.indexOf(left, foreach.index);
          if (leftOff >= 0) {
            for (const group of left.split(',').map(s => s.trim()).filter(Boolean)) {
              const words = scanWords(group);
              if (words.length === 0) continue;
              const last = words[words.length - 1];
              const gpos = text.indexOf(group, leftOff);
              if (gpos < 0) continue;
              if (!pushToken(out, i, gpos + last.start, last.length, T.parameter, M.declaration, lineLen)) break;
            }
          }
        }
      }
    }

    // Type names from the document (skip TYPE_WORDS — TextMate already colors those).
    if (types.size > 0) {
      for (const w of scanWords(text)) {
        if (!types.has(w.text)) continue;
        if (inStr(w.start)) continue;
        if (w.start > 0 && text[w.start - 1] === '.') continue;
        if (w.start >= 2 && text.slice(w.start - 2, w.start) === '::') continue;
        if (!pushToken(out, i, w.start, w.length, T.class, 0, lineLen)) break;
      }
    }

    if (text.includes('::')) {
      const scopeAccess = /\b([A-Za-z_]\w*)\s*::\s*([A-Za-z_]\w*)\b/g;
      let m: RegExpExecArray | null;
      while ((m = scopeAccess.exec(text))) {
        if (inStr(m.index)) continue;
        const memberCol = text.indexOf(m[2], m.index + m[1].length);
        if (!pushToken(out, i, m.index, m[1].length, T.class, 0, lineLen)) break;
        if (!pushToken(out, i, memberCol, m[2].length, T.enumMember, 0, lineLen)) break;
      }
    }

    if (text.includes('.')) {
      const memberCall = /\b([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*(?=\()/g;
      let m: RegExpExecArray | null;
      while ((m = memberCall.exec(text))) {
        if (inStr(m.index)) continue;
        const methodCol = m.index + m[1].length + 1;
        const mods = BUILTIN_SET.has(m[2]) ? M.defaultLibrary : 0;
        if (!pushToken(out, i, methodCol, m[2].length, T.method, mods, lineLen)) break;
      }

      const memberProp = /\b([A-Za-z_]\w*)\.([A-Za-z_]\w*)\b(?!\s*\()/g;
      while ((m = memberProp.exec(text))) {
        if (inStr(m.index)) continue;
        const owner = m[1];
        const propCol = m.index + m[1].length + 1;
        if (/^[A-Z]/.test(owner) || symbols.enums.has(owner)) {
          if (!pushToken(out, i, m.index, owner.length, T.class, 0, lineLen)) break;
          if (!pushToken(out, i, propCol, m[2].length, T.enumMember, 0, lineLen)) break;
        } else if (!pushToken(out, i, propCol, m[2].length, T.property, 0, lineLen)) {
          break;
        }
      }
    }

    if (text.includes('case')) {
      const caseMember = /\bcase\s+([A-Za-z_]\w*)\b/g;
      let m: RegExpExecArray | null;
      while ((m = caseMember.exec(text))) {
        if (inStr(m.index)) continue;
        if (!pushToken(out, i, m.index + m[0].lastIndexOf(m[1]), m[1].length, T.enumMember, 0, lineLen)) break;
      }
    }

    if (text.includes('(') || text.includes('print') || text.includes('PRINT')) {
      const callRe = /\b([A-Za-z_]\w*)\s*(?=\()/g;
      let m: RegExpExecArray | null;
      while ((m = callRe.exec(text))) {
        if (inStr(m.index)) continue;
        if (m.index > 0 && text[m.index - 1] === '.') continue;
        const name = m[1];
        if (declName && name === declName) continue;
        if (CONTROL_CALLS.has(name) || types.has(name) || TYPE_WORDS.has(name)) continue;
        if (KEYWORD_SET.has(name) && !defined.has(name) && !BUILTIN_SET.has(name)) continue;
        let mods = 0;
        if (BUILTIN_SET.has(name)) mods |= M.defaultLibrary;
        if (defined.has(name) || BUILTIN_SET.has(name)) {
          if (!pushToken(out, i, m.index, name.length, T.function, mods, lineLen)) break;
        }
      }

      const bareBuiltin = /\b(print|PRINT)\b(?!\s*\()/g;
      while ((m = bareBuiltin.exec(text))) {
        if (inStr(m.index)) continue;
        if (!pushToken(out, i, m.index, m[1].length, T.function, M.defaultLibrary, lineLen)) break;
      }
    }
  }

  return out;
}

export class ErelangSemanticTokensProvider implements vscode.DocumentSemanticTokensProvider {
  private readonly _empty = new vscode.SemanticTokensBuilder(legend).build();

  provideDocumentSemanticTokens(
    _doc: vscode.TextDocument,
    _token: vscode.CancellationToken,
  ): vscode.SemanticTokens {
    return this._empty;
  }

  invalidate(_docUri?: string): void {}
}

export {
  legend as erelangSemanticLegend,
  TOKEN_TYPES,
  TOKEN_MODIFIERS,
  T as SemanticTokenType,
  M as SemanticTokenModifier,
  MAX_SEMANTIC_LINES,
};
