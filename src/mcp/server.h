#pragma once
// MCP (Model Context Protocol) server for a hashstash project. The tool set is
// the agent-facing API: browse, query the graph, lock, edit, test, commit.
// hs mcp serves it over stdio; hshub will serve the same tools over HTTP.
#include <nlohmann/json.hpp>

#include <istream>
#include <ostream>

#include "ops/repo.h"

namespace hs {

class McpServer {
 public:
  explicit McpServer(Repo& repo) : repo_(repo) {}
  // Handles one JSON-RPC message; returns null for notifications.
  nlohmann::json handle(const nlohmann::json& msg);
  // Newline-delimited JSON-RPC over the given streams until EOF.
  void serve(std::istream& in, std::ostream& out);
  static nlohmann::json tool_list();

 private:
  nlohmann::json call_tool(const std::string& name, const nlohmann::json& args);
  Repo& repo_;
};

}  // namespace hs
