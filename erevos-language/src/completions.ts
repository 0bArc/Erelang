// =============================================================================
// Erelang — Completion provider
// =============================================================================

import * as vscode from 'vscode';
import * as fs     from 'fs';
import * as path   from 'path';
import {
  LANGUAGE_KEYWORDS, BUILT_INS, DEPRECATED_BUILT_INS,
} from './constants';
import { PrintStringContext } from './types';
import { collect, collectEntityInstances, collectEntityActions, collectEntityFields } from './symbols';
import { collectImports } from './imports';

// ─── Debug (lightweight, no circular dep) ────────────────────────────────────

let _debugChannel: vscode.OutputChannel | undefined;
export function setDebugChannel(ch: vscode.OutputChannel): void { _debugChannel = ch; }

function dbg(msg: string): void {
  if (!vscode.workspace.getConfiguration('erelang').get<boolean>('debugCompletion', false)) return;
  _debugChannel?.appendLine(msg);
}

function dbgCompletion(branch: string, pos: vscode.Position, prefix: string, detail?: string): void {
  dbg(`[completion] ${branch} @ ${pos.line + 1}:${pos.character + 1}  prefix="${prefix}"${detail ? '  ' + detail : ''}`);
}

// ─── Print-String Context ────────────────────────────────────────────────────

function parsePrintStringContext(line: string, cursor: number): PrintStringContext | null {
  const pm = /\bprint\b/.exec(line);
  if (!pm) return null;
  const q0 = line.indexOf('"', pm.index + pm[0].length);
  if (q0 < 0) return null;
  const q1 = line.indexOf('"', q0 + 1);
  if (q1 < 0 || cursor <= q0 || cursor > q1) return null;
  const before = line.slice(q0 + 1, cursor);
  if (before.endsWith('{')) {
    const next = line.charAt(cursor);
    return { interpolation: true, partial: '', replaceStart: cursor, replaceEnd: cursor, shouldAppendClosingBrace: next !== '}' };
  }
  const lo = before.lastIndexOf('{');
  const lc = before.lastIndexOf('}');
  if (lo > lc) {
    const partial = before.slice(lo + 1);
    if (partial.length > 0 && !/^[A-Za-z_]\w*$/.test(partial)) return null;
    const rs   = q0 + 1 + lo + 1;
    const next = line.charAt(cursor);
    return { interpolation: true, partial, replaceStart: rs, replaceEnd: cursor, shouldAppendClosingBrace: next !== '}' };
  }
  return null;
}

// ─── Include Path Completions ────────────────────────────────────────────────

function includePathCompletions(doc: vscode.TextDocument, pos: vscode.Position, prefix: string): vscode.CompletionItem[] | null {
  const angleMatch = /^\s*#\s*include\s*<([^>]*)$/.exec(prefix);
  const quoteMatch = /^\s*#\s*include\s*"([^"]*)$/.exec(prefix);
  const bareMatch  = /^\s*#\s*include\s+([A-Za-z0-9_./\\-]*)$/.exec(prefix);
  const match = angleMatch ?? quoteMatch ?? bareMatch;
  if (!match) return null;

  const mode: 'angle' | 'quote' | 'bare' = angleMatch ? 'angle' : quoteMatch ? 'quote' : 'bare';
  const partial    = match[1].replace(/\\/g, '/');
  const slashIdx   = partial.lastIndexOf('/');
  const dirPart    = slashIdx >= 0 ? partial.slice(0, slashIdx + 1) : '';
  const namePart   = slashIdx >= 0 ? partial.slice(slashIdx + 1) : partial;
  const replaceLen = mode === 'bare' ? partial.length : namePart.length;
  const replaceRange = new vscode.Range(pos.line, pos.character - replaceLen, pos.line, pos.character);

  const candidateDirs = new Set<string>();
  candidateDirs.add(path.resolve(path.dirname(doc.uri.fsPath), dirPart || '.'));
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    candidateDirs.add(path.resolve(folder.uri.fsPath, dirPart || '.'));
  }

  const out: vscode.CompletionItem[] = [];
  const builtinModules = [
    'builtin/fs','builtin/erefs','builtin/path','builtin/erepath',
    'builtin/regex','builtin/crypto','builtin/network','builtin/net',
    'builtin/math','builtin/binary','builtin/threads','builtin/monitor',
    'builtin/data','builtin/perm','builtin/system',
  ];
  for (const mod of builtinModules) {
    if (!mod.toLowerCase().startsWith(partial.toLowerCase())) continue;
    const insert = mode === 'bare'
      ? `<${mod}>;`
      : (dirPart && mod.toLowerCase().startsWith(dirPart.toLowerCase()) ? mod.slice(dirPart.length) : mod);
    const label = mode === 'bare' ? `<${mod}>;` : mod;
    const ci    = new vscode.CompletionItem(label, vscode.CompletionItemKind.Module);
    ci.insertText = insert;
    ci.range      = replaceRange;
    ci.detail     = mode === 'bare' ? 'include builtin module' : 'builtin module';
    out.push(ci);
  }

  const seen = new Set<string>();
  for (const dir of candidateDirs) {
    let entries: fs.Dirent[] = [];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { continue; }
    for (const e of entries) {
      if (!e.name.toLowerCase().startsWith(namePart.toLowerCase())) continue;
      if (e.isDirectory()) {
        const insert = dirPart + e.name + '/';
        if (seen.has(insert)) continue; seen.add(insert);
        const ci = new vscode.CompletionItem(mode === 'bare' ? `<${insert}` : e.name + '/', vscode.CompletionItemKind.Folder);
        ci.insertText = mode === 'bare' ? `<${insert}` : e.name + '/';
        ci.range      = replaceRange;
        ci.detail     = 'folder';
        out.push(ci);
      } else if (e.isFile() && /\.(elan|ere|0bs)$/i.test(e.name)) {
        const insert = dirPart + e.name;
        if (seen.has(insert)) continue; seen.add(insert);
        const ci = new vscode.CompletionItem(mode === 'bare' ? `<${insert}>;` : e.name, vscode.CompletionItemKind.File);
        ci.insertText = mode === 'bare' ? `<${insert}>;` : e.name;
        ci.range      = replaceRange;
        ci.detail     = mode === 'bare' ? 'include script file' : 'script file';
        out.push(ci);
      }
    }
  }
  return out;
}

function includeDirectiveCompletions(pos: vscode.Position, prefix: string): vscode.CompletionItem[] | null {
  const trimmed = prefix.trimStart();
  if (!trimmed.startsWith('#')) return null;
  const directiveMatch = /^(\s*)#\s*[A-Za-z]*$/.exec(prefix);
  if (!directiveMatch) return null;
  const afterHash   = trimmed.slice(1);
  const includeStem = afterHash.match(/^\s*([A-Za-z]*)/);
  const typed       = (includeStem?.[1] ?? '').toLowerCase();
  if (typed.length > 0 && !'include'.startsWith(typed)) return null;
  const rest = afterHash.slice(includeStem?.[0].length ?? 0);
  if (typed === 'include' && rest.length > 0) return null;
  if (rest.length > 0 && !/^\s*$/.test(rest)) return null;
  const keyword = new vscode.CompletionItem('#include', vscode.CompletionItemKind.Keyword);
  keyword.insertText = '#include ';
  keyword.range      = new vscode.Range(pos.line, directiveMatch[1].length, pos.line, pos.character);
  keyword.detail     = 'start include directive';
  keyword.sortText   = 'a_include';
  return [keyword];
}

// ─── Context Helpers ─────────────────────────────────────────────────────────

export function isForeachColonCtx(prefix: string): boolean {
  return /\bfor\s*\([^)]*:\s*[A-Za-z_]*$/.test(prefix);
}

export function isDictLiteralCtx(prefix: string): boolean {
  return /[{,]\s*(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*[A-Za-z_]*$/.test(prefix);
}

/** Header complete, body `{` not started — don't pop completions (avoids Enter → `case`). */
export function isControlFlowAwaitingBody(prefix: string): boolean {
  const t = prefix.trimEnd();
  return (
    /\b(switch|match|if|else|for|while|try|catch|repeat|do|unsafe|parallel|namespace)\s*(\([^)]*\))?\s*$/.test(t) ||
    /\b(action|entity|struct|enum|hook)\s+[A-Za-z_]\w*\s*(\([^)]*\))?\s*$/.test(t)
  );
}

/** Set by extension auto-retrigger; suppress empty-prefix dump on Invoke. */
let _autoSuggestFromEdit = false;
export function markAutoSuggestFromEdit(): void { _autoSuggestFromEdit = true; }
export function consumeAutoSuggestFromEdit(): boolean {
  if (!_autoSuggestFromEdit) return false;
  _autoSuggestFromEdit = false;
  return true;
}

// ─── Dot (Member Access) Completion ──────────────────────────────────────────

function memberCompletions(
  doc: vscode.TextDocument,
  pos: vscode.Position,
  prefix: string,
): vscode.CompletionItem[] | null {
  const dot = /([A-Za-z_]\w*)\.([A-Za-z_]\w*)?$/.exec(prefix);
  if (!dot) return null;

  const obj     = dot[1];
  const partial = dot[2] ?? '';
  dbgCompletion('member-access', pos, prefix, `obj=${obj}`);

  // ── 1. Module alias completion ──────────────────────────────────────────
  const imported = collectImports(doc);
  const modMethods = imported.aliasToActions.get(obj);
  if (modMethods && modMethods.size > 0) {
    return [...modMethods]
      .filter(m => partial.length === 0 || m.startsWith(partial))
      .map(m => {
        const ci  = new vscode.CompletionItem(m, vscode.CompletionItemKind.Method);
        ci.detail = `${obj} module`;
        return ci;
      });
  }

  // ── 2. `self` — suggest current entity's fields and actions ────────────
  if (obj === 'self') {
    const entityActions = collectEntityActions(doc);
    const entityFields  = collectEntityFields(doc);
    const items: vscode.CompletionItem[] = [];
    for (const [, actions] of entityActions) {
      for (const a of actions) {
        if (partial.length > 0 && !a.startsWith(partial)) continue;
        const ci  = new vscode.CompletionItem(a, vscode.CompletionItemKind.Method);
        ci.detail = 'self action';
        items.push(ci);
      }
    }
    for (const [, fields] of entityFields) {
      for (const f of fields) {
        if (partial.length > 0 && !f.startsWith(partial)) continue;
        const ci  = new vscode.CompletionItem(f, vscode.CompletionItemKind.Field);
        ci.detail = 'self field';
        items.push(ci);
      }
    }
    return items;
  }

  // ── 3. Entity instance — resolve var → Entity → actions/fields ─────────
  const varToEntity   = collectEntityInstances(doc, pos.line);
  const entityName    = varToEntity.get(obj);
  if (entityName) {
    const entityActions = collectEntityActions(doc);
    const entityFields  = collectEntityFields(doc);
    const items: vscode.CompletionItem[] = [];
    for (const a of entityActions.get(entityName) ?? []) {
      if (partial.length > 0 && !a.startsWith(partial)) continue;
      const ci  = new vscode.CompletionItem(a, vscode.CompletionItemKind.Method);
      ci.detail = `${entityName} action`;
      items.push(ci);
    }
    for (const f of entityFields.get(entityName) ?? []) {
      if (partial.length > 0 && !f.startsWith(partial)) continue;
      const ci  = new vscode.CompletionItem(f, vscode.CompletionItemKind.Field);
      ci.detail = `${entityName} field`;
      items.push(ci);
    }
    return items;
  }

  return [];
}

// ─── Main Completion Provider ─────────────────────────────────────────────────

const MEANINGFUL_TRIGGERS = new Set(['#', '.', ':', '"', '<', '/', '\\']);

export class ErelangCompletionProvider implements vscode.CompletionItemProvider {
  provideCompletionItems(
    doc: vscode.TextDocument,
    pos: vscode.Position,
    _token: vscode.CancellationToken,
    context?: vscode.CompletionContext,
  ): vscode.CompletionItem[] {
    const prefix   = doc.lineAt(pos.line).text.slice(0, pos.character);
    const fullLine = doc.lineAt(pos.line).text;
    const col      = collect(doc, pos.line);

    // ── #include directive keyword ─────────────────────────────────────────
    const includeDirective = includeDirectiveCompletions(pos, prefix);
    if (includeDirective) {
      dbgCompletion('include-directive', pos, prefix, `${includeDirective.length} items`);
      return includeDirective;
    }

    // ── Print / interpolation context ──────────────────────────────────────
    const pctx = parsePrintStringContext(fullLine, pos.character);
    if (pctx) {
      dbgCompletion('print-ctx', pos, prefix, 'interpolation');
      const names = new Set([...col.locals, ...col.globals, ...col.fields, ...col.actions]);
      const range = new vscode.Range(pos.line, pctx.replaceStart, pos.line, pctx.replaceEnd);
      return [...names]
        .filter(n => pctx.partial.length === 0 || n.startsWith(pctx.partial))
        .map(n => {
          const ci  = new vscode.CompletionItem(n, vscode.CompletionItemKind.Variable);
          ci.range  = range;
          ci.detail = 'interpolation variable';
          ci.insertText = pctx.shouldAppendClosingBrace ? `${n}}` : n;
          return ci;
        });
    }

    // ── #include path completions ──────────────────────────────────────────
    const incl = includePathCompletions(doc, pos, prefix);
    if (incl) { dbgCompletion('include-path', pos, prefix, `${incl.length} items`); return incl; }

    // ── Member access (dot) — handles modules, self, and entity instances ──
    // null = no dot in prefix; [] = dot found but no known obj → suppress global list
    const memberItems = memberCompletions(doc, pos, prefix);
    if (memberItems !== null) return memberItems;

    // ── Global / fallback suggestions ──────────────────────────────────────
    if (isControlFlowAwaitingBody(prefix)) {
      dbgCompletion('control-flow-body-suppressed', pos, prefix);
      return [];
    }

    const fromAutoEdit = consumeAutoSuggestFromEdit();
    const identMatch = /[A-Za-z_]\w*$/.exec(prefix);
    const partial    = identMatch?.[0] ?? '';
    const manual     = !context || context.triggerKind === vscode.CompletionTriggerKind.Invoke;
    const triggered  = context?.triggerCharacter && MEANINGFUL_TRIGGERS.has(context.triggerCharacter);

    // Empty prefix: only explicit trigger chars. Block auto-retrigger Invoke dump (Enter → top item).
    if (partial.length === 0 && (fromAutoEdit || (!manual && !triggered))) {
      dbgCompletion('global-fallback-suppressed', pos, prefix);
      return [];
    }

    const replaceRange = partial.length > 0
      ? new vscode.Range(pos.line, pos.character - partial.length, pos.line, pos.character)
      : undefined;

    const matchesPartial = (n: string) =>
      partial.length === 0 || n.toLowerCase().startsWith(partial.toLowerCase());

    const items: vscode.CompletionItem[] = [];
    const seen  = new Set<string>();

    const add = (names: Iterable<string>, kind: vscode.CompletionItemKind, detail?: string, sort = 'z') => {
      for (const n of names) {
        if (!matchesPartial(n) || seen.has(n)) continue;
        seen.add(n);
        const ci  = new vscode.CompletionItem(n, kind);
        ci.detail = detail ?? (kind === vscode.CompletionItemKind.Function ? 'action' : kind === vscode.CompletionItemKind.Class ? 'entity' : undefined);
        ci.sortText = `${sort}_${n}`;
        if (replaceRange) ci.range = replaceRange;
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
      if (!matchesPartial(kw) || seen.has(kw)) continue;
      seen.add(kw);
      const ci  = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
      ci.detail   = 'keyword';
      ci.sortText = `aa_${kw}`;
      if (replaceRange) ci.range = replaceRange;
      items.push(ci);
    }

    for (const [enumName, members] of col.enumMembers) {
      for (const mem of members) {
        const q = `${enumName}.${mem}`;
        if (!matchesPartial(q) || seen.has(q)) continue;
        seen.add(q);
        const ci  = new vscode.CompletionItem(q, vscode.CompletionItemKind.EnumMember);
        ci.detail   = 'enum member';
        ci.sortText = `e_${q}`;
        if (replaceRange) ci.range = replaceRange;
        items.push(ci);
      }
    }

    const imported = collectImports(doc);

    // Module aliases (e.g. `fs`, `math`) — only shown when declared via #include / import
    for (const alias of imported.aliasToActions.keys()) {
      if (!matchesPartial(alias) || seen.has(alias)) continue;
      seen.add(alias);
      const ci  = new vscode.CompletionItem(alias, vscode.CompletionItemKind.Module);
      ci.detail   = 'module alias';
      ci.sortText = `b_${alias}`;
      if (replaceRange) ci.range = replaceRange;
      items.push(ci);
    }

    for (const n of imported.allActions) {
      if (!matchesPartial(n) || seen.has(n)) continue;
      seen.add(n);
      const ci  = new vscode.CompletionItem(n, vscode.CompletionItemKind.Function);
      ci.detail   = 'imported action';
      ci.sortText = `m_${n}`;
      if (replaceRange) ci.range = replaceRange;
      items.push(ci);
    }

    for (const b of BUILT_INS) {
      if (DEPRECATED_BUILT_INS.has(b) || !matchesPartial(b) || seen.has(b)) continue;
      seen.add(b);
      const ci  = new vscode.CompletionItem(b, vscode.CompletionItemKind.Function);
      ci.detail   = 'builtin';
      ci.sortText = `z_${b}`;
      if (replaceRange) ci.range = replaceRange;
      if (b.toLowerCase() === 'print' && partial.toLowerCase().startsWith('p')) ci.preselect = true;
      items.push(ci);
    }

    if (/\bprint\s*$/.test(prefix)) {
      const snippet = new vscode.CompletionItem('print "{value}";', vscode.CompletionItemKind.Snippet);
      snippet.insertText = new vscode.SnippetString('print "{$1}";');
      snippet.detail     = 'print interpolation';
      snippet.sortText   = 'aa_print_snippet';
      if (replaceRange) snippet.range = replaceRange;
      items.unshift(snippet);
    }

    dbgCompletion('global-fallback', pos, prefix, `${items.length} items`);
    return items;
  }
}
