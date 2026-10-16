// mcp/types.hpp -- MCP 数据类型与版本协商（header-only）
//
// 基线：MCP 2026-07-28 最小合规子集（无状态线协议，无 Mcp-Session-Id / Last-Event-ID）。
#ifndef MCP_TYPES_HPP
#define MCP_TYPES_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace mcp {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// 协议版本协商
// ---------------------------------------------------------------------------

/// 本 SDK 支持的最新协议版本。
inline constexpr std::string_view kLatestProtocolVersion = "2026-07-28";

/// 支持的协议版本列表（按优先级从高到低）。
inline const std::vector<std::string>& supported_protocol_versions() {
  static const std::vector<std::string> versions{"2026-07-28", "2025-11-25"};
  return versions;
}

/// 请求版本在支持列表内则回显，否则回落至最新支持版本（空串同样回落）。
inline std::string negotiate_protocol_version(std::string_view requested) {
  for (const auto& v : supported_protocol_versions()) {
    if (requested == v) return v;
  }
  return std::string(kLatestProtocolVersion);
}

// ---------------------------------------------------------------------------
// 内容块 / 工具结果
// ---------------------------------------------------------------------------

/// MCP content block。最小子集只实现 text 块；image / resource 以 type + 原始字段扩展。
struct content_block {
  std::string type = "text";
  std::string text;
  std::string mime_type;  // 可选，非空时输出 mimeType

  static content_block text_block(std::string t) { return content_block{"text", std::move(t), {}}; }
};

inline void to_json(json& j, const content_block& c) {
  j = json::object();
  j["type"] = c.type;
  j["text"] = c.text;
  if (!c.mime_type.empty()) j["mimeType"] = c.mime_type;
}

inline void from_json(const json& j, content_block& c) {
  if (j.contains("type")) c.type = j.at("type").get<std::string>();
  if (j.contains("text")) c.text = j.at("text").get<std::string>();
  if (j.contains("mimeType")) c.mime_type = j.at("mimeType").get<std::string>();
}

/// tools/call 的结果。handler 抛异常时由 server 层置 is_error = true。
struct tool_result {
  std::vector<content_block> content;
  bool is_error = false;
  /// 可选的 structuredContent 扩展点（null 表示不输出该字段）。
  json structured_content = nullptr;

  static tool_result text(std::string t) {
    tool_result r;
    r.content.push_back(content_block::text_block(std::move(t)));
    return r;
  }
  static tool_result error(std::string message) {
    tool_result r;
    r.content.push_back(content_block::text_block(std::move(message)));
    r.is_error = true;
    return r;
  }
};

inline void to_json(json& j, const tool_result& r) {
  j = json::object();
  j["content"] = r.content;
  j["isError"] = r.is_error;
  if (!r.structured_content.is_null()) j["structuredContent"] = r.structured_content;
}

// ---------------------------------------------------------------------------
// 工具描述
// ---------------------------------------------------------------------------

/// tools/list 中返回的工具元数据。input_schema 为 JSON Schema 对象。
struct tool {
  std::string name;
  std::string description;
  json input_schema = json::object({{"type", "object"}, {"properties", json::object()}});
  std::string title;               // 可选
  json output_schema = nullptr;    // 可选扩展点
  json annotations = nullptr;      // 可选扩展点
};

inline void to_json(json& j, const tool& t) {
  j = json::object();
  j["name"] = t.name;
  if (!t.title.empty()) j["title"] = t.title;
  j["description"] = t.description;
  j["inputSchema"] = t.input_schema.is_null() ? json::object() : t.input_schema;
  if (!t.output_schema.is_null()) j["outputSchema"] = t.output_schema;
  if (!t.annotations.is_null()) j["annotations"] = t.annotations;
}

// ---------------------------------------------------------------------------
// 服务端信息 / 能力
// ---------------------------------------------------------------------------

struct server_info {
  std::string name = "mcp-cpp-server";
  std::string version = "1.0.0";
};

inline void to_json(json& j, const server_info& i) {
  j = json::object();
  j["name"] = i.name;
  j["version"] = i.version;
}

/// capabilities：最小子集仅声明 tools，且不做 listChanged 通知（无状态、注册表只读）。
struct capabilities {
  bool tools_list_changed = false;
  bool logging = false;
};

inline void to_json(json& j, const capabilities& c) {
  j = json::object();
  j["tools"] = json::object({{"listChanged", c.tools_list_changed}});
  if (c.logging) j["logging"] = json::object();
}

}  // namespace mcp

#endif  // MCP_TYPES_HPP
