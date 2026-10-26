# Run TAK Server and Gateway with Docker Compose

This local stack runs the official TAK Server 5.7-RELEASE-14 fixture, PostGIS,
and TAK Gateway. It builds the server with
[`Dockerfile.source`](../tests/tak_server/Dockerfile.source) and the gateway with
[`Dockerfile`](Dockerfile). Use native Linux, Docker Engine, Compose v2, and
BuildKit, with at least 6 GiB available for runtime plus source-build resources.
The initial build downloads and compiles pinned dependencies. CMake runs inside
the gateway builder; host commands use Docker Compose.

The gateway image entrypoint optionally initializes a new persistent configuration,
then runs the gateway from that saved store. Environment variables are initial
values, not repeated overrides: after initialization, GUI changes survive
container restart and recreation.

From the repository root as a non-root user:

```sh
mkdir -p docker/state docker/credentials
chmod 700 docker/state docker/credentials
export TAK_GATEWAY_UID="$(id -u)" TAK_GATEWAY_GID="$(id -g)"
docker compose -f docker/compose.yaml config --quiet
docker compose -f docker/compose.yaml build takserver gateway
docker compose -f docker/compose.yaml up -d --wait --wait-timeout 600
```

Initialization creates fresh passwords and server PKI, verifies authenticated
REST readiness, and exports an encrypted client key, its password, client
certificate, and trust CA to the gateway. Server signing keys remain in the
server volume. Gateway startup seeds a verified TLS connection to
`takserver:8089`, a TCP output, and the GUI in the persistent store.

Open `http://127.0.0.1:8080`. Add or edit TAK connections in the browser by ZIP
upload or manual configuration. The GUI and TCP consumer ports are published on the
host's localhost only. Inside the container the listener binds `0.0.0.0` so
Docker can forward those ports; use the same bind for a browser-created TCP
output intended for host forwarding.

`docker/state` is a writable persistent bind mount owned by your configured
UID/GID. Keep it private: it contains configuration revisions, client keys and
password files. Generated server credentials are mounted read-only at
`/run/credentials`. `docker/credentials` is mounted read-only at `/run/imports`
for optional administrator-provided files. The root filesystem remains
read-only; temporary imports and status use private `/run/tak-gateway` memory.

## Optional first-boot environment

| Variable | Purpose |
| --- | --- |
| `TAK_GATEWAY_STATE_DIR` | Store selection, default `/var/lib/tak-gateway`; applies on every invocation. |
| `TAK_GATEWAY_CONFIG_FILE` | TOML base to import once, using a container path. |
| `TAK_GATEWAY_IMPORT_ZIP` | ATAK package to import once, using a container path. |
| `TAK_GATEWAY_OUTPUTS` | One `ID=URI` output per line. |
| `TAK_GATEWAY_SET` | One `PATH=TOML_VALUE` assignment per line; Compose defaults to the local server input. |
| `TAK_GATEWAY_WEB_BIND` | Enable the management interface at this address. |
| `TAK_GATEWAY_STATUS_FILE` | Initial status-file location. |
| `TAK_GATEWAY_LOG_LEVEL` | Initial logging level. |
| `TAK_GATEWAY_LOG_FORMAT` | Initial logging format. |

The entrypoint builds one quoted `config --if-uninitialized` invocation from
these values. It does not evaluate shell expressions. Skip empty lines in the
multiline lists; use a TOML file for values needing embedded newlines. Passwords
belong in protected files or referenced environment variables, not literal
password schema fields.

For example, to seed an ATAK package instead of the local server connection,
put it at `docker/credentials/server.zip` before the first start and set:

```sh
export TAK_GATEWAY_SET=
export TAK_GATEWAY_IMPORT_ZIP=/run/imports/server.zip
docker compose -f docker/compose.yaml up -d
```

An explicitly empty `TAK_GATEWAY_SET` disables the default local-server seed.
When importing a TOML base, also clear or replace this default so it does not
replace that file's inputs. The combined stack still starts its local server.

The guarded initializer ignores changed or stale seed settings when a store
already exists. Edit that store through the GUI. To use the offline CLI, stop
the gateway first and run the administrative command against the same mounts:

```sh
docker compose -f docker/compose.yaml stop gateway
docker compose -f docker/compose.yaml run --rm --no-deps gateway \
  config --log-level debug
docker compose -f docker/compose.yaml up -d
```

Explicit `config`, `validate`, `redact` and help commands bypass automatic
environment initialization. The entrypoint finishes with `exec`, so stop
signals reach the foreground gateway and allow its ordinary drain period.

Changing seed variables after initialization does not edit saved settings.
For a separate initial deployment, use a new state directory; retain the old
directory if its configuration or credentials are still needed.

```sh
docker compose -f docker/compose.yaml exec -T gateway \
  cat /run/tak-gateway/status.json
docker compose -f docker/compose.yaml down
```

Consumers use [u32be XML framing](../docs/api.md#delivery-and-stream-boundaries)
at `127.0.0.1:9000` for the initial output. A TLS-active state alone does not
prove server authorization; inspect the GUI's connection evidence and
authorized event counts.

Server streaming and REST ports are published on loopback at `8089` and `8443`.
Set `TAK_GATEWAY_PORT`, `TAK_SERVER_TLS_PORT`, or `TAK_SERVER_REST_PORT` before
startup to change the host TCP-output or server ports. For startup failures,
inspect `docker compose -f docker/compose.yaml ps -a` and the failing service's
logs. Look for `XML_ACTIVE` and `protocol_offer_received=true`; confirm
`events_received` increases when the server sends authorized traffic.

`down` retains both the named server/database volumes and the gateway's bind
mounted state. To reset the local server, including its database, PKI, and
passwords, use `docker compose -f docker/compose.yaml down --volumes`. Before
starting again, move `docker/state` to a private backup and create a fresh
`docker/state` directory with mode `700`, so the gateway initializes against
the new server credentials. Keep a backup if saved connections or uploaded
credentials are still needed.

This reuses the development [server fixture](../tests/tak_server/README.md);
it does not provide a production TAK Server or WebTAK deployment.
