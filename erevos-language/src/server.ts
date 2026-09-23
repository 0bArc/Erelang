import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  CompletionItem,
  CompletionItemKind,
  Diagnostic,
  DiagnosticSeverity,
  TextDocumentPositionParams,
  Hover,
  MarkupKind,
  CompletionParams,
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { spawn } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { resolveErelangExe } from './erelangPath';
import { LANGUAGE_KEYWORDS, BUILT_INS, ACTION_RE, TYPED_FUNC_RE, LET_RE, STRUCT_RE, ENUM_RE, ENTITY_RE } from './constants';

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let workspaceRoots: string[] = [];
let configuredExe = '';

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
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: { triggerCharacters: ['.', '#', '<', '"', '/'] },
      hoverProvider: true,
    },
  };
});

connection.onInitialized(() => {
  connection.workspace?.getConfiguration?.('erelang').then((cfg: { executablePath?: string }) => {
    if (cfg?.executablePath) configuredExe = cfg.executablePath;
  }).catch(() => undefined);
});

function uriToFsPath(uri: string): string {
  let p = uri.replace(/^file:\/\//, '');
  if (process.platform === 'win32') {
    p = decodeURIComponent(p.replace(/^\/([A-Za-z]:)/, '$1'));
  } else {
    p = decodeURIComponent(p);
  }
  return p;
}

function parseCheckOutput(stderr: string): Diagnostic[] {
  const out: Diagnostic[] = [];
  const re = /\[(error|warn|hint)\]\s+(\w+):\s+(.+?)(?:\s+\(([^)]*)\))?\s+at\s+(\d+):(\d+)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(stderr)) !== null) {
    const sev = m[1] === 'warn' ? DiagnosticSeverity.Warning
      : m[1] === 'hint' ? DiagnosticSeverity.Information
      : DiagnosticSeverity.Error;
    const line = Math.max(0, parseInt(m[5], 10) - 1);
    const col = Math.max(0, parseInt(m[6], 10) - 1);
    const msg = `${m[2]}: ${m[3]}${m[4] ? ` (${m[4]})` : ''}`;
    out.push({
      severity: sev,
      range: { start: { line, character: col }, end: { line, character: col + 1 } },
      message: msg,
      source: 'erelang',
    });
  }
  if (!out.length && stderr.trim()) {
    for (const line of stderr.split(/\r?\n/)) {
      const t = line.trim();
      if (!t) continue;
      out.push({
        severity: DiagnosticSeverity.Error,
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
        message: t,
        source: 'erelang',
      });
    }
  }
  return out;
}

const checkTimers = new Map<string, NodeJS.Timeout>();

function scheduleCheck(doc: TextDocument) {
  const key = doc.uri;
  const prev = checkTimers.get(key);
  if (prev) clearTimeout(prev);
  checkTimers.set(key, setTimeout(() => {
    checkTimers.delete(key);
    void runCheck(doc);
  }, 400));
}

async function runCheck(doc: TextDocument) {
  const exe = resolveErelangExe(workspaceRoots, configuredExe);
  if (!exe) {
    connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
    return;
  }
  const tmp = path.join(os.tmpdir(), `erelang-lsp-${process.pid}-${Date.now()}.elan`);
  try {
    fs.writeFileSync(tmp, doc.getText(), 'utf8');
    const child = spawn(exe, ['--check', tmp], { windowsHide: true });
    let stderr = '';
    child.stderr.on('data', (d: Buffer) => { stderr += d.toString('utf8'); });
    child.stdout.on('data', () => undefined);
    const code: number = await new Promise(resolve => {
      child.on('close', (c) => resolve(c ?? 1));
      child.on('error', () => resolve(1));
    });
    const diags = code === 0 ? [] : parseCheckOutput(stderr);
    connection.sendDiagnostics({ uri: doc.uri, diagnostics: diags });
  } catch (e) {
    connection.sendDiagnostics({
      uri: doc.uri,
      diagnostics: [{
        severity: DiagnosticSeverity.Error,
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
        message: `erelang --check failed: ${e instanceof Error ? e.message : String(e)}`,
        source: 'erelang',
      }],
    });
  } finally {
    try { fs.unlinkSync(tmp); } catch { /* ignore */ }
  }
}

documents.onDidChangeContent(e => scheduleCheck(e.document));
documents.onDidOpen(e => scheduleCheck(e.document));
documents.onDidClose(e => {
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

function collectNames(text: string): { actions: string[]; locals: string[]; types: string[] } {
  const actions: string[] = [];
  const locals: string[] = [];
  const types: string[] = [];
  for (const line of text.split(/\r?\n/)) {
    let m = ACTION_RE.exec(line) || TYPED_FUNC_RE.exec(line);
    if (m?.[1]) actions.push(m[1]);
    m = LET_RE.exec(line);
    if (m?.[1]) locals.push(m[1]);
    m = STRUCT_RE.exec(line) || ENUM_RE.exec(line) || ENTITY_RE.exec(line);
    if (m?.[1]) types.push(m[1]);
  }
  return { actions, locals, types };
}

function actionSignature(text: string, name: string): string | undefined {
  const re = new RegExp(
    String.raw`^\s*(?:public|private|export)?\s*(?:async\s+)?(?:action\s+)?(?:\w+\s+)?${name}\s*\(([^)]*)\)\s*(?::\s*([^{\n]+))?`,
    'm',
  );
  const m = re.exec(text);
  if (!m) return undefined;
  const ret = (m[2] || 'void').trim();
  return `${name}(${m[1].trim()}): ${ret}`;
}

connection.onCompletion((params: CompletionParams): CompletionItem[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const pos = params.position;
  const line = doc.getText({
    start: { line: pos.line, character: 0 },
    end: { line: pos.line, character: pos.character },
  });
  const names = collectNames(doc.getText());
  const items: CompletionItem[] = [];
  const seen = new Set<string>();
  const add = (label: string, kind: CompletionItemKind, detail?: string) => {
    if (seen.has(label)) return;
    seen.add(label);
    items.push({ label, kind, detail });
  };
  const member = /([A-Za-z_]\w*)\.([A-Za-z_]\w*)?$/.exec(line);
  if (member) {
    for (const a of names.actions) add(a, CompletionItemKind.Method);
    return items;
  }
  for (const k of LANGUAGE_KEYWORDS) add(k, CompletionItemKind.Keyword);
  for (const b of BUILT_INS) add(b, CompletionItemKind.Function, 'builtin');
  for (const a of names.actions) add(a, CompletionItemKind.Function, 'action');
  for (const t of names.types) add(t, CompletionItemKind.Class, 'type');
  for (const l of names.locals) add(l, CompletionItemKind.Variable, 'local');
  return items;
});

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const pos = params.position;
  const lineText = doc.getText({
    start: { line: pos.line, character: 0 },
    end: { line: pos.line + 1, character: 0 },
  });
  const line = lineText.replace(/\r?\n$/, '');
  let start = pos.character;
  let end = pos.character;
  while (start > 0 && /[A-Za-z0-9_]/.test(line[start - 1])) start--;
  while (end < line.length && /[A-Za-z0-9_]/.test(line[end])) end++;
  if (start === end) return null;
  const word = line.slice(start, end);
  const text = doc.getText();
  const sig = actionSignature(text, word);
  if (sig) {
    return { contents: { kind: MarkupKind.Markdown, value: '```erelang\n' + sig + '\n```' } };
  }
  if ((BUILT_INS as readonly string[]).includes(word)) {
    return { contents: { kind: MarkupKind.Markdown, value: `**builtin** \`${word}\`` } };
  }
  if (LANGUAGE_KEYWORDS.includes(word)) {
    return { contents: { kind: MarkupKind.Markdown, value: `**keyword** \`${word}\`` } };
  }
  return null;
});

documents.listen(connection);
connection.listen();
