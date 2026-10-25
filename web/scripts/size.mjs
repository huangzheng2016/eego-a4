// Prints raw and gzipped sizes of the emitted assets.
import { readFileSync, readdirSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = join(dirname(fileURLToPath(import.meta.url)), '..', 'dist', 'assets');
let total = 0;
for (const f of readdirSync(dir)) {
  const buf = readFileSync(join(dir, f));
  const gz = gzipSync(buf, { level: 9 }).length;
  total += gz;
  console.log(`${f.padEnd(12)} ${(buf.length / 1024).toFixed(1).padStart(8)} KiB  gzip ${(gz / 1024).toFixed(1).padStart(7)} KiB`);
}
console.log(`total gzip ${(total / 1024).toFixed(1)} KiB`);
if (total > 150 * 1024) {
  console.error('bundle exceeds 150 KiB gzip budget');
  process.exit(1);
}
