const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { spawn } = require('node:child_process');
const { createMessageConnection, StreamMessageReader, StreamMessageWriter } = require('vscode-jsonrpc/node');

const exe = process.env.ERELANG_TEST_EXE || path.resolve(__dirname, '../../build/debug/bin/Debug/erelang.exe');

test('LSP diagnoses unsaved buffers after an edit/save burst and clears on close',
  { skip: !fs.existsSync(exe), timeout: 15_000 }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'erelang-lsp-'));
    const file = path.join(dir, 'main.elan');
    const uri = pathToFileURL(file).href.replace(/^file:\/\/\/([A-Za-z]):/, (_, drive) => `file:///${drive.toLowerCase()}%3A`);
    const caller = path.join(dir, 'caller.elan');
    const callerUri = pathToFileURL(caller).href;
    fs.writeFileSync(caller, '#include "main.elan" as main\naction caller() { main.main(); }\nrun caller;');
    const child = spawn(process.execPath, [path.resolve(__dirname, '../out/server.js'), '--stdio'],
      { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    const connection = createMessageConnection(new StreamMessageReader(child.stdout), new StreamMessageWriter(child.stdin));
    const notifications = [];
    let waiter;
    connection.onRequest('workspace/configuration', () => [{ executablePath: exe }]);
    connection.onRequest('erelang/projectFiles', () => ({ uris: [uri, callerUri], complete: true }));
    connection.onNotification('textDocument/publishDiagnostics', value => {
      notifications.push(value);
      waiter?.(value);
    });
    const nextDiagnostics = () => new Promise((resolve, reject) => {
      const timeout = setTimeout(() => { waiter = undefined; reject(new Error('No LSP diagnostics')); }, 6000);
      waiter = value => { clearTimeout(timeout); waiter = undefined; resolve(value); };
    });
    child.stderr.on('data', () => {});
    connection.listen();
    try {
      const init = await connection.sendRequest('initialize', {
        processId: process.pid, capabilities: {}, rootUri: pathToFileURL(dir).href,
        initializationOptions: { erelangPath: exe, projectFiles: true },
      });
      assert.equal(init.capabilities.textDocumentSync.change, 2);
      await connection.sendNotification('initialized', {});
      const invalid = 'action main() {\n  missing_function();\n}\nrun main;\n';
      // The disk contains a valid program; diagnostics must use the unsaved buffer.
      fs.writeFileSync(file, 'action main() { print("disk"); }\nrun main;\n');
      let next = nextDiagnostics();
      await connection.sendNotification('textDocument/didOpen', {
        textDocument: { uri, languageId: 'erelang', version: 1, text: invalid },
      });
      assert.ok((await next).diagnostics.some(d => d.severity === 1));

      const valid = 'action main() {\n  int n = 1;\n  print(n);\n}\naction other() { int n = 2; print(n); }\nrun main;\n';
      next = nextDiagnostics();
      for (let version = 2; version <= 9; version++) {
        const text = version === 9 ? valid : invalid;
        await connection.sendNotification('textDocument/didChange', {
          textDocument: { uri, version }, contentChanges: [{ text }],
        });
        await connection.sendNotification('textDocument/didSave', { textDocument: { uri } });
      }
      assert.ok((await next).diagnostics.every(d => d.severity !== 1));
      assert.equal(notifications.length, 2, 'intermediate versions must not churn diagnostics');
      const definition = await connection.sendRequest('textDocument/definition', {
        textDocument: { uri }, position: { line: 2, character: 8 },
      });
      assert.equal(definition.range.start.line, 1);
      const renamed = await connection.sendRequest('textDocument/rename', {
        textDocument: { uri }, position: { line: 2, character: 8 }, newName: 'total',
      });
      assert.equal(renamed.documentChanges[0].textDocument.version, 9);
      assert.deepEqual(renamed.documentChanges[0].edits.map(e => e.range.start.line), [1, 2]);
      const actionRename = await connection.sendRequest('textDocument/rename', {
        textDocument: { uri }, position: { line: 0, character: 9 }, newName: 'launch',
      });
      assert.equal(actionRename.documentChanges.length, 2);
      const callerEdits = actionRename.documentChanges.find(change => change.textDocument.uri === callerUri);
      assert.equal(callerEdits.textDocument.version, null);
      assert.equal(callerEdits.edits.length, 1, 'rename call target, not the module alias');
      next = nextDiagnostics();
      await connection.sendNotification('textDocument/didClose', { textDocument: { uri } });
      assert.deepEqual((await next).diagnostics, []);
      await connection.sendRequest('shutdown');
      await connection.sendNotification('exit');
    } finally {
      connection.dispose();
      child.kill();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
