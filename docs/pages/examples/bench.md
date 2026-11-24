# Benchmarks

Timing loops, recursion, lists and maps.

```elan
--8<-- "examples/types/bench.elan"
```

## Sample results

Release build, one run on the development machine. Numbers vary by machine.

| Benchmark | Time |
|-----------|------|
| int loop 1e6 | 133 ms |
| float loop 3e5 | 126 ms |
| fib(22) | 24 ms |
| list push 5e4 | 12 ms |
| map insert 2e4 | 17 ms |

The full suite lives in `examples/bench/run.ps1`.

