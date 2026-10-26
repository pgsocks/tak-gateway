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
OpenSSL, c-ares, Expat, miniz, TOML parsing, and the CoT codec are acquired through
CPM. Building OpenSSL from source requires Perl and Make (nmake on Windows).
For a local build, `TG_USE_SYSTEM_OPENSSL=ON` selects an installed OpenSSL 3;
put environment-specific settings in ignored `CMakeUserPresets.json`.

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

## Authenticate with one TAK Server

After [building](#build-and-run-contract-tests), import an administrator-provided
ATAK package and choose a local output directly on the command line:

```sh
build/src/tak-gateway run --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 --status-file status.json
```

No TOML file is required. Use `validate` with the same arguments to check local
settings without connecting, or `redact` to inspect the resolved settings
without secret references. Repeat `--output` for additional consumers. See
[CLI arguments](docs/cli.md) for output types, settings and TOML overrides, and
[ATAK packages](docs/atak-import.md) for the uploaded ZIP's structure and live
connection results.

In another terminal, inspect `status.json`: look for `XML_ACTIVE`, then confirm
`events_received` increases when the server sends an event authorized for your
identity. Validation, `XML_ACTIVE`, and `credentials_sent` alone do not prove
server acceptance; `credentials_sent=false` is normal for certificate-only
authentication. For `BACKOFF`, inspect `failure` and `reconnects`; check the
endpoint, CA, certificate hostname, credentials, and server permissions.

For separate PEM credentials, configure the TLS input through `--set`:

```sh
build/src/tak-gateway run \
  --set 'inputs[0].id="tak"' --set 'inputs[0].kind="tls"' \
  --set 'inputs[0].host="tak.example.com"' --set 'inputs[0].port=8089' \
  --set 'inputs[0].uid="gateway-reader"' \
  --set 'inputs[0].protocol="xml-only"' \
  --set 'inputs[0].tls.ca_file="credentials/ca.pem"' \
  --set 'inputs[0].tls.cert_file="credentials/client.pem"' \
  --set 'inputs[0].tls.key_file="credentials/client.key"' \
  --output local=tcp://127.0.0.1:9000 --status-file status.json
```

Confirm the endpoint supports XML; `8089` is an example port. Paths resolve
from the working directory when no TOML file is supplied. On POSIX, protect
private keys, bundles and password files with `chmod 600`. For an encrypted key,
append `--set 'inputs[0].tls.key_password_file="credentials/key-password"'`.
To use PKCS#12, replace the three PEM settings above with:

```sh
--set 'inputs[0].tls.ca_file="credentials/ca.pem"' \
--set 'inputs[0].tls.pkcs12_file="credentials/client.p12"' \
--set 'inputs[0].tls.pkcs12_password_file="credentials/bundle-password"'
```

For streaming username/password authentication, append:

```sh
--set 'inputs[0].auth.mode="password"' \
--set 'inputs[0].auth.username="reader"' \
--set 'inputs[0].auth.password_file="credentials/password"'
```

Passwords belong in the referenced files. Streaming credentials are separate
from certificate enrollment and REST login; real-server password authorization
remains [unverified](docs/testing.md#placement-and-maintenance).
See the [API reference](docs/api.md#version-1-configuration-schema) for
environment-variable alternatives and TLS `server_name` overrides, and the
[output framing contract](docs/api.md#delivery-and-stream-boundaries) for reading
TCP records. Ctrl-C stops intake and allows a two-second drain.

For a local TAK Server and gateway stack, see the
[Docker Compose quickstart](docker/README.md).

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
