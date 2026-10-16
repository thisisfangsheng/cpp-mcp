// mcp/jsonrpc.hpp -- JSON-RPC 2.0 消息模型（header-only）
//
// 只覆盖 MCP Streamable HTTP 所需的最小子集：
//   * 请求 / 响应 / 错误对象的构造与解析
//   * MCP 禁用 batch，因此本层不提供批量编解码
#ifndef MCP_JSONRPC_HPP
#define MCP_JSONRPC_HPP

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

using json = nlohmann::json;

namespace rpc {

/// JSON-RPC 2.0 标准错误码。
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;

/// HTTP 层鉴权失败使用的实现自定义错误码（JSON-RPC 预留区间 -32000..-32099）。
inline constexpr int kUnauthorized = -32001;

inline constexpr std::string_view kVersion = "2.0";

/// 一条 JSON-RPC 请求。`has_id == false` 表示通知（notification）。
struct request {
  json id = nullptr;
  bool has_id = false;
  std::string method;
  json params = nullptr;
};

/// 从已解析的 JSON 对象中构造请求；不符合请求结构时返回 nullopt。
/// 对 `jsonrpc` 字段采取宽松策略：存在则必须为 "2.0"，缺失时按请求处理。
inline std::optional<request> parse(const json& j) {
  if (!j.is_object()) return std::nullopt;

  if (auto v = j.find("jsonrpc"); v != j.end()) {
    if (!v->is_string() || v->get_ref<const std::string&>() != kVersion) return std::nullopt;
  }

  auto method = j.find("method");
  if (method == j.end() || !method->is_string() || method->get_ref<const std::string&>().empty())
    return std::nullopt;

  request r;
  r.method = method->get<std::string>();

  if (auto p = j.find("params"); p != j.end()) {
    if (!(p->is_object() || p->is_array() || p->is_null())) return std::nullopt;
    r.params = *p;
  }

  if (auto i = j.find("id"); i != j.end()) {
    r.id = *i;
    r.has_id = true;
  }
  return r;
}

/// 构造成功响应（result 为空对象时序列化为 {}）。
inline json make_result(json id, json result) {
  json r = json::object();
  r["jsonrpc"] = std::string(kVersion);
  r["id"] = id.is_null() ? json(nullptr) : std::move(id);
  r["result"] = result.is_null() ? json::object() : std::move(result);
  return r;
}

/// 构造错误响应。data 为 null 时不写入该字段。
inline json make_error(json id, int code, std::string message, json data = nullptr) {
  json err = json::object();
  err["code"] = code;
  err["message"] = std::move(message);
  if (!data.is_null()) err["data"] = std::move(data);

  json r = json::object();
  r["jsonrpc"] = std::string(kVersion);
  r["id"] = id.is_null() ? json(nullptr) : std::move(id);
  r["error"] = std::move(err);
  return r;
}

}  // namespace rpc
}  // namespace mcp

#endif  // MCP_JSONRPC_HPP
