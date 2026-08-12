'use strict';
const { performance } = require('perf_hooks');

const N = 5000000;
const t0 = performance.now();
let sum = 0;
for (let i = 0; i < N; i++) {
  sum = sum + i;
}
const t1 = performance.now();
console.log('INNER_MS=' + Math.round(t1 - t0));
console.log('SUM=' + sum);
