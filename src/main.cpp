// 示例 MCP server（SDK 使用范例）：echo、add、analyze_image，Bearer token 鉴权。
//
// 环境变量：
//   MCP_HOST           监听地址（默认 0.0.0.0）
//   MCP_PORT           监听端口（默认 8080）
//   MCP_THREADS        io_context 线程数（默认 1，注册表只读故并发安全）
//   MCP_SERVER_NAME    serverInfo.name（默认 mcp-cpp-example）
//   MCP_SERVER_VERSION serverInfo.version（默认 1.0.0）
//   MCP_AUTH_TOKEN     Bearer token；为空则关闭鉴权（仅限可信内网）
//   MCP_BODY_LIMIT     请求体上限字节数（默认 16777216 = 16MB）
//   ARK_API_KEY        Ark API key；调用 analyze_image 时必需
//   ARK_MODEL          Ark 模型 ID（默认 doubao-seed-2-1-pro-260628）
//   ARK_PROXY          可选代理；设为 direct 可禁用 libcurl 的环境代理

#include <mcp/mcp.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using mcp::json;

namespace {

std::string env_or(const char* key, std::string fallback);

constexpr std::size_t kMaxImageBase64Size = 12u * 1024 * 1024;
constexpr std::size_t kMaxArkResponseSize = 8u * 1024 * 1024;

class curl_global_guard {
 public:
  curl_global_guard() {
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
      throw std::runtime_error(std::string("libcurl initialization failed: ") +
                               curl_easy_strerror(rc));
  }
  ~curl_global_guard() { curl_global_cleanup(); }
  curl_global_guard(const curl_global_guard&) = delete;
  curl_global_guard& operator=(const curl_global_guard&) = delete;
};

struct curl_easy_deleter {
  void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};

struct curl_headers {
  curl_slist* list = nullptr;
  ~curl_headers() { curl_slist_free_all(list); }

  void append(const std::string& value) {
    curl_slist* updated = curl_slist_append(list, value.c_str());
    if (!updated) throw std::bad_alloc();
    list = updated;
  }
};

void check_curl(CURLcode code, const char* operation) {
  if (code != CURLE_OK)
    throw std::runtime_error(std::string(operation) + ": " + curl_easy_strerror(code));
}

std::size_t append_response(char* data, std::size_t size, std::size_t count, void* context) {
  const std::size_t bytes = size * count;
  auto& response = *static_cast<std::string*>(context);
  if (bytes > kMaxArkResponseSize - response.size()) return 0;
  response.append(data, bytes);
  return bytes;
}

bool is_base64(std::string_view value) {
  if (value.empty() || value.size() % 4 != 0 || value.size() > kMaxImageBase64Size)
    return false;

  std::size_t padding = 0;
  if (value.back() == '=') ++padding;
  if (padding < value.size() && value[value.size() - padding - 1] == '=') ++padding;
  if (padding > 2) return false;

  for (std::size_t i = 0; i < value.size() - padding; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/'))
      return false;
  }
  for (std::size_t i = value.size() - padding; i < value.size(); ++i) {
    if (value[i] != '=') return false;
  }
  return true;
}

std::string required_string(const json& params, const char* key) {
  auto it = params.find(key);
  if (it == params.end() || !it->is_string() || it->get_ref<const std::string&>().empty())
    throw std::invalid_argument(std::string("argument '") + key + "' must be a non-empty string");
  return it->get<std::string>();
}

std::string optional_string(const json& params, const char* key, std::string fallback) {
  auto it = params.find(key);
  if (it == params.end()) return fallback;
  if (!it->is_string())
    throw std::invalid_argument(std::string("argument '") + key + "' must be a string");
  return it->get<std::string>();
}

std::string describe_image(const json& params) {
  const std::string api_key = env_or("ARK_API_KEY", "");
  if (api_key.empty()) throw std::runtime_error("ARK_API_KEY is not configured");

  const std::string image_base64 = required_string(params, "image_base64");
  if (!is_base64(image_base64))
    throw std::invalid_argument("image_base64 must be valid, single-line base64 (max 12 MiB)");

  const std::string mime_type = optional_string(params, "mime_type", "image/jpeg");
  if (mime_type != "image/jpeg" && mime_type != "image/png" && mime_type != "image/webp")
    throw std::invalid_argument("mime_type must be image/jpeg, image/png, or image/webp");
  const std::string prompt = optional_string(params, "prompt", "请详细描述这张图片");

  const json request_body = {
      {"model", env_or("ARK_MODEL", "doubao-seed-2-1-pro-260628")},
      {"messages", json::array({
                       json{{"role", "user"},
                            {"content", json::array({
                                            json{{"type", "text"}, {"text", prompt}},
                                            json{{"type", "image_url"},
                                                 {"image_url", {{"url", "data:" + mime_type +
                                                                           ";base64," +
                                                                           image_base64}}}},
                                        })}},
                   })},
  };
  const std::string serialized_request = request_body.dump();
  const std::string api_url = "https://ark.cn-beijing.volces.com/api/v3/chat/completions";

  std::unique_ptr<CURL, curl_easy_deleter> handle(curl_easy_init());
  if (!handle) throw std::runtime_error("libcurl could not create a request handle");

  curl_headers headers;
  headers.append("Content-Type: application/json");
  headers.append("Authorization: Bearer " + api_key);

  std::string response_body;
  char error_buffer[CURL_ERROR_SIZE] = {};
  auto setopt = [&handle](CURLoption option, auto value) {
    check_curl(curl_easy_setopt(handle.get(), option, value), "libcurl option failed");
  };
  setopt(CURLOPT_URL, api_url.c_str());
  setopt(CURLOPT_HTTPHEADER, headers.list);
  setopt(CURLOPT_POST, 1L);
  setopt(CURLOPT_POSTFIELDS, serialized_request.c_str());
  setopt(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(serialized_request.size()));
  setopt(CURLOPT_WRITEFUNCTION, &append_response);
  setopt(CURLOPT_WRITEDATA, &response_body);
  setopt(CURLOPT_ERRORBUFFER, error_buffer);
  setopt(CURLOPT_CONNECTTIMEOUT_MS, 15000L);
  setopt(CURLOPT_TIMEOUT_MS, 120000L);
  setopt(CURLOPT_NOSIGNAL, 1L);
  setopt(CURLOPT_FOLLOWLOCATION, 0L);

  const std::string proxy = env_or("ARK_PROXY", "");
  if (proxy == "direct") setopt(CURLOPT_PROXY, "");
  else if (!proxy.empty()) setopt(CURLOPT_PROXY, proxy.c_str());

  const CURLcode result = curl_easy_perform(handle.get());
  if (result != CURLE_OK) {
    const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
    throw std::runtime_error("Ark API request failed: " + detail);
  }

  long status = 0;
  check_curl(curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status),
             "could not read Ark API status");
  const json response = json::parse(response_body, nullptr, false);
  if (status < 200 || status >= 300) {
    const std::string detail = response_body.substr(0, 1024);
    throw std::runtime_error("Ark API returned HTTP " + std::to_string(status) +
                             (detail.empty() ? "" : ": " + detail));
  }
  if (response.is_discarded()) throw std::runtime_error("Ark API returned invalid JSON");

  auto choices = response.find("choices");
  if (choices == response.end() || !choices->is_array() || choices->empty())
    throw std::runtime_error("Ark API response has no choices");
  const auto& message = choices->at(0).at("message");
  const auto& content = message.at("content");
  if (content.is_string()) return content.get<std::string>();
  if (content.is_array()) {
    std::string text;
    for (const auto& part : content) {
      if (part.is_object()) {
        auto item = part.find("text");
        if (item != part.end() && item->is_string()) {
          if (!text.empty()) text += "\n";
          text += item->get<std::string>();
        }
      }
    }
    if (!text.empty()) return text;
  }
  throw std::runtime_error("Ark API response has no text content");
}

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

mcp::json analyze_image_schema() {
  return mcp::json::object({
      {"type", "object"},
      {"properties", mcp::json::object({
          {"image_base64", mcp::json::object({
                                {"type", "string"},
                                {"description", "Image bytes encoded as single-line base64"}})},
          {"mime_type", mcp::json::object({
                            {"type", "string"},
                            {"enum", mcp::json::array({"image/jpeg", "image/png", "image/webp"})},
                            {"description", "Image MIME type; defaults to image/jpeg"}})},
          {"prompt", mcp::json::object({
                          {"type", "string"},
                          {"description", "Question or instruction for the vision model"}})},
      })},
      {"required", mcp::json::array({"image_base64"})},
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
  core.register_tool("analyze_image",
                     "Describe or answer questions about a base64-encoded image using the Ark vision model.",
                     analyze_image_schema(), [](const json& params) {
                       return mcp::tool_result::text(describe_image(params));
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
  cfg.body_limit = static_cast<std::size_t>(env_uint("MCP_BODY_LIMIT", 16 * 1024 * 1024));

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
    curl_global_guard curl;
    transport.run();
  } catch (const std::exception& e) {
    std::cerr << "[mcp] fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
