import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  Diagnostic,
  DiagnosticSeverity,
  TextDocumentPositionParams,
  Hover,
  MarkupKind,
  RenameParams,
  WorkspaceEdit,
  Location,
  ReferenceParams,
  TextEdit,
  Range,
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { spawn } from 'child_process';
import * as fs from 'fs';
import { fileURLToPath } from 'url';
import { resolveErelangExe } from './erelangPath';
import { parseCheckOutput } from './checkOutput';
import {
  splitLines,
  resolveSymbol,
  formatHoverMarkdown,
  findReferencesInText,
  renameEdits,
  collectDeclarations,
} from './analysis';

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let workspaceRoots: string[] = [];
let configuredExe = '';
/** undefined = not resolved yet, null = resolved but missing (cached so `where` is not re-spawned). */
let cachedExe: string | null | undefined;

function getExe(): string | undefined {
  if (cachedExe === undefined) cachedExe = resolveErelangExe(workspaceRoots, configuredExe) ?? null;
  return cachedExe ?? undefined;
}

connection.onInitialize((params: InitializeParams) => {
  workspaceRoots = (params.workspaceFolders ?? [])
    .map(f => {
      try {
        return decodeURIComponent(f.uri.replace(/^file:\/\//, '').replace(/^\/([A-Za-z]:)/, '$1'));
      } catch {
        return '';
      }
    })
    .filter(Boolean);
  if (!workspaceRoots.length && params.rootUri) {
    try {
      workspaceRoots = [decodeURIComponent(params.rootUri.replace(/^file:\/\//, '').replace(/^\/([A-Za-z]:)/, '$1'))];
    } catch { /* ignore */ }
  }
  const init = params.initializationOptions as { erelangPath?: string } | undefined;
  if (init?.erelangPath) configuredExe = init.erelangPath;
  return {
    capabilities: {
      textDocumentSync: {
        openClose: true,
        change: TextDocumentSyncKind.Incremental,
        save: { includeText: false },
      },
      // Completion is served by the client-side ErelangCompletionProvider only.
      hoverProvider: true,
      renameProvider: true,
      referencesProvider: true,
    },
  };
});

connection.onInitialized(() => {
  connection.workspace?.getConfiguration?.('erelang').then((cfg: { executablePath?: string }) => {
    if (cfg?.executablePath) configuredExe = cfg.executablePath;
    cachedExe = undefined;
  }).catch(() => undefined);
});

connection.onDidChangeConfiguration(change => {
  const cfg = (change.settings as { erelang?: { executablePath?: string } } | undefined)?.erelang;
  configuredExe = cfg?.executablePath ?? '';
  cachedExe = undefined;
});

/** Skip host --check on huge amalgamated sources (e.g. compiler/codegen.elan) — freezes Cursor. */
const MAX_CHECK_CHARS = 48_000;
const MAX_CHECK_LINES = 2_500;
const CHECK_DELAY_MS = 800;
const BURST_DELAY_MS = 1_500;
const BURST_CHANGES = 5;

const checkTimers = new Map<string, NodeJS.Timeout>();
const changeBursts = new Map<string, number>();
const lastPublished = new Map<string, string>();
/** Docs waiting for the single check slot, in arrival order. */
const checkQueue = new Set<string>();
let running: { uri: string; child: ReturnType<typeof spawn> } | undefined;

function shouldSkipCheck(doc: TextDocument): boolean {
  const uri = doc.uri.replace(/\\/g, '/').toLowerCase();
  if (!uri.startsWith('file:')) return true;
  if (uri.includes('/.firecrawl/') || uri.includes('/node_modules/') || uri.includes('/build/')) {
    return true;
  }
  if (doc.lineCount > MAX_CHECK_LINES) return true;
  if (doc.getText().length > MAX_CHECK_CHARS) return true;
  return false;
}

/** Publish only when the set actually changed — avoids Problems-panel churn during bulk edits. */
function publish(uri: string, diagnostics: Diagnostic[]) {
  const key = JSON.stringify(diagnostics);
  if (lastPublished.get(uri) === key) return;
  lastPublished.set(uri, key);
  connection.sendDiagnostics({ uri, diagnostics });
}

function cancelCheck(uri: string) {
  const timer = checkTimers.get(uri);
  if (timer) clearTimeout(timer);
  checkTimers.delete(uri);
  checkQueue.delete(uri);
  if (running?.uri === uri) {
    try { running.child.kill(); } catch { /* ignore */ }
  }
}

function scheduleCheck(uri: string, delayOverride?: number) {
  cancelCheck(uri);
  const doc = documents.get(uri);
  if (!doc) return;
  if (shouldSkipCheck(doc)) {
    publish(uri, []);
    return;
  }
  const burst = (changeBursts.get(uri) ?? 0) + 1;
  changeBursts.set(uri, burst);
  const delay = delayOverride ?? (burst > BURST_CHANGES ? BURST_DELAY_MS : CHECK_DELAY_MS);
  checkTimers.set(uri, setTimeout(() => {
    checkTimers.delete(uri);
    changeBursts.delete(uri);
    checkQueue.add(uri);
    pumpQueue();
  }, delay));
}

function pumpQueue() {
  if (running) return;
  const next = checkQueue.values().next();
  if (next.done) return;
  checkQueue.delete(next.value);
  void runCheck(next.value).finally(() => {
    running = undefined;
    pumpQueue();
  });
}

/**
 * Checks the file on disk, not a temp copy: includes resolve relative to the
 * script, and AI edits land on disk anyway. Unsaved buffers keep their last
 * diagnostics until saved.
 */
async function runCheck(uri: string) {
  const doc = documents.get(uri);
  if (!doc || shouldSkipCheck(doc)) return;
  const exe = getExe();
  if (!exe) {
    publish(uri, []);
    return;
  }
  let filePath: string;
  let onDisk: string;
  try {
    filePath = fileURLToPath(uri);
    onDisk = await fs.promises.readFile(filePath, 'utf8');
  } catch {
    return;
  }
  if (onDisk !== doc.getText()) return;

  const version = doc.version;
  try {
    const child = spawn(exe, ['--check', filePath], { windowsHide: true });
    running = { uri, child };
    let stderr = '';
    child.stderr.on('data', (d: Buffer) => { stderr += d.toString('utf8'); });
    child.stdout.on('data', () => undefined);
    const code: number | null = await new Promise(resolve => {
      child.on('close', (c) => resolve(c));
      child.on('error', () => resolve(-1));
    });
    // Killed (newer edit) or document moved on while checking: drop the result.
    if (code === null || documents.get(uri)?.version !== version) return;
    if (code === -1) throw new Error(`could not start ${exe}`);
    publish(uri, parseCheckOutput(stderr));
  } catch (e) {
    publish(uri, [{
      severity: DiagnosticSeverity.Error,
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
      message: `erelang --check failed: ${e instanceof Error ? e.message : String(e)}`,
      source: 'erelang',
    }]);
  }
}

documents.onDidChangeContent(e => scheduleCheck(e.document.uri));
documents.onDidSave(e => scheduleCheck(e.document.uri, 150));
documents.onDidClose(e => {
  const uri = e.document.uri;
  cancelCheck(uri);
  changeBursts.delete(uri);
  lastPublished.delete(uri);
  connection.sendDiagnostics({ uri, diagnostics: [] });
});

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const info = resolveSymbol(splitLines(doc.getText()), params.position);
  if (!info) return null;
  return {
    contents: { kind: MarkupKind.Markdown, value: formatHoverMarkdown(info) },
    range: info.range,
  };
});

connection.onReferences((params: ReferenceParams): Location[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const text = doc.getText();
  const sym = resolveSymbol(splitLines(text), params.position);
  if (!sym) return [];

  const locations: Location[] = [];
  const addFrom = (uri: string, src: string) => {
    for (const r of findReferencesInText(src, sym.name)) {
      locations.push({
        uri,
        range: {
          start: { line: r.line, character: r.start },
          end: { line: r.line, character: r.start + r.length },
        },
      });
    }
  };

  addFrom(doc.uri, text);

  if (sym.kind === 'action' || sym.kind === 'struct' || sym.kind === 'enum'
    || sym.kind === 'entity' || sym.kind === 'typeAlias' || sym.kind === 'namespace') {
    for (const other of documents.all()) {
      if (other.uri === doc.uri) continue;
      const decls = collectDeclarations(splitLines(other.getText()));
      if (!decls.some(d => d.name === sym.name && d.kind === sym.kind)) continue;
      addFrom(other.uri, other.getText());
    }
  }

  return locations;
});

connection.onRenameRequest((params: RenameParams): WorkspaceEdit | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const text = doc.getText();
  const result = renameEdits(text, params.position, params.newName);
  if (!result) return null;

  const changes: { [uri: string]: TextEdit[] } = {};
  const toEdits = (src: string): TextEdit[] => {
    return findReferencesInText(src, result.name).map(r => ({
      range: {
        start: { line: r.line, character: r.start },
        end: { line: r.line, character: r.start + r.length },
      } as Range,
      newText: params.newName,
    }));
  };

  changes[doc.uri] = toEdits(text);

  if (result.kind === 'action') {
    for (const other of documents.all()) {
      if (other.uri === doc.uri) continue;
      const decls = collectDeclarations(splitLines(other.getText()));
      if (!decls.some(d => d.name === result.name && d.kind === 'action')) continue;
      changes[other.uri] = toEdits(other.getText());
    }
  }

  return { changes };
});

documents.listen(connection);
connection.listen();
