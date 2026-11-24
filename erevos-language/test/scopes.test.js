const test = require('node:test');
const assert = require('node:assert/strict');
const { renameEdits, findReferencesAt, resolveSymbol, splitLines } = require('../out/analysis');
const { visibleBindings } = require('../out/scopeAnalysis');

const src = ['action first(int value) {', '  int count = value;', '  if (true) {',
  '    string count = "inner";', '    print(count);', '  }', '  print(count);', '}',
  'action second() {', '  int count = 2;', '  print(count);', '}', 'run second;'].join('\n');

test('rename follows lexical identity across shadowing and sibling functions', () => {
  assert.deepEqual(renameEdits(src, { line: 1, character: 7 }, 'total').edits.map(e => e.range.start.line), [1, 6]);
  assert.deepEqual(renameEdits(src, { line: 4, character: 12 }, 'label').edits.map(e => e.range.start.line), [3, 4]);
});
test('hover chooses the visible declaration rather than the last matching spelling', () => {
  assert.equal(resolveSymbol(splitLines(src), { line: 4, character: 12 }).detail, 'string');
  assert.equal(resolveSymbol(splitLines(src), { line: 6, character: 10 }).detail, 'int');
});
test('references to parameters stay inside their function', () => {
  assert.deepEqual(findReferencesAt(src, { line: 0, character: 18 }).refs.map(r => r.line), [0, 1]);
});
test('comments, strings and member names cannot be renamed as locals', () => {
  const text = 'action main() { int count = 1; /* count {\n count } */ print(count); obj.count(); print("count"); }';
  const edits = renameEdits(text, { line: 0, character: 22 }, 'total').edits;
  assert.equal(edits.length, 2);
  assert.equal(renameEdits(text, { line: 1, character: 2 }, 'total'), null);
});
test('rename rejects keywords and binding capture', () => {
  assert.equal(renameEdits(src, { line: 1, character: 7 }, 'return'), null);
  assert.equal(renameEdits(src, { line: 1, character: 7 }, 'value'), null);
  const text = 'action main() { int x = 1; if (true) { int y = 2; print(x); } }';
  assert.equal(renameEdits(text, { line: 0, character: 20 }, 'y'), null);
});
test('completions exclude sibling locals, later declarations and ended blocks', () => {
  const names = visibleBindings(src, { line: 6, character: 2 }).map(b => b.token.text);
  assert.deepEqual(names.sort(), ['count', 'first', 'second', 'value']);
  assert.equal(visibleBindings(src, { line: 8, character: 18 }).some(b => b.token.text === 'count'), false);
});
test('multiline signatures bind their parameters to the body', () => {
  const text = 'action echo(\n  item: string\n): string {\n return item;\n}\naction other() { string item = "x"; }';
  assert.deepEqual(renameEdits(text, { line: 3, character: 9 }, 'message').edits.map(e => e.range.start.line), [1, 3]);
});

test('rename refuses to leave string interpolation pointing at an old name', () => {
  const text = 'action main() { int n = 1; print("value {n}"); }';
  assert.equal(renameEdits(text, { line: 0, character: 20 }, 'total'), null);
});

test('loop and closure parameters do not bind to unrelated outer declarations', () => {
  const text = 'action main() { int value = 1; any f = lambda(int input) { return input + value; }; }';
  const edits = renameEdits(text, { line: 0, character: text.indexOf('input') + 1 }, 'argument').edits;
  assert.equal(edits.length, 2);
});
