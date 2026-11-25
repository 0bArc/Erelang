# Benchmarks

Timing loops, recursion, lists and maps.

```elan
--8<-- "examples/types/bench.elan"
```

## Sample results

Release build, one run on the development machine. Numbers vary by machine.

| Benchmark | Time |
|-----------|------|
| int loop 1e6 | 5 ms |
| float loop 3e5 | 1 ms |
| fib(22) | 2 ms |
| list push 5e4 | 2 ms |
| map insert 2e4 | 11 ms |

The full suite lives in `examples/bench/run.ps1`.

## What runs fast

Loops (`while`, `do`, `for`, `for (x : xs)`, `repeat`) and actions are compiled to bytecode
with variables in numbered slots when they only use:

- `int`, `float`, `bool` and `string` values, including mixed `int` / `float` arithmetic
- lists and maps: literals, ranges (`1 .. n`), `xs[i]`, `xs[i] = v`, `m[k] = v`, for-each and
  the list and map methods (`push`, `len`, `get`, `has`, `keys`, ...)
- operators, `if`, `break`, `continue`, `return`, `print` and string interpolation
- calls to other actions, `string.*` / `math.*` builtins that return a plain value, and
  `int()`, `float()`, `string()`, `bool()`

A method call is compiled only when its receiver is declared with a list or map type
(`Array<int> xs`, `Map<string, int> m`, or a parameter typed that way).

Anything else (structs, entities, pointers, closures, `try`, `match`, other builtins) runs on
the interpreter as before. Code that reads lists or maps runs compiled only if every action it
calls runs compiled too, and a list or map that holds a heap, weak or file handle or a struct
sends that code back to the interpreter. Results are the same either way; only the speed
differs.

