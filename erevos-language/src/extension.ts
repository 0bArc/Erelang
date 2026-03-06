// =============================================================================
// Erelang Language Extension — VS Code
// =============================================================================
//
// Provides:  completion, symbol navigation, semicolon diagnostics, debug tools.
// Coloring:  delegated entirely to TextMate grammar (no semantic‑token override).
//
// =============================================================================

import * as vscode from 'vscode';
import * as fs    from 'fs';
import * as path  from 'path';

// ─── Declaration Patterns ────────────────────────────────────────────────────

const ENTITY_RE       = /^\s*(?:public|private)?\s*entity\s+([A-Za-z_]\w*)/;
const ACTION_RE       = /^\s*(?:public|private)?\s*(?:async\s+)?action\s+([A-Za-z_]\w*)/;
const FIELD_RE        = /^\s*(?:public|private)?\s*field\s+([A-Za-z_]\w*)/;
const STRUCT_RE       = /^\s*(?:public|private|export)?\s*struct\s+([A-Za-z_]\w*)/;
const ENUM_RE         = /^\s*(?:public|private|export)?\s*enum\s+([A-Za-z_]\w*)/;
const TYPE_ALIAS_RE   = /^\s*(?:public|private|export)?\s*type\s+([A-Za-z_]\w*)\s*=/;
const HOOK_RE         = /^\s*hook\s+([A-Za-z_]\w*)/;
const LET_RE          = /^\s*(?:let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap|Array<[^>]+>|Map<[^>]+>|HashMap<[^>]+>)\s+([A-Za-z_]\w*)/;
const GLOBAL_RE       = /^\s*(?:public|private|export)?\s*global\s+([A-Za-z_]\w*)/;
const INCLUDE_ALIAS_RE = /^\s*#include\s*(<[^>]+>|"[^"]+")\s*(?:as\s+([A-Za-z_]\w*))?/;
const IMPORT_ALIAS_RE  = /^\s*import\s+([A-Za-z_][\w./-]*)\s*(?:as\s+([A-Za-z_]\w*))?/;

// ─── Method / Keyword Lists ─────────────────────────────────────────────────

const MODULE_METHODS: Record<string, string[]> = {
  'builtin/erefs':  ['cwd','chdir','mkdir','read','write','append','copy','move','exists','list','remove'],
  'builtin/erepath': ['join','dirname','basename','ext'],
};

const CHAIN_METHODS     = ['lstrip','rstrip','strip','lower','upper'];

const ARRAY_METHODS     = [
  'forEach','push','push_back','emplace_back','append','insert','set','get','at',
  'erase','remove_at','remove','pop','pop_back','front','back','first','last',
  'len','size','length','capacity','empty','clear','contains','find','index_of',
  'join','reserve','shrink_to_fit',
];

const DICTIONARY_METHODS = [
  'set','put','insert','emplace','try_emplace','insert_or_assign','get','at',
  'has','contains','containsKey','count','getOr','getOrDefault','get_or',
  'get_or_default','remove','erase','clear','size','len','length','empty',
  'keys','values','items','entries','merge','clone',
  'set_path','get_path','has_path','remove_path','forEach',
];

const LANGUAGE_KEYWORDS = [
  'entity','action','field','let','const','global','int','double','string',
  'bool','char','auto','Array','Map','HashMap','constexpr','static','struct',
  'enum','type','import','export','run','if','else','for','while','switch',
  'break','continue','return','match','try','catch','async','await','namespace',
  'unsafe','repeat','do','extern',
  'static_cast','dynamic_cast','reinterpret_cast','bit_cast',
  'sizeof','typeof','decltype','alignof','offsetof','is_base_of',
  '#if','#elif','#else','#endif','#ifdef','#ifndef','#define',
];

const BUILT_INS: readonly string[] = [
  'print','PRINT','sleep','now_ms','now_iso','env','username','computer_name',
  'machine_guid','uuid','rand_int','hwid','args_count','args_get','input',
  'os.args','os.args_count','os.args_get',
  'toint','toInt','tofloat','tostr','toString',
  'dynamic_cast','reinterpret_cast','bit_cast','bitcast','to_json','from_json',
  'sizeof','typeof','decltype','alignof','offsetof','is_base_of',
  'string.starts_with','string.ends_with','string.find','string.substr','string.len',
  'ptr_new','ptr_get','ptr_set','ptr_free','ptr_valid','malloc','free',
  'make_unique','make_shared','unique_reset','shared_reset',
  'string.lstrip','string.rstrip','string.strip','string.lower','string.upper',
  'read_text','write_text','append_text','file_exists','mkdirs','copy_file',
  'move_file','delete_file','list_files','cwd','chdir',
  'exec','os.exec','spawn','os.spawn','exit','read_line','stdin_read',
  'stderr_print','file_mtime','file_size',
  'option_none','option_some','option_is_some','option_unwrap_or',
  'option.none','option.some','option.is_some','option.unwrap_or',
  'result_ok','result_err','result_is_ok','result_unwrap_or',
  'result.ok','result.err','result.is_ok','result.unwrap_or',
  'file_open','file_close','file_read','file_write','file_seek','file_tell',
  'file_flush','fopen','fclose','fread','fwrite','fseek','ftell','fflush',
  'path_join','path_dirname','path_basename','path_ext',
  'strbuf_new','strbuf_append','strbuf_clear','strbuf_len',
  'strbuf_to_string','strbuf_free','strbuf_reserve',
  'string_buffer_new','string_buffer_append','string_buffer_clear',
  'string_buffer_len','string_buffer_to_string','string_buffer_free',
  'string_buffer_reserve',
  'color.red','color.green','color.yellow','color.blue','color.magenta',
  'color.cyan','color.bold','color.reset',
  'set_new','set_add','set_has','set_remove','set_size','set_values',
  'set_union','set_intersect','set_diff',
  'queue_new','queue_push','queue_pop','queue_peek','queue_len','queue_clear',
  'table_new','table_put','table_get','table_has','table_remove','table_rows',
  'table_columns','table_row_keys','table_clear_row','table_count_row',
  'http_get','http_download','hls_download_best','url_encode',
  'network.ip.flush','network.ip.release','network.ip.renew',
  'network.ip.registerdns',
  'network.debug.enable','network.debug.disable','network.debug.status',
  'network.debug.last','network.debug.clear','network.debug.log_tail',
  'language_name','language_version','language_about','language_limitations',
  'win_window_create','win_button_create','win_checkbox_create',
  'win_radiobutton_create','win_slider_create','win_textbox_create',
  'win_label_create','win_on','win_show','win_loop','win_get_text',
  'win_set_text','win_get_check','win_set_check','win_get_slider',
  'win_set_slider','win_close','win_auto_scale','win_set_scale',
  'win_message_box',
  'ui_window_create','ui_label','ui_button','ui_checkbox','ui_radio',
  'ui_slider','ui_textbox','ui_same_line','ui_newline','ui_spacer',
  'ui_separator','ui_load',
  'data_new','data_set','data_get','data_has','data_keys','data_save','data_load',
  'hash_fnv1a','random_bytes',
  'regex_match','regex_find','regex_replace',
  'perm_grant','perm_revoke','perm_has','perm_list',
  'bin_new','bin_from_hex','bin_to_hex','bin_len','bin_get','bin_set',
  'bin_fill','bin_slice',
  'thread_run','thread_join','thread_done',
  'collatz_len','collatz_sweep','collatz_best_steps','collatz_avg_steps',
  'char_is_digit','char_is_space','char_is_alpha','char_is_ident_start',
  'char_is_ident_part',
];

const DEPRECATED_BUILT_INS = new Set<string>([
  'list_new','list_push','list_get','list_len','list_join','list_clear',
  'list_remove_at',
  'dict_new','dict_set','dict_get','dict_has','dict_keys','dict_values',
  'dict_get_or','dict_remove','dict_clear','dict_size','dict_merge',
  'dict_clone','dict_items','dict_entries',
  'dict_set_path','dict_get_path','dict_has_path','dict_remove_path',
  'hashmap_new','hashmap_set','hashmap_put','hashmap_get','hashmap_has',
  'hashmap_contains','hashmap_get_or','hashmap_get_or_default',
  'hashmap_remove','hashmap_clear','hashmap_size','hashmap_keys',
  'hashmap_values','hashmap_merge',
]);

// ─── Data Types ──────────────────────────────────────────────────────────────

interface CollectedSymbols {
  entities:     Set<string>;
  actions:      Set<string>;
  fields:       Set<string>;
  hooks:        Set<string>;
  globals:      Set<string>;
  locals:       Set<string>;
  arrays:       Set<string>;
  dictionaries: Set<string>;
  structs:      Set<string>;
  enums:        Set<string>;
  typeAliases:  Set<string>;
  structFields: Map<string, Set<string>>;
  enumMembers:  Map<string, Set<string>>;
}

interface ImportedSymbols {
  aliasToActions: Map<string, Set<string>>;
  allActions:     Set<string>;
}

interface PolicyCacheEntry {
  mtimeMs: number;
  values: Map<string, string>;
}

type PrintStringContext = {
  interpolation:           boolean;
  partial:                 string;
  replaceStart:            number;
  replaceEnd:              number;
  shouldAppendClosingBrace: boolean;
};

// ─── Utility: Identifier Scanning ───────────────────────────────────────────

function isIdentStart(ch: string): boolean { return /[A-Za-z_]/.test(ch); }
function isIdentPart(ch: string):  boolean { return /[A-Za-z0-9_]/.test(ch); }

type WordToken = { text: string; start: number; length: number };

function scanWords(line: string): WordToken[] {
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

// ─── Utility: For‑Each Header Parsing ───────────────────────────────────────

type RangeToken = { start: number; length: number };

function parseForEachHeader(line: string): { loopVars: RangeToken[]; iterable: RangeToken | null } | null {
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
    const last  = words[words.length - 1];          // variable name (skip type prefix)
    loopVars.push({ start: pos + last.start, length: last.length });
  }

  const rightOff   = lp + 1 + inside.indexOf(right);
  const rightWords = scanWords(right);
  const iterable   = rightWords.length > 0
    ? { start: rightOff + rightWords[0].start, length: rightWords[0].length }
    : null;

  return loopVars.length > 0 ? { loopVars, iterable } : null;
}

/** Extract the local‑variable names introduced by a for‑each header. */
function foreachLocalNames(line: string): string[] {
  const parsed = parseForEachHeader(line);
  if (!parsed) return [];
  return parsed.loopVars
    .map(r => line.slice(r.start, r.start + r.length))
    .filter(n => n.length > 0);
}

// ─── Utility: Print‑String Context ──────────────────────────────────────────

function parsePrintStringContext(line: string, cursor: number): PrintStringContext | null {
  const pm = /\bprint\b/.exec(line);
  if (!pm) return null;

  const q0 = line.indexOf('"', pm.index + pm[0].length);
  if (q0 < 0) return null;
  const q1 = line.indexOf('"', q0 + 1);
  if (q1 < 0 || cursor <= q0 || cursor > q1) return null;

  const before = line.slice(q0 + 1, cursor);

  // Just typed '{'
  if (before.endsWith('{')) {
    const next = line.charAt(cursor);
    return { interpolation: true, partial: '', replaceStart: cursor, replaceEnd: cursor, shouldAppendClosingBrace: next !== '}' };
  }

  // Inside an open interpolation brace
  const lo = before.lastIndexOf('{');
  const lc = before.lastIndexOf('}');
  if (lo > lc) {
    const partial = before.slice(lo + 1);
    if (partial.length > 0 && !/^[A-Za-z_]\w*$/.test(partial)) return null;
    const rs   = q0 + 1 + lo + 1;
    const next = line.charAt(cursor);
    return { interpolation: true, partial, replaceStart: rs, replaceEnd: cursor, shouldAppendClosingBrace: next !== '}' };
  }

  // Plain identifier before cursor (not inside braces)
  const im    = /[A-Za-z_]\w*$/.exec(before);
  const partial = im ? im[0] : '';
  const rs      = im ? cursor - partial.length : cursor;
  return { interpolation: false, partial, replaceStart: rs, replaceEnd: cursor, shouldAppendClosingBrace: true };
}

// ─── Utility: Context Detection ─────────────────────────────────────────────

function isForeachColonCtx(prefix: string): boolean {
  return /\bfor\s*\([^)]*:\s*[A-Za-z_]*$/.test(prefix);
}

function isDictLiteralCtx(prefix: string): boolean {
  return /[{,]\s*(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*[A-Za-z_]*$/.test(prefix);
}

// ─── Symbol Collection ──────────────────────────────────────────────────────

function collect(doc: vscode.TextDocument, uptoLine?: number): CollectedSymbols {
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

    // Detect array / dict variable declarations
    const decl = /^\s*(let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap)\s+([A-Za-z_]\w*)\s*=\s*(.+)\s*$/.exec(text);
    if (decl) {
      const tw  = decl[1].toLowerCase();
      const vn  = decl[2];
      const rhs = decl[3].trim();
      if (tw === 'array' || rhs.startsWith('list_new(') || rhs.startsWith('[')) out.arrays.add(vn);
      if (tw === 'map' || tw === 'dictionary' || tw === 'hashmap' || rhs.startsWith('dict_new(') || rhs.startsWith('hashmap_new(') || rhs.startsWith('{')) out.dictionaries.add(vn);
    }

    // Typed generic declarations: Array<T>, Map<K,V>, HashMap<K,V>
    const gen = /^\s*(?:constexpr\s+)?(?:static\s+)?(Array<[^>]+>|Map<[^>]+>|HashMap<[^>]+>)\s+([A-Za-z_]\w*)\s*=/.exec(text);
    if (gen) {
      const tw = gen[1];
      const vn = gen[2];
      if (tw.startsWith('Array<'))                           out.arrays.add(vn);
      if (tw.startsWith('Map<') || tw.startsWith('HashMap<')) out.dictionaries.add(vn);
      out.locals.add(vn);
    }

    // Struct fields
    if (activeStruct) {
      const sf = /^\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w<>,]*)/.exec(text);
      if (sf) out.structFields.get(activeStruct)?.add(sf[1]);
      if (/\}/.test(text)) activeStruct = null;
    }

    // Enum members
    if (activeEnum) {
      const em = /^\s*([A-Za-z_]\w*)\s*(?:,|;|$)/.exec(text);
      if (em && !/^\s*\}/.test(text)) out.enumMembers.get(activeEnum)?.add(em[1]);
      if (/\}/.test(text)) activeEnum = null;
    }

    // For‑each loop variables
    for (const name of foreachLocalNames(text)) out.locals.add(name);
  }
  return out;
}

// ─── Module / Import Resolution ─────────────────────────────────────────────

function normalizeSpec(spec: string): string {
  const t = spec.trim();
  if (t.startsWith('<') && t.endsWith('>')) return t.slice(1, -1).toLowerCase();
  if (t.startsWith('"') && t.endsWith('"')) return t.slice(1, -1).toLowerCase();
  return t.toLowerCase();
}

function defaultAlias(spec: string): string {
  return normalizeSpec(spec).split('/').pop()!.replace(/[^A-Za-z0-9_]/g, '_');
}

function resolveIncludeFile(doc: vscode.TextDocument, spec: string): string | null {
  const norm = normalizeSpec(spec);
  if (!norm || norm.startsWith('builtin/')) return null;
  const dir  = path.dirname(doc.uri.fsPath);
  const exts = ['.elan', '.ere', '.0bs'];
  const candidates: string[] = [path.resolve(dir, norm)];
  if (!path.extname(norm)) for (const e of exts) candidates.push(path.resolve(dir, norm + e));
  for (const f of vscode.workspace.workspaceFolders ?? []) {
    candidates.push(path.resolve(f.uri.fsPath, norm));
    if (!path.extname(norm)) for (const e of exts) candidates.push(path.resolve(f.uri.fsPath, norm + e));
  }
  for (const c of candidates) {
    try { if (fs.existsSync(c) && fs.statSync(c).isFile()) return c; } catch { /* skip */ }
  }
  return null;
}

function extractActions(filePath: string): Set<string> {
  const names = new Set<string>();
  try {
    for (const line of fs.readFileSync(filePath, 'utf8').split(/\r?\n/)) {
      const m = ACTION_RE.exec(line);
      if (m) names.add(m[1]);
    }
  } catch { /* skip */ }
  return names;
}

function collectImports(doc: vscode.TextDocument): ImportedSymbols {
  const aliasToActions = new Map<string, Set<string>>();
  const allActions     = new Set<string>();

  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    let spec: string | null = null;
    let alias: string | null = null;

    const inc = INCLUDE_ALIAS_RE.exec(text);
    if (inc) { spec = inc[1]; alias = inc[2] ?? defaultAlias(inc[1]); }
    else { const imp = IMPORT_ALIAS_RE.exec(text); if (imp) { spec = imp[1]; alias = imp[2] ?? defaultAlias(imp[1]); } }
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

// ─── Include Path Completions ───────────────────────────────────────────────

function includePathCompletions(doc: vscode.TextDocument, prefix: string): vscode.CompletionItem[] | null {
  const m = /^\s*#include\s*<([^>]*)$/.exec(prefix) ?? /^\s*#include\s*"([^"]*)$/.exec(prefix);
  if (!m) return null;
  const partial  = m[1].replace(/\\/g, '/');
  const slashIdx = partial.lastIndexOf('/');
  const dirPart  = slashIdx >= 0 ? partial.slice(0, slashIdx + 1) : '';
  const namePart = slashIdx >= 0 ? partial.slice(slashIdx + 1) : partial;
  const target   = path.resolve(path.dirname(doc.uri.fsPath), dirPart || '.');
  let entries: fs.Dirent[] = [];
  try { entries = fs.readdirSync(target, { withFileTypes: true }); } catch { return []; }

  const out: vscode.CompletionItem[] = [];
  for (const e of entries) {
    if (!e.name.toLowerCase().startsWith(namePart.toLowerCase())) continue;
    if (e.isDirectory()) {
      const ci = new vscode.CompletionItem(e.name + '/', vscode.CompletionItemKind.Folder);
      ci.insertText = dirPart + e.name + '/';
      ci.detail = 'folder';
      out.push(ci);
    } else if (e.isFile() && /\.(elan|ere|0bs)$/i.test(e.name)) {
      const ci = new vscode.CompletionItem(e.name, vscode.CompletionItemKind.File);
      ci.insertText = dirPart + e.name;
      ci.detail = 'script file';
      out.push(ci);
    }
  }
  return out;
}

// ─── Debug Logging ──────────────────────────────────────────────────────────

let _debugChannel: vscode.OutputChannel | undefined;
const _policyCache = new Map<string, PolicyCacheEntry>();

function dbg(msg: string): void {
  if (!vscode.workspace.getConfiguration('erelang').get<boolean>('debugCompletion', false)) return;
  _debugChannel?.appendLine(msg);
}

function dbgCompletion(branch: string, pos: vscode.Position, prefix: string, detail?: string): void {
  dbg(`[completion] ${branch} @ ${pos.line + 1}:${pos.character + 1}  prefix="${prefix}"${detail ? '  ' + detail : ''}`);
}

function parsePolicyBoolean(raw: string | undefined, fallback: boolean): boolean {
  if (!raw) return fallback;
  const value = raw.trim().toLowerCase();
  if (value === 'true' || value === '1' || value === 'yes' || value === 'on') return true;
  if (value === 'false' || value === '0' || value === 'no' || value === 'off') return false;
  return fallback;
}

function findPolicyFile(doc: vscode.TextDocument): string | null {
  const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
  if (folder) {
    const candidate = path.join(folder.uri.fsPath, 'policy.cfg');
    if (fs.existsSync(candidate)) return candidate;
  }
  for (const workspaceFolder of vscode.workspace.workspaceFolders ?? []) {
    const candidate = path.join(workspaceFolder.uri.fsPath, 'policy.cfg');
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

function readPolicyValues(policyPath: string): Map<string, string> {
  try {
    const stat = fs.statSync(policyPath);
    const cached = _policyCache.get(policyPath);
    if (cached && cached.mtimeMs === stat.mtimeMs) {
      return cached.values;
    }

    const values = new Map<string, string>();
    const text = fs.readFileSync(policyPath, 'utf8');
    for (const rawLine of text.split(/\r?\n/)) {
      const line = rawLine.trim();
      if (!line || line.startsWith('#')) continue;
      const idx = line.indexOf('=');
      if (idx <= 0) continue;
      const key = line.slice(0, idx).trim().toLowerCase();
      const value = line.slice(idx + 1).trim();
      values.set(key, value);
    }

    _policyCache.set(policyPath, { mtimeMs: stat.mtimeMs, values });
    return values;
  } catch {
    return new Map<string, string>();
  }
}

function suggestVariablesInQuotesEnabled(doc: vscode.TextDocument): boolean {
  const policyPath = findPolicyFile(doc);
  if (!policyPath) return true;
  const values = readPolicyValues(policyPath);
  return parsePolicyBoolean(values.get('suggest_variable_in_quotes'), true);
}

// ─── Completion Provider ────────────────────────────────────────────────────

class ErelangCompletionProvider implements vscode.CompletionItemProvider {
  provideCompletionItems(doc: vscode.TextDocument, pos: vscode.Position): vscode.CompletionItem[] {
    const prefix   = doc.lineAt(pos.line).text.slice(0, pos.character);
    const fullLine = doc.lineAt(pos.line).text;
    const col      = collect(doc, pos.line);

    // ── Print / interpolation context ──────────────────────────────────
    const pctx = parsePrintStringContext(fullLine, pos.character);
    if (pctx) {
      const allowPlainQuoteSuggest = suggestVariablesInQuotesEnabled(doc);
      if (!pctx.interpolation && !allowPlainQuoteSuggest) {
        dbgCompletion('print-ctx-disabled-by-policy', pos, prefix, 'suggest_variable_in_quotes=false');
        return [];
      }
      dbgCompletion('print-ctx', pos, prefix, pctx.interpolation ? 'interpolation' : 'plain');
      const names = new Set([...col.locals, ...col.globals, ...col.fields, ...col.actions]);
      const range = new vscode.Range(pos.line, pctx.replaceStart, pos.line, pctx.replaceEnd);
      return [...names]
        .filter(n => pctx.partial.length === 0 || n.startsWith(pctx.partial))
        .map(n => {
          const ci  = new vscode.CompletionItem(n, vscode.CompletionItemKind.Variable);
          ci.range  = range;
          ci.detail = pctx.interpolation ? 'interpolation variable' : 'print variable';
          ci.insertText = pctx.interpolation
            ? (pctx.shouldAppendClosingBrace ? `${n}}` : n)
            : `{${n}}`;
          return ci;
        });
    }

    // ── print snippet ──────────────────────────────────────────────────
    if (/^\s*print\s*$/.test(prefix)) {
      dbgCompletion('print-snippet', pos, prefix);
      const ci = new vscode.CompletionItem('print "{value}";', vscode.CompletionItemKind.Snippet);
      ci.insertText = new vscode.SnippetString('print "{$1}";');
      ci.detail = 'print interpolation';
      return [ci];
    }

    // ── #include path completions ──────────────────────────────────────
    const incl = includePathCompletions(doc, prefix);
    if (incl) { dbgCompletion('include-path', pos, prefix, `${incl.length} items`); return incl; }

    // ── Member access (obj.method) ─────────────────────────────────────
    const dot = /([A-Za-z_]\w*)\.([A-Za-z_]\w*)?$/.exec(prefix);
    if (dot) {
      const obj     = dot[1];
      const partial = dot[2] ?? '';
      dbgCompletion('member-access', pos, prefix, `obj=${obj}`);
      const imported = collectImports(doc);
      const methods  = new Set<string>(imported.aliasToActions.get(obj) ?? []);
      for (const f of col.structFields.get(obj) ?? []) methods.add(f);
      for (const e of col.enumMembers.get(obj) ?? [])  methods.add(e);
      if (col.arrays.has(obj))       for (const m of ARRAY_METHODS) methods.add(m);
      if (col.dictionaries.has(obj)) for (const m of DICTIONARY_METHODS) methods.add(m);
      return [...methods]
        .filter(m => partial.length === 0 || m.startsWith(partial))
        .map(m => {
          const ci  = new vscode.CompletionItem(m, vscode.CompletionItemKind.Method);
          ci.detail = imported.aliasToActions.has(obj) ? `${obj} module` : `${obj} method`;
          return ci;
        });
    }

    // ── Colon chain (string methods) ───────────────────────────────────
    const chain = /([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)?$/.exec(prefix);
    if (chain) {
      const left = chain[1];
      if (isForeachColonCtx(prefix) || isDictLiteralCtx(prefix)) {
        dbgCompletion('colon-blocked', pos, prefix, `left=${left}`);
        // Fall through to global suggestions instead of returning empty
      } else if (col.arrays.has(left) || col.dictionaries.has(left)) {
        dbgCompletion('colon-collection', pos, prefix, `left=${left}`);
        // Fall through to global suggestions
      } else {
        dbgCompletion('string-chain', pos, prefix, `left=${left}`);
        const partial = chain[2] ?? '';
        return CHAIN_METHODS
          .filter(m => partial.length === 0 || m.startsWith(partial))
          .map(m => {
            const ci  = new vscode.CompletionItem(m, vscode.CompletionItemKind.Method);
            ci.detail = 'string method';
            return ci;
          });
      }
    }

    // ── Global / fallback suggestions ──────────────────────────────────
    const items: vscode.CompletionItem[] = [];
    const seen  = new Set<string>();

    const add = (names: Iterable<string>, kind: vscode.CompletionItemKind, detail?: string, sort = 'z') => {
      for (const n of names) {
        if (seen.has(n)) continue;
        seen.add(n);
        const ci  = new vscode.CompletionItem(n, kind);
        ci.detail = detail ?? (kind === vscode.CompletionItemKind.Function ? 'action' : kind === vscode.CompletionItemKind.Class ? 'entity' : undefined);
        ci.sortText = `${sort}_${n}`;
        items.push(ci);
      }
    };

    add(col.locals,      vscode.CompletionItemKind.Variable,       'local variable', 'a');
    add(col.globals,     vscode.CompletionItemKind.Variable,       'global',         'a');
    add(col.entities,    vscode.CompletionItemKind.Class,           undefined,        'b');
    add(col.structs,     vscode.CompletionItemKind.Struct,         'struct',         'b');
    add(col.enums,       vscode.CompletionItemKind.Enum,           'enum',           'b');
    add(col.typeAliases, vscode.CompletionItemKind.TypeParameter,  'type alias',     'b');
    add(col.actions,     vscode.CompletionItemKind.Function,        undefined,        'c');
    add(col.fields,      vscode.CompletionItemKind.Field,           undefined,        'c');
    add(col.hooks,       vscode.CompletionItemKind.Event,           undefined,        'c');

    for (const kw of LANGUAGE_KEYWORDS) {
      if (seen.has(kw)) continue;
      seen.add(kw);
      const ci  = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
      ci.detail = 'keyword';
      ci.sortText = `aa_${kw}`;
      items.push(ci);
    }

    for (const [enumName, members] of col.enumMembers) {
      for (const mem of members) {
        const q = `${enumName}.${mem}`;
        if (seen.has(q)) continue;
        seen.add(q);
        const ci  = new vscode.CompletionItem(q, vscode.CompletionItemKind.EnumMember);
        ci.detail = 'enum member';
        ci.sortText = `e_${q}`;
        items.push(ci);
      }
    }

    const imported = collectImports(doc);
    for (const n of imported.allActions) {
      if (seen.has(n)) continue;
      seen.add(n);
      const ci  = new vscode.CompletionItem(n, vscode.CompletionItemKind.Function);
      ci.detail = 'imported action';
      ci.sortText = `m_${n}`;
      items.push(ci);
    }

    for (const b of BUILT_INS) {
      if (DEPRECATED_BUILT_INS.has(b) || seen.has(b)) continue;
      seen.add(b);
      const ci  = new vscode.CompletionItem(b, vscode.CompletionItemKind.Function);
      ci.detail = 'builtin';
      ci.sortText = `z_${b}`;
      items.push(ci);
    }

    dbgCompletion('global-fallback', pos, prefix, `${items.length} items`);
    return items;
  }
}

// ─── Symbol Providers ───────────────────────────────────────────────────────

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    const out: vscode.SymbolInformation[] = [];
    for (let i = 0; i < doc.lineCount; i++) {
      const line = doc.lineAt(i).text;
      let m: RegExpExecArray | null;
      if      ((m = ENTITY_RE.exec(line))) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = ACTION_RE.exec(line))) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = FIELD_RE.exec(line)))  out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = HOOK_RE.exec(line)))   out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event,    '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
    }
    return out;
  }
}

class ErelangWorkspaceSymbolProvider implements vscode.WorkspaceSymbolProvider {
  async provideWorkspaceSymbols(query: string): Promise<vscode.SymbolInformation[]> {
    const uris = await vscode.workspace.findFiles('**/*.{0bs,ere,elan}');
    const out: vscode.SymbolInformation[] = [];
    for (const uri of uris) {
      const doc = await vscode.workspace.openTextDocument(uri);
      for (let i = 0; i < doc.lineCount; i++) {
        const line = doc.lineAt(i).text;
        let m: RegExpExecArray | null;
        if      ((m = ENTITY_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = ACTION_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = FIELD_RE.exec(line))  && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
        else if ((m = HOOK_RE.exec(line))   && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event,    '', new vscode.Location(uri, new vscode.Position(i, 0))));
      }
    }
    return out;
  }
}

// ─── Semicolon Diagnostics ──────────────────────────────────────────────────

function needsSemicolon(line: string): boolean {
  const t = line.trim();
  if (!t) return false;
  if (t.startsWith('//') || t.startsWith('#') || t.startsWith('@'))       return false;
  if (t.startsWith('/*') || t.startsWith('*') || t.endsWith('*/'))        return false;
  if (t.endsWith(';') || t.endsWith('{') || t.endsWith('}') || t.endsWith(':')) return false;
  if (t.endsWith(',') || t.endsWith('(') || t.endsWith('['))              return false;
  if (/^(if|else|while|for|switch|match|try|catch|do|repeat|unsafe|parallel|namespace)\b/.test(t)) return false;
  if (/^(public|private|export)?\s*(action|entity|struct|enum|hook)\b/.test(t)) return false;
  if (/^[A-Za-z_]\w*\s*:\s*[A-Za-z_][\w<>,\s]*[,]?$/.test(t))            return false;
  if (/^(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*.+[,]?$/.test(t))         return false;
  return true;
}

function validateSemicolons(doc: vscode.TextDocument, coll: vscode.DiagnosticCollection): void {
  if (doc.languageId !== 'erelang') return;
  const diags: vscode.Diagnostic[] = [];
  for (let i = 0; i < doc.lineCount; i++) {
    const text = doc.lineAt(i).text;
    if (!needsSemicolon(text)) continue;
    const len   = text.trimEnd().length;
    const start = Math.max(0, len - 1);
    const range = new vscode.Range(i, start, i, len);
    const d     = new vscode.Diagnostic(range, 'Missing semicolon (;)', vscode.DiagnosticSeverity.Error);
    d.source    = 'erelang';
    diags.push(d);
  }
  coll.set(doc.uri, diags);
}

// ─── Auto‑Retrigger Logic ───────────────────────────────────────────────────

function shouldRetrigger(change: vscode.TextDocumentContentChangeEvent, prefix: string, fullLine: string, cursor: vscode.Position): boolean {
  // Trigger on single identifier‑character insert, opening interpolation brace,
  // or single-char delete.
  const typed   = change.text.length === 1 && /[A-Za-z0-9_]/.test(change.text);
  const openBrace = change.text.includes('{');
  const deleted = change.text.length === 0 && change.rangeLength > 0;
  const insertedNewline = change.text.includes('\n') || change.text.includes('\r');
  if (!typed && !openBrace && !deleted && !insertedNewline) return false;
  if (/^\s*\/\//.test(prefix)) return false;   // inside comment

  if (insertedNewline) {
    // Keep suggestions responsive after pressing Enter (or accepting completion
    // via Enter in some editor states) so autocomplete doesn't "die".
    return true;
  }

  const printCtx = parsePrintStringContext(fullLine, cursor.character);
  if (printCtx) {
    // After '{' is physically inserted, open suggestions asynchronously.
    // While typing inside the interpolation, let the editor filter the existing
    // widget locally; only retrigger on delete.
    if (openBrace) return true;
    return deleted;
  }

  // Always retrigger when cursor is on a partial identifier
  if (/[A-Za-z_]\w*$/.test(prefix)) return true;

  return false;
}

// ─── Activation ─────────────────────────────────────────────────────────────

export function activate(ctx: vscode.ExtensionContext) {
  console.log('Erelang language extension active (v2)');

  // Debug output channel
  _debugChannel = vscode.window.createOutputChannel('Erelang Language Debug');
  ctx.subscriptions.push(_debugChannel);

  // Semicolon diagnostics
  const semiDiags = vscode.languages.createDiagnosticCollection('erelang-semicolons');
  ctx.subscriptions.push(semiDiags);
  const refreshSemi = (d: vscode.TextDocument) => validateSemicolons(d, semiDiags);
  ctx.subscriptions.push(vscode.workspace.onDidOpenTextDocument(refreshSemi));
  ctx.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => refreshSemi(e.document)));
  ctx.subscriptions.push(vscode.workspace.onDidSaveTextDocument(refreshSemi));
  for (const d of vscode.workspace.textDocuments) refreshSemi(d);

  // ── Auto‑retrigger suggest on identifier typing ──────────────────────
  let retriggerTimer: NodeJS.Timeout | undefined;
  ctx.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument(event => {
      if (event.document.languageId !== 'erelang') return;
      if (event.contentChanges.length === 0) return;
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.toString() !== event.document.uri.toString()) return;

      const cursor   = editor.selection.active;
      const lineText = event.document.lineAt(cursor.line).text;
      const prefix   = lineText.slice(0, cursor.character);

      const shouldTrigger = event.contentChanges.some(change =>
        shouldRetrigger(change, prefix, lineText, cursor)
      );
      if (!shouldTrigger) return;

      if (retriggerTimer) clearTimeout(retriggerTimer);
      retriggerTimer = setTimeout(() => {
        void vscode.commands.executeCommand('editor.action.triggerSuggest');
      }, 15);
    })
  );

  // ── Debug command ────────────────────────────────────────────────────
  ctx.subscriptions.push(
    vscode.commands.registerCommand('erelang.debugCompletionContext', () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'erelang') {
        vscode.window.showWarningMessage('Open an Erelang file first.');
        return;
      }
      const cur     = editor.selection.active;
      const line    = editor.document.lineAt(cur.line).text;
      const prefix  = line.slice(0, cur.character);
      const pctx    = parsePrintStringContext(line, cur.character);
      const forEach = parseForEachHeader(line);
      const col     = collect(editor.document, cur.line);

      _debugChannel!.appendLine('═══ Erelang Completion Context ═══');
      _debugChannel!.appendLine(`cursor:       ${cur.line + 1}:${cur.character + 1}`);
      _debugChannel!.appendLine(`line:         ${line}`);
      _debugChannel!.appendLine(`prefix:       ${prefix}`);
      _debugChannel!.appendLine(`printCtx:     ${pctx ? JSON.stringify(pctx) : 'none'}`);
      _debugChannel!.appendLine(`foreach:      ${forEach ? JSON.stringify(forEach) : 'none'}`);
      _debugChannel!.appendLine(`locals:       ${[...col.locals].join(', ') || 'none'}`);
      _debugChannel!.appendLine(`arrays:       ${[...col.arrays].join(', ') || 'none'}`);
      _debugChannel!.appendLine(`dictionaries: ${[...col.dictionaries].join(', ') || 'none'}`);
      _debugChannel!.appendLine(`foreachCtx:   ${isForeachColonCtx(prefix)}`);
      _debugChannel!.appendLine(`dictLitCtx:   ${isDictLiteralCtx(prefix)}`);
      _debugChannel!.appendLine('');
      _debugChannel!.show(true);
      vscode.window.showInformationMessage('Context dumped → Output > Erelang Language Debug');
    })
  );

  // ── Register providers ───────────────────────────────────────────────
  // NOTE: No semantic‑token provider. All coloring is handled by TextMate
  // grammar (syntaxes/erevos.tmLanguage.json). Semantic tokens were
  // actively overriding TextMate's variable.other.readwrite scope with the
  // generic "variable" semantic type, which maps to the default text color
  // in most themes — making foreach loop vars appear uncolored.

  ctx.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      { language: 'erelang' },
      new ErelangCompletionProvider(),
      '.', ':', '"', '_',
    ),
  );
  ctx.subscriptions.push(
    vscode.languages.registerDocumentSymbolProvider({ language: 'erelang' }, new ErelangDocumentSymbolProvider()),
  );
  ctx.subscriptions.push(
    vscode.languages.registerWorkspaceSymbolProvider(new ErelangWorkspaceSymbolProvider()),
  );
}

export function deactivate() {}
