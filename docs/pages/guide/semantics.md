# Core semantics (0.x)

This chapter defines the interpreter behavior covered by the conformance suite. Each rule below
has an executable witness. A change to a rule requires updating its test and this chapter together.
This is a specification of the covered core, not a claim that every native-backend operation has
the same coverage. Run `tests/run.ps1 -Exe <path-to-erelang.exe>` to check a build.

## Bindings and scope

Names are case-sensitive. A local declaration is visible after its declaration in its lexical block.
Parameters are visible throughout their action body. Nested blocks can use outer bindings;
bindings introduced inside a branch are not visible in its sibling branch or after the branch.
Assignments to an existing outer binding update that binding.

Redeclaring a name already visible in the current action, including an enclosing local or parameter,
is rejected with `TC030`. Inner-block shadowing is therefore not currently allowed. Sibling branches
may declare the same spelling because their lexical scopes do not overlap. Different actions have
independent local scopes. Use before declaration is `TC010`. Assignment to `const` is `TC051`;
this protects rebinding and does not promise recursive immutability of referenced objects.

Closures capture mutable outer variables by shared cells. Changes through a captured variable are
visible to later closure calls and to the enclosing action. There is no borrow checker.

Evidence: `tests/semantics/scopes.elan`, `redeclaration_neg.elan`, `shadowing_neg.elan`,
`use_before_declaration_neg.elan`, `branch_leak_neg.elan`, `const_assignment_neg.elan` in the same
directory; `tests/closures/capture_mutate.elan`.

## Numbers and conversions

`int` arithmetic uses signed 64-bit values. Addition, subtraction, multiplication, and unary negation
wrap modulo 2^64 and interpret the result as a signed value. Integer division truncates toward zero.
The remainder has the dividend's sign. The minimum signed integer divided by -1 wraps to the minimum;
its remainder modulo -1 is zero. Division and remainder by zero throw runtime errors.

Floating arithmetic uses the host's double-precision representation. This chapter does not promise
bit-identical transcendental math results across platforms. `int(9.99)` and `int("9.99")` truncate
toward zero; conversion is explicit at call sites. Float-to-integer conversion outside the signed
64-bit range fails rather than promising a wrapped result.

Evidence: `tests/semantics/arithmetic.elan`, `division_zero_neg.elan`, `remainder_zero_neg.elan`,
`conversion_overflow_neg.elan`.

## Evaluation and equality

Binary expressions evaluate their left operand before their right operand. `&&` evaluates its
right operand only if the left is true; `||` evaluates its right operand only if the left is false.
`??` evaluates its right operand only when the left is null. The selected operand is evaluated once.
The conditional expression evaluates only its selected branch. Statically known conditions must be
`bool`; `if (1)` is rejected with `TC060`. Values of open types (`any`/`unknown`) still require runtime
interpretation; this is not a guarantee of static type safety for those values.

`==` compares numeric values (including booleans as 0/1) with promotion when a floating operand is present. `===` first requires
the same runtime value kind; an integer and floating value are strictly unequal even if numerically
equal. Strings compare by their content, so `"007"` and `"7"` are distinct strings. Mixed nonnumeric
runtime kinds fall back to display-text equality; for example an `any` containing `"1"` compares
equal with an `any` containing integer 1 under `==`, but not `===`. This chapter does
not define arbitrary collection handles as deeply equal; callers must not assume structural equality
for every runtime handle. `!=` and `!==` negate their corresponding equality operation.

Evidence: `tests/semantics/equality.elan`, `short_circuit.elan`, `condition_type_neg.elan`.

## Ownership and async boundaries

Ordinary structs copy on assignment. `heap<T>` transfers unique ownership, `shared<T>` shares a
reference count, and `weak<T>` does not keep a shared allocation alive. Scope exit releases owned
handles; using a moved-from unique owner fails. Raw pointers have no static lifetime verification.
See [Memory](../advanced/memory.md) for the exact APIs and current buffer limitations.

`await` is permitted in async actions. Failures from awaited actions propagate to the awaiting
action and can be caught. Cancellation is cooperative: a cancellation request is not a guarantee
that an arbitrary native or blocking operation terminates immediately. Ordering of unrelated async
work and OS threads is not specified. No data-race safety guarantee is made for shared mutable state.

Evidence: `tests/structs/assign_copy.elan`, `tests/memory/heap_move.elan`,
`tests/memory/shared_weak.elan`, `tests/async/fail_await.elan`, `tests/concurrency/cancel.elan`.

## Compatibility and unsettled behavior

The interpreter is the reference for these tests. Native code generation remains a separate coverage
surface. Import-gating is an API rule, not an OS security sandbox. Undefined raw-pointer lifetimes,
deep equality of arbitrary handles, async scheduling order, and cross-platform floating math are not
stabilized contracts. Extend this chapter with assertion-based tests before promising more behavior.
