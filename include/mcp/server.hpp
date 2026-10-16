// mcp/server.hpp -- 与传输层解耦的 MCP 协议核心（header-only）
//
// 职责：协议版本协商、工具注册表、JSON-RPC 分发。
// 非职责：HTTP / TLS / 鉴权（分别在 http.hpp 与部署层）。
//
// 线程安全（NFR-3）：工具注册必须在开始服务前完成；服务期间注册表只读，
// 多连接并发调用 handle() 安全。handler 自身的线程安全责任在使用者。
#ifndef MCP_SERVER_HPP
#define MCP_SERVER_HPP

#include <mcp/jsonrpc.hpp>
#include <mcp/types.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

/// 一次请求的处理结果：状态码 + 内容类型 + 响应体。传输层据此生成 HTTP 报文。
struct dispatch_result {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

/// 日志回调（默认写 stderr，遵循 12-factor）。
using logger = std::function<void(std::string_view)>;

class server {
 public:
  using tool_handler = std::function<tool_result(const json& params)>;

  explicit server(server_info info = {}, std::string instructions = {})
      : info_(std::move(info)), instructions_(std::move(instructions)) {}

  // -- 注册（服务启动前调用） ------------------------------------------------

  void register_tool(tool t, tool_handler handler) {
    if (t.name.empty()) throw std::invalid_argument("tool name must not be empty");
    entry e{std::move(t), std::move(handler)};
    auto [it, inserted] = entries_.emplace(e.meta.name, std::move(e));
    if (!inserted) throw std::invalid_argument("duplicate tool name: " + it->first);
  }

  void register_tool(std::string name, std::string description, json input_schema,
                     tool_handler handler) {
    tool t;
    t.name = std::move(name);
    t.description = std::move(description);
    t.input_schema = std::move(input_schema);
    register_tool(std::move(t), std::move(handler));
  }

  std::size_t tool_count() const noexcept { return entries_.size(); }

  /// 当前注册的工具清单（注册顺序不保证，按名称排序输出以保证可重现）。
  std::vector<tool> tools() const {
    std::vector<tool> out;
    out.reserve(entries_.size());
    for (const auto& [name, e] : entries_) out.push_back(e.meta);
    std::sort(out.begin(), out.end(),
              [](const tool& a, const tool& b) { return a.name < b.name; });
    return out;
  }

  // -- 配置 ----------------------------------------------------------------

  void set_logger(logger l) { logger_ = std::move(l); }
  void set_instructions(std::string s) { instructions_ = std::move(s); }
  const server_info& info() const noexcept { return info_; }

  // -- 协议入口 ------------------------------------------------------------

  /// 处理一个请求体（单条 JSON-RPC 消息），返回可直接写入 HTTP 的结果。
  dispatch_result handle(std::string_view body) const {
    if (body.empty()) {
      return {400, "application/json",
              rpc::make_error(nullptr, rpc::kInvalidRequest, "Empty request body").dump()};
    }

    json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
      return {400, "application/json",
              rpc::make_error(nullptr, rpc::kParseError, "Parse error").dump()};
    }

    // MCP 明确禁用 JSON-RPC batch。
    if (parsed.is_array()) {
      return {400, "application/json",
              rpc::make_error(nullptr, rpc::kInvalidRequest,
                              "Batch requests are not supported by MCP")
                  .dump()};
    }
    if (!parsed.is_object()) {
      return {400, "application/json",
              rpc::make_error(nullptr, rpc::kInvalidRequest, "Invalid Request").dump()};
    }

    auto req = rpc::parse(parsed);
    if (!req) {
      json id = nullptr;
      if (auto i = parsed.find("id"); i != parsed.end()) id = *i;
      return {400, "application/json",
              rpc::make_error(std::move(id), rpc::kInvalidRequest, "Invalid Request").dump()};
    }

    // 通知：不产生 JSON-RPC 响应。
    if (!req->has_id) {
      log("notification: " + req->method);
      return {202, "text/plain", std::string()};
    }

    json response = dispatch(*req);
    // 协议层错误（method not found / invalid params）一律 HTTP 200；
    // 仅语法与结构错误在上文以 400 返回。
    return {200, "application/json", response.dump()};
  }

  /// 分发一条带 id 的请求，返回完整的 JSON-RPC 响应对象。
  json dispatch(const rpc::request& req) const {
    if (req.method == "initialize") return on_initialize(req);
    if (req.method == "ping") return rpc::make_result(req.id, json::object());
    if (req.method == "tools/list") return on_tools_list(req);
    if (req.method == "tools/call") return on_tools_call(req);
    // 扩展点：resources/list、prompts/list、logging/setLevel ...
    return rpc::make_error(req.id, rpc::kMethodNotFound, "Method not found: " + req.method);
  }

 private:
  struct entry {
    tool meta;
    tool_handler handler;
  };

  void log(const std::string& msg) const {
    if (logger_) {
      logger_(msg);
      return;
    }
    std::ostringstream os;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << " [mcp] " << msg;
    std::cerr << os.str() << std::endl;
  }

  json on_initialize(const rpc::request& req) const {
    std::string requested;
    if (req.params.is_object()) {
      if (auto it = req.params.find("protocolVersion"); it != req.params.end() && it->is_string())
        requested = it->get<std::string>();
    }
    const std::string negotiated = negotiate_protocol_version(requested);

    json result = json::object();
    result["protocolVersion"] = negotiated;
    result["capabilities"] = capabilities{};
    result["serverInfo"] = info_;
    if (!instructions_.empty()) result["instructions"] = instructions_;

    log("initialize: requested='" + requested + "' negotiated='" + negotiated + "'");
    return rpc::make_result(req.id, std::move(result));
  }

  json on_tools_list(const rpc::request& req) const {
    json result = json::object();
    result["tools"] = tools();
    return rpc::make_result(req.id, std::move(result));
  }

  json on_tools_call(const rpc::request& req) const {
    if (!req.params.is_object())
      return rpc::make_error(req.id, rpc::kInvalidParams, "params must be an object");

    auto name_it = req.params.find("name");
    if (name_it == req.params.end() || !name_it->is_string())
      return rpc::make_error(req.id, rpc::kInvalidParams, "params.name must be a string");

    const std::string name = name_it->get<std::string>();
    json arguments = json::object();
    if (auto a = req.params.find("arguments"); a != req.params.end() && a->is_object())
      arguments = *a;

    auto it = entries_.find(name);
    if (it == entries_.end())
      return rpc::make_error(req.id, rpc::kInvalidParams, "Unknown tool: " + name);

    try {
      tool_result tr = it->second.handler(arguments);
      if (tr.content.empty() && !tr.is_error)
        tr.content.push_back(content_block::text_block(std::string()));
      json result;
      to_json(result, tr);
      return rpc::make_result(req.id, std::move(result));
    } catch (const std::exception& e) {
      // 协议内错误：仍为 result，但 isError = true。
      log("tool '" + name + "' failed: " + e.what());
      json result;
      to_json(result, tool_result::error(std::string("tool execution failed: ") + e.what()));
      return rpc::make_result(req.id, std::move(result));
    } catch (...) {
      log("tool '" + name + "' failed with unknown exception");
      json result;
      to_json(result, tool_result::error("tool execution failed: unknown error"));
      return rpc::make_result(req.id, std::move(result));
    }
  }

  server_info info_;
  std::string instructions_;
  std::unordered_map<std::string, entry> entries_;
  logger logger_;
};

}  // namespace mcp

#endif  // MCP_SERVER_HPP
