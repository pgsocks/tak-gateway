# Writing an API contract

Use this guide to write the contract for a native library with public C and
procedural C++ interfaces. It is for maintainers who understand the intended
use cases and the project's implementation restrictions. The template has no
product API yet. These prompts and illustrative examples establish no product
guarantees; replace the guide with the actual contract as you design the API.

## Start with one operation

Choose a caller workflow and describe its input, result, failure outcomes,
and resource obligations. Record those decisions before implementing it.
Include the corresponding public declarations, then map each observable
requirement to a unit case or acceptance scenario. A reader should be able to
write a correct call and an independent test from the contract alone.

## Define inputs and outputs

For each operation, specify valid argument values, units, lengths, encoding,
and whether null pointers are permitted. State which boundaries are included.
Explain output values on success and failure, and whether an operation may
partially change state before failing.

Identify preconditions that callers must satisfy and distinguish them from
invalid inputs that the API detects and reports. Do not promise validation or
recovery the implementation cannot provide. Keep C and C++ declarations
consistent with the same observable behavior.

## Assign resource ownership

State who allocates and releases every buffer, handle, and returned resource.
For borrowed data, identify how long it remains valid and which operations
invalidate it. For ownership transfers, state exactly when responsibility
changes hands, including failure paths.

Describe allocation failure, cleanup after partially completed operations,
and matching creation and destruction operations. For example, a borrowed
result needs an explicit lifetime rule; "the caller does not free it" alone
does not tell the caller when it can safely read it.

## Describe errors and state transitions

Define the public status results and the conditions producing each one.
Explain how diagnostic information is obtained and how long it remains valid.
Describe whether a failed operation can be retried and the state available
after failure. Keep errors within the project's public status contract.

For opaque handles, document valid states and the operations allowed in each
state. Cover initialization, shutdown, repeated cleanup, and use after release
where the API defines behavior. Avoid implying that arbitrary invalid memory
or stale handles can always be detected.

## Define callbacks and concurrency

Describe callback arguments, caller state, invocation timing, and the lifetime
of data supplied to a callback. State whether callbacks may call the library
again, request cancellation, or release the associated handle.

Choose and document thread-safety requirements for independent handles,
shared handles, and global state. Identify caller synchronization obligations
and any thread affinity. Explicitly document unsupported concurrent use;
do not let silence become an accidental thread-safety promise.

## State compatibility commitments

Identify supported header language versions, platforms, and toolchains.
Distinguish source compatibility from binary compatibility. Define which
releases can interoperate, and how callers identify versions when needed.

Consider exported symbols, public struct layout, integer widths, calling
conventions, and allocation across library boundaries when deciding ABI
commitments. A C-compatible header alone does not establish binary
compatibility across arbitrary platforms and toolchains. Specify the intended
compatibility boundary and test it when applicable.

Consult the target ABI and compiler documentation when choosing these
commitments. For example, [GCC's code generation options][gcc-abi] describe
options that change enum sizes or struct layout and affect binary compatibility.

## Validate and maintain the contract

Review each operation from the caller's perspective. Can the caller determine
valid inputs, interpret every result, and release all resources on every path?
Resolve unanswered questions before declaring the contract complete.

Add unit coverage for normal results, boundaries, specified invalid inputs,
errors, and ownership obligations. Add acceptance scenarios for meaningful
workflows. Follow the [testing guide](testing.md) for placement and execution.
Record the reason for consequential decisions in the [design rationale](design.md).

When intended behavior changes, revise the contract and its expectations
together. If tests disagree with intended behavior, first determine whether
the contract changed or the implementation contains a bug. Vague lifetime or
failure descriptions are reasons to revisit the corresponding decisions,
rather than asking callers to infer behavior from implementation details.

[gcc-abi]: https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html
