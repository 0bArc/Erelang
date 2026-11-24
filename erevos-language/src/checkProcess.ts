import { spawn } from 'child_process';

/** Bounded compiler invocation. Cancellation is silent; failures remain actionable. */
export function checkProcess(
  exe: string, args: string[], signal: AbortSignal,
  timeoutMs = 10_000, maxOutputBytes = 1024 * 1024,
  input?: string,
  onStdout?: (text: string) => void,
): Promise<string | undefined> {
  if (signal.aborted) return Promise.resolve(undefined);
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { windowsHide: true });
    // EPIPE is expected if the compiler rejects arguments before consuming input.
    child.stdin.on('error', () => undefined);
    child.stdin.end(input);
    const chunks: Buffer[] = [];
    const stdoutChunks: Buffer[] = [];
    let bytes = 0;
    let failure: Error | undefined;
    const stop = () => { child.kill(); };
    const timer = setTimeout(() => {
      failure = new Error(`check exceeded ${timeoutMs} ms`);
      stop();
    }, timeoutMs);
    signal.addEventListener('abort', stop, { once: true });
    // Drain both streams, but bound retained diagnostics and stop runaway output.
    child.stdout.on('data', (data: Buffer) => receive(data, false));
    child.stderr.on('data', (data: Buffer) => receive(data, true));
    function receive(data: Buffer, retain: boolean): void {
      bytes += data.length;
      if (bytes > maxOutputBytes) {
        failure ??= new Error(`check output exceeded ${maxOutputBytes} bytes`);
        stop();
      } else if (retain) chunks.push(data);
      else if (onStdout) stdoutChunks.push(data);
    }
    const cleanup = () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', stop);
    };
    child.once('error', error => {
      cleanup();
      if (signal.aborted) resolve(undefined);
      else reject(error);
    });
    child.once('close', (_code, exitSignal) => {
      cleanup();
      if (signal.aborted) resolve(undefined);
      else if (failure) reject(failure);
      else if (exitSignal) reject(new Error(`check terminated by ${exitSignal}`));
      else {
        onStdout?.(Buffer.concat(stdoutChunks).toString('utf8'));
        resolve(Buffer.concat(chunks).toString('utf8'));
      }
    });
  });
}
