# mcp-cpp-sdk

纯 C++（C++20）实现的 **MCP server SDK** 与示例服务：**100% header-only**，仅复用 Boost 头文件
（Beast/Asio）与 nlohmann/json，除 Boost 外零第三方编译依赖；支持 **Streamable HTTP remote
transport**，以 Docker 容器形态交付，经前置反向代理（Caddy）通过宿主机公网 IP 提供 HTTPS 访问。

协议基线：MCP **2026-07-28** 最小合规子集（无状态线协议：无 `Mcp-Session-Id`、无 `Last-Event-ID`
恢复），向下兼容协商 2025-11-25。

---

## 1. 目录结构

| 路径 | 说明 |
|---|---|
| `include/mcp/jsonrpc.hpp` | JSON-RPC 2.0 消息模型与错误码 |
| `include/mcp/types.hpp` | MCP 数据类型、协议版本协商 |
| `include/mcp/server.hpp` | 协议核心：工具注册表 + 请求分发（与传输层解耦） |
| `include/mcp/http.hpp` | Streamable HTTP 传输层（Beast + Asio 协程） |
| `include/mcp/mcp.hpp` | 聚合头，使用者只需 `#include <mcp/mcp.hpp>` |
| `src/main.cpp` | 示例 server：`echo` / `add` 工具 + Bearer 鉴权 |
| `CMakeLists.txt` | 构建脚本（`find_package` 优先、FetchContent 兜底） |
| `Dockerfile` | 多阶段构建：alpine 编译 musl 全静态二进制 → 极简运行镜像 |
| `docker-compose.yml` | 编排：`mcp-server`（不映射公网）+ `caddy`（唯一 443 入口） |
| `Caddyfile` | TLS 终结、关闭响应缓冲 |
| `.env.example` | 凭据模板（复制为 `.env`，不入库、不进镜像） |
| `scripts/smoke_test.sh` | 本地验收脚本，覆盖验收标准 A-1 ~ A-8 |
| `spec.md` | 需求规格说明书 |

## 2. 本地构建与运行

依赖：C++20 编译器（gcc 11+ / clang 14+）、CMake 3.20+、Boost 1.75+ 头文件、`nlohmann_json` 3.11+
（系统无该包时 CMake 自动 FetchContent 拉取，需要网络）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

运行示例服务（无 token 时会打印告警并关闭鉴权）：

```bash
MCP_HOST=127.0.0.1 MCP_PORT=8080 MCP_AUTH_TOKEN=test-token-123 ./build/mcp_server
```

冒烟测试（A-1 ~ A-8 + CORS/keep-alive 断言，共 37 条）：

```bash
./scripts/smoke_test.sh build/mcp_server
# passed: 37   failed: 0
```

手工验证：

```bash
curl -s http://127.0.0.1:8080/health                       # 200 ok（无鉴权）
curl -s -X POST http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer test-token-123' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add","arguments":{"a":40,"b":2}}}'
# {"id":1,"jsonrpc":"2.0","result":{"content":[{"text":"42","type":"text"}],"isError":false}}
```

## 3. SDK 用法（最少代码）

```cpp
#include <mcp/mcp.hpp>

int main() {
  const char* token_env = std::getenv("MCP_AUTH_TOKEN");
  mcp::server core({"my-server", "1.0.0"}, "optional instructions");

  core.register_tool(
      "add", "Add two numbers.",
      mcp::json::object({{"type", "object"},
                         {"properties", mcp::json::object({
                             {"a", {{"type", "number"}}},
                             {"b", {{"type", "number"}}}})},
                         {"required", mcp::json::array({"a", "b"})}}),
      [](const mcp::json& args) {
        return mcp::tool_result::text(std::to_string(args.at("a").get<double>() +
                                                     args.at("b").get<double>()));
      });

  mcp::http::server_config cfg;   // 0.0.0.0:8080, 4MB body, 30s/60s 超时
  mcp::http::server transport(
      cfg,
      [&core](const mcp::http::request& req) {          // MCP 业务处理
        const auto r = core.handle(req.body);
        return mcp::http::response{r.status, r.content_type, r.body, {}, true};
      },
      mcp::http::bearer_auth(token_env ? token_env : ""));           // 鉴权钩子

  transport.run();                // 阻塞；SIGINT/SIGTERM 时优雅返回
}
```

要点：

- **注册必须在 `run()` 之前完成**；服务期间注册表只读，多连接并发分发安全（NFR-3）。
- handler 抛异常（含 `std::exception` 派生）时，SDK 返回 `result.isError = true` 的**协议内错误**，
  而非 JSON-RPC error；未知工具名才是 `-32602`。
- 默认单线程 `io_context`；`cfg.threads = N` 时由 N 个线程驱动同一 `io_context`。

## 4. HTTP 行为

| 请求 | 响应 |
|---|---|
| `POST /mcp` | `application/json`；鉴权通过后交给 `mcp::server::handle()` |
| `POST /mcp` 鉴权失败 | `401` + `WWW-Authenticate: Bearer realm="mcp"` + JSON-RPC `-32001` |
| 通知（无 `id`） | `202` + 空 body |
| 坏 JSON | `400` + `-32700` |
| 批量（array）/ 非法结构 | `400` + `-32600`（MCP 禁用 batch） |
| 未知方法 | `200` + `-32601` |
| `GET /health`、`HEAD /health` | `200 "ok"`（**不受鉴权约束**，供容器/负载均衡探针） |
| `GET /mcp`、`DELETE /mcp` | `405`（无会话可终止、不开服务器流） |
| `OPTIONS *` | CORS 预检：`204` + `Allow: POST, OPTIONS`；鉴权仍作用于 POST 本身 |
| 其它路径 | `404` |

传输参数（FR-12）：HTTP/1.1 keep-alive 复用；请求头读取超时 30s、响应写超时 60s、请求体上限 4 MB。
每连接一个 C++20 协程，连接级异常（断开/超时/非法报文）在协程内捕获，不会逃逸进程。

## 5. 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `MCP_HOST` | `0.0.0.0` | 监听地址 |
| `MCP_PORT` | `8080` | 监听端口 |
| `MCP_THREADS` | `1` | `io_context` 线程数 |
| `MCP_SERVER_NAME` / `MCP_SERVER_VERSION` | `mcp-cpp-example` / `1.0.0` | `serverInfo` |
| `MCP_AUTH_TOKEN` | 空 | Bearer token；**为空则关闭鉴权**（仅限可信内网） |
| `MCP_BODY_LIMIT` | `4194304` | 请求体上限（字节） |

## 6. Docker 部署（公网 IP + HTTPS）

架构：

```
公网 MCP 客户端 ──HTTPS──▶ caddy:443（TLS 终结，唯一映射公网）
                              │ docker 网络 mcpnet
                              ▼
                        mcp-server:8080（仅 expose）
```

步骤：

```bash
cp .env.example .env && vi .env        # 设置 MCP_AUTH_TOKEN
docker compose up -d --build
docker compose ps                      # mcp-server 应为 healthy
```

- 宿主机安全组**仅放行 443**（HTTP/3 需要同时放行 443/udp）。
- 无域名时使用 Caddy 自签 CA（`tls internal`，需 Caddy 2.7+ 的 `default_sni` 兜住
  「客户端直连 IP、不带 SNI」的场景）；客户端需跳过校验或信任 CA：
  `docker compose cp mcp-caddy:/data/caddy/pki/authorities/local/root.crt ./root.crt`。
- 有域名：Caddyfile 站点地址改成域名（删除 `:443` 通配项与 `default_sni`）并删除 `tls internal`
  一行，Caddy 自动申请公信证书。
- 验证：`curl -k https://<公网IP>/mcp -H "Authorization: Bearer $TOKEN" -d '{...}'`

运维基线（已在 compose 中）：`restart: unless-stopped`、`stop_grace_period: 15s`、日志
json-file 10m×3、CPU/内存限额、只读 rootfs + tmpfs、`cap_drop: ALL` + `no-new-privileges`、
非 root（uid 10001）。

水平扩展（D-8，无状态线协议，无需粘性会话）：

```bash
docker compose up -d --scale mcp-server=3
```

> 已在本地 Docker 环境实测：`docker compose up -d --build` 后 `mcp-server` 呈现 healthy，
> `curl -k https://127.0.0.1/health` → `ok`、`POST https://127.0.0.1/mcp` → `42`、
> 无 token → `401`、`GET /mcp` → `405`；`--scale mcp-server=3` 三副本均 healthy 且请求正常分发。
> 目标宿主机上还需确认安全组放行 443 并使客户端信任自签 CA。

## 7. 安全与合规提示

- TLS 由前置反向代理终结，SDK 内不做 TLS（N-1）。
- 仅提供 Bearer token 钩子，无 OAuth / 多租户（N-2）。
- 不开服务器主动事件流，`GET /mcp` 返回 405，仍是合规形态（N-3）。
- 未实现 sampling / elicitation / resources / prompts，接口留扩展点（N-4）。
- 纯 JSON 响应即合规；SSE 以扩展点形式保留（N-5）。
