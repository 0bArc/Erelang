'use strict';
const { performance } = require('perf_hooks');

const N = 20000;
const t0 = performance.now();
let s = '';
for (let i = 0; i < N; i++) {
  s = s + 'x';
}
const t1 = performance.now();
console.log('INNER_MS=' + Math.round(t1 - t0));
console.log('LEN=' + s.length);
