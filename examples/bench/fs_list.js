'use strict';
const fs = require('fs');
const path = require('path');
const { performance } = require('perf_hooks');

const root = process.argv[2] || 'bench_tree';
const t0 = performance.now();
let count = 0;
for (const name of fs.readdirSync(root)) {
  const full = path.join(root, name);
  const st = fs.statSync(full);
  if (st.isDirectory()) {
    count++;
    for (const child of fs.readdirSync(full)) {
      const cfull = path.join(full, child);
      if (fs.statSync(cfull).isFile()) count++;
    }
  } else if (st.isFile()) {
    count++;
  }
}
const t1 = performance.now();
console.log('INNER_MS=' + Math.round(t1 - t0));
console.log('COUNT=' + count);
