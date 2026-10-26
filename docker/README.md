# Connect TAK Gateway with Docker Compose

This runs only the gateway, connecting to an existing TAK Server over verified
TLS. Install Docker Engine with Compose v2 and BuildKit on the host. Compose
builds the image and starts the container; CMake and the compiler run inside
the image's build stage. The first build downloads the toolchain and pinned
source dependencies. Run these commands from the repository root as a non-root
user:

```sh
mkdir -p docker/credentials
cp docker/gateway.toml.example docker/gateway.toml
export TAK_GATEWAY_UID="$(id -u)" TAK_GATEWAY_GID="$(id -g)"
```

Place your administrator-provided `ca.pem`, `client.pem`, and `client.key` in
`docker/credentials/`. Edit `docker/gateway.toml` with a container-reachable
server hostname and XML-compatible streaming port (`8089` is an example).
`localhost` inside the container refers to the gateway. For an encrypted key,
put its passphrase in `docker/credentials/key-password` and uncomment
`key_password_file`. Protect private keys and password files with `chmod 600`;
the container uses your UID/GID to read them.

```sh
docker compose -f docker/compose.yaml build
```

Validate the configuration, then start the gateway and inspect its status:

```sh
docker compose -f docker/compose.yaml run --rm --no-deps gateway \
  validate --config /config/gateway.toml
docker compose -f docker/compose.yaml up -d
docker compose -f docker/compose.yaml exec -T gateway \
  cat /run/tak-gateway/status.json
```

Look for `XML_ACTIVE`, then an increasing `events_received` count when the
server sends authorized traffic. Local validation and active state alone do
not confirm server acceptance. For `BACKOFF`, inspect `failure`, `reconnects`,
and the server's trust, hostname, credentials, and permissions.

Consumers connect to `127.0.0.1:9000` using
[u32be XML framing](../docs/api.md#delivery-and-stream-boundaries). The output
binds `0.0.0.0` inside the container so Docker can forward that loopback host
port. Configuration and credentials are mounted read-only; status uses temporary
memory and disappears when the container stops.

For PKCS#12 or streaming passwords, adapt the
[authentication options](../README.md#authenticate-with-one-tak-server), using
credential paths under `/run/credentials/`. After edits, recreate the gateway
with `docker compose -f docker/compose.yaml up -d --force-recreate`.

```sh
docker compose -f docker/compose.yaml down
```
