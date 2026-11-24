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
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { CheckQueue } from './checkQueue';
import { checkProcess } from './checkProcess';
import { bindingIndex, tokenAt, useCompilerTokens } from './scopeAnalysis';
import { fileURLToPath } from 'url';
import * as fs from 'fs';
import { projectDefinition, definitionLocation, tokenRange, sourceKey } from './projectAnalysis';
import { resolveErelangExe } from './erelangPath';
import { parseCheckOutput } from './checkOutput';
import {
  splitLines,
  resolveSymbol,
  formatHoverMarkdown,
  renameEdits,
} from './analysis';

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let workspaceRoots: string[] = [];
let configuredExe = '';
let supportsProjectFiles = false;
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
        return fileURLToPath(f.uri);
      } catch {
        return '';
      }
    })
    .filter(Boolean);
  if (!workspaceRoots.length && params.rootUri) {
    try {
      workspaceRoots = [fileURLToPath(params.rootUri)];
    } catch { /* ignore */ }
  }
  const init = params.initializationOptions as { erelangPath?: string; projectFiles?: boolean } | undefined;
  supportsProjectFiles = init?.projectFiles === true;
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
      definitionProvider: true,
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
const checkQueue = new CheckQueue(runCheck, error => connection.console.error(String(error)));

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
  checkQueue.cancel(uri);
}

function scheduleCheck(uri: string) {
  cancelCheck(uri);
  const doc = documents.get(uri);
  if (!doc) return;
  if (shouldSkipCheck(doc)) {
    publish(uri, []);
    return;
  }
  const burst = (changeBursts.get(uri) ?? 0) + 1;
  changeBursts.set(uri, burst);
  const delay = burst > BURST_CHANGES ? BURST_DELAY_MS : CHECK_DELAY_MS;
  checkTimers.set(uri, setTimeout(() => {
    checkTimers.delete(uri);
    changeBursts.delete(uri);
    checkQueue.enqueue(uri);
  }, delay));
}

/**
 * Checks an in-memory source overlay through the compiler. The original path
 * preserves include resolution and the file on disk is never rewritten.
 */
async function runCheck(uri: string, signal: AbortSignal) {
  const doc = documents.get(uri);
  if (signal.aborted || !doc || shouldSkipCheck(doc)) return;
  const version = doc.version;
  const isCurrent = () => !signal.aborted && documents.get(uri) === doc && doc.version === version;
  const exe = getExe();
  if (!exe) {
    publish(uri, []);
    return;
  }
  let filePath: string;
  try {
    filePath = fileURLToPath(uri);
  } catch {
    return;
  }
  if (!isCurrent()) return;

  try {
    const stderr = await checkProcess(exe, ['--check-stdin', filePath], signal,
      10_000, 1024 * 1024, doc.getText(), output => {
        if (!isCurrent()) return;
        useCompilerTokens(doc.getText(), output);
        void connection.sendNotification('erelang/compilerTokens', { uri, version, output });
      });
    if (stderr === undefined || !isCurrent()) return;
    publish(uri, parseCheckOutput(stderr));
  } catch (e) {
    if (!isCurrent()) return;
    publish(uri, [{
      severity: DiagnosticSeverity.Error,
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
      message: `erelang --check failed: ${e instanceof Error ? e.message : String(e)}`,
      source: 'erelang',
    }]);
  }
}

documents.onDidChangeContent(e => scheduleCheck(e.document.uri));
// Saves participate in the same debounce: AI/autosave bursts must not bypass it.
documents.onDidSave(e => scheduleCheck(e.document.uri));
documents.onDidClose(e => {
  const uri = e.document.uri;
  cancelCheck(uri);
  changeBursts.delete(uri);
  lastPublished.delete(uri);
  connection.sendDiagnostics({ uri, diagnostics: [] });
});

connection.onShutdown(() => {
  for (const timer of checkTimers.values()) clearTimeout(timer);
  checkTimers.clear();
  checkQueue.dispose();
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

const readSource = async (uri: string): Promise<string | undefined> => {
  const open = documents.get(uri) ?? documents.all().find(d => sourceKey(d.uri) === sourceKey(uri));
  if (open) return shouldSkipCheck(open) ? undefined : open.getText();
  try {
    const file = fileURLToPath(uri);
    if ((await fs.promises.stat(file)).size > MAX_CHECK_CHARS) return undefined;
    return await fs.promises.readFile(file, 'utf8');
  } catch { return undefined; }
};

const resolveDefinition: typeof projectDefinition = async (uri, text, pos, read, visited) => {
  const def = await projectDefinition(uri, text, pos, read, visited);
  if (def) def.uri = documents.all().find(d => sourceKey(d.uri) === sourceKey(def.uri))?.uri ?? def.uri;
  return def;
};

connection.onDefinition(async params => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || shouldSkipCheck(doc)) return null;
  const def = await resolveDefinition(doc.uri, doc.getText(), params.position, readSource);
  return def ? definitionLocation(def) : null;
});

async function projectReferences(def: Awaited<ReturnType<typeof projectDefinition>>, includeDeclaration: boolean,
  cancelled: () => boolean = () => false): Promise<Location[]> {
  if (!def) return [];
  const locations: Location[] = [];
  const sources = new Map(documents.all().filter(d => !shouldSkipCheck(d)).map(d => [d.uri, d.getText()]));
  if (supportsProjectFiles && def.binding.kind === 'action') {
    const files = await connection.sendRequest<{ uris: string[]; complete: boolean }>('erelang/projectFiles');
    if (!files.complete) throw new Error('Project exceeds 2000 Erelang files; narrow the workspace before renaming.');
    let bytes = 0;
    // Explicit navigation requests may index closed files. Never do this on document edits.
    for (const uri of files.uris) {
      if (cancelled()) return [];
      if ([...sources.keys()].some(key => sourceKey(key) === sourceKey(uri))) continue;
      const source = await readSource(uri);
      if (source === undefined) throw new Error('Cannot safely index project file: ' + uri);
      bytes += source.length;
      if (bytes > 8_000_000) throw new Error('Project analysis exceeds 8 MB; narrow the workspace before renaming.');
      sources.set(uri, source);
    }
  }
  const targetSource = await readSource(def.uri);
  if (targetSource !== undefined && ![...sources.keys()].some(key => sourceKey(key) === sourceKey(def.uri))) sources.set(def.uri, targetSource);
  const reads = new Map<string, Promise<string | undefined>>();
  const read = (uri: string) => {
    if (!reads.has(uri)) reads.set(uri, sources.has(uri) ? Promise.resolve(sources.get(uri)) : readSource(uri));
    return reads.get(uri)!;
  };
  for (const [uri, text] of sources) {
    if (cancelled()) return [];
    for (const t of bindingIndex(text).tokens) {
      if (cancelled()) return [];
      if (t.text !== def.binding.token.text) continue;
      const resolved = await resolveDefinition(uri, text, { line: t.line, character: t.start }, read);
      if (!resolved || sourceKey(resolved.uri) !== sourceKey(def.uri) || resolved.binding.token.line !== def.binding.token.line
        || resolved.binding.token.start !== def.binding.token.start) continue;
      if (!includeDeclaration && sourceKey(uri) === sourceKey(def.uri) && t.line === def.binding.token.line
        && t.start === def.binding.token.start) continue;
      locations.push({ uri, range: tokenRange(t) });
    }
  }
  return locations;
}

connection.onReferences(async (params: ReferenceParams, token): Promise<Location[]> => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || shouldSkipCheck(doc)) return [];
  const def = await resolveDefinition(doc.uri, doc.getText(), params.position, readSource);
  return projectReferences(def, params.context.includeDeclaration, () => token.isCancellationRequested);
});

connection.onRenameRequest(async (params: RenameParams, token): Promise<WorkspaceEdit | null> => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || shouldSkipCheck(doc)) return null;
  const snapshots = new Map(documents.all().map(d => [d.uri, { version: d.version, text: d.getText() }]));
  const def = await resolveDefinition(doc.uri, doc.getText(), params.position, readSource);
  if (!def) return null;
  const source = await readSource(def.uri);
  if (source === undefined) return null;
  const result = renameEdits(source, { line: def.binding.token.line, character: def.binding.token.start }, params.newName);
  if (!result) return null;
  const refs = await projectReferences(def, true, () => token.isCancellationRequested);
  if (token.isCancellationRequested) return null;
  const edits = new Map<string, TextEdit[]>();
  for (const ref of refs) {
    const text = ref.uri === def.uri ? source : snapshots.get(ref.uri)?.text ?? await readSource(ref.uri);
    if (text === undefined) return null;
    if (ref.uri !== def.uri && bindingIndex(text).bindings.some(b => b.token.text === params.newName)) return null;
    if (!edits.has(ref.uri)) edits.set(ref.uri, []);
    edits.get(ref.uri)!.push({ range: ref.range, newText: params.newName });
  }
  if ([...snapshots].some(([uri, snapshot]) => documents.get(uri)?.version !== snapshot.version)) return null;
  return { documentChanges: [...edits].map(([uri, changes]) => ({
    textDocument: { uri, version: snapshots.get(uri)?.version ?? null }, edits: changes,
  })) };
});

documents.listen(connection);
connection.listen();
