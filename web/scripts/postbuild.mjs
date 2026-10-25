// Writes dist/version.json for the device loader (/loader.js), which reads
// `version` | `build` | `hash` and appends it as ?v= to assets/app.{js,css}.
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync, copyFileSync, mkdirSync } from 'node:fs';
import { execSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
const files = ['assets/app.js', 'assets/app.css'];
const hash = createHash('sha256');
for (const f of files) {
  const p = join(dist, f);
  if (!existsSync(p)) {
    console.error(`postbuild: missing ${f} — check vite.config.ts output names`);
    process.exit(1);
  }
  hash.update(readFileSync(p));
}
let commit = process.env.GITHUB_SHA ?? '';
if (!commit) {
  try {
    commit = execSync('git rev-parse HEAD', { cwd: root, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
  } catch {
    commit = 'local';
  }
}
const digest = hash.digest('hex').slice(0, 16);
const info = {
  name: pkg.name,
  // The device loader uses version||build||hash as the ?v= cache buster, so
  // `version` must change with every content change.
  version: `${pkg.version}+${digest}`,
  app_version: pkg.version,
  build: `${pkg.version}+${commit.slice(0, 12)}`,
  hash: digest,
  commit,
  built_at: new Date().toISOString(),
  assets: { js: files[0], css: files[1] },
};
writeFileSync(join(dist, 'version.json'), JSON.stringify(info, null, 2) + '\n');
// Reference loader page (the device serves its own copy at "/").
mkdirSync(join(dist, 'loader'), { recursive: true });
for (const f of ['index.html', 'loader.js']) copyFileSync(join(root, 'loader', f), join(dist, 'loader', f));
// GitHub Pages: skip Jekyll processing.
writeFileSync(join(dist, '.nojekyll'), '');
console.log(`postbuild: version.json hash=${digest} build=${info.build}`);
