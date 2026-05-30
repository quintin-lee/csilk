FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake gcc g++ git make \
    libyaml-dev zlib1g-dev libcurl4-openssl-dev \
    libssl-dev libsqlite3-dev libpq-dev \
    pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . .

RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && \
    cmake --install build --prefix /usr/local

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libyaml-0-2 zlib1g libcurl4 libssl3t64 libsqlite3-0 libpq5 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libcsilk.a /usr/local/lib/
COPY --from=builder /usr/local/include /usr/local/include
COPY --from=builder /build/build/example_server /usr/local/bin/example_server
COPY --from=builder /build/config_multi.yaml /etc/csilk/config.yaml

RUN useradd -m -s /bin/bash csilk && \
    mkdir -p /var/log/csilk /var/lib/csilk && \
    chown -R csilk:csilk /var/log/csilk /var/lib/csilk

USER csilk
WORKDIR /var/lib/csilk

EXPOSE 8080 8443

CMD ["example_server", "/etc/csilk/config.yaml"]
