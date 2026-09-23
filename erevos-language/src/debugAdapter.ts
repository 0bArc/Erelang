import {
  LoggingDebugSession,
  InitializedEvent,
  StoppedEvent,
  TerminatedEvent,
  OutputEvent,
  Breakpoint,
  StackFrame,
  Source,
  Thread,
  Scope,
  Variable,
} from '@vscode/debugadapter';
import { DebugProtocol } from '@vscode/debugprotocol';
import { spawn, ChildProcessWithoutNullStreams } from 'child_process';
import * as path from 'path';
import { resolveErelangExe } from './erelangPath';

interface LaunchRequestArguments extends DebugProtocol.LaunchRequestArguments {
  program: string;
  erelangPath?: string;
  cwd?: string;
  stopOnEntry?: boolean;
}

class ErelangDebugSession extends LoggingDebugSession {
  private static THREAD_ID = 1;
  private proc: ChildProcessWithoutNullStreams | undefined;
  private programPath = '';
  private stoppedLine = 1;
  private locals: Record<string, string> = {};
  private ready = false;
  private pendingBreaks: number[] = [];
  private stderrBuf = '';

  public constructor() {
    super('erelang-debug.txt');
    this.setDebuggerLinesStartAt1(true);
    this.setDebuggerColumnsStartAt1(true);
  }

  protected initializeRequest(
    response: DebugProtocol.InitializeResponse,
    _args: DebugProtocol.InitializeRequestArguments,
  ): void {
    response.body = response.body || {};
    response.body.supportsConfigurationDoneRequest = true;
    response.body.supportsTerminateRequest = true;
    this.sendResponse(response);
  }

  protected async launchRequest(
    response: DebugProtocol.LaunchResponse,
    args: LaunchRequestArguments,
  ): Promise<void> {
    this.programPath = path.resolve(args.program);
    const cwd = args.cwd || path.dirname(this.programPath);
    const exe = resolveErelangExe([cwd, path.resolve(cwd, '..'), path.resolve(cwd, '../..')], args.erelangPath);
    if (!exe) {
      this.sendErrorResponse(response, 2001, 'erelang.exe not found (set erelang.executablePath)');
      return;
    }
    try {
      this.proc = spawn(exe, [this.programPath, '--dap'], {
        cwd,
        env: { ...process.env, ERELANG_DEBUG: '1' },
        windowsHide: true,
      });
    } catch (e) {
      this.sendErrorResponse(response, 2002, `Failed to launch: ${e instanceof Error ? e.message : String(e)}`);
      return;
    }
    this.proc.stdout.on('data', (d: Buffer) => {
      this.sendEvent(new OutputEvent(d.toString('utf8'), 'stdout'));
    });
    this.proc.stderr.on('data', (d: Buffer) => {
      this.stderrBuf += d.toString('utf8');
      this.drainStderr();
    });
    this.proc.on('exit', () => {
      this.sendEvent(new TerminatedEvent());
    });
    this.sendResponse(response);
    this.sendEvent(new InitializedEvent());
  }

  private drainStderr(): void {
    let idx: number;
    while ((idx = this.stderrBuf.indexOf('\n')) >= 0) {
      const line = this.stderrBuf.slice(0, idx).replace(/\r$/, '');
      this.stderrBuf = this.stderrBuf.slice(idx + 1);
      if (line.startsWith('!dap ')) {
        this.handleDap(line.slice(5));
      } else if (line.length) {
        this.sendEvent(new OutputEvent(line + '\n', 'stderr'));
      }
    }
  }

  private handleDap(payload: string): void {
    if (payload === 'ready') {
      this.ready = true;
      for (const line of this.pendingBreaks) {
        this.writeCmd(`break ${line}`);
      }
      this.pendingBreaks = [];
      if (this.pendingBreaks.length === 0) {
        // breakpoints already sent via setBreakPoints before continue
      }
      return;
    }
    if (payload.startsWith('stopped ')) {
      const m = /line=(\d+)/.exec(payload);
      this.stoppedLine = m ? parseInt(m[1], 10) : 1;
      const reason = /reason=(\w+)/.exec(payload)?.[1] || 'breakpoint';
      this.sendEvent(new StoppedEvent(reason === 'step' ? 'step' : 'breakpoint', ErelangDebugSession.THREAD_ID));
      return;
    }
    if (payload.startsWith('locals ')) {
      try {
        this.locals = JSON.parse(payload.slice(7)) as Record<string, string>;
      } catch {
        this.locals = {};
      }
    }
  }

  private writeCmd(cmd: string): void {
    if (!this.proc || !this.proc.stdin.writable) return;
    this.proc.stdin.write(`!dap ${cmd}\n`);
  }

  protected setBreakPointsRequest(
    response: DebugProtocol.SetBreakpointsResponse,
    args: DebugProtocol.SetBreakpointsArguments,
  ): void {
    const lines = (args.breakpoints || []).map(b => b.line);
    const actual: DebugProtocol.Breakpoint[] = lines.map(line => {
      const bp = new Breakpoint(true, line);
      return bp;
    });
    if (this.ready) {
      this.writeCmd('clear');
      for (const line of lines) this.writeCmd(`break ${line}`);
    } else {
      this.pendingBreaks = lines.slice();
    }
    response.body = { breakpoints: actual };
    this.sendResponse(response);
  }

  protected configurationDoneRequest(
    response: DebugProtocol.ConfigurationDoneResponse,
    _args: DebugProtocol.ConfigurationDoneArguments,
  ): void {
    this.writeCmd('go');
    this.sendResponse(response);
  }

  protected threadsRequest(response: DebugProtocol.ThreadsResponse): void {
    response.body = { threads: [new Thread(ErelangDebugSession.THREAD_ID, 'main')] };
    this.sendResponse(response);
  }

  protected stackTraceRequest(
    response: DebugProtocol.StackTraceResponse,
    _args: DebugProtocol.StackTraceArguments,
  ): void {
    const src = new Source(path.basename(this.programPath), this.programPath);
    response.body = {
      stackFrames: [new StackFrame(1, 'main', src, this.stoppedLine, 1)],
      totalFrames: 1,
    };
    this.sendResponse(response);
  }

  protected scopesRequest(
    response: DebugProtocol.ScopesResponse,
    _args: DebugProtocol.ScopesArguments,
  ): void {
    response.body = { scopes: [new Scope('Locals', 1, false)] };
    this.sendResponse(response);
  }

  protected variablesRequest(
    response: DebugProtocol.VariablesResponse,
    _args: DebugProtocol.VariablesArguments,
  ): void {
    const vars: Variable[] = Object.entries(this.locals).map(
      ([name, value]) => new Variable(name, value),
    );
    response.body = { variables: vars };
    this.sendResponse(response);
  }

  protected continueRequest(
    response: DebugProtocol.ContinueResponse,
    _args: DebugProtocol.ContinueArguments,
  ): void {
    this.writeCmd('continue');
    response.body = { allThreadsContinued: true };
    this.sendResponse(response);
  }

  protected nextRequest(
    response: DebugProtocol.NextResponse,
    _args: DebugProtocol.NextArguments,
  ): void {
    this.writeCmd('step');
    this.sendResponse(response);
  }

  protected stepInRequest(
    response: DebugProtocol.StepInResponse,
    args: DebugProtocol.StepInArguments,
  ): void {
    this.nextRequest(response, args);
  }

  protected stepOutRequest(
    response: DebugProtocol.StepOutResponse,
    args: DebugProtocol.StepOutArguments,
  ): void {
    this.continueRequest(response as DebugProtocol.ContinueResponse, args as DebugProtocol.ContinueArguments);
  }

  protected disconnectRequest(
    response: DebugProtocol.DisconnectResponse,
    _args: DebugProtocol.DisconnectArguments,
  ): void {
    this.writeCmd('quit');
    this.proc?.kill();
    this.sendResponse(response);
  }

  protected terminateRequest(
    response: DebugProtocol.TerminateResponse,
    _args: DebugProtocol.TerminateArguments,
  ): void {
    this.writeCmd('quit');
    this.proc?.kill();
    this.sendResponse(response);
  }
}

ErelangDebugSession.run(ErelangDebugSession);
