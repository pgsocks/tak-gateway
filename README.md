# TAK Gateway

TAK Gateway is a receive-oriented native gateway that aggregates
independent TAK Server TLS streams and UDP/multicast sources into complete XML
CoT events for TCP, UDP, and platform named-pipe consumers.

The procedural C++17 implementation exposes C11 and C++17 headers and a
foreground `tak-gateway` executable. Native and Cucumber suites exercise the
public contracts. Live Docker checks against official TAK Server
5.7-RELEASE-14 verify REST-created mission notifications through both XML/TLS
and negotiated Protobuf v1 ingress to framed XML output. Native Windows
behavior has not been verified.

The full contract includes XML preservation, TAK v1 Protobuf conversion with
explicit opaque-preservation/rejection policy, authenticated sessions and
negotiation, bounded independent output queues, and visible delivery failures.
Delivery is live and best effort, with duplicates retained and no durable replay.

## Design and contracts

- [Design rationale](docs/design.md): architecture, alternatives, and tradeoffs.
- [Public API and configuration](docs/api.md): C11/procedural C++17 contracts,
  lifecycle, TOML schema, and status snapshots.
- [Research](docs/research.md): pinned protocol, codec, and server evidence.
- [Testing](docs/testing.md): build selections, product suites, and infrastructure.
- [TAK Server harness](tests/tak_server/README.md): source build, verified mTLS
  REST-to-CoT smoke test, and optional gateway output verification.

## Build and run contract tests

Use CMake 3.24 or newer and a C/C++ toolchain supporting C11 and C++17.
Initial CPM dependency downloads need network access:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The build checks both header language interfaces and runs the native tests.
OpenSSL, c-ares, Expat, TOML parsing, and the CoT codec are acquired through
CPM. Building OpenSSL from source requires Perl and Make (nmake on Windows).
For a local build, `TG_USE_SYSTEM_OPENSSL=ON` selects an installed OpenSSL 3;
put environment-specific settings in ignored `CMakeUserPresets.json`.

Run the foreground gateway with a configuration matching
[the TOML schema](docs/api.md):

```sh
build/src/tak-gateway validate --config gateway.toml
build/src/tak-gateway redact --config gateway.toml
build/src/tak-gateway run --config gateway.toml
```

`redact` prints explicit defaults with secret references removed. SIGINT or
SIGTERM stops intake and permits a two-second drain before exit.

Acceptance testing also needs Ruby, Bundler, and `patch`:

```sh
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Runner infrastructure and populated product suites must pass. Native Cucumber
steps call the same public library used by the executable.

## Docker TAK Server harness

On native Linux with Docker Engine, Compose v2, BuildKit, and Python 3.9+:

```sh
python3 -m unittest discover -s tests/tak_server -p 'test_*.py'
python3 tests/tak_server/harness.py build
python3 tests/tak_server/harness.py run --skip-build
```

The harness builds pinned official TAK Server source, provisions isolated
PostGIS and fresh test PKI, creates a mission through verified mTLS REST, and
checks the resulting CoT notification on an authenticated XML/TLS subscription.
It removes its own mission and disposable fixture resources afterward.

Verify the gateway's TCP records using either ingress encoding:

```sh
python3 tests/tak_server/harness.py run --skip-build \
  --gateway "$PWD/build/src/tak-gateway"
python3 tests/tak_server/harness.py run --skip-build \
  --gateway "$PWD/build/src/tak-gateway" --gateway-protocol protobuf
```

See the harness guide for persistent fixtures, provenance, trust configuration,
and cleanup.

## Contributing

Follow [CONTRIBUTING.md](CONTRIBUTING.md). Library sources use procedural
C++17 with C11-compatible public C interfaces; ownership and errors are explicit.
Test tooling may use the test frameworks' C++ conventions. No stable binary ABI
or released interoperability commitment is established. This repository has
not selected a project license; review applicable upstream notices before
redistribution.
