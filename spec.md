# C++ MCP Server SDK 需求规格说明书（Spec）

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0（2026-10-15） |
| 交付物代号 | mcp-cpp-sdk |
| 状态 | 已实现并通过本地验证（部署验收项待目标宿主机执行） |

---

## 1. 概述

构建一个**完全用 C++ 实现**的 MCP（Model Context Protocol）server SDK 及示例服务：

- 以 **远程传输（remote transport）** 方式对外提供服务，客户端可通过**宿主机公网 IP** 直接访问；
- 以 **Docker 容器** 形态交付部署；
- SDK 本体要求 **100% header-only**，复用已有的 **Boost** 资产，除 Boost 外零第三方编译依赖。

## 2. 目标与非目标

### 2.1 目标

| 编号 | 目标 |
|---|---|
| O-1 | 产出可复用的 C++ MCP server SDK（header-only 头文件集合 + 示例 server） |
| O-2 | 支持 MCP remote transport：Streamable HTTP（单端点 `POST /mcp`） |
| O-3 | 容器化后可通过宿主机公网 IP 经 HTTPS 访问（`https://<公网IP>/mcp`） |
| O-4 | 提供 docker compose 一键部署方案（应用容器 + TLS 反向代理） |

### 2.2 非目标（Non-goals，明确不做）

| 编号 | 内容 | 理由 |
|---|---|---|
| N-1 | SDK 内做 TLS 终结 | 全 header-only 的前提是无 OpenSSL；TLS 由前置反向代理负责 |
| N-2 | OAuth 2.1 / 多租户鉴权 | 仅要求 Bearer token 钩子；多租户场景留作后续扩展 |
| N-3 | 服务器主动事件流（GET /mcp 开 SSE） | 规范中为可选特性；不实现时以 405 应答，仍是合规形态 |
| N-4 | sampling / elicitation / resources / prompts 等扩展能力 | 最小可用优先，接口留扩展点 |
| N-5 | SSE 流式响应作为核心能力 | 2026-07-28 无状态线协议下，纯 JSON 响应即合规；SSE 以扩展点形式提供 |

## 3. 功能需求

### 3.1 协议层

| 编号 | 需求 | 优先级 |
|---|---|---|
| FR-1 | `initialize`：协议版本协商——客户端请求的版本若在支持列表（2026-07-28、2025-11-25）内则回显，否则回落至最新支持版本；返回 `capabilities`（tools.listChanged=false）与 `serverInfo` | 必须 |
| FR-2 | `ping`：返回空 result `{}` | 必须 |
| FR-3 | `tools/list`：返回工具清单，含 `name` / `description` / `inputSchema`（JSON Schema） | 必须 |
| FR-4 | `tools/call`：分发到注册的 handler；成功返回 `content`（text 块）；handler 抛异常 → `result.isError=true`（协议内错误）；未知工具名 → JSON-RPC 错误 `-32602` | 必须 |
| FR-5 | 通知（无 id 的消息）：一律以 **HTTP 202 + 空 body** 应答，不产生 JSON-RPC 响应 | 必须 |
| FR-6 | 错误路径：解析失败 → `-32700`（HTTP 400）；批量请求（array）/ 非法结构 → `-32600`（HTTP 400，MCP 禁用 batch）；未知方法 → `-32601`（HTTP 200） | 必须 |
| FR-7 | 鉴权钩子：支持按请求校验（如 `Authorization: Bearer <token>`），失败返回 401；token 通过环境变量注入，不硬编码 | 必须 |
| FR-8 | `GET /health`：无鉴权探针端点，返回 200（供容器/负载均衡健康检查使用；不得复用 GET /mcp） | 必须 |
| FR-9 | `OPTIONS` 预检：按 CORS 语义应答（浏览器直连场景），鉴权仍作用于 POST 本身 | 推荐 |

### 3.2 传输层

| 编号 | 需求 | 优先级 |
|---|---|---|
| FR-10 | 实现 Streamable HTTP：单一 `POST /mcp` 端点，请求/响应均为 `application/json` | 必须 |
| FR-11 | `GET /mcp`、`DELETE /mcp` → **405 Method Not Allowed**（无会话可终止、不开服务器流） | 必须 |
| FR-12 | HTTP/1.1 keep-alive 连接复用；请求头超时 30s、响应写超时 60s、请求体上限 4 MB | 必须 |
| FR-13 | 每连接一个 C++20 协程驱动；连接级异常（断开/超时）不得逃逸进程 | 必须 |
| FR-14 | 收到 SIGINT/SIGTERM 后停止接收并退出（容器优雅停机配合 `stop_grace_period`） | 必须 |

## 4. 非功能需求

| 编号 | 需求 |
|---|---|
| NFR-1 | **100% header-only**：SDK 协议层与传输层均为纯头文件；唯一编译产物是使用者的 main.cpp |
| NFR-2 | **零链接依赖**：不链接 OpenSSL、不链接任何 Boost 编译库；支持 musl 全静态链接，运行时容器内仅一个静态二进制 |
| NFR-3 | 并发模型：工具注册完成后注册表只读，多连接并发分发安全；handler 的线程安全责任在使用者（默认单线程 io_context，示例即可用） |
| NFR-4 | 日志走 stdout/stderr（12-factor），由容器日志驱动收集并轮转 |
| NFR-5 | 部署安全基线：非 root 运行（uid 10001）、只读 rootfs、资源限额、公网仅暴露反代的 443 |

## 5. 技术约束与选型

| 编号 | 约束 |
|---|---|
| C-1 | C++20（需协程支持）；gcc 11+ / clang 14+ |
| C-2 | 传输层使用 **Boost.Beast + Boost.Asio**（复用既有 Boost 资产）；协议层使用 **nlohmann/json**（header-only） |
| C-3 | 协议基线：**MCP 2026-07-28 规范**的最小合规子集（无状态线协议：无 `Mcp-Session-Id`、无 `Last-Event-ID` 恢复），向下兼容协商 2025-11-25 |
| C-4 | 构建系统 CMake 3.20+；依赖查找 `find_package` 优先、FetchContent 兜底 |
| C-5 | SDK 不引入构建期网络依赖即可编译（系统包提供依赖时） |

## 6. 部署架构需求

```
公网 MCP 客户端
      │  HTTPS  https://<宿主机公网IP>/mcp  (Authorization: Bearer)
      ▼
宿主机安全组：仅放行 443
┌────────────────────────────────────────────┐
│ caddy 容器 —— TLS 终结、flush_interval -1    │ ← 唯一映射公网的容器（443）
│        │ docker 内部网络                     │
│ mcp-server 容器 —— 监听 0.0.0.0:8080        │ ← 仅 expose，不映射公网
└────────────────────────────────────────────┘
```

| 编号 | 需求 |
|---|---|
| D-1 | 多阶段 Dockerfile：alpine 构建阶段编译 musl 全静态二进制 → alpine 运行阶段仅含该二进制 |
| D-2 | 容器以非 root 运行；`HEALTHCHECK` 调用 `GET /health`；`EXPOSE 8080` |
| D-3 | compose 编排：`mcp-server`（不映射公网端口）+ `caddy`（443 唯一入口）；健康检查通过后才接入流量 |
| D-4 | 反向代理必须关闭响应缓冲（`flush_interval -1`），为 SSE 扩展预留 |
| D-5 | 运维参数：`restart: unless-stopped`、`stop_grace_period: 15s`、日志轮转（10m×3）、内存/CPU 限额、只读 rootfs + tmpfs |
| D-6 | 凭据管理：token 走 `.env` → 环境变量注入，`.env` 不入库、不进镜像 |
| D-7 | 证书策略：无域名时用反代自签 CA（`tls internal`）；有域名时改一行 Caddyfile 自动签发公信证书 |
| D-8 | 无状态线协议 → 多副本水平扩展无需粘性会话（`--scale` 即可） |

## 7. 验收标准

A-1 ~ A-8 为协议/服务行为断言，**已在本地实测通过**；A-9 为部署验收项，需在具备 Docker 环境的目标宿主机执行。

| 编号 | 验收断言 | 状态 |
|---|---|---|
| A-1 | `initialize`（请求 2026-07-28）→ result.protocolVersion == "2026-07-28"，含 capabilities 与 serverInfo | ✅ 本地通过 |
| A-2 | notification 请求 → HTTP 202，空 body | ✅ 本地通过 |
| A-3 | `tools/list` → 返回全部注册工具及 inputSchema | ✅ 本地通过 |
| A-4 | `tools/call add {a:40,b:2}` → text "42"；echo 工具往返正确 | ✅ 本地通过 |
| A-5 | 未知方法 → -32601；未知工具 → -32602；坏 JSON → HTTP 400/-32700；batch → HTTP 400/-32600 | ✅ 本地通过 |
| A-6 | `GET /health` → 200 "ok"；`GET /mcp` → 405 | ✅ 本地通过 |
| A-7 | 无 token / 错误 token → 401；正确 token → 200 | ✅ 本地通过 |
| A-8 | SIGTERM → 进程输出关闭日志并退出（优雅停机） | ✅ 本地通过 |
| A-9 | 目标宿主机：`docker compose up -d --build` 后，经公网 IP + 安全组 443 可达 `/mcp`，健康检查通过 | ⏳ 待目标环境执行 |

## 8. 交付物清单

| 交付物 | 路径 |
|---|---|
| SDK 头文件（协议层 ×3、传输层 ×1、聚合头 ×1） | `include/mcp/{jsonrpc,types,server,http,mcp}.hpp` |
| 示例 server（echo/add 工具 + Bearer 鉴权） | `src/main.cpp` |
| 构建脚本 | `CMakeLists.txt` |
| 容器与编排 | `Dockerfile`、`docker-compose.yml`、`Caddyfile`、`.dockerignore` |
| 使用文档 | `README.md` |
| 本需求规格 | `SPEC.md` |

## 9. 参考

- MCP 规范 2026-07-28（Streamable HTTP transport；HTTP+SSE 自 2025-03 弃用、本版移除）
- Boost.Beast / Boost.Asio（header-only HTTP 与网络库，C++20 协程）
- nlohmann/json（header-only JSON 库）

---

### 附：演进记录（需求提出顺序）

1. 纯 C++ MCP server，支持 remote transport，可经宿主机公网 IP 访问，容器化部署；
2. 追加：采用全 header-only 技术方案（排除编译型 SDK）；
3. 追加：复用已有 Boost；产出形态为可复用 SDK（非一次性 server）；
4. 追加：Docker 部署为必须交付能力。
