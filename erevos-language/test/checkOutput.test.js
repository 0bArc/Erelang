const assert = require('node:assert/strict');
const test = require('node:test');

const { parseCheckOutput } = require('../out/checkOutput');

const ERROR = 1;
const WARNING = 2;
const INFO = 3;

test('positioned error keeps line/col (1-based -> 0-based)', () => {
  const [d] = parseCheckOutput('[error] TC001: Unknown action or builtin: foo (main) at 3:5\n');
  assert.equal(d.severity, ERROR);
  assert.deepEqual(d.range.start, { line: 2, character: 4 });
  assert.equal(d.message, 'TC001: Unknown action or builtin: foo (main)');
});

test('position-less warning stays a warning', () => {
  const [d] = parseCheckOutput('[warn] TC120: Unused variable: p (main)');
  assert.equal(d.severity, WARNING);
  assert.deepEqual(d.range.start, { line: 0, character: 0 });
});

test('hint and TC111 map to information', () => {
  const ds = parseCheckOutput([
    '[hint] TC001: Check spelling (is_tok)',
    '[error] TC111: No run target set (expected action main or run directive) (program)',
  ].join('\r\n'));
  assert.deepEqual(ds.map(d => d.severity), [INFO, INFO]);
});

test('untagged loader error becomes an error', () => {
  const ds = parseCheckOutput('Check error: cannot open include "x.elan"\n\n');
  assert.equal(ds.length, 1);
  assert.equal(ds[0].severity, ERROR);
  assert.equal(ds[0].message, 'Check error: cannot open include "x.elan"');
});

test('missing-semicolon messages from old compilers are not editor diagnostics', () => {
  for (const message of ['Expected ;', "Expected ';'", 'Expected ; or a new line', 'Missing semicolon (;)',
    'Check error: Expected ;', 'Check error: Syntax errors (1):\n  - <input>:3:5: Expected ; (near print)',
    'Check error: Syntax errors (1):\n  - <input>:3:5: ; (near print)']) {
    assert.deepEqual(parseCheckOutput(message), [], message);
  }
});

test('filtering a semicolon error preserves other syntax and type errors', () => {
  const ds = parseCheckOutput('Check error: Syntax errors (2):\n'
    + '  - C:\\project\\main.elan:3:5: Expected ; or a new line\n'
    + '  - <input>:4:2: Expected expression near token: \';\'\n'
    + '[error] TC001: Unknown action: nope (main) at 7:3');
  assert.equal(ds.length, 2);
  assert.match(ds[0].message, /Expected expression/);
  assert.equal(ds[1].code, 'TC001');
  assert.equal(parseCheckOutput('Check error: cannot open include "Expected ;"').length, 1);
});
