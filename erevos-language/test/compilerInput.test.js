const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { useCompilerTokens, bindingIndex, tokenAt } = require('../out/scopeAnalysis');
const exe = process.env.ERELANG_TEST_EXE || path.resolve(__dirname, '../../build/debug/bin/Debug/erelang.exe');

test('compiler stdin overlay preserves relative imports without writing the root file', { skip: !fs.existsSync(exe) }, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'erelang-overlay-'));
  try {
    fs.writeFileSync(path.join(dir, 'helper.elan'), 'public action helper(): int { return 7; }');
    const root = path.join(dir, 'unsaved.elan');
    const input = '#include "helper.elan"\naction main() { print(helper()); }\nrun main;\n';
    const result = spawnSync(exe, ['--check-stdin', root], { input, encoding: 'utf8', timeout: 5000, windowsHide: true });
    assert.equal(result.status, 0, result.stderr);
    assert.equal(fs.existsSync(root), false);
    assert.match(result.stdout, /!token\t2\t1\taction/);
    assert.equal(result.stdout.split('\n').some(line => line === '7'), false, 'checking must not execute print');
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('compiler UTF-8 columns are converted to editor UTF-16 positions', { skip: !fs.existsSync(exe) }, () => {
  const input = 'action main() { print("é😀"); int count = 1; print(count); }\nrun main;';
  const result = spawnSync(exe, ['--check-stdin', path.resolve('unsaved.elan')], { input, encoding: 'utf8', timeout: 5000, windowsHide: true });
  assert.equal(result.status, 0, result.stderr);
  useCompilerTokens(input, result.stdout);
  const index = bindingIndex(input);
  const start = input.lastIndexOf('count');
  const binding = index.resolve(tokenAt(index, { line: 0, character: start }));
  assert.equal(binding.token.start, input.indexOf('count'));
});
