const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const Module = require('node:module');

class SemanticTokensBuilder {
  constructor(legend) {
    this.legend = legend;
    this.items = [];
  }
  push(line, start, length, type, modifiers) {
    this.items.push({ line, start, length, type, modifiers });
  }
  build() {
    return { items: this.items };
  }
}

const vscode = {
  Diagnostic: class {
    constructor(range, message, severity) {
      this.range = range;
      this.message = message;
      this.severity = severity;
    }
  },
  DiagnosticSeverity: { Error: 0 },
  Range: class {
    constructor(startLine, startCharacter, endLine, endCharacter) {
      this.start = { line: startLine, character: startCharacter };
      this.end = { line: endLine, character: endCharacter };
    }
  },
  SemanticTokensLegend: class {
    constructor(tokenTypes, tokenModifiers) {
      this.tokenTypes = tokenTypes;
      this.tokenModifiers = tokenModifiers;
    }
  },
  SemanticTokensBuilder,
  workspace: {
    workspaceFolders: [],
  },
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return vscode;
  return originalLoad.call(this, request, parent, isMain);
};

const { collect, collectEntityInstances, collectEntityMembers } = require('../out/symbols');
const { validateDocument } = require('../out/diagnostics');
const { extractActions } = require('../out/imports');
const {
  classifyDocumentTokens,
  ErelangSemanticTokensProvider,
  SemanticTokenType: T,
  SemanticTokenModifier: M,
} = require('../out/semantic-tokens');

function document(lines, version = 1) {
  return {
    isClosed: false,
    languageId: 'erelang',
    lineCount: lines.length,
    uri: { fsPath: 'test.elan', toString: () => 'file:///test.elan' },
    version,
    lineAt: index => ({ text: lines[index] }),
  };
}

function hasToken(tokens, text, lines, type, modsMask = 0) {
  return tokens.some(tok => {
    if (tok.type !== type) return false;
    if (modsMask && (tok.modifiers & modsMask) !== modsMask) return false;
    const slice = lines[tok.line].slice(tok.start, tok.start + tok.length);
    return slice === text;
  });
}

test('entity instance Counter c exposes init and bump', () => {
  const lines = [
    'public entity Counter {',
    '  public action init(v: int) {}',
    '  public int bump(int step) { return 0; }',
    '}',
    'Counter c = new Counter();',
  ];
  const doc = document(lines);
  assert.equal(collectEntityInstances(doc).get('c'), 'Counter');
  const members = collectEntityMembers(doc).actions.get('Counter');
  assert(members && members.has('init'));
  assert(members && members.has('bump'));
});

test('symbol cache follows document versions', () => {
  const lines = ['public action first() {', '}'];
  const doc = document(lines);
  assert(collect(doc).actions.has('first'));

  lines[0] = 'public action second() {';
  doc.version = 2;
  const symbols = collect(doc);
  assert(symbols.actions.has('second'));
  assert(!symbols.actions.has('first'));
});

test('large documents skip heuristic diagnostics', () => {
  const doc = document(new Array(50_001).fill('int value = 1'));
  let diagnostics;
  const collection = {
    delete() {},
    set(_uri, value) { diagnostics = value; },
  };
  validateDocument(doc, collection);
  assert.deepEqual(diagnostics, []);
});

test('imported action cache tracks file modification time', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'erevos-'));
  const file = path.join(dir, 'module.elan');
  try {
    fs.writeFileSync(file, 'public action first() {}');
    assert((await extractActions(file)).has('first'));
    await new Promise(resolve => setTimeout(resolve, 20));
    fs.writeFileSync(file, 'public action second() {}');
    const actions = await extractActions(file);
    assert(actions.has('second'));
    assert(!actions.has('first'));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('semantic tokens classify declarations calls methods and literals', () => {
  const lines = [
    'struct Vec2 {',
    '  field x',
    '}',
    'enum Color { Red, Green }',
    'action paint(int amount, Vec2 pos) {',
    '  int n = 42;',
    '  pos.set(1, 2);',
    '  double len = pos.length_sq();',
    '  print("hi {n}");',
    '  return true;',
    '}',
  ];
  const tokens = classifyDocumentTokens(document(lines));

  assert(hasToken(tokens, 'Vec2', lines, T.class, M.declaration));
  assert(hasToken(tokens, 'Color', lines, T.class, M.declaration));
  assert(hasToken(tokens, 'Red', lines, T.enumMember, M.declaration));
  assert(hasToken(tokens, 'paint', lines, T.function, M.declaration));
  assert(hasToken(tokens, 'amount', lines, T.parameter, M.declaration));
  assert(hasToken(tokens, 'pos', lines, T.parameter, M.declaration));
  assert(hasToken(tokens, 'x', lines, T.property, M.declaration));
  assert(hasToken(tokens, 'n', lines, T.variable, M.declaration));
  assert(hasToken(tokens, 'set', lines, T.method));
  assert(hasToken(tokens, 'length_sq', lines, T.method));
  assert(hasToken(tokens, 'print', lines, T.function, M.defaultLibrary));
  // Literals stay on TextMate — semantic path skips strings/numbers/keywords for speed.
  assert(!tokens.some(tok => tok.type === T.number || tok.type === T.string || tok.type === T.keyword));
});

test('scoped enum access Mode::Walk is typed and enum-colored', () => {
  const lines = [
    'enum Mode { Walk, Run }',
    'action main {',
    '  test_switch(Mode::Walk);',
    '  test_switch(Mode.Run);',
    '}',
  ];
  const tokens = classifyDocumentTokens(document(lines));
  assert(hasToken(tokens, 'Mode', lines, T.class));
  assert(hasToken(tokens, 'Walk', lines, T.enumMember));
  assert(hasToken(tokens, 'Run', lines, T.enumMember));
});

test('semantic provider stays idle so the editor does not freeze', async () => {
  const lines = ['action hello() {', '  print(1);', '}'];
  const doc = document(lines, 3);
  const provider = new ErelangSemanticTokensProvider();
  const first = await provider.provideDocumentSemanticTokens(doc, { isCancellationRequested: false });
  const second = await provider.provideDocumentSemanticTokens(doc, { isCancellationRequested: false });
  assert.equal(first, second);
  assert.deepEqual(first.items ?? first, second.items ?? second);
});

test('very large files skip semantic classification', () => {
  const { MAX_SEMANTIC_LINES } = require('../out/semantic-tokens');
  const doc = document(new Array(MAX_SEMANTIC_LINES + 1).fill('int value = 1'));
  const tokens = classifyDocumentTokens(doc);
  assert.equal(tokens.length, 0);
});

test('textmate grammar keeps method and property scopes distinct', () => {
  const grammar = JSON.parse(
    fs.readFileSync(path.join(__dirname, '../syntaxes/erevos.tmLanguage.json'), 'utf8'),
  );
  const patterns = grammar.repository.memberAccess.patterns;
  assert.equal(patterns[0].captures['2'].name, 'entity.name.function.erelang');
  assert.equal(patterns[1].captures['2'].name, 'variable.other.constant.erelang');
  assert.equal(patterns[2].captures['2'].name, 'variable.other.property.erelang');
  assert.equal(
    grammar.repository.scopeAccess.patterns[0].captures['3'].name,
    'variable.other.constant.erelang',
  );
  assert.equal(
    grammar.repository.memberAccess.patterns[1].captures['2'].name,
    'variable.other.constant.erelang',
  );
  assert.ok(grammar.repository.strings.patterns[0].patterns.some(
    p => p.name === 'meta.interpolation.erelang',
  ));
  assert.ok(grammar.repository.foreachHeader.patterns[0].match);
  assert.ok(!grammar.repository.foreachHeader.patterns[0].begin);
});
