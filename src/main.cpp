// 示例 MCP server（SDK 使用范例）：echo + add 两个工具，Bearer token 鉴权。
//
// 环境变量：
//   MCP_HOST           监听地址（默认 0.0.0.0）
//   MCP_PORT           监听端口（默认 8080）
//   MCP_THREADS        io_context 线程数（默认 1，注册表只读故并发安全）
//   MCP_SERVER_NAME    serverInfo.name（默认 mcp-cpp-example）
//   MCP_SERVER_VERSION serverInfo.version（默认 1.0.0）
//   MCP_AUTH_TOKEN     Bearer token；为空则关闭鉴权（仅限可信内网）
//   MCP_BODY_LIMIT     请求体上限字节数（默认 4194304 = 4MB）

#include <mcp/mcp.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using mcp::json;

namespace {

std::string env_or(const char* key, std::string fallback) {
  if (const char* v = std::getenv(key)) {
    if (*v) return std::string(v);
  }
  return fallback;
}

unsigned env_uint(const char* key, unsigned fallback) {
  const std::string v = env_or(key, "");
  if (v.empty()) return fallback;
  try {
    return static_cast<unsigned>(std::stoul(v));
  } catch (const std::exception&) {
    return fallback;
  }
}

std::string format_number(const json& v) {
  if (v.is_number_integer()) return std::to_string(v.get<std::int64_t>());
  if (v.is_number_unsigned()) return std::to_string(v.get<std::uint64_t>());
  const double d = v.get<double>();
  if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15)
    return std::to_string(static_cast<std::int64_t>(d));
  std::ostringstream os;
  os << d;
  return os.str();
}

double require_number(const json& params, const char* key) {
  auto it = params.find(key);
  if (it == params.end() || !it->is_number())
    throw std::invalid_argument(std::string("argument '") + key + "' must be a number");
  return it->get<double>();
}

mcp::json echo_schema() {
  return mcp::json::object({
      {"type", "object"},
      {"properties", mcp::json::object({
          {"text", mcp::json::object({{"type", "string"},
                                      {"description", "Text to echo back"}})},
      })},
      {"required", mcp::json::array({"text"})},
  });
}

mcp::json add_schema() {
  return mcp::json::object({
      {"type", "object"},
      {"properties", mcp::json::object({
          {"a", mcp::json::object({{"type", "number"}, {"description", "First addend"}})},
          {"b", mcp::json::object({{"type", "number"}, {"description", "Second addend"}})},
      })},
      {"required", mcp::json::array({"a", "b"})},
  });
}

}  // namespace

int main() {
  mcp::server_info info{env_or("MCP_SERVER_NAME", "mcp-cpp-example"),
                        env_or("MCP_SERVER_VERSION", "1.0.0")};

  mcp::server core(info, "Example MCP server built with the header-only C++ SDK.");
  core.register_tool("echo", "Echo the provided text back to the caller.", echo_schema(),
                     [](const json& params) {
                       auto it = params.find("text");
                       if (it == params.end() || !it->is_string())
                         throw std::invalid_argument("argument 'text' must be a string");
                       return mcp::tool_result::text(it->get<std::string>());
                     });
  core.register_tool("add", "Add two numbers and return the sum.", add_schema(),
                     [](const json& params) {
                       const double a = require_number(params, "a");
                       const double b = require_number(params, "b");
                       return mcp::tool_result::text(format_number(json(a + b)));
                     });

  const std::string token = env_or("MCP_AUTH_TOKEN", "");
  if (token.empty()) {
    std::cerr << "[mcp] WARNING: MCP_AUTH_TOKEN is empty, authentication is DISABLED"
              << std::endl;
  }

  mcp::http::server_config cfg;
  cfg.address = env_or("MCP_HOST", "0.0.0.0");
  cfg.port = static_cast<unsigned short>(env_uint("MCP_PORT", 8080));
  cfg.threads = env_uint("MCP_THREADS", 1);
  cfg.body_limit = static_cast<std::size_t>(env_uint("MCP_BODY_LIMIT", 4 * 1024 * 1024));

  mcp::http::server transport(
      cfg,
      [&core](const mcp::http::request& req) {
        const mcp::dispatch_result r = core.handle(req.body);
        mcp::http::response out;
        out.status = r.status;
        out.content_type = r.content_type;
        out.body = r.body;
        return out;
      },
      mcp::http::bearer_auth(token));

  try {
    transport.run();
  } catch (const std::exception& e) {
    std::cerr << "[mcp] fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
