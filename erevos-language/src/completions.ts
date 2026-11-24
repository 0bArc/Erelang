import * as vscode from 'vscode';
import * as path   from 'path';
import { PrintStringContext, DocumentIndex } from './types';
import { getDocumentIndex, declarationAt, MAX_INDEX_LINES, MAX_INDEX_LINE_CHARS, MAX_INDEX_CHARS } from './symbols';
import { collectImports, listDirectoryCached } from './imports';
import {
  ManifestMember, allModulePaths, coreBuiltins, keywords, typeWords, moduleForPath, signature,
  typeKindFor, typeSpec, visibleMembers,
} from './manifest';
import { moduleSnippets } from './moduleSnippets';
import { visibleBindings } from './scopeAnalysis';

let _debugChannel: vscode.OutputChannel | undefined;
export function setDebugChannel(ch: vscode.OutputChannel): void { _debugChannel = ch; }

const BULK_EDIT_WINDOW_MS = 400;
const BULK_EDIT_MIN_CHARS = 400;
const _bulkEditUntil = new WeakMap<vscode.TextDocument, number>();

export function noteDocumentEdit(e: vscode.TextDocumentChangeEvent): void {
  if (e.document.languageId !== 'erelang') return;
  let size = 0;
  for (const c of e.contentChanges) {
    size += c.text.length + (c.rangeLength ?? 0);
  }
  if (size >= BULK_EDIT_MIN_CHARS || e.contentChanges.length > 20) {
    _bulkEditUntil.set(e.document, Date.now() + BULK_EDIT_WINDOW_MS);
  }
}

export function isDocumentBulkEditing(doc: vscode.TextDocument): boolean {
  return Date.now() < (_bulkEditUntil.get(doc) ?? 0);
}

function shouldSkipHeavyCompletion(doc: vscode.TextDocument): boolean {
  return doc.lineCount > MAX_INDEX_LINES || isDocumentBulkEditing(doc) || doc.getText().length > MAX_INDEX_CHARS;
}

function dbg(msg: string): void {
  if (!vscode.workspace.getConfiguration('erelang').get<boolean>('debugCompletion', false)) return;
  _debugChannel?.appendLine(msg);
}

function dbgCompletion(branch: string, pos: vscode.Position, prefix: string, detail?: string): void {
  dbg(`[completion] ${branch} @ ${pos.line + 1}:${pos.character + 1}  prefix="${prefix}"${detail ? '  ' + detail : ''}`);
}

// Calls whose result is a handle with its own methods (string handles like "ws:3").
const HANDLE_RESULTS: Record<string, string> = {
  ws_connect: 'ws', tcp_connect: 'tcp', udp_bind: 'udp', http_get_resp: 'resp', file_open: 'file',
  chan_new: 'chan', proc_start: 'proc',
};

function memberItem(m: ManifestMember, detailPrefix: string): vscode.CompletionItem {
  const ci = new vscode.CompletionItem(m.name, vscode.CompletionItemKind.Method);
  ci.detail = signature(m, detailPrefix);
  ci.documentation = m.doc;
  return ci;
}

/**
 * Member type kind of the receiver `obj` at `line`, or undefined when the variable is unknown.
 * Typed declarations win; an untyped `x = ws.connect(...)` or a string holding a handle is resolved
 * through the call that produced it.
 */
async function receiverKind(doc: vscode.TextDocument, index: DocumentIndex, obj: string, line: number): Promise<string | undefined> {
  const binding = visibleBindings(doc.getText(), { line, character: doc.lineAt(line).text.length })
    .find(b => b.token.text === obj && b.kind !== 'action');
  if (!binding) return undefined;
  const decl = (index.declarations.get(obj) ?? []).find(d => d.line === binding.token.line);
  if (!decl && binding.detail) return typeKindFor(binding.detail);
  if (!decl) return undefined;
  if (decl.init) {
    const call = /^([A-Za-z_]\w*)\s*\.\s*([A-Za-z_][\w.]*)\s*\(/.exec(decl.init) ?? /^([A-Za-z_]\w*)\s*\(/.exec(decl.init);
    if (call) {
      if (call.length === 3) {
        const imported = await collectImports(doc);
        const spec = imported.aliasToModule.get(call[1]);
        const mod = spec ? moduleForPath(spec) : undefined;
        const member = mod?.members.find(m => m.name === call[2]);
        const kind = member?.builtin ? HANDLE_RESULTS[member.builtin] : undefined;
        if (kind) return kind;
      } else if (HANDLE_RESULTS[call[1]]) {
        return HANDLE_RESULTS[call[1]];
      }
    }
  }
  return typeKindFor(decl.type);
}

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

async function includePathCompletions(
  doc: vscode.TextDocument,
  pos: vscode.Position,
  prefix: string,
  token: vscode.CancellationToken,
): Promise<vscode.CompletionItem[] | null> {
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
  const nameLower = namePart.toLowerCase();
  const partialLower = partial.toLowerCase();

  const out: vscode.CompletionItem[] = [];
  for (const mod of allModulePaths()) {
    if (!mod.toLowerCase().startsWith(partialLower)) continue;
    const insert = mode === 'bare'
      ? `<${mod}>;`
      : (dirPart && mod.toLowerCase().startsWith(dirPart.toLowerCase()) ? mod.slice(dirPart.length) : mod);
    const label = mode === 'bare' ? `<${mod}>;` : mod;
    const ci    = new vscode.CompletionItem(label, vscode.CompletionItemKind.Module);
    ci.insertText = insert;
    ci.range      = replaceRange;
    ci.detail     = mode === 'bare' ? 'include builtin module' : 'builtin module';
    ci.sortText   = `a_${mod}`;
    out.push(ci);
  }

  if (token.isCancellationRequested) return [];

  const candidateDirs = new Set<string>();
  candidateDirs.add(path.resolve(path.dirname(doc.uri.fsPath), dirPart || '.'));
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    candidateDirs.add(path.resolve(folder.uri.fsPath, dirPart || '.'));
  }

  const seen = new Set<string>();
  for (const dir of candidateDirs) {
    if (token.isCancellationRequested) return out;
    const entries = await listDirectoryCached(dir);
    for (const e of entries) {
      if (!e.name.toLowerCase().startsWith(nameLower)) continue;
      if (e.isDirectory) {
        const insert = dirPart + e.name + '/';
        if (seen.has(insert)) continue;
        seen.add(insert);
        const ci = new vscode.CompletionItem(mode === 'bare' ? `<${insert}` : e.name + '/', vscode.CompletionItemKind.Folder);
        ci.insertText = mode === 'bare' ? `<${insert}` : e.name + '/';
        ci.range      = replaceRange;
        ci.detail     = 'folder';
        out.push(ci);
      } else if (e.isFile && /\.(elan|ere|0bs)$/i.test(e.name)) {
        const insert = dirPart + e.name;
        if (seen.has(insert)) continue;
        seen.add(insert);
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

export function isForeachColonCtx(prefix: string): boolean {
  return /\bfor\s*\([^)]*:\s*[A-Za-z_]*$/.test(prefix);
}

export function isDictLiteralCtx(prefix: string): boolean {
  return /[{,]\s*(?:"[^"]*"|'[^']*'|[A-Za-z_]\w*)\s*:\s*[A-Za-z_]*$/.test(prefix);
}

export function isControlFlowAwaitingBody(prefix: string): boolean {
  const t = prefix.trimEnd();
  return (
    /\b(switch|match|if|else|for|while|try|catch|repeat|do|unsafe|parallel|namespace)\s*(\([^)\n]{0,200}\))?\s*$/.test(t) ||
    /\b(action|entity|struct|enum|hook)\s+[A-Za-z_]\w*\s*(\([^)\n]{0,200}\))?\s*$/.test(t)
  );
}

async function memberCompletions(
  doc: vscode.TextDocument,
  pos: vscode.Position,
  prefix: string,
  token: vscode.CancellationToken,
): Promise<vscode.CompletionItem[] | null> {
  const dot = /([A-Za-z_]\w*)\.([A-Za-z_]\w*)?$/.exec(prefix);
  if (!dot) return null;

  // Grouped module members: `ai.regex.` lists generate, explain, optimize, ...
  const group = /([A-Za-z_]\w*)\.([A-Za-z_]\w*)\.([A-Za-z_]\w*)?$/.exec(prefix);
  if (group) {
    const imported = await collectImports(doc);
    const spec = imported.aliasToModule.get(group[1]);
    const mod = spec ? moduleForPath(spec) : undefined;
    if (mod) {
      const head = `${group[2]}.`;
      const items: vscode.CompletionItem[] = [];
      for (const m of visibleMembers(mod)) {
        if (!m.name.startsWith(head)) continue;
        const rest = m.name.slice(head.length);
        if (group[3] && !rest.startsWith(group[3])) continue;
        const ci = memberItem(m, `${group[1]}.`);
        ci.label = rest;
        ci.insertText = rest;
        items.push(ci);
      }
      if (items.length > 0) return items;
    }
  }

  const obj     = dot[1];
  const partial = dot[2] ?? '';
  dbgCompletion('member-access', pos, prefix, `obj=${obj}`);

  const index = getDocumentIndex(doc);
  const entityActions = index.entityMembers.actions;
  const entityFields  = index.entityMembers.fields;

  if (obj === 'self') {
    const items: vscode.CompletionItem[] = [];
    for (const [, actions] of entityActions) {
      for (const a of actions) {
        if (partial.length > 0 && !a.startsWith(partial)) continue;
        const ci  = new vscode.CompletionItem(a, vscode.CompletionItemKind.Method);
        ci.detail = 'self action';
        items.push(ci);
      }
    }
    for (const [, flds] of entityFields) {
      for (const f of flds) {
        if (partial.length > 0 && !f.startsWith(partial)) continue;
        const ci  = new vscode.CompletionItem(f, vscode.CompletionItemKind.Field);
        ci.detail = 'self field';
        items.push(ci);
      }
    }
    return items;
  }

  const entityName = index.entityInstances.get(obj);
  if (entityName) {
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

  if (token.isCancellationRequested) return [];
  const imported = await collectImports(doc);
  if (token.isCancellationRequested) return [];

  // A builtin module alias: its members, including grouped names (ai.regex.generate).
  const spec = imported.aliasToModule.get(obj);
  const mod = spec ? moduleForPath(spec) : undefined;
  if (mod) {
    const items: vscode.CompletionItem[] = [];
    for (const m of visibleMembers(mod)) {
      if (partial.length > 0 && !m.name.startsWith(partial)) continue;
      items.push(memberItem(m, `${obj}.`));
    }
    return items;
  }

  // A script include alias: the actions of that file.
  const scriptActions = imported.aliasToActions.get(obj);
  if (scriptActions && scriptActions.size > 0) {
    return [...scriptActions]
      .filter(m => partial.length === 0 || m.startsWith(partial))
      .map(m => {
        const ci  = new vscode.CompletionItem(m, vscode.CompletionItemKind.Method);
        ci.detail = `${obj} action`;
        return ci;
      });
  }

  // A struct instance: its fields.
  const decl = declarationAt(index, obj, pos.line);
  if (decl) {
    const fields = index.symbols.structFields.get(decl.type.replace(/<.*$/, ''));
    if (fields && fields.size > 0) {
      return [...fields]
        .filter(f => partial.length === 0 || f.startsWith(partial))
        .map(f => {
          const ci = new vscode.CompletionItem(f, vscode.CompletionItemKind.Field);
          ci.detail = `${decl.type} field`;
          return ci;
        });
    }
  }

  // A typed variable: the members of its type. Unknown or undeclared: nothing, never a guess.
  const kind = await receiverKind(doc, index, obj, pos.line);
  const t = kind ? typeSpec(kind) : undefined;
  if (!t) return [];
  return t.members
    .filter(m => partial.length === 0 || m.name.startsWith(partial))
    .map(m => memberItem(m, `${kind}.`));
}

const MEANINGFUL_TRIGGERS = new Set(['#', '.', ':', '"', '<', '/', '\\']);

export class ErelangCompletionProvider implements vscode.CompletionItemProvider {
  async provideCompletionItems(
    doc: vscode.TextDocument,
    pos: vscode.Position,
    token: vscode.CancellationToken,
    context?: vscode.CompletionContext,
  ): Promise<vscode.CompletionItem[] | undefined> {
    try {
      if (token.isCancellationRequested) return [];
      return await this.provideCompletionItemsInner(doc, pos, token, context);
    } catch {
      return [];
    }
  }

  private async provideCompletionItemsInner(
    doc: vscode.TextDocument,
    pos: vscode.Position,
    token: vscode.CancellationToken,
    context?: vscode.CompletionContext,
  ): Promise<vscode.CompletionItem[] | undefined> {
    if (token.isCancellationRequested) return [];
    if (shouldSkipHeavyCompletion(doc)) return [];

    const startVersion = doc.version;
    const stale = () =>
      token.isCancellationRequested || doc.isClosed || doc.version !== startVersion || isDocumentBulkEditing(doc);

    const fullLine = doc.lineAt(pos.line).text;
    if (fullLine.length > MAX_INDEX_LINE_CHARS) return [];
    const prefix = fullLine.slice(0, pos.character);

    if (context?.triggerCharacter === ' ') {
      const incl = await includePathCompletions(doc, pos, prefix, token);
      if (stale()) return [];
      return incl && incl.length > 0 ? incl : undefined;
    }

    const includeDirective = includeDirectiveCompletions(pos, prefix);
    if (includeDirective) {
      dbgCompletion('include-directive', pos, prefix, `${includeDirective.length} items`);
      return includeDirective;
    }

    const incl = await includePathCompletions(doc, pos, prefix, token);
    if (stale()) return [];
    if (incl) {
      dbgCompletion('include-path', pos, prefix, `${incl.length} items`);
      return incl;
    }

    if (stale()) return [];
    const memberItems = await memberCompletions(doc, pos, prefix, token);
    if (stale()) return [];
    if (memberItems !== null) return memberItems;

    // Namespace::member completions
    const scopeMatch = /([A-Za-z_]\w*)\s*::\s*([A-Za-z_]?\w*)$/.exec(prefix);
    if (scopeMatch) {
      const ns = scopeMatch[1];
      const partialMember = scopeMatch[2] ?? '';
      const index = getDocumentIndex(doc);
      if (stale()) return [];
      const members = index.symbols.namespaceMembers.get(ns);
      if (members && members.size > 0) {
        return [...members]
          .filter(m => partialMember.length === 0 || m.startsWith(partialMember))
          .map(m => {
            const ci = new vscode.CompletionItem(m, vscode.CompletionItemKind.Method);
            ci.detail = `${ns}::`;
            ci.range = new vscode.Range(
              pos.line, pos.character - partialMember.length, pos.line, pos.character);
            return ci;
          });
      }
    }

    if (stale()) return [];

    const index = getDocumentIndex(doc);
    if (stale()) return [];
    const col = index.symbols;

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

    if (isControlFlowAwaitingBody(prefix)) {
      dbgCompletion('control-flow-body-suppressed', pos, prefix);
      return [];
    }

    const identMatch = /[A-Za-z_]\w*$/.exec(prefix);
    const partial    = identMatch?.[0] ?? '';
    const manual     = !context || context.triggerKind === vscode.CompletionTriggerKind.Invoke;
    const triggered  = context?.triggerCharacter && MEANINGFUL_TRIGGERS.has(context.triggerCharacter);

    if (partial.length === 0 && !manual && !triggered) {
      dbgCompletion('global-fallback-suppressed', pos, prefix);
      return [];
    }

    if (stale()) return [];

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

    const visible = visibleBindings(doc.getText(), pos);
    add(visible.filter(b => b.kind !== 'action').map(b => b.token.text),
      vscode.CompletionItemKind.Variable, 'local variable', 'a');
    add(col.globals,     vscode.CompletionItemKind.Variable,       'global',         'a');
    add(col.namespaces,  vscode.CompletionItemKind.Module,         'namespace',      'b');
    add(col.entities,    vscode.CompletionItemKind.Class,           undefined,        'b');
    add(col.structs,     vscode.CompletionItemKind.Struct,         'struct',         'b');
    add(col.enums,       vscode.CompletionItemKind.Enum,           'enum',           'b');
    add(col.typeAliases, vscode.CompletionItemKind.TypeParameter,  'type alias',     'b');
    add(visible.filter(b => b.kind === 'action').map(b => b.token.text),
      vscode.CompletionItemKind.Function, undefined, 'c');
    add(col.fields,      vscode.CompletionItemKind.Field,           undefined,        'c');
    add(col.hooks,       vscode.CompletionItemKind.Event,           undefined,        'c');

    for (const [ns, members] of col.namespaceMembers) {
      for (const mem of members) {
        const q = `${ns}::${mem}`;
        if (!matchesPartial(q) && !matchesPartial(mem)) continue;
        if (seen.has(q)) continue;
        seen.add(q);
        const ci = new vscode.CompletionItem(q, vscode.CompletionItemKind.Method);
        ci.detail = 'namespace member';
        ci.sortText = `d_${q}`;
        if (replaceRange) ci.range = replaceRange;
        items.push(ci);
      }
    }

    for (const kw of [...keywords(), ...typeWords()]) {
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

    if (stale()) return items;

    // Core builtins only: module members appear after their alias, and only when imported.
    for (const b of coreBuiltins()) {
      const n = b.name;
      if (!matchesPartial(n) || seen.has(n)) continue;
      seen.add(n);
      const ci  = new vscode.CompletionItem(n, vscode.CompletionItemKind.Function);
      ci.detail   = signature(b);
      ci.documentation = b.doc;
      ci.sortText = `z_${n}`;
      if (replaceRange) ci.range = replaceRange;
      if (n === 'print' && partial.toLowerCase().startsWith('p')) ci.preselect = true;
      items.push(ci);
    }

    const wantImports = manual || triggered || partial.length >= 1;
    if (wantImports) {
      const imported = await collectImports(doc);
      if (stale()) return items;

      const aliases = new Set<string>([...imported.aliasToModule.keys(), ...imported.aliasToActions.keys()]);
      for (const alias of aliases) {
        if (!matchesPartial(alias) || seen.has(alias)) continue;
        seen.add(alias);
        const spec = imported.aliasToModule.get(alias);
        const mod = spec ? moduleForPath(spec) : undefined;
        const ci  = new vscode.CompletionItem(alias, vscode.CompletionItemKind.Module);
        ci.detail   = mod ? `${spec} (${mod.summary})` : 'module alias';
        ci.sortText = `b_${alias}`;
        if (replaceRange) ci.range = replaceRange;
        items.push(ci);
      }

      // Snippets that only make sense with a module imported (re.expect, ai.regex.generate, ...).
      for (const [alias, spec] of imported.aliasToModule) {
        const mod = moduleForPath(spec);
        if (!mod) continue;
        for (const sn of moduleSnippets(mod.id, alias)) {
          if (!matchesPartial(sn.prefix) || seen.has(`snippet:${sn.prefix}`)) continue;
          seen.add(`snippet:${sn.prefix}`);
          const ci = new vscode.CompletionItem(sn.prefix, vscode.CompletionItemKind.Snippet);
          ci.insertText = new vscode.SnippetString(sn.body);
          ci.detail = sn.description;
          ci.sortText = `c_${sn.prefix}`;
          if (replaceRange) ci.range = replaceRange;
          items.push(ci);
        }
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
