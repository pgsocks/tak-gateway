# Run TAK Gateway with Docker Compose

This deployment runs the gateway against existing TAK servers. Use Docker Engine,
Compose v2, and BuildKit. The image builds the gateway and its pinned dependencies
from source; CMake runs inside the builder.

From the repository root as a non-root user:

```sh
mkdir -p docker/state docker/credentials
chmod 700 docker/state docker/credentials
export TAK_GATEWAY_UID="$(id -u)" TAK_GATEWAY_GID="$(id -g)"
docker compose -f docker/compose.yaml config --quiet
docker compose -f docker/compose.yaml up -d --build
```

Open `http://127.0.0.1:8080` and add an existing server through ATAK ZIP upload or
manual configuration. The initial configuration has no inputs and one TCP output
at `127.0.0.1:9000`, using [u32be XML framing](../docs/api.md#delivery-and-stream-boundaries).
The server's hostname must be reachable from inside the container.

Both published ports bind to the host's localhost. Inside the container, keep the
management listener and published TCP outputs bound to `0.0.0.0` so Docker can
forward traffic. Set `TAK_GATEWAY_PORT` before startup to change the host TCP-output
port; the container still listens on port `9000`.

`docker/state` holds persistent configuration and uploaded credentials, owned by
the selected UID/GID. Keep this directory private. `docker/credentials` mounts
read-only at `/run/imports` for administrator-provided files; private keys and
password files need mode `600` and must be readable by the selected UID/GID. The
root filesystem is read-only; imports and status use private `/run/tak-gateway`
memory.

## First-boot configuration

The entrypoint initializes an empty store once, then runs its saved configuration.
GUI changes survive container restarts and recreation. Environment variables are
initial values; changing them later does not edit an existing store.

For an initial ATAK package import, place it at `docker/credentials/server.zip`
before the first start:

```sh
export TAK_GATEWAY_IMPORT_ZIP=/run/imports/server.zip
docker compose -f docker/compose.yaml up -d
```

Other initial settings use the following variables:

| Variable | Purpose |
| --- | --- |
| `TAK_GATEWAY_CONFIG_FILE` | TOML base to import, using a container path such as `/run/imports/gateway.toml`. |
| `TAK_GATEWAY_IMPORT_ZIP` | ATAK package to import, using a container path. |
| `TAK_GATEWAY_OUTPUTS` | One `ID=URI` output per line; defaults to `local=tcp://0.0.0.0:9000`. |
| `TAK_GATEWAY_SET` | One `PATH=TOML_VALUE` assignment per line; empty by default. |
| `TAK_GATEWAY_WEB_BIND` | Management listener; defaults to `0.0.0.0:8080`. |
| `TAK_GATEWAY_STATUS_FILE` | Initial status-file location; Compose sets `/run/tak-gateway/status.json`. |
| `TAK_GATEWAY_LOG_LEVEL` | Initial logging level. |
| `TAK_GATEWAY_LOG_FORMAT` | Initial logging format. |

An explicitly empty `TAK_GATEWAY_OUTPUTS` skips the default output, for example
when an imported TOML file supplies every output. Relative credential paths in
that TOML resolve beside the imported file. Keep referenced files available at
their mounted paths. ZIP uploads copy their credentials into the persistent store.

The entrypoint passes these values to one quoted `config --if-uninitialized`
invocation and skips empty lines in multiline lists. Use a TOML file for values
needing embedded newlines. Passwords belong in protected files or referenced
environment variables. See the [CLI guide](../docs/cli.md) for configuration rules.

## Saved settings and shutdown

Use the GUI to edit a running gateway. For offline CLI changes, stop it first:

```sh
docker compose -f docker/compose.yaml stop gateway
docker compose -f docker/compose.yaml run --rm gateway config --log-level debug
docker compose -f docker/compose.yaml up -d
```

Explicit `config`, `validate`, `redact`, and help commands bypass environment
initialization. `TAK_GATEWAY_STATE_DIR` selects the store on every image invocation
and defaults to `/var/lib/tak-gateway`; Compose mounts its state directory there.
The entrypoint uses `exec`, allowing stop signals to reach the gateway and its
ordinary drain period.

```sh
docker compose -f docker/compose.yaml exec -T gateway cat /run/tak-gateway/status.json
docker compose -f docker/compose.yaml down
```

`down` retains the bind-mounted state. For startup failures, inspect
`docker compose -f docker/compose.yaml logs gateway`.

When migrating from the former bundled-server stack, stop its services using the
previous Compose definition and retain any needed server data and credentials.
Saved gateway inputs that reference generated `/run/credentials` files need to be
reconfigured for an existing server, through the GUI or an offline CLI edit.
