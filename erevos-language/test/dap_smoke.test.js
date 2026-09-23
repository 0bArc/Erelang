const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

function findErelang() {
  const roots = [
    path.resolve(__dirname, '..', '..'),
    path.resolve(__dirname, '..'),
  ];
  for (const root of roots) {
    for (const rel of [
      path.join('build', 'bin', 'Debug', 'erelang.exe'),
      path.join('build', 'bin', 'erelang.exe'),
      path.join('build', 'Debug', 'erelang.exe'),
    ]) {
      const p = path.join(root, rel);
      if (fs.existsSync(p)) return p;
    }
  }
  return null;
}

test('DAP line protocol: breakpoint stop then continue', async (t) => {
  const exe = findErelang();
  if (!exe) {
    t.skip('erelang.exe not built');
    return;
  }
  const script = path.resolve(__dirname, '..', '..', 'examples', 'debug_smoke.elan');
  assert.ok(fs.existsSync(script), 'debug_smoke.elan missing');

  const child = spawn(exe, [script, '--dap'], {
    env: { ...process.env, ERELANG_DEBUG: '1' },
    windowsHide: true,
  });

  let stderr = '';
  const events = [];
  let resolveReady;
  const ready = new Promise((r) => { resolveReady = r; });

  child.stderr.on('data', (d) => {
    stderr += d.toString('utf8');
    let idx;
    while ((idx = stderr.indexOf('\n')) >= 0) {
      const line = stderr.slice(0, idx).replace(/\r$/, '');
      stderr = stderr.slice(idx + 1);
      if (!line.startsWith('!dap ')) continue;
      const payload = line.slice(5);
      events.push(payload);
      if (payload === 'ready') resolveReady();
    }
  });

  await Promise.race([
    ready,
    new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting ready')), 8000)),
  ]);

  child.stdin.write('!dap break 4\n');
  child.stdin.write('!dap go\n');

  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timeout waiting stopped')), 8000);
    const check = setInterval(() => {
      if (events.some(e => e.startsWith('stopped '))) {
        clearInterval(check);
        clearTimeout(timer);
        resolve();
      }
    }, 50);
  });

  const stopped = events.find(e => e.startsWith('stopped '));
  assert.match(stopped, /line=4/);
  assert.match(stopped, /reason=breakpoint/);

  child.stdin.write('!dap continue\n');

  const code = await new Promise((resolve) => {
    child.on('close', (c) => resolve(c ?? 1));
  });
  assert.equal(code, 0);
});
