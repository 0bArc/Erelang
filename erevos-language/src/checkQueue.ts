/** Serializes the entire check, including async preparation before spawning. */
export class CheckQueue {
  private readonly pending = new Set<string>();
  private active?: { uri: string; controller: AbortController };
  private disposed = false;

  constructor(private readonly check: (uri: string, signal: AbortSignal) => Promise<void>,
    private readonly onError: (error: unknown) => void = () => undefined) {}

  enqueue(uri: string): void {
    if (this.disposed) return;
    this.pending.add(uri);
    this.pump();
  }

  cancel(uri: string): void {
    this.pending.delete(uri);
    if (this.active?.uri === uri) this.active.controller.abort();
  }

  dispose(): void {
    this.disposed = true;
    this.pending.clear();
    this.active?.controller.abort();
  }

  private pump(): void {
    if (this.disposed || this.active) return;
    const next = this.pending.values().next();
    if (next.done) return;
    const uri = next.value;
    this.pending.delete(uri);
    const controller = new AbortController();
    this.active = { uri, controller };
    void Promise.resolve().then(() => this.check(uri, controller.signal))
      .catch(error => this.onError(error))
      .finally(() => {
        this.active = undefined;
        this.pump();
      });
  }
}
