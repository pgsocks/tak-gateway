# syntax=docker/dockerfile:1
FROM debian:trixie-slim AS build

RUN --mount=type=secret,id=proxy_ca \
    bundle=/etc/ssl/certs/ca-certificates.crt; \
    if [ -f /run/secrets/proxy_ca ]; then bundle=/run/secrets/proxy_ca; fi; \
    apt-get -o Acquire::https::CaInfo="$bundle" update && \
    apt-get -o Acquire::https::CaInfo="$bundle" install -y \
      --no-install-recommends build-essential ca-certificates cmake git perl && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/

RUN --mount=type=secret,id=proxy_ca <<'BUILD'
set -eu
bundle=/etc/ssl/certs/ca-certificates.crt
if [ -f /run/secrets/proxy_ca ]; then bundle=/run/secrets/proxy_ca; fi
export SSL_CERT_FILE="$bundle" GIT_SSL_CAINFO="$bundle"
cmake --preset no-tests
cmake --build --preset no-tests --parallel 2
BUILD

FROM debian:trixie-slim AS runtime
RUN --mount=type=secret,id=proxy_ca \
    bundle=/etc/ssl/certs/ca-certificates.crt; \
    if [ -f /run/secrets/proxy_ca ]; then bundle=/run/secrets/proxy_ca; fi; \
    apt-get -o Acquire::https::CaInfo="$bundle" update && \
    apt-get -o Acquire::https::CaInfo="$bundle" install -y \
      --no-install-recommends ca-certificates libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/no-tests/src/tak-gateway /usr/local/bin/tak-gateway
COPY --chmod=755 docker/entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN mkdir -p /var/lib/tak-gateway && \
    chown 1000:1000 /var/lib/tak-gateway && \
    chmod 700 /var/lib/tak-gateway
ENV TAK_GATEWAY_STATE_DIR=/var/lib/tak-gateway
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["run"]
