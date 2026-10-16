// mcp/mcp.hpp -- 聚合头：包含这一头文件即可使用整个 SDK。
//
//   #include <mcp/mcp.hpp>
//
// 目录：
//   mcp/jsonrpc.hpp  JSON-RPC 2.0 消息模型与错误码
//   mcp/types.hpp    MCP 数据类型、协议版本协商
//   mcp/server.hpp   协议核心（注册表 + 分发），与传输层解耦
//   mcp/http.hpp     Streamable HTTP 传输层（Boost.Beast + Boost.Asio 协程）
#ifndef MCP_MCP_HPP
#define MCP_MCP_HPP

#include <mcp/http.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/server.hpp>
#include <mcp/types.hpp>

/// SDK 版本。
namespace mcp {
inline constexpr std::string_view sdk_version = "1.0.0";
inline constexpr std::string_view sdk_name = "mcp-cpp-sdk";
}

#endif  // MCP_MCP_HPP
