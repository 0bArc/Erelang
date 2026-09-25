const assert = require('node:assert/strict');
const test = require('node:test');

const {
  splitLines,
  resolveSymbol,
  formatHoverMarkdown,
  findReferencesAt,
  renameEdits,
  findReferencesInText,
  collectDeclarations,
} = require('../out/analysis');

const SAMPLE = [
  'struct Vec2 {',
  '  x: int',
  '  y: int',
  '}',
  'enum Color { Red, Green }',
  'action paint(int amount, Vec2 pos): int {',
  '  int n = amount;',
  '  print("amount");',
  '  // amount comment',
  '  return n;',
  '}',
  'action main() {',
  '  paint(1, pos);',
  '}',
].join('\n');

test('hover resolves local with type from source', () => {
  const lines = splitLines(SAMPLE);
  const info = resolveSymbol(lines, { line: 6, character: 6 });
  assert.ok(info);
  assert.equal(info.name, 'n');
  assert.equal(info.kind, 'local');
  assert.equal(info.detail, 'int');
  const md = formatHoverMarkdown(info);
  assert.match(md, /\*\*local\*\*/);
  assert.match(md, /`n`/);
  assert.match(md, /`int`/);
});

test('hover resolves action signature from source', () => {
  const lines = splitLines(SAMPLE);
  const info = resolveSymbol(lines, { line: 5, character: 8 });
  assert.ok(info);
  assert.equal(info.name, 'paint');
  assert.equal(info.kind, 'action');
  assert.equal(info.detail, 'paint(int amount, Vec2 pos): int');
  const md = formatHoverMarkdown(info);
  assert.match(md, /paint\(int amount, Vec2 pos\): int/);
});

test('hover resolves parameter type from source', () => {
  const lines = splitLines(SAMPLE);
  const info = resolveSymbol(lines, { line: 5, character: 18 });
  assert.ok(info);
  assert.equal(info.name, 'amount');
  assert.equal(info.kind, 'parameter');
  assert.equal(info.detail, 'int');
});

test('hover resolves struct and field', () => {
  const lines = splitLines(SAMPLE);
  const st = resolveSymbol(lines, { line: 0, character: 8 });
  assert.ok(st);
  assert.equal(st.name, 'Vec2');
  assert.equal(st.kind, 'struct');
  const field = resolveSymbol(lines, { line: 1, character: 2 });
  assert.ok(field);
  assert.equal(field.name, 'x');
  assert.equal(field.kind, 'field');
  assert.equal(field.detail, 'int');
});

test('hover resolves enum', () => {
  const lines = splitLines(SAMPLE);
  const en = resolveSymbol(lines, { line: 4, character: 6 });
  assert.ok(en);
  assert.equal(en.name, 'Color');
  assert.equal(en.kind, 'enum');
});

test('hover does not invent types for bare identifiers', () => {
  const src = 'action main() {\n  foo();\n}';
  const info = resolveSymbol(splitLines(src), { line: 1, character: 2 });
  assert.equal(info, null);
});

test('find references skips strings and comments', () => {
  const hit = findReferencesAt(SAMPLE, { line: 5, character: 18 });
  assert.ok(hit);
  assert.equal(hit.name, 'amount');
  const lines = hit.refs.map(r => r.line);
  assert.ok(lines.includes(5));
  assert.ok(lines.includes(6));
  assert.ok(!lines.includes(7), 'string literal must not match');
  assert.ok(!lines.includes(8), 'comment must not match');
});

test('rename local renames word-boundary refs only', () => {
  const src = [
    'action main() {',
    '  int count = 1;',
    '  int count2 = count;',
    '  print("count");',
    '  return count;',
    '}',
  ].join('\n');
  const result = renameEdits(src, { line: 1, character: 6 }, 'total');
  assert.ok(result);
  assert.equal(result.name, 'count');
  assert.equal(result.kind, 'local');
  assert.equal(result.edits.length, 3);
  for (const e of result.edits) {
    assert.equal(e.newText, 'total');
  }
  const lines = result.edits.map(e => e.range.start.line).sort((a, b) => a - b);
  assert.deepEqual(lines, [1, 2, 4]);
});

test('rename action renames declaration and calls', () => {
  const result = renameEdits(SAMPLE, { line: 5, character: 8 }, 'draw');
  assert.ok(result);
  assert.equal(result.kind, 'action');
  const lines = result.edits.map(e => e.range.start.line).sort((a, b) => a - b);
  assert.deepEqual(lines, [5, 12]);
});

test('rename rejects keywords and invalid names', () => {
  assert.equal(renameEdits(SAMPLE, { line: 6, character: 6 }, '1bad'), null);
  const kwSrc = 'action main() {\n  return 1;\n}';
  assert.equal(renameEdits(kwSrc, { line: 1, character: 2 }, 'go'), null);
});

test('collectDeclarations finds params locals actions structs', () => {
  const decls = collectDeclarations(splitLines(SAMPLE));
  const kinds = Object.fromEntries(decls.map(d => [`${d.kind}:${d.name}`, d.detail ?? '']));
  assert.equal(kinds['struct:Vec2'], '');
  assert.equal(kinds['field:x'], 'int');
  assert.equal(kinds['action:paint'], 'paint(int amount, Vec2 pos): int');
  assert.equal(kinds['parameter:amount'], 'int');
  assert.equal(kinds['parameter:pos'], 'Vec2');
  assert.equal(kinds['local:n'], 'int');
});

test('findReferencesInText respects word boundaries', () => {
  const refs = findReferencesInText('int count = 1;\nint count2 = count;', 'count');
  assert.equal(refs.length, 2);
  assert.equal(refs[0].start, 4);
  assert.equal(refs[1].line, 1);
});
