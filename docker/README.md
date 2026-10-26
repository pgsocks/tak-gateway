# Run TAK Server and Gateway with Docker Compose

This local stack runs the official TAK Server 5.7-RELEASE-14 fixture, PostGIS,
and TAK Gateway. It builds the server with
[`Dockerfile.source`](../tests/tak_server/Dockerfile.source) and the gateway with
[`Dockerfile`](Dockerfile). Use native Linux, Docker Engine, Compose v2, and
BuildKit, with at least 6 GiB available for runtime plus source-build resources.
The initial build downloads and compiles pinned dependencies. CMake runs inside
the gateway builder; host commands use Docker Compose.

From the repository root as a non-root user:

```sh
export TAK_GATEWAY_UID="$(id -u)" TAK_GATEWAY_GID="$(id -g)"
docker compose -f docker/compose.yaml build takserver gateway
docker compose -f docker/compose.yaml up -d --wait --wait-timeout 600
docker compose -f docker/compose.yaml exec -T gateway \
  cat /run/tak-gateway/status.json
```

The gateway receives its input, output, and status settings as
[CLI arguments](../docs/cli.md); no TOML file is required.

Initialization creates fresh passwords and server PKI, verifies authenticated
REST readiness, and gives the gateway an encrypted client key, its password,
client certificate, and trust CA. The gateway connects to `takserver:8089` over
verified TLS. Its credentials are mounted read-only under a non-root UID;
server signing keys remain in the server volume.

Look for `XML_ACTIVE` and `protocol_offer_received=true` in status. Confirm
`events_received` increases when the server sends authorized traffic; active
state alone does not prove delivery. For startup failures, inspect
`docker compose -f docker/compose.yaml ps -a` and the failing service's logs.

Consumers connect to `127.0.0.1:9000` using
[u32be XML framing](../docs/api.md#delivery-and-stream-boundaries). Server
streaming and REST ports are published on loopback at `8089` and `8443`.
Set `TAK_GATEWAY_PORT`, `TAK_SERVER_TLS_PORT`, or `TAK_SERVER_REST_PORT` before
startup to change the host ports.

Stop the stack while retaining its database, PKI, and passwords:

```sh
docker compose -f docker/compose.yaml down
```

Restart with the same `up` command. To reset the entire local stack, including
all database contents, certificates, and passwords:

```sh
docker compose -f docker/compose.yaml down --volumes
```

This reuses the development [server fixture](../tests/tak_server/README.md);
it does not provide a production TAK Server or WebTAK deployment.
