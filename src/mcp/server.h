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
  // Channel mode (stdio): also push this agent's events (new requests on what
  // it holds, answers, locks handed to it, requests done) into the session as
  // notifications/claude/channel, so a Claude Code session started with
  // channels enabled is woken by them. Other clients ignore the notifications.
  void enable_channel(bool on) { channel_ = on; }
  // Handles one JSON-RPC message; returns null for notifications.
  nlohmann::json handle(const nlohmann::json& msg);
  // Newline-delimited JSON-RPC over the given streams until EOF.
  void serve(std::istream& in, std::ostream& out);
  static nlohmann::json tool_list();

 private:
  nlohmann::json call_tool(const std::string& name, const nlohmann::json& args);
  Repo& repo_;
  bool channel_ = false;
};

}  // namespace hs
