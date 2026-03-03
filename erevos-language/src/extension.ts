import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

// Simple regex patterns for declarations
const ENTITY_RE = /^\s*(?:public|private)?\s*entity\s+([A-Za-z_][A-Za-z0-9_]*)/;
const ACTION_RE = /^\s*(?:public|private)?\s*action\s+([A-Za-z_][A-Za-z0-9_]*)/;
const FIELD_RE  = /^\s*(?:public|private)?\s*field\s+([A-Za-z_][A-Za-z0-9_]*)/;
const HOOK_RE   = /^\s*hook\s+([A-Za-z_][A-Za-z0-9_]*)/;
const INCLUDE_ALIAS_RE = /^\s*#include\s*(<[^>]+>|"[^"]+")\s*(?:as\s+([A-Za-z_][A-Za-z0-9_]*))?/;
const IMPORT_ALIAS_RE = /^\s*import\s+([A-Za-z_][A-Za-z0-9_./-]*)\s*(?:as\s+([A-Za-z_][A-Za-z0-9_]*))?/;

const MODULE_METHODS_BY_SPEC: Record<string, string[]> = {
  'builtin/erefs': ['cwd', 'chdir', 'mkdir', 'read', 'write', 'append', 'copy', 'move', 'exists', 'list', 'remove'],
  'builtin/erepath': ['join', 'dirname', 'basename', 'ext']
};

// Built‑in functions & keywords (extended)
const BUILT_INS = [
  // Core / time / env
  'print','PRINT','sleep','now_ms','now_iso','env','username','computer_name','machine_guid','uuid','rand_int','hwid','args_count','args_get','input',
  'toint','toInt','tofloat','tostr','toString',
  'string.lstrip','string.rstrip','string.strip','string.lower','string.upper',
  // Filesystem
  'read_text','write_text','append_text','file_exists','mkdirs','copy_file','move_file','delete_file','list_files','cwd','chdir',
  'path_join','path_dirname','path_basename','path_ext',
  // Collections
  'list_new','list_push','list_get','list_len','list_join','list_clear','list_remove_at',
  'dict_new','dict_set','dict_get','dict_has','dict_keys','dict_values','dict_get_or','dict_remove','dict_clear','dict_size','dict_merge','dict_clone','dict_items','dict_entries',
  'dict_set_path','dict_get_path','dict_has_path','dict_remove_path',
  'table_new','table_put','table_get','table_has','table_remove','table_rows','table_columns','table_row_keys','table_clear_row','table_count_row',
  // Network
  'http_get','http_download','hls_download_best','url_encode','network.ip.flush','network.ip.release','network.ip.renew','network.ip.registerdns',
  'network.debug.enable','network.debug.disable','network.debug.status','network.debug.last','network.debug.clear','network.debug.log_tail',
  // Language meta
  'language_name','language_version','language_about','language_limitations',
  // Windowing / UI
  'win_window_create','win_button_create','win_checkbox_create','win_radiobutton_create','win_slider_create','win_textbox_create','win_label_create','win_on','win_show','win_loop','win_get_text','win_set_text','win_get_check','win_set_check','win_get_slider','win_set_slider','win_close','win_auto_scale','win_set_scale','win_message_box',
  'ui_window_create','ui_label','ui_button','ui_checkbox','ui_radio','ui_slider','ui_textbox','ui_same_line','ui_newline','ui_spacer','ui_separator','ui_load',
  // Data store
  'data_new','data_set','data_get','data_has','data_keys','data_save','data_load',
  // Crypto
  'hash_fnv1a','random_bytes',
  // Regex
  'regex_match','regex_find','regex_replace',
  // Permissions
  'perm_grant','perm_revoke','perm_has','perm_list',
  // Binary buffers
  'bin_new','bin_from_hex','bin_to_hex','bin_len','bin_get','bin_set','bin_fill','bin_slice',
  // Threads
  'thread_run','thread_join','thread_done',
  // Collatz / math extras
  'collatz_len','collatz_sweep','collatz_best_steps','collatz_avg_steps'
];

interface CollectedSymbols {
  entities: Set<string>;
  actions: Set<string>;
  fields: Set<string>;
  hooks: Set<string>;
}

interface ImportedSymbols {
  aliasToActions: Map<string, Set<string>>;
  allActions: Set<string>;
}

function normalizeModuleSpec(moduleSpec: string): string {
  const trimmed = moduleSpec.trim();
  if (trimmed.startsWith('<') && trimmed.endsWith('>')) return trimmed.slice(1, -1).toLowerCase();
  if (trimmed.startsWith('"') && trimmed.endsWith('"')) return trimmed.slice(1, -1).toLowerCase();
  return trimmed.toLowerCase();
}

function defaultAliasFromSpec(moduleSpec: string): string {
  const spec = normalizeModuleSpec(moduleSpec);
  const parts = spec.split('/');
  return parts[parts.length - 1].replace(/[^A-Za-z0-9_]/g, '_');
}

function collectModuleAliases(document: vscode.TextDocument): Map<string, string[]> {
  const aliasToMethods = new Map<string, string[]>();
  for (let index = 0; index < document.lineCount; index++) {
    const text = document.lineAt(index).text;
    let moduleSpec: string | null = null;
    let alias: string | null = null;

    const includeMatch = INCLUDE_ALIAS_RE.exec(text);
    if (includeMatch) {
      moduleSpec = normalizeModuleSpec(includeMatch[1]);
      alias = includeMatch[2] ?? defaultAliasFromSpec(includeMatch[1]);
    } else {
      const importMatch = IMPORT_ALIAS_RE.exec(text);
      if (importMatch) {
        moduleSpec = normalizeModuleSpec(importMatch[1]);
        alias = importMatch[2] ?? defaultAliasFromSpec(importMatch[1]);
      }
    }

    if (!moduleSpec || !alias) continue;
    const methods = MODULE_METHODS_BY_SPEC[moduleSpec];
    if (!methods) continue;
    aliasToMethods.set(alias, methods);
  }

  return aliasToMethods;
}

function tryResolveIncludeFile(document: vscode.TextDocument, moduleSpec: string): string | null {
  const spec = normalizeModuleSpec(moduleSpec);
  if (!spec || spec.startsWith('builtin/')) return null;

  const docDir = path.dirname(document.uri.fsPath);
  const exts = ['.elan', '.ere', '.0bs'];
  const candidates: string[] = [];

  const direct = path.resolve(docDir, spec);
  candidates.push(direct);

  if (!path.extname(spec)) {
    for (const ext of exts) {
      candidates.push(path.resolve(docDir, spec + ext));
    }
  }

  if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
    for (const folder of vscode.workspace.workspaceFolders) {
      candidates.push(path.resolve(folder.uri.fsPath, spec));
      if (!path.extname(spec)) {
        for (const ext of exts) {
          candidates.push(path.resolve(folder.uri.fsPath, spec + ext));
        }
      }
    }
  }

  for (const candidate of candidates) {
    try {
      if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
        return candidate;
      }
    } catch {
      // Ignore inaccessible candidates.
    }
  }
  return null;
}

function extractActionsFromFile(filePath: string): Set<string> {
  const names = new Set<string>();
  try {
    const content = fs.readFileSync(filePath, 'utf8');
    for (const line of content.split(/\r?\n/)) {
      const m = ACTION_RE.exec(line);
      if (m) names.add(m[1]);
    }
  } catch {
    // Ignore unreadable files.
  }
  return names;
}

function collectImportedSymbols(document: vscode.TextDocument): ImportedSymbols {
  const aliasToActions = new Map<string, Set<string>>();
  const allActions = new Set<string>();

  for (let index = 0; index < document.lineCount; index++) {
    const text = document.lineAt(index).text;
    let moduleSpec: string | null = null;
    let alias: string | null = null;

    const includeMatch = INCLUDE_ALIAS_RE.exec(text);
    if (includeMatch) {
      moduleSpec = includeMatch[1];
      alias = includeMatch[2] ?? defaultAliasFromSpec(includeMatch[1]);
    } else {
      const importMatch = IMPORT_ALIAS_RE.exec(text);
      if (importMatch) {
        moduleSpec = importMatch[1];
        alias = importMatch[2] ?? defaultAliasFromSpec(importMatch[1]);
      }
    }

    if (!moduleSpec || !alias) continue;

    const builtinMethods = MODULE_METHODS_BY_SPEC[normalizeModuleSpec(moduleSpec)];
    if (builtinMethods) {
      aliasToActions.set(alias, new Set(builtinMethods));
      continue;
    }

    const resolved = tryResolveIncludeFile(document, moduleSpec);
    if (!resolved) continue;

    const actions = extractActionsFromFile(resolved);
    if (actions.size === 0) continue;
    aliasToActions.set(alias, actions);
    for (const name of actions) allActions.add(name);
  }

  return { aliasToActions, allActions };
}

function provideIncludePathCompletions(document: vscode.TextDocument, linePrefix: string): vscode.CompletionItem[] | null {
  const includeMatch = /^\s*#include\s*<([^>]*)$/.exec(linePrefix) ?? /^\s*#include\s*"([^"]*)$/.exec(linePrefix);
  if (!includeMatch) return null;

  const partial = includeMatch[1].replace(/\\/g, '/');
  const slashIdx = partial.lastIndexOf('/');
  const dirPart = slashIdx >= 0 ? partial.slice(0, slashIdx + 1) : '';
  const namePart = slashIdx >= 0 ? partial.slice(slashIdx + 1) : partial;

  const docDir = path.dirname(document.uri.fsPath);
  const targetDir = path.resolve(docDir, dirPart || '.');
  let entries: fs.Dirent[] = [];
  try {
    entries = fs.readdirSync(targetDir, { withFileTypes: true });
  } catch {
    return [];
  }

  const out: vscode.CompletionItem[] = [];
  for (const entry of entries) {
    if (!entry.name.toLowerCase().startsWith(namePart.toLowerCase())) continue;

    if (entry.isDirectory()) {
      const label = entry.name + '/';
      const ci = new vscode.CompletionItem(label, vscode.CompletionItemKind.Folder);
      ci.insertText = dirPart + label;
      ci.detail = 'folder';
      out.push(ci);
      continue;
    }

    if (!entry.isFile()) continue;
    const lower = entry.name.toLowerCase();
    if (!(lower.endsWith('.elan') || lower.endsWith('.ere') || lower.endsWith('.0bs'))) continue;

    const ci = new vscode.CompletionItem(entry.name, vscode.CompletionItemKind.File);
    ci.insertText = dirPart + entry.name;
    ci.detail = 'script file';
    out.push(ci);
  }
  return out;
}

function collect(document: vscode.TextDocument): CollectedSymbols {
  const out: CollectedSymbols = { entities: new Set(), actions: new Set(), fields: new Set(), hooks: new Set() };
  for (let i=0;i<document.lineCount;i++) {
    const text = document.lineAt(i).text;
    let m = ENTITY_RE.exec(text); if (m) out.entities.add(m[1]);
    m = ACTION_RE.exec(text); if (m) out.actions.add(m[1]);
    m = FIELD_RE.exec(text); if (m) out.fields.add(m[1]);
    m = HOOK_RE.exec(text); if (m) out.hooks.add(m[1]);
  }
  return out;
}

class ErelangCompletionProvider implements vscode.CompletionItemProvider {
  provideCompletionItems(doc: vscode.TextDocument, pos: vscode.Position): vscode.CompletionItem[] {
    const linePrefix = doc.lineAt(pos.line).text.slice(0, pos.character);

    const includeItems = provideIncludePathCompletions(doc, linePrefix);
    if (includeItems) return includeItems;

    const memberAccess = /([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)?$/.exec(linePrefix);
    if (memberAccess) {
      const objectName = memberAccess[1];
      const partialName = memberAccess[2] ?? '';
      const imported = collectImportedSymbols(doc);
      const methods = imported.aliasToActions.get(objectName) ?? new Set<string>();
      return [...methods]
        .filter((methodName) => partialName.length === 0 || methodName.startsWith(partialName))
        .map((methodName) => {
          const completionItem = new vscode.CompletionItem(methodName, vscode.CompletionItemKind.Method);
          completionItem.detail = `${objectName} module`;
          return completionItem;
        });
    }

    const col = collect(doc);
    const items: vscode.CompletionItem[] = [];
    const pushAll = (names: Iterable<string>, kind: vscode.CompletionItemKind) => {
      for (const n of names) {
        const ci = new vscode.CompletionItem(n, kind);
        ci.detail = kind === vscode.CompletionItemKind.Function ? 'action' : kind === vscode.CompletionItemKind.Class ? 'entity' : undefined;
        items.push(ci);
      }
    };
    pushAll(col.entities, vscode.CompletionItemKind.Class);
    pushAll(col.actions, vscode.CompletionItemKind.Function);
    pushAll(col.fields, vscode.CompletionItemKind.Field);
    pushAll(col.hooks, vscode.CompletionItemKind.Event);

    const imported = collectImportedSymbols(doc);
    for (const name of imported.allActions) {
      const ci = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
      ci.detail = 'imported action';
      items.push(ci);
    }

    for (const b of BUILT_INS) {
      const ci = new vscode.CompletionItem(b, vscode.CompletionItemKind.Function);
      ci.detail = 'builtin';
      items.push(ci);
    }
    return items;
  }
}

class ErelangDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
  provideDocumentSymbols(doc: vscode.TextDocument): vscode.SymbolInformation[] {
    const symbols: vscode.SymbolInformation[] = [];
    for (let i=0;i<doc.lineCount;i++) {
      const line = doc.lineAt(i).text;
      let m: RegExpExecArray | null;
      if ((m = ENTITY_RE.exec(line))) symbols.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = ACTION_RE.exec(line))) symbols.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = FIELD_RE.exec(line))) symbols.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
      else if ((m = HOOK_RE.exec(line))) symbols.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event, '', new vscode.Location(doc.uri, new vscode.Position(i, 0))));
    }
    return symbols;
  }
}

class ErelangWorkspaceSymbolProvider implements vscode.WorkspaceSymbolProvider {
  async provideWorkspaceSymbols(query: string): Promise<vscode.SymbolInformation[]> {
    const uris = await vscode.workspace.findFiles('**/*.{0bs,ere,elan}');
    const out: vscode.SymbolInformation[] = [];
    for (const uri of uris) {
      const doc = await vscode.workspace.openTextDocument(uri);
      for (let i=0;i<doc.lineCount;i++) {
        const line = doc.lineAt(i).text;
        let m: RegExpExecArray | null;
        if ((m = ENTITY_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Class, '', new vscode.Location(uri, new vscode.Position(i,0))));
        else if ((m = ACTION_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Function, '', new vscode.Location(uri, new vscode.Position(i,0))));
        else if ((m = FIELD_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Field, '', new vscode.Location(uri, new vscode.Position(i,0))));
        else if ((m = HOOK_RE.exec(line)) && m[1].includes(query)) out.push(new vscode.SymbolInformation(m[1], vscode.SymbolKind.Event, '', new vscode.Location(uri, new vscode.Position(i,0))));
      }
    }
    return out;
  }
}

export function activate(context: vscode.ExtensionContext) {
  console.log('Erelang language extension active');
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider({ language: 'erelang' }, new ErelangCompletionProvider(), '.', ':')
  );
  context.subscriptions.push(
    vscode.languages.registerDocumentSymbolProvider({ language: 'erelang' }, new ErelangDocumentSymbolProvider())
  );
  context.subscriptions.push(
    vscode.languages.registerWorkspaceSymbolProvider(new ErelangWorkspaceSymbolProvider())
  );
}

export function deactivate() {}
