const assert = require('node:assert/strict');
const test = require('node:test');
const { projectDefinition } = require('../out/projectAnalysis');

const { pathToFileURL } = require('node:url');
const path = require('node:path');
const fileUri = name => pathToFileURL(path.resolve('project', name)).href;
const uri = fileUri('main.elan');
const sources = new Map([
  [fileUri('helper.elan'), 'public action greet(int value) { print value; }'],
  [fileUri('other.elan'), 'public action greet() {}'],
]);
const read = async uri => sources.get(uri);
test('definition follows a direct import, including a closed source file', async () => {
  const text = '#include "helper.elan" as helper\naction main() { helper.greet(1); }';
  const def = await projectDefinition(uri, text, { line: 1, character: text.split('\n')[1].indexOf('greet') + 1 }, read);
  assert.equal(def.uri, fileUri('helper.elan'));
  assert.equal(def.binding.token.text, 'greet');
});
test('unrelated files and a locally bound receiver never supply imported definitions', async () => {
  assert.equal(await projectDefinition(uri, 'action main() { greet(); }', { line: 0, character: 17 }, read), undefined);
  const text = '#include "helper.elan" as helper\naction main() { string helper = "x"; helper.greet(); }';
  assert.equal(await projectDefinition(uri, text, { line: 1, character: text.split('\n')[1].indexOf('greet') + 1 }, read), undefined);
});
test('an unsaved source supplied by the reader takes precedence', async () => {
  const text = '#include "helper.elan"\naction main() { newer(); }';
  const def = await projectDefinition(uri, text, { line: 1, character: text.split('\n')[1].indexOf('newer') + 1 }, async () => 'action newer() {}');
  assert.equal(def.binding.token.text, 'newer');
});
