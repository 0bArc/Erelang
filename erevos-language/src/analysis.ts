import {
  ACTION_RE, TYPED_FUNC_RE, FIELD_RE, STRUCT_RE, ENUM_RE, ENTITY_RE,
  TYPE_ALIAS_RE, TRAIT_RE, HOOK_RE, LET_RE, GLOBAL_RE, LANGUAGE_KEYWORDS,
  NAMESPACE_RE, BUILT_INS,
} from './constants';

export type SymbolKindName =
  | 'local' | 'parameter' | 'action' | 'struct' | 'enum' | 'field'
  | 'entity' | 'global' | 'hook' | 'namespace' | 'typeAlias' | 'enumMember'
  | 'builtin' | 'keyword';

export interface DeclInfo {
  name: string;
  kind: SymbolKindName;
  line: number;
  start: number;
  length: number;
  detail?: string;
}

export interface TextPos {
  line: number;
  character: number;
}

export interface TextRange {
  start: TextPos;
  end: TextPos;
}

export interface HoverInfo {
  name: string;
  kind: SymbolKindName;
  detail?: string;
  range: TextRange;
}

export interface RefLocation {
  line: number;
  start: number;
  length: number;
}

const KEYWORD_SET = new Set(LANGUAGE_KEYWORDS);
const BUILTIN_SET = new Set(BUILT_INS as readonly string[]);
const TYPE_WORDS = new Set([
  'void', 'int', 'double', 'float', 'string', 'str', 'bool', 'char', 'auto',
  'any', 'pointer', 'Array', 'Map', 'HashMap', 'array', 'map', 'hashmap',
  'dictionary', 'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'uint',
  'unsigned', 'never', 'unit',
]);

export function isIdentStart(ch: string): boolean {
  const c = ch.charCodeAt(0);
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95;
}

export function isIdentPart(ch: string): boolean {
  const c = ch.charCodeAt(0);
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95;
}

export function splitLines(text: string): string[] {
  return text.split(/\r?\n/);
}

export function wordAt(line: string, character: number): { word: string; start: number; end: number } | null {
  if (character < 0 || character > line.length) return null;
  let start = character;
  let end = character;
  while (start > 0 && isIdentPart(line[start - 1])) start--;
  while (end < line.length && isIdentPart(line[end])) end++;
  if (start === end) return null;
  if (start < line.length && !isIdentStart(line[start])) return null;
  return { word: line.slice(start, end), start, end };
}

export function isInStringLiteral(line: string, index: number): boolean {
  let inDq = false;
  let inSq = false;
  const n = Math.min(index, line.length);
  for (let i = 0; i < n; i++) {
    const ch = line.charCodeAt(i);
    if (ch === 92) { i++; continue; }
    if (ch === 34 && !inSq) inDq = !inDq;
    else if (ch === 39 && !inDq) inSq = !inSq;
  }
  return inDq || inSq;
}

export function lineCommentStart(line: string): number {
  let inDq = false;
  let inSq = false;
  for (let i = 0; i < line.length - 1; i++) {
    const ch = line.charCodeAt(i);
    if (ch === 92) { i++; continue; }
    if (ch === 34 && !inSq) inDq = !inDq;
    else if (ch === 39 && !inDq) inSq = !inSq;
    else if (!inDq && !inSq && ch === 47 && line.charCodeAt(i + 1) === 47) return i;
  }
  return -1;
}

export function isInComment(line: string, index: number): boolean {
  const c = lineCommentStart(line);
  return c >= 0 && index >= c;
}

function nameOffset(line: string, name: string, from = 0): number {
  let i = from;
  while (i < line.length) {
    const at = line.indexOf(name, i);
    if (at < 0) return -1;
    const before = at === 0 || !isIdentPart(line[at - 1]);
    const after = at + name.length >= line.length || !isIdentPart(line[at + name.length]);
    if (before && after && !isInStringLiteral(line, at) && !isInComment(line, at)) return at;
    i = at + 1;
  }
  return -1;
}

function addDecl(
  out: DeclInfo[],
  line: number,
  text: string,
  name: string,
  kind: SymbolKindName,
  detail?: string,
  from = 0,
): void {
  const start = nameOffset(text, name, from);
  if (start < 0) return;
  out.push({ name, kind, line, start, length: name.length, detail });
}

function parseParamList(inside: string): { name: string; type?: string }[] {
  const params: { name: string; type?: string }[] = [];
  if (!inside.trim()) return params;
  for (const raw of inside.split(',')) {
    const part = raw.trim();
    if (!part) continue;
    const colon = part.indexOf(':');
    if (colon >= 0) {
      const left = part.slice(0, colon).trim();
      const type = part.slice(colon + 1).trim().replace(/\s*=.*$/, '').trim();
      const words = left.match(/[A-Za-z_]\w*/g);
      if (!words || words.length === 0) continue;
      const name = words[words.length - 1];
      if (KEYWORD_SET.has(name) || TYPE_WORDS.has(name)) continue;
      params.push({ name, type: type || undefined });
      continue;
    }
    const words = part.match(/[A-Za-z_]\w*/g);
    if (!words || words.length === 0) continue;
    if (words.length >= 2) {
      const type = words[0];
      const name = words[words.length - 1];
      if (KEYWORD_SET.has(name)) continue;
      params.push({
        name,
        type: TYPE_WORDS.has(type) || /^[A-Z]/.test(type) ? type : undefined,
      });
    } else {
      const name = words[0];
      if (KEYWORD_SET.has(name) || TYPE_WORDS.has(name)) continue;
      params.push({ name });
    }
  }
  return params;
}

function actionHeader(line: string): { name: string; params: string; ret?: string; nameStart: number } | null {
  let m = ACTION_RE.exec(line);
  if (m) {
    const name = m[1];
    const nameStart = nameOffset(line, name);
    if (nameStart < 0) return null;
    const rest = line.slice(nameStart + name.length);
    const pm = /^\s*\(([^)]*)\)\s*(?::\s*([^{\n]+))?/.exec(rest);
    return {
      name,
      params: pm ? pm[1] : '',
      ret: pm?.[2]?.trim() || undefined,
      nameStart,
    };
  }
  m = TYPED_FUNC_RE.exec(line);
  if (m) {
    const name = m[1];
    const nameStart = nameOffset(line, name);
    if (nameStart < 0) return null;
    const retM = /^\s*(?:public|private|export)?\s*(?:async\s+)?(\w+(?:<[^>\n]{0,80}>)?)\s+/.exec(line);
    const ret = retM?.[1];
    const rest = line.slice(nameStart + name.length);
    const pm = /^\s*\(([^)]*)\)/.exec(rest);
    return {
      name,
      params: pm ? pm[1] : '',
      ret: ret && ret !== 'action' ? ret : undefined,
      nameStart,
    };
  }
  return null;
}

function typedLocal(line: string): { name: string; type?: string; start: number } | null {
  const colon = /^\s*(?:let|const)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>,\s]*)/.exec(line);
  if (colon) {
    const start = nameOffset(line, colon[1]);
    if (start < 0) return null;
    return { name: colon[1], type: colon[2].trim().replace(/\s*=.*$/, '').trim(), start };
  }
  const typed = /^\s*(?:public|private|export)?\s*(?:constexpr\s+)?(?:static\s+)?(Array<[^>\n]{0,80}>|Map<[^>\n]{0,80}>|HashMap<[^>\n]{0,80}>|[A-Za-z_]\w*(?:<[^>\n]{0,80}>)?)\s+([A-Za-z_]\w*)\s*(?:=|;)/.exec(line);
  if (typed) {
    const typeName = typed[1];
    const base = typeName.replace(/<.*$/, '');
    if (KEYWORD_SET.has(base) && !TYPE_WORDS.has(base) && base !== 'Array' && base !== 'Map' && base !== 'HashMap') {
      return null;
    }
    if (!TYPE_WORDS.has(base) && !/^[A-Z]/.test(base) && !typeName.includes('<')
      && !['let', 'const', 'int', 'string', 'str', 'bool', 'char', 'auto', 'double', 'float',
        'array', 'map', 'dictionary', 'hashmap'].includes(base)) {
      return null;
    }
    const start = nameOffset(line, typed[2]);
    if (start < 0) return null;
    return { name: typed[2], type: typeName, start };
  }
  const letm = LET_RE.exec(line);
  if (letm) {
    const start = nameOffset(line, letm[1]);
    if (start < 0) return null;
    const prefix = /^\s*((?:let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap|Array<[^>\n]{0,80}>|Map<[^>\n]{0,80}>|HashMap<[^>\n]{0,80}>|[A-Za-z_]\w*<[^>\n]{0,80}>))/.exec(line);
    let type: string | undefined;
    if (prefix) {
      const t = prefix[1];
      if (t !== 'let' && t !== 'const' && t !== 'constexpr' && t !== 'static') type = t;
    }
    return { name: letm[1], type, start };
  }
  return null;
}

function foreachLocals(line: string): { name: string; start: number }[] {
  const fi = line.indexOf('for');
  if (fi < 0) return [];
  if (fi > 0 && isIdentPart(line[fi - 1])) return [];
  if (fi + 3 < line.length && isIdentPart(line[fi + 3])) return [];
  const lp = line.indexOf('(', fi + 3);
  if (lp < 0) return [];
  const rp = line.indexOf(')', lp + 1);
  if (rp < 0) return [];
  const inside = line.slice(lp + 1, rp);
  const colon = inside.indexOf(':');
  const inKw = colon >= 0 ? -1 : inside.indexOf(' in ');
  if (colon < 0 && inKw < 0) return [];
  const split = colon >= 0 ? colon : inKw;
  const left = inside.slice(0, split).trim();
  if (!left) return [];
  const out: { name: string; start: number }[] = [];
  let cursor = lp + 1 + inside.indexOf(left);
  for (const group of left.split(',').map(s => s.trim()).filter(Boolean)) {
    const pos = line.indexOf(group, cursor);
    if (pos < 0) continue;
    cursor = pos + group.length;
    const words = group.match(/[A-Za-z_]\w*/g);
    if (!words || words.length === 0) continue;
    const name = words[words.length - 1];
    const start = nameOffset(line, name, pos);
    if (start >= 0) out.push({ name, start });
  }
  return out;
}

export function collectDeclarations(lines: string[]): DeclInfo[] {
  const out: DeclInfo[] = [];
  let activeStruct: string | null = null;
  let activeEnum: string | null = null;

  for (let i = 0; i < lines.length; i++) {
    const text = lines[i];
    const trimmed = text.trimStart();
    if (!trimmed || trimmed.startsWith('//')) continue;

    let m: RegExpExecArray | null;

    if ((m = NAMESPACE_RE.exec(text))) addDecl(out, i, text, m[1], 'namespace');
    if ((m = ENTITY_RE.exec(text))) addDecl(out, i, text, m[1], 'entity');
    if ((m = STRUCT_RE.exec(text))) {
      addDecl(out, i, text, m[1], 'struct');
      activeStruct = m[1];
    }
    if ((m = ENUM_RE.exec(text))) {
      addDecl(out, i, text, m[1], 'enum');
      activeEnum = m[1];
    }
    if ((m = TYPE_ALIAS_RE.exec(text))) addDecl(out, i, text, m[1], 'typeAlias');
    if ((m = TRAIT_RE.exec(text))) addDecl(out, i, text, m[1], 'typeAlias');
    if ((m = FIELD_RE.exec(text))) addDecl(out, i, text, m[1], 'field');
    if ((m = HOOK_RE.exec(text))) addDecl(out, i, text, m[1], 'hook');
    if ((m = GLOBAL_RE.exec(text))) addDecl(out, i, text, m[1], 'global');

    const header = actionHeader(text);
    if (header) {
      const detail = header.ret !== undefined
        ? `${header.name}(${header.params.trim()}): ${header.ret}`
        : `${header.name}(${header.params.trim()})`;
      out.push({
        name: header.name,
        kind: 'action',
        line: i,
        start: header.nameStart,
        length: header.name.length,
        detail,
      });
      for (const p of parseParamList(header.params)) {
        const pStart = nameOffset(text, p.name, header.nameStart + header.name.length);
        if (pStart < 0) continue;
        out.push({
          name: p.name,
          kind: 'parameter',
          line: i,
          start: pStart,
          length: p.name.length,
          detail: p.type,
        });
      }
    }

    const local = typedLocal(text);
    if (local && !header) {
      out.push({
        name: local.name,
        kind: 'local',
        line: i,
        start: local.start,
        length: local.name.length,
        detail: local.type,
      });
    }

    for (const fl of foreachLocals(text)) {
      out.push({ name: fl.name, kind: 'local', line: i, start: fl.start, length: fl.name.length });
    }

    if (activeStruct) {
      const sf = /^\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>,]*)/.exec(text);
      if (sf && !STRUCT_RE.exec(text)) {
        addDecl(out, i, text, sf[1], 'field', sf[2]);
      }
      if (/\}/.test(text)) activeStruct = null;
    }

    if (activeEnum) {
      const em = /^\s*([A-Za-z_]\w*)\s*(?:,|;|$)/.exec(text);
      if (em && !/^\s*\}/.test(text) && !ENUM_RE.exec(text)) {
        addDecl(out, i, text, em[1], 'enumMember', activeEnum);
      }
      if (/\}/.test(text)) activeEnum = null;
    }
  }

  return out;
}

const KIND_PRIORITY: Record<SymbolKindName, number> = {
  parameter: 10,
  local: 9,
  field: 8,
  enumMember: 7,
  action: 6,
  global: 5,
  hook: 4,
  entity: 3,
  struct: 3,
  enum: 3,
  typeAlias: 3,
  namespace: 2,
  builtin: 1,
  keyword: 0,
};

export function resolveSymbol(lines: string[], pos: TextPos): HoverInfo | null {
  if (pos.line < 0 || pos.line >= lines.length) return null;
  const line = lines[pos.line];
  if (isInStringLiteral(line, pos.character) || isInComment(line, pos.character)) return null;
  const w = wordAt(line, pos.character);
  if (!w) return null;

  const range: TextRange = {
    start: { line: pos.line, character: w.start },
    end: { line: pos.line, character: w.end },
  };

  const decls = collectDeclarations(lines).filter(d => d.name === w.word);
  const onDecl = decls.find(d => d.line === pos.line && pos.character >= d.start && pos.character <= d.start + d.length);
  if (onDecl) {
    return { name: onDecl.name, kind: onDecl.kind, detail: onDecl.detail, range };
  }

  if (decls.length > 0) {
    decls.sort((a, b) => KIND_PRIORITY[b.kind] - KIND_PRIORITY[a.kind] || b.line - a.line);
    const best = decls[0];
    return { name: best.name, kind: best.kind, detail: best.detail, range };
  }

  if (BUILTIN_SET.has(w.word) || BUILTIN_SET.has(w.word.toLowerCase())) {
    return { name: w.word, kind: 'builtin', range };
  }
  if (KEYWORD_SET.has(w.word)) {
    return { name: w.word, kind: 'keyword', range };
  }
  return null;
}

export function formatHoverMarkdown(info: HoverInfo): string {
  const kind = info.kind;
  if (kind === 'keyword') return `**keyword** \`${info.name}\``;
  if (kind === 'builtin') return `**builtin** \`${info.name}\``;
  if (info.detail) {
    if (kind === 'action') {
      return `**action** \`${info.name}\`\n\n\`\`\`erelang\n${info.detail}\n\`\`\``;
    }
    if (kind === 'parameter' || kind === 'local' || kind === 'field' || kind === 'global') {
      return `**${kind}** \`${info.name}\`: \`${info.detail}\``;
    }
    if (kind === 'enumMember') {
      return `**enumMember** \`${info.name}\` of \`${info.detail}\``;
    }
    return `**${kind}** \`${info.name}\`\n\n\`\`\`erelang\n${info.detail}\n\`\`\``;
  }
  return `**${kind}** \`${info.name}\``;
}

export function findReferencesInText(text: string, name: string): RefLocation[] {
  const lines = splitLines(text);
  const out: RefLocation[] = [];
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    let from = 0;
    while (from < line.length) {
      const at = nameOffset(line, name, from);
      if (at < 0) break;
      out.push({ line: i, start: at, length: name.length });
      from = at + name.length;
    }
  }
  return out;
}

export function findReferencesAt(text: string, pos: TextPos): { name: string; kind: SymbolKindName; refs: RefLocation[] } | null {
  const lines = splitLines(text);
  const sym = resolveSymbol(lines, pos);
  if (!sym) return null;
  if (sym.kind === 'keyword' || sym.kind === 'builtin') {
    return { name: sym.name, kind: sym.kind, refs: findReferencesInText(text, sym.name) };
  }
  return { name: sym.name, kind: sym.kind, refs: findReferencesInText(text, sym.name) };
}

export function renameEdits(
  text: string,
  pos: TextPos,
  newName: string,
): { name: string; kind: SymbolKindName; edits: { range: TextRange; newText: string }[] } | null {
  if (!/^[A-Za-z_]\w*$/.test(newName)) return null;
  const lines = splitLines(text);
  const sym = resolveSymbol(lines, pos);
  if (!sym) return null;
  if (sym.kind === 'keyword' || sym.kind === 'builtin') return null;
  if (sym.kind !== 'local' && sym.kind !== 'parameter' && sym.kind !== 'action') return null;

  const refs = findReferencesInText(text, sym.name);
  const edits = refs.map(r => ({
    range: {
      start: { line: r.line, character: r.start },
      end: { line: r.line, character: r.start + r.length },
    },
    newText: newName,
  }));
  return { name: sym.name, kind: sym.kind, edits };
}
