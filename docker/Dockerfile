# syntax=docker/dockerfile:1
FROM debian:bookworm-slim AS build

RUN --mount=type=secret,id=proxy_ca \
    bundle=/etc/ssl/certs/ca-certificates.crt; \
    if [ -f /run/secrets/proxy_ca ]; then bundle=/run/secrets/proxy_ca; fi; \
    apt-get -o Acquire::https::CaInfo="$bundle" update && \
    apt-get -o Acquire::https::CaInfo="$bundle" install -y \
      --no-install-recommends build-essential ca-certificates cmake git \
      libssl-dev && \
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
cat > CMakeUserPresets.json <<'JSON'
{
  "version": 5,
  "configurePresets": [{
    "name": "container",
    "inherits": "no-tests",
    "cacheVariables": {
      "CMAKE_BUILD_TYPE": "Release",
      "CMAKE_TLS_CAINFO": "$env{SSL_CERT_FILE}",
      "TG_USE_SYSTEM_OPENSSL": true
    }
  }],
  "buildPresets": [{"name": "container", "configurePreset": "container"}],
  "testPresets": [{
    "name": "container",
    "inherits": "no-tests",
    "configurePreset": "container"
  }]
}
JSON
cmake --preset container
cmake --build --preset container --parallel 2
BUILD

FROM debian:bookworm-slim AS runtime
RUN --mount=type=secret,id=proxy_ca \
    bundle=/etc/ssl/certs/ca-certificates.crt; \
    if [ -f /run/secrets/proxy_ca ]; then bundle=/run/secrets/proxy_ca; fi; \
    apt-get -o Acquire::https::CaInfo="$bundle" update && \
    apt-get -o Acquire::https::CaInfo="$bundle" install -y \
      --no-install-recommends ca-certificates libssl3 libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/container/src/tak-gateway /usr/local/bin/tak-gateway
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/tak-gateway"]
CMD ["run", "--config", "/config/gateway.toml"]
