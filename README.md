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
OpenSSL, c-ares, Expat, miniz, CivetWeb, yyjson, TOML parsing and the CoT codec
are acquired through
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

## Configure once, then run

Configure the gateway offline, then start it from the saved settings:

```sh
build/src/tak-gateway config --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 \
  --status-file status.json --web-bind 127.0.0.1:8080
build/src/tak-gateway run
```

Open `http://127.0.0.1:8080` to add or edit TAK connections and outputs,
upload an ATAK ZIP, or configure a connection manually. The GUI saves changes
and applies them without restarting unrelated connections or outputs. Its
connection details distinguish DNS, TCP, TLS, certificate and protocol errors.
A server that closes after credentials without an explanation is shown as
unconfirmed authentication, rather than a guessed password failure.

The web interface is optional and has no login. Prefer a localhost bind;
selecting a LAN/WAN address gives clients reaching that address control over
the gateway. To disable it offline, use:

```sh
build/src/tak-gateway config --set 'web.enabled=false'
```

`config` edits a persistent store and exits without connecting. `run` accepts
only the state-directory selector and loads that store. `validate` checks saved
references without connecting, and `redact` prints saved settings without
passwords or private-key references:

```sh
build/src/tak-gateway validate
build/src/tak-gateway redact
```

Select an independent instance with `--state-dir DIRECTORY`, or set
`TAK_GATEWAY_STATE_DIR`. The default is the platform's per-user state directory.
CLI edits require the gateway to be stopped; use the GUI for live changes.
An optional `config --config FILE` imports TOML as the new gateway base, with
subsequent options overriding its fields. Later runs use the saved copy.

See [CLI configuration](docs/cli.md), [web management](docs/web.md), and
[ATAK packages](docs/atak-import.md) for options, package structure and the
supplied package's live-connection results.

For separate administrator-provided PEM credentials, configure the input with
schema settings. Confirm the endpoint supports XML; `8089` is an example:

```sh
build/src/tak-gateway config \
  --set 'inputs[0].id="tak"' --set 'inputs[0].kind="tls"' \
  --set 'inputs[0].host="tak.example.com"' --set 'inputs[0].port=8089' \
  --set 'inputs[0].uid="gateway-reader"' \
  --set 'inputs[0].protocol="xml-only"' \
  --set 'inputs[0].tls.ca_file="credentials/ca.pem"' \
  --set 'inputs[0].tls.cert_file="credentials/client.pem"' \
  --set 'inputs[0].tls.key_file="credentials/client.key"' \
  --output local=tcp://127.0.0.1:9000 --status-file status.json
build/src/tak-gateway run
```

Relative paths resolve beside an explicitly imported TOML file, otherwise from
the configuration command's working directory, and are saved resolved. On
POSIX, protect private keys, bundles and password files with `chmod 600`.
For encrypted PEM, add
`--set 'inputs[0].tls.key_password_file="credentials/key-password"'`.
For PKCS#12, replace the three PEM settings with:

```sh
--set 'inputs[0].tls.ca_file="credentials/ca.pem"' \
--set 'inputs[0].tls.pkcs12_file="credentials/client.p12"' \
--set 'inputs[0].tls.pkcs12_password_file="credentials/bundle-password"'
```

For streaming username/password authentication, add:

```sh
--set 'inputs[0].auth.mode="password"' \
--set 'inputs[0].auth.username="reader"' \
--set 'inputs[0].auth.password_file="credentials/password"'
```

Passwords belong in protected files or referenced environment variables.
`XML_ACTIVE` and `credentials_sent` alone do not prove remote authorization;
look for protocol offers or authorized events. See the
[API reference](docs/api.md#version-1-configuration-schema) and
[output framing contract](docs/api.md#delivery-and-stream-boundaries).
Ctrl-C stops intake and allows a two-second drain.

For a local TAK Server and gateway stack with persistent settings, see the
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
