# D-1：多阶段构建 —— alpine 编译 musl 全静态二进制 → alpine 运行阶段仅含该二进制
ARG ALPINE_VERSION=3.21

# ------------------------------- 构建阶段 -----------------------------------
FROM alpine:${ALPINE_VERSION} AS builder

# build-base: gcc/g++/make；boost-dev: Beast/Asio 头文件；nlohmann-json: JSON 头文件
# （C-5：系统包已提供依赖时，构建期无需网络访问）
RUN apk add --no-cache \
      build-base \
      cmake \
      linux-headers \
      boost-dev \
      nlohmann-json \
      git

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

# NFR-2：全静态链接，运行阶段不需要任何共享库
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DMCP_STATIC_LINK=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
 && cmake --build build --parallel "$(nproc)" \
 && strip build/mcp_server

# ------------------------------- 运行阶段 -----------------------------------
FROM alpine:${ALPINE_VERSION}

RUN addgroup -g 10001 -S mcp \
 && adduser -u 10001 -S -G mcp -H -D -s /sbin/nologin mcp

COPY --from=builder /src/build/mcp_server /usr/local/bin/mcp_server

# NFR-5：非 root 运行
USER 10001:10001
EXPOSE 8080

ENV MCP_HOST=0.0.0.0 \
    MCP_PORT=8080 \
    MCP_THREADS=1 \
    MCP_SERVER_NAME=mcp-cpp-example \
    MCP_SERVER_VERSION=1.0.0

# D-2：健康检查走 GET /health（无鉴权）
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/mcp_server"]
