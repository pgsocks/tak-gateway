# Testing

GoogleTest checks public API contracts. Cucumber describes observable behavior
and workflows in Gherkin, with Cucumber-Cpp steps calling the public software
interfaces. Populated native and acceptance suites exercise the gateway library.
Separate infrastructure regressions verify runner behavior. All must pass.

The build also compiles the public headers as C11 and procedural C++17.
The [official TAK Server harness](../tests/tak_server/README.md) has independent
Python standard-library tests and opt-in live REST-to-XML/TLS verification:

```sh
python3 -m unittest discover -s tests/tak_server -p 'test_*.py'
python3 tests/tak_server/harness.py build
python3 tests/tak_server/harness.py run --skip-build
```

The live smoke test uses the real source-built server and a reference TLS
subscriber. Gateway mode requires a supplied working executable. Offline
harness tests, source builds and reference-subscriber success do not establish
live gateway interoperability. Network, TLS identity, multicast, FIFO and
shutdown tests require their real OS prerequisites; do not replace failures
with simulated success. Native Windows pipe behavior remains a platform
verification obligation in the full design.

## Placement and maintenance

Current contract coverage is grouped by public behavior:

| Area | Unit coverage | Cucumber workflow |
| --- | --- | --- |
| Framing/datagrams | Split points, coalescing, UTF-8, unsafe XML, limits, EOF | Complete event framing |
| Normalization | Exact XML, controls plus events, opaque recovery, conflicts | Loss-aware XML normalization |
| Sessions | Credentials, upgrade boundaries, deadlines, reset, maintenance | Independent TAK session state |
| Delivery | Order, duplicates, short writes, retention, per-output failure | Bounded independent output delivery |
| Configuration | Schema, references, redaction, ownership, diagnostics | Validation before connecting |
| Runtime adapters | Actual UDP/TCP sockets, POSIX FIFO, Linux multicast | Local network pipeline and lifecycle |

The native suite covers parsing, conversion, ownership, queues, configuration,
and real UDP/TCP/FIFO/Linux multicast adapters. The Docker harness verifies
REST mission creation through both XML/TLS and negotiated Protobuf v1 ingress
to the executable's framed XML output. Select the latter using
`--gateway-protocol protobuf` together with `--gateway`.

Additional platform/deployment checks remain: streaming-password authorization
against a configured server, simultaneous real TLS sources, IPv6 multicast,
native Windows named-pipe ACL/I/O, and supervised/read-only-container deployment.
Do not infer these results from the mission smoke tests.

Put unit test sources under `tests/unit/`. Cover normal results, contract
boundaries, invalid inputs where specified, error results, and ownership or
cleanup obligations. Add coverage when an API contract changes and regression
tests when fixing a bug.

Put Gherkin `.feature` files under `features/` and compiled native step
definitions under `tests/acceptance/`. Add scenarios for new or newly clarified
observable behavior and meaningful workflows. Describe outcomes using the
language of a caller or user. Keep each scenario focused on one behavior;
avoid coupling scenarios to private implementation details.

Revise expectations when intended behavior changes. Implementation refactors
retain existing expectations, and bugs require fixing the implementation.
Remove coverage when behavior is intentionally retired or equivalent coverage
replaces it. Explain the reason in the change and remove unused steps and
fixtures together with the retired coverage.

## Build selections

Use the shared CMake presets from the repository root. Each selection has
matching configure, build, and test presets and its own build directory:

| Preset | Build directory | Test selection |
| --- | --- | --- |
| `default` | `build` | Unit tests |
| `acceptance` | `build/acceptance` | Both product suites and infrastructure |
| `acceptance-only` | `build/acceptance-only` | Acceptance and infrastructure |
| `no-suites` | `build/no-suites` | Both suite options off |
| `no-tests` | `build/no-tests` | Test infrastructure off |

All test options are independent under CMake's standard master switch:

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Enable test infrastructure and dependencies |
| `BUILD_UNIT_TESTS` | `ON` | Build and discover GoogleTest unit cases |
| `BUILD_ACCEPTANCE_TESTS` | `OFF` | Build and run Cucumber acceptance tests |

`BUILD_TESTING=OFF` overrides both suite options. With both suite options off,
no test dependencies are fetched. GoogleTest is fetched whenever either suite
is enabled, because native Cucumber steps also use its assertions. Cucumber
and its dependencies are fetched only for acceptance tests. All dependencies
use pinned CPM packages and are excluded from installation.

Infrastructure regressions are enabled with acceptance testing and use the
same prerequisites. They have the `infrastructure` label, which is separate
from product labels `unit` and `acceptance`. The default configuration remains
Ruby-free. The `no-suites` and `no-tests` presets explicitly allow no CTest
entries; the other shared test presets fail when a filter selects no entries.

Library code and test tooling use C++17, with C11-compatible public headers.
Test code may follow GoogleTest and Cucumber framework conventions as
described in [CONTRIBUTING.md](../CONTRIBUTING.md).

## Local customization

Use the ignored `CMakeUserPresets.json` for special local environment options
or one-off tests. Inherit from a shared configure preset, choose a unique name,
and set a separate `binaryDir` such as `${sourceDir}/build-local`. Add matching
build and test presets that reference the local configure preset. Test presets
can inherit shared settings such as failure output, but must override
`configurePreset` to select the local build directory. Do not commit this file.

Set local compiler paths, generators, cache variables, and environment options
in user presets instead of passing configuration overrides on the command
line. For a multi-configuration generator, set the same `configuration` value,
such as `Debug`, in the local build and test presets. Use executables under the
corresponding configuration subdirectory. Set `CMAKE_INSTALL_PREFIX` in a user
configure preset to override the shared `$HOME/.local` installation prefix.

Add, remove, or alter supported project configurations in `CMakePresets.json`
and update their documented commands, as required by
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Unit tests

Install CMake 3.24 or newer, Git, and a C/C++ toolchain supporting C++17 for
library sources and tests. Initial dependency downloads require network
access. Ruby and Bundler are unnecessary for the default configuration:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
ctest --preset default -L unit
```

An empty `unit_tests` runner fails normal execution. CTest registers a failing
`unit_empty` entry when GoogleTest discovery finds zero cases, even when other
suites have tests. After adding cases, `gtest_discover_tests` automatically
discovers them, applies the `unit` label, and removes the empty-suite fallback.
Listing remains valid with no cases. List or focus cases with CTest or GoogleTest:

```sh
ctest --preset default -N
ctest --preset default -L unit -R 'SuiteName.CaseName'
./build/tests/unit/unit_tests --gtest_list_tests
./build/tests/unit/unit_tests --gtest_filter='SuiteName.*'
```

For a multi-configuration generator, use local build and test presets with
matching `configuration` values, as described under local customization.

## Acceptance tests

Install Ruby and Bundler, then install the locked gems from the repository
root. Also install `patch`, which CPM uses for CMake build integration patches.
The Gemfile pins Cucumber 7.1.0 and cucumber-wire 6.2.1, matching
[Cucumber-Cpp 0.9.0's Gemfile][upstream-gems]. Use a Ruby version compatible
with these gems; the tooling has been verified with Ruby 3.3.6 and Bundler
2.6.9. Ruby and Bundler must be available on `PATH` during configuration.
To select other installations, set the `RUBY_EXECUTABLE` and
`BUNDLER_EXECUTABLE` cache variables in a user configure preset. Both must
belong to the same Ruby installation.

```sh
gem install bundler
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance
ctest --preset acceptance -L acceptance
```

To keep installed gems local, configure Bundler before installing:

```sh
bundle config set --local path vendor/bundle
bundle install
```

Those directories are ignored by Git. Keep the resolved `Gemfile.lock` in
version control. Configuration checks
prerequisites and the installed bundle without installing gems. If setup is
missing, follow the reported install command and configure again.

CTest registers one suite named `acceptance`, labeled `acceptance`. The Ruby
launcher starts `acceptance_steps`, using the upstream Cucumber-Cpp server,
bound to `127.0.0.1` on an ephemeral port. It waits up to ten seconds for the
server's readiness output, passes the selected port to the wire configuration,
and runs Cucumber in strict mode. Zero selected scenarios and undefined,
pending, or failed steps fail the suite. Cucumber support glue counts selected
scenarios using its event hooks, including after feature and tag filters.
The launcher propagates the result and stops its child processes on
completion, startup failure, or an interrupt. It escalates from termination
to forced cleanup after two seconds if necessary. Each invocation gets its
own server and port.

Use the launcher for feature paths, line selectors, and tag filters. Run these
commands from the repository root, substituting an existing feature and tag:

```sh
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature:12
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps --tags '@workflow'
```

Additional Cucumber arguments are forwarded; strict mode remains enabled.
The launcher loads repository support glue and wire definitions even for
focused runs. For a multi-configuration generator, use the server executable
under `tests/acceptance/Debug/` after a Debug build.

[upstream-gems]: https://github.com/cucumber/cucumber-cpp/blob/v0.9.0/Gemfile

## Infrastructure regressions

After configuring and building an acceptance-enabled preset, run the permanent
runner checks from the repository root:

```sh
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Both commands must pass. Running `ctest --preset acceptance` executes all
groups. CI runs infrastructure first, then product suites for each compiler.

Fixtures under `tests/infrastructure/` exercise real native wire steps and
controlled startup, exit, and process-tree behavior. The harness also checks
empty and populated unit discovery using an isolated CMake project. Acceptance
runs use temporary copies of the launcher, bundle manifest, and support glue,
so added product features and hooks do not affect these fixtures. Fixture
cases and features are not registered in the product suites. Temporary files,
ports, children, and descendants are cleaned up after each check, including
failure. The harness uses Ruby's standard library and the existing native test
dependencies.

Checks cover successful and focused execution, custom formatting, strict step
failures, empty selections, missing executables and gems, early exit, startup
timeout, excessive readiness output, server and consumer failures, forced
cleanup, interruption, and concurrent isolation. Repeated fixture invocations
also exercise port reuse. On POSIX, interruption uses an actual `SIGINT`. On
Windows, where CI may lack a console, a fixture raises the same exception as
the launcher's signal handler; delivery of console events remains environment
dependent.

If a regression fails, inspect its diagnostic and CTest's captured output.
Rebuild after modifying native fixtures and rerun the infrastructure label.
Avoid moving fixture cases into product directories to make empty suites pass.

## Common setup issues

- If configuration reports missing Ruby or Bundler, install them and ensure
  both executables are on `PATH`, or select matching paths in a user preset.
- If configuration or the launcher reports unavailable gems, run
  `bundle install` with the selected Ruby and Bundler, then configure again.
- If a unit suite contains no cases, register real product unit sources and
  rebuild. If acceptance selects no scenarios, add product features or correct
  the focused path, line selector, or tag filter.
- If a shared preset selects no CTest entries, check the label or regular
  expression. Use the explicit opt-out presets when intentionally disabling
  product testing.

## Opting out

Build acceptance tests alone, disable units, or disable all tests:

```sh
cmake --preset acceptance-only
cmake --build --preset acceptance-only
ctest --preset acceptance-only -L acceptance

cmake --preset no-suites
cmake --build --preset no-suites
ctest --preset no-suites

cmake --preset no-tests
cmake --build --preset no-tests
ctest --preset no-tests
```

The presets explicitly set the test selections on each configuration run.
Disabling acceptance later does not delete dependency sources already
downloaded in that build directory.

## Contributor workflow

Register each new `.cpp` file explicitly with `target_sources(unit_tests
PRIVATE ...)` in `tests/unit/CMakeLists.txt`, or with
`target_sources(acceptance_steps PRIVATE ...)` in
`tests/acceptance/CMakeLists.txt`. Rebuild to discover new unit cases. Gherkin
files need no CMake registration: Cucumber finds them under `features/` or
through a focused path. Shared contract fixtures are not a substitute for
independent expected results or actual platform fixtures.

As software targets are added under `src/`, link the software under test with
`target_link_libraries(unit_tests PRIVATE your_library)` and
`target_link_libraries(acceptance_steps PRIVATE your_library)`. Use public
headers and interfaces rather than compiling a second copy of implementation
sources into the test runners. Register native steps directly as executable
sources so the linker retains their static registrations.

For native steps using GoogleTest assertions, include `<gtest/gtest.h>` before
`<cucumber-cpp/autodetect.hpp>` so Cucumber selects its GoogleTest driver.
Use Cucumber scenario contexts and before/after hooks to establish and clean
up each scenario's resources. Reset state between scenarios, use unique
temporary paths, and release files, handles, and processes even on failure.
Scenarios must work alone and in any order. Share reusable steps without
depending on another scenario's actions or fixtures.

Before review, follow the checks in [CONTRIBUTING.md](../CONTRIBUTING.md).
Run both suites for affected behavior and inspect the discovered test and
scenario counts. For runner or build changes, exercise all five build
selections, failure results, focused runs, repeated runs, missing
prerequisites, startup failures, interrupts, and server cleanup.
