import * as vscode from 'vscode';

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
  'print','PRINT','sleep','now_ms','now_iso','env','username','computer_name','machine_guid','uuid','rand_int','hwid','args_count','args_get',
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
    const memberAccess = /([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)?$/.exec(linePrefix);
    if (memberAccess) {
      const objectName = memberAccess[1];
      const partialName = memberAccess[2] ?? '';
      const aliases = collectModuleAliases(doc);
      const methods = aliases.get(objectName) ?? [];
      return methods
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
