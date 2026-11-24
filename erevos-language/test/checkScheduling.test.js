const assert = require('node:assert/strict');
const test = require('node:test');
const { CheckQueue } = require('../out/checkQueue');
const { checkProcess } = require('../out/checkProcess');

const tick = () => new Promise(resolve => setImmediate(resolve));
function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

test('check slot covers asynchronous preparation and deduplicates waiting files', async () => {
  const gate = deferred();
  const started = [];
  let active = 0;
  let peak = 0;
  const queue = new CheckQueue(async uri => {
    started.push(uri);
    peak = Math.max(peak, ++active);
    if (uri === 'first') await gate.promise;
    active--;
  });
  try {
    queue.enqueue('first');
    queue.enqueue('second');
    queue.enqueue('second');
    queue.enqueue('third');
    await tick();
    assert.deepEqual(started, ['first']);
    gate.resolve();
    await tick();
    assert.deepEqual(started, ['first', 'second', 'third']);
    assert.equal(peak, 1);
  } finally { gate.resolve(); queue.dispose(); }
});

test('edits cancel preparation before spawning and the latest request still runs', async () => {
  const gate = deferred();
  const spawned = [];
  let first = true;
  const queue = new CheckQueue(async (uri, signal) => {
    if (first) { first = false; await gate.promise; }
    if (!signal.aborted) spawned.push(uri);
  });
  try {
    queue.enqueue('edited');
    await tick();
    queue.cancel('edited');
    queue.enqueue('edited');
    gate.resolve();
    await tick();
    assert.deepEqual(spawned, ['edited']);
  } finally { gate.resolve(); queue.dispose(); }
});

test('close removes pending files; shutdown cancels active work', async () => {
  const gate = deferred();
  const started = [];
  let activeSignal;
  const queue = new CheckQueue(async (uri, signal) => {
    started.push(uri);
    activeSignal = signal;
    await gate.promise;
  });
  queue.enqueue('active');
  queue.enqueue('closed');
  queue.cancel('closed');
  await tick();
  queue.dispose();
  queue.enqueue('after shutdown');
  assert.equal(activeSignal.aborted, true);
  gate.resolve();
  await tick();
  assert.deepEqual(started, ['active']);
});

test('a failed check releases the slot for the next file', async () => {
  const errors = [];
  const started = [];
  const queue = new CheckQueue(async uri => {
    started.push(uri);
    if (uri === 'bad') throw new Error('failed');
  }, error => errors.push(error.message));
  try {
    queue.enqueue('bad');
    queue.enqueue('good');
    await tick();
    assert.deepEqual(started, ['bad', 'good']);
    assert.deepEqual(errors, ['failed']);
  } finally { queue.dispose(); }
});

test('compiler diagnostics survive nonzero exits and UTF-8 chunk boundaries', async () => {
  const script = 'const b = Buffer.from("error: café"); process.stderr.write(b.subarray(0,11)); setTimeout(() => { process.stderr.write(b.subarray(11)); process.exitCode = 1; }, 10);';
  assert.equal(await checkProcess(process.execPath, ['-e', script], new AbortController().signal), 'error: café');
});

test('hung compiler is killed and reports a timeout', async () => {
  await assert.rejects(checkProcess(process.execPath, ['-e', 'setInterval(() => {}, 1000)'],
    new AbortController().signal, 200), /exceeded 200 ms/);
});

test('runaway compiler output is bounded on either stream', async () => {
  for (const stream of ['stdout', 'stderr']) {
    await assert.rejects(checkProcess(process.execPath,
      ['-e', `setInterval(() => process.${stream}.write('x'.repeat(4096)), 1)`],
      new AbortController().signal, 5000, 1024), /output exceeded 1024 bytes/);
  }
});

test('cancelled and already cancelled compiler work is silent', async () => {
  const controller = new AbortController();
  const result = checkProcess(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], controller.signal);
  controller.abort();
  assert.equal(await result, undefined);
  assert.equal(await checkProcess('missing-executable', [], controller.signal), undefined);
});

test('spawn errors reject without leaving timeout handles', async () => {
  await assert.rejects(checkProcess('missing-erelang-test-executable', [], new AbortController().signal), /ENOENT/);
});
