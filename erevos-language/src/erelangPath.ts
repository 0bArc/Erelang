import * as fs from 'fs';
import * as path from 'path';
import { spawnSync } from 'child_process';

export function resolveErelangExe(workspaceRoots: string[], configured?: string): string | undefined {
  if (configured && configured.trim()) {
    const p = configured.trim();
    if (fs.existsSync(p)) return p;
  }
  const candidates: string[] = [];
  for (const root of workspaceRoots) {
    candidates.push(
      path.join(root, 'build', 'bin', 'Debug', 'erelang.exe'),
      path.join(root, 'build', 'bin', 'Release', 'erelang.exe'),
      path.join(root, 'build', 'bin', 'erelang.exe'),
      path.join(root, 'build', 'Debug', 'erelang.exe'),
      path.join(root, 'build', 'Release', 'erelang.exe'),
    );
  }
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  const which = spawnSync(process.platform === 'win32' ? 'where' : 'which', ['erelang'], { encoding: 'utf8' });
  if (which.status === 0) {
    const line = (which.stdout || '').split(/\r?\n/).map(s => s.trim()).find(Boolean);
    if (line && fs.existsSync(line)) return line;
  }
  return undefined;
}
