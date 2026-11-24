// Completion behavior: members come from the receiver's real type, module members only after an
// include, and the bundled manifest matches what erelang itself reports.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const Module = require('node:module');
const { execFileSync } = require('node:child_process');

class Position { constructor(line, character) { this.line = line; this.character = character; } }
class Range { constructor(a, b, c, d) { this.start = { line: a, character: b }; this.end = { line: c, character: d }; } }
class CompletionItem { constructor(label, kind) { this.label = label; this.kind = kind; } }
class SnippetString { constructor(value) { this.value = value; } }

const vscode = {
  Position, Range, CompletionItem, SnippetString,
  CompletionItemKind: new Proxy({}, { get: (_t, k) => String(k) }),
  CompletionTriggerKind: { Invoke: 0, TriggerCharacter: 1 },
  workspace: { workspaceFolders: [], getConfiguration: () => ({ get: (_k, d) => d }) },
  Uri: { file: p => ({ fsPath: p, toString: () => p }) },
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return vscode;
  return originalLoad.call(this, request, parent, isMain);
};

const { ErelangCompletionProvider, noteDocumentEdit, isDocumentBulkEditing } = require('../out/completions');
const { manifest, BUNDLED_MANIFEST_PATH } = require('../out/manifest');

let docCounter = 0;
function makeDoc(text) {
  const lines = text.split('\n');
  const uri = `file:///test/doc${++docCounter}.elan`;
  return {
    uri: { toString: () => uri, fsPath: `C:/test/doc${docCounter}.elan` },
    version: 1,
    languageId: 'erelang',
    lineCount: lines.length,
    isClosed: false,
    lineAt: i => ({ text: lines[i] }),
    getText: () => text,
  };
}

// `|` marks the cursor.
async function complete(textWithCursor) {
  const lines = textWithCursor.split('\n');
  const line = lines.findIndex(l => l.includes('|'));
  const character = lines[line].indexOf('|');
  lines[line] = lines[line].replace('|', '');
  const doc = makeDoc(lines.join('\n'));
  const provider = new ErelangCompletionProvider();
  const token = { isCancellationRequested: false };
  const items = await provider.provideCompletionItems(doc, new Position(line, character), token,
    { triggerKind: 0 });
  return (items ?? []).map(i => (typeof i.label === 'string' ? i.label : i.label.label));
}

test('manifest is bundled and has modules', () => {
  assert.ok(fs.existsSync(BUNDLED_MANIFEST_PATH), 'resources/builtins.json missing; run npm run gen');
  assert.ok(manifest().modules.length > 10);
  assert.ok(manifest().modules.some(m => m.id === 'ai'));
});

test('bulk edits pause only the edited document', async () => {
  const edited = makeDoc('action main() {}');
  const other = makeDoc('string s = "hello";\ns.');
  noteDocumentEdit({ document: edited, contentChanges: [{ text: 'x'.repeat(500), rangeLength: 0 }] });
  assert.equal(isDocumentBulkEditing(edited), true);
  assert.equal(isDocumentBulkEditing(other), false);
  const provider = new ErelangCompletionProvider();
  const items = await provider.provideCompletionItems(other, new Position(1, 2),
    { isCancellationRequested: false }, { triggerKind: 0 });
  assert.ok(items.some(item => item.label === 'split'));
});

test('completion skips oversized lines before running contextual regexes', async () => {
  const doc = makeDoc('x'.repeat(30_000));
  const items = await new ErelangCompletionProvider().provideCompletionItems(doc, new Position(0, 30_000),
    { isCancellationRequested: false }, { triggerKind: 0 });
  assert.deepEqual(items, []);
});

test('member of an undeclared variable offers nothing', async () => {
  const items = await complete('public action main() {\n    x.|\n}');
  assert.deepEqual(items, []);
});

test('member of a typed array offers array methods only', async () => {
  const items = await complete('public action main() {\n    Array<int> xs = [1, 2];\n    xs.|\n}');
  assert.ok(items.includes('push'));
  assert.ok(items.includes('len'));
  assert.ok(!items.includes('keys'), 'map method leaked into array members');
});

test('member of a typed map offers map methods', async () => {
  const items = await complete('public action main() {\n    Map<string, int> m = {"a": 1};\n    m.|\n}');
  assert.ok(items.includes('keys'));
  assert.ok(items.includes('getOr'));
  assert.ok(!items.includes('push'));
});

test('member of a string offers string methods', async () => {
  const items = await complete('public action main() {\n    string s = "hi";\n    s.|\n}');
  assert.ok(items.includes('split'));
  assert.ok(items.includes('upper'));
  assert.ok(!items.includes('push'));
});

test('parameters are typed too', async () => {
  const items = await complete('public action f(names: Array<string>) {\n    names.|\n}');
  assert.ok(items.includes('join'));
});

test('handle from ws.connect offers socket methods', async () => {
  const src = '#include <builtin/websocket> as ws\npublic action main() {\n    string sock = ws.connect("ws://x");\n    sock.|\n}';
  const items = await complete(src);
  assert.ok(items.includes('recv_timeout'));
  assert.ok(items.includes('close_code'));
});

test('module members need the include', async () => {
  const without = await complete('public action main() {\n    re|\n}');
  assert.ok(!without.some(n => n.startsWith('re.')), `unexpected: ${without.filter(n => n.startsWith('re.'))}`);
  const membersWithout = await complete('public action main() {\n    re.|\n}');
  assert.deepEqual(membersWithout, []);

  const withInclude = await complete('#include <builtin/regex> as re\npublic action main() {\n    re.|\n}');
  assert.ok(withInclude.includes('expect'));
  assert.ok(withInclude.includes('match'));
  assert.ok(!withInclude.includes('regex_match'), 'hidden legacy name offered');
});

test('alias and its snippets appear once imported', async () => {
  const items = await complete('#include <builtin/regex> as re\npublic action main() {\n    re|\n}');
  assert.ok(items.includes('re'));
  assert.ok(items.includes('re.expect'), 'module snippet missing');
});

test('grouped ai members', async () => {
  const items = await complete('#include <builtin/ai> as ai\npublic action main() {\n    ai.regex.|\n}');
  assert.ok(items.includes('generate'));
  assert.ok(items.includes('explain'));
  assert.ok(!items.includes('kind'));
});

test('include paths come from the manifest', async () => {
  const items = await complete('#include <builtin/a|');
  assert.ok(items.includes('builtin/ai'));
});

test('bundled manifest matches erelang --dump-builtins', t => {
  const root = path.resolve(__dirname, '..', '..');
  const exe = [
    process.env.ERELANG_TEST_EXE,
    path.join(root, 'build', 'release', 'bin', 'Release', process.platform === 'win32' ? 'erelang.exe' : 'erelang'),
    path.join(root, 'build', 'debug', 'bin', 'Debug', process.platform === 'win32' ? 'erelang.exe' : 'erelang'),
  ].find(p => p && fs.existsSync(p));
  if (!exe) { t.skip('no erelang build'); return; }
  const live = JSON.parse(execFileSync(exe, ['--dump-builtins', '--json'], { encoding: 'utf8' }));
  const bundled = JSON.parse(fs.readFileSync(BUNDLED_MANIFEST_PATH, 'utf8'));
  assert.deepEqual(bundled, live, 'resources/builtins.json is stale: run npm run gen');
});
