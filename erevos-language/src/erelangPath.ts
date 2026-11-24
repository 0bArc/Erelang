import * as fs from 'fs';
import * as path from 'path';

export function resolveErelangExe(workspaceRoots: string[], configured?: string): string | undefined {
  if (configured && configured.trim()) {
    const p = configured.trim();
    if (fs.existsSync(p)) return p;
  }
  const binary = process.platform === 'win32' ? 'erelang.exe' : 'erelang';
  const candidates: string[] = [];
  for (const root of workspaceRoots) {
    candidates.push(
      path.join(root, 'build', 'debug', 'bin', 'Debug', binary),
      path.join(root, 'build', 'release', 'bin', 'Release', binary),
    );
  }
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  // Resolve PATH directly: spawning `where`/`which` synchronously blocks the editor host.
  for (const entry of (process.env.PATH ?? '').split(path.delimiter)) {
    const dir = entry.trim().replace(/^"(.*)"$/, '$1');
    if (!dir) continue;
    const candidate = path.join(dir, binary);
    try {
      if (fs.statSync(candidate).isFile()) return candidate;
    } catch { /* missing PATH entry */ }
  }
  return undefined;
}
