ARG UBUNTU_IMAGE=ubuntu:noble-20260810

FROM ${UBUNTU_IMAGE} AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 构建层显式使用 GCC 与 C++17，避免宿主机工具链影响容器产物。
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        cmake \
        g++ \
        libboost-system-dev \
        libdrogon-dev \
        libpq-dev \
        libsqlite3-dev \
        libssl-dev \
        libyaml-cpp-dev \
        ninja-build \
        nlohmann-json3-dev \
    && apt-get clean

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -G Ninja \
        -DBUILD_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER=gcc \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    && cmake --build /build --target labbridge_server labbridge_agent \
    && grep -q -- '-std=c++17' /build/compile_commands.json \
    && ! grep -q -- '-std=c++20' /build/compile_commands.json

FROM ${UBUNTU_IMAGE} AS server-runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libdrogon1t64 \
        libpq5 \
        libssl3t64 \
        libyaml-cpp0.8 \
    && apt-get clean \
    && groupadd --gid 10001 labbridge \
    && useradd --uid 10001 --gid labbridge --no-create-home \
        --shell /usr/sbin/nologin labbridge \
    && install --directory --owner=labbridge --group=labbridge \
        /var/lib/labbridge/archive

COPY --from=builder /build/labbridge_server /usr/local/bin/labbridge_server
COPY deploy/demo/server.yaml /etc/labbridge/server.yaml

USER labbridge
WORKDIR /var/lib/labbridge
EXPOSE 18080
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/labbridge_server"]
CMD ["/etc/labbridge/server.yaml"]

FROM ${UBUNTU_IMAGE} AS agent-runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        libsqlite3-0 \
        libssl3t64 \
        libyaml-cpp0.8 \
    && apt-get clean \
    && groupadd --gid 10001 labbridge \
    && useradd --uid 10001 --gid labbridge --no-create-home \
        --shell /usr/sbin/nologin labbridge \
    && install --directory --owner=labbridge --group=labbridge \
        /var/lib/labbridge /var/lib/labbridge/work /demo/inbox

COPY --from=builder /build/labbridge_agent /usr/local/bin/labbridge_agent
COPY deploy/demo/agent.yaml /etc/labbridge/agent.yaml

USER labbridge
WORKDIR /var/lib/labbridge
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/labbridge_agent"]
CMD ["/etc/labbridge/agent.yaml"]

