// mcp/http.hpp -- Streamable HTTP 传输层（header-only，Boost.Beast + Boost.Asio）
//
// 形态：单一 POST /mcp 端点；每连接一个 C++20 协程；keep-alive 复用。
//   GET  /health  -> 200 "ok"（无鉴权，容器/负载均衡探针）
//   POST /mcp     -> 交由 handler（鉴权通过后）
//   GET|DELETE /mcp -> 405（无会话可终止，不开服务器流）
//   OPTIONS      -> CORS 预检
//
// 依赖：仅 Boost 头文件（Beast/Asio），不链接任何 Boost 编译库，支持 musl 全静态链接。
#ifndef MCP_HTTP_HPP
#define MCP_HTTP_HPP

#include <mcp/server.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mcp::http {

/// 传输层看到的请求（已去掉 HTTP 细节，便于单测）。
struct request {
  std::string method;
  std::string target;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  std::string header(std::string_view name) const {
    for (const auto& kv : headers) {
      if (kv.first.size() == name.size() &&
          std::equal(kv.first.begin(), kv.first.end(), name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
          }))
        return kv.second;
    }
    return {};
  }
};

/// 传输层要写的响应。
struct response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  bool keep_alive = true;

  static response text(int status_code, std::string body_text) {
    return response{status_code, "text/plain", std::move(body_text), {}, true};
  }
};

using request_handler = std::function<response(const request&)>;
/// 返回 false 时以 401 应答。nullptr 表示不做鉴权。
using auth_checker = std::function<bool(const request&)>;

struct server_config {
  std::string address = "0.0.0.0";
  unsigned short port = 8080;
  unsigned threads = 1;                                   // io_context 线程数
  std::size_t body_limit = 4u * 1024 * 1024;              // 请求体上限 4 MB
  std::chrono::seconds header_timeout{30};                // 请求头/请求体读取超时
  std::chrono::seconds write_timeout{60};                 // 响应写超时
  std::string mcp_path = "/mcp";
  std::string health_path = "/health";
  bool cors = true;
};

/// 单线程（或 N 线程）io_context + 每连接一协程的 HTTP/1.1 服务器。
class server {
 public:
  server(server_config cfg, request_handler handler, auth_checker auth = nullptr,
         logger log = nullptr)
      : cfg_(std::move(cfg)), handler_(std::move(handler)), auth_(std::move(auth)),
        log_(std::move(log)) {}

  /// 阻塞运行，直到收到 SIGINT/SIGTERM 或调用 stop()。
  void run() {
    boost::asio::io_context ioc{static_cast<int>(cfg_.threads < 1 ? 1 : cfg_.threads)};
    ioc_ = &ioc;

    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(cfg_.address, ec);
    if (ec) throw std::runtime_error("invalid listen address: " + cfg_.address);

    auto acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(
        ioc, boost::asio::ip::tcp::endpoint{addr, cfg_.port});
    acceptor_ = acceptor.get();

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([this, &ioc](const boost::system::error_code&, int signum) {
      log("signal " + std::to_string(signum) + " received, shutting down");
      boost::system::error_code ignored;
      if (acceptor_) acceptor_->close(ignored);
      ioc.stop();
    });

    boost::asio::co_spawn(ioc, listener(*acceptor), boost::asio::detached);

    log("listening on " + cfg_.address + ":" + std::to_string(cfg_.port) +
        " (mcp=" + cfg_.mcp_path + ", health=" + cfg_.health_path + ")");
    std::cout << "mcp server listening on " << cfg_.address << ":" << cfg_.port << std::endl;

    const unsigned n = cfg_.threads < 1 ? 1 : cfg_.threads;
    std::vector<std::thread> pool;
    pool.reserve(n - 1);
    for (unsigned i = 1; i < n; ++i) pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();
    for (auto& t : pool) t.join();

    log("event loop stopped");
    std::cout << "mcp server stopped" << std::endl;
    ioc_ = nullptr;
    acceptor_ = nullptr;
  }

  /// 从其它线程请求停止。
  void stop() {
    if (!ioc_) return;
    boost::system::error_code ignored;
    if (acceptor_) acceptor_->close(ignored);
    ioc_->stop();
  }

 private:
  void log(const std::string& msg) const {
    if (log_) {
      log_(msg);
      return;
    }
    std::cerr << "[mcp-http] " << msg << std::endl;
  }

  static std::string path_of(std::string_view target) {
    const auto pos = target.find('?');
    return std::string(pos == std::string_view::npos ? target : target.substr(0, pos));
  }

  boost::asio::awaitable<void> listener(boost::asio::ip::tcp::acceptor& acc) {
    auto executor = co_await boost::asio::this_coro::executor;
    for (;;) {
      boost::asio::ip::tcp::socket socket(executor);
      boost::system::error_code ec;
      co_await acc.async_accept(socket, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec) {
        if (ec == boost::asio::error::operation_aborted) co_return;  // 优雅停机
        log("accept failed: " + ec.message());
        continue;
      }
      boost::asio::co_spawn(executor, session(std::move(socket)), boost::asio::detached);
    }
  }

  boost::asio::awaitable<void> session(boost::asio::ip::tcp::socket socket) {
    namespace beast = boost::beast;
    namespace http = boost::beast::http;

    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;

    try {
      for (;;) {
        http::request_parser<http::string_body> parser;
        parser.body_limit(cfg_.body_limit);

        stream.expires_after(cfg_.header_timeout);
        boost::system::error_code ec;
        co_await http::async_read(
            stream, buffer, parser,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;  // 连接断开、超时或报文不合法：静默关闭

        const auto& msg = parser.get();
        request req;
        req.method = std::string(msg.method_string());
        req.target = std::string(msg.target());
        req.body = msg.body();
        for (const auto& field : msg)
          req.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));

        const response rsp = route(req);
        const bool keep_alive = rsp.keep_alive;

        http::response<http::string_body> res{http::int_to_status(static_cast<unsigned>(rsp.status)),
                                              msg.version()};
        res.set(http::field::server, "mcp-cpp-sdk/1.0");
        if (!rsp.content_type.empty()) res.set(http::field::content_type, rsp.content_type);
        for (const auto& kv : rsp.headers) res.set(kv.first, kv.second);
        if (cfg_.cors) {
          const std::string origin = req.header("Origin");
          res.set(http::field::access_control_allow_origin, origin.empty() ? "*" : origin);
          res.set(http::field::access_control_allow_credentials, "false");
          res.set(http::field::access_control_expose_headers, "Mcp-Session-Id, MCP-Protocol-Version");
        }
        res.keep_alive(keep_alive);
        res.body() = rsp.body;
        res.prepare_payload();

        stream.expires_after(cfg_.write_timeout);
        co_await http::async_write(
            stream, res, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;

        if (!keep_alive) break;
      }
    } catch (const std::exception& e) {
      // FR-13：连接级异常不得逃逸进程。
      log("connection error: " + std::string(e.what()));
    } catch (...) {
      log("connection error: unknown exception");
    }

    boost::system::error_code ignored;
    stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
    stream.expires_never();
    co_return;
  }

  response route(const request& req) const {
    const std::string path = path_of(req.target);

    // FR-8：健康检查不复用 GET /mcp，且不受鉴权约束。
    if (path == cfg_.health_path && (req.method == "GET" || req.method == "HEAD"))
      return response::text(200, "ok");

    if (path == cfg_.mcp_path) {
      if (req.method == "OPTIONS") return preflight(req);
      if (req.method == "POST") {
        // FR-7：鉴权钩子作用于 POST 本身，不作用于预检与健康检查。
        if (auth_ && !auth_(req)) {
          return {401, "application/json",
                  mcp::rpc::make_error(nullptr, mcp::rpc::kUnauthorized,
                                       "Unauthorized: missing or invalid Authorization header")
                      .dump(),
                  {{"WWW-Authenticate", "Bearer realm=\"mcp\""}}, true};
        }
        return handler_ ? handler_(req)
                        : response::text(500, "no request handler configured");
      }
      // FR-11：无会话可终止、不开服务器流。
      return response{405, "text/plain", "Method Not Allowed", {{"Allow", "POST, OPTIONS"}}, true};
    }

    return response::text(404, "Not Found");
  }

  response preflight(const request& req) const {
    const std::string requested_headers = req.header("Access-Control-Request-Headers");
    response rsp;
    rsp.status = 204;
    rsp.content_type.clear();
    rsp.headers = {
        {"Allow", "POST, OPTIONS"},
        {"Access-Control-Allow-Methods", "POST, OPTIONS"},
        {"Access-Control-Allow-Headers",
         requested_headers.empty()
             ? "Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version"
             : requested_headers},
        {"Access-Control-Max-Age", "86400"},
    };
    if (!cfg_.cors) rsp.status = 405;
    return rsp;
  }

  server_config cfg_;
  request_handler handler_;
  auth_checker auth_;
  logger log_;
  boost::asio::io_context* ioc_ = nullptr;
  boost::asio::ip::tcp::acceptor* acceptor_ = nullptr;
};

/// Bearer token 校验器；token 为空时放行（并在构造时不打印 token 本身）。
inline auth_checker bearer_auth(std::string expected_token) {
  const std::string prefix = "Bearer ";
  return [token = std::move(expected_token), prefix](const request& req) -> bool {
    if (token.empty()) return true;
    const std::string header = req.header("Authorization");
    if (header.size() <= prefix.size()) return false;
    if (header.compare(0, prefix.size(), prefix) != 0) return false;
    return header.compare(prefix.size(), std::string::npos, token) == 0;
  };
}

}  // namespace mcp::http

#endif  // MCP_HTTP_HPP
