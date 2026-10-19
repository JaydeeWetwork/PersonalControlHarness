// MCP wiring self-test (P2c 2026-10-13, review F-M9): the closest
// automated check to the live `--mcp-stdio` smoke that this environment
// can run. The shipped Release binary refuses to boot AT ALL in this
// tree (the baked mod-signing key is the retired test key; main.cpp's
// release guard exits before any game code) and the Debug tree cannot
// rebuild here (missing libxcursor-dev headers for GLFW), so the literal
// "launch the binary and pipe JSON-RPC" smoke is not exercisable — that
// is a pre-existing environment fact, not an MCP gap.
//
// What this DOES exercise, in-process, against the REAL Game (headless
// harness fixture) and the REAL engine McpServer:
//   1. Game::mcpBackends() fills the controller/player/vision/audio seams
//      from the game's live adapters (non-null, right objects).
//   2. Game::setMcpServer() accepts the server (the grant/revoke console
//      commands register on that call — the registry itself is private,
//      so registration success is checked via the call not failing; the
//      grant EFFECT is checked through the server's mask in step 4).
//   3. A full JSON-RPC round trip through McpServer::handleMessage:
//      initialize handshake, then tools/list after the initialized
//      notification — the same dispatch the stdio thread runs.
//   4. The permission flow end to end at the server level: a gated tool
//      call (player view) is denied at mask 0, then
//      grantPermissions(kPermWorldRead) — exactly what the console
//      command calls — makes the same call succeed against the game's
//      real player adapter.
//   5. A scripted StdioTransport round trip through runStdio(): the
//      transport loop the shipped thread runs, fed lines instead of fds.
//
// NOT exercised (needs the live binary): the poll/fd transport and
// stdout redirection in main.cpp, the window/event loop running
// concurrently with the server thread, and the console command handler
// itself (private registry; its body is a thin parse over the same
// grantPermissions call checked in step 4).
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "headless_harness.h"
#include "lithokraft/game/game.h"
#include "litholattice/mcp/server.h"
#include "litholattice/mods/mod_interface.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  std::printf("[%s] mcp-wiring: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

struct ScriptedTransport final : litholattice::mcp::StdioTransport {
  std::vector<std::string> in;
  std::vector<std::string> out;
  size_t next = 0;
  bool readLine(std::string& line) override {
    if (next >= in.size()) return false;
    line = in[next++];
    return true;
  }
  void writeLine(std::string_view line) override { out.emplace_back(line); }
};
}  // namespace

int main() {
  using namespace lithokraft;
  namespace mcp = litholattice::mcp;

  HeadlessGame hg;
  Game& game = hg.game();
  check(game.ok(), "headless game constructs");

  // 1. Backends from the game's adapters.
  mcp::McpBackends backends = game.mcpBackends();
  check(backends.controller && backends.player && backends.vision && backends.audio,
        "mcpBackends fills controller/player/vision/audio");
  check(backends.sim == nullptr && backends.world == nullptr && backends.writer == nullptr,
        "unwired seams stay null (honest 'not wired')");

  // 2. Server + setMcpServer.
  mcp::McpServer server(game.mcpBackends());
  game.setMcpServer(&server);
  check(server.permissionMask() == 0, "agent starts deny-by-default (mask 0)");

  // 3. JSON-RPC round trip: initialize, initialized, tools/list.
  const auto init = server.handleMessage(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"wiring-test","version":"0"}}})");
  check(init && init->find("\"result\"") != std::string::npos &&
            init->find("litholattice") != std::string::npos,
        "initialize round trip returns the server identity");
  server.handleMessage(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  const auto tools = server.handleMessage(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
  check(tools && tools->find("player_look") != std::string::npos, "tools/list names the player-control tools");

  // 4. Gating: player_get_state is backed by the game's real player view
  // and gated on world_read. Denied at mask 0; granted -> real snapshot.
  const auto denied = server.handleMessage(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"player_get_state","arguments":{}}})");
  check(denied && denied->find("isError") != std::string::npos &&
            denied->find("world_read") != std::string::npos,
        "gated tool denied at mask 0, naming the missing bit");
  server.grantPermissions(kPermWorldRead);  // what `mcp grant world_read` calls
  check(server.permissionMask() == uint32_t(kPermWorldRead), "grant flips the mask");
  const auto granted = server.handleMessage(
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"player_get_state","arguments":{}}})");
  check(granted && granted->find("isError\":true") == std::string::npos &&
            granted->find("selectedSlot") != std::string::npos,
        "granted tool returns the game's real player snapshot");
  server.revokePermissions(kPermWorldRead);
  check(server.permissionMask() == 0, "revoke clears the mask");

  // 5. runStdio over a scripted transport (the thread's loop, minus fds).
  {
    mcp::McpServer stdioServer(game.mcpBackends());
    ScriptedTransport t;
    t.in = {R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"stdio-test","version":"0"}}})",
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
            R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})"};
    stdioServer.runStdio(t);
    check(t.out.size() == 2 && t.out[1].find("tools") != std::string::npos,
          "runStdio answers initialize + tools/list over a transport");
  }

  game.setMcpServer(nullptr);
  std::printf("MCP WIRING SELFTEST: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
  return failures;
}
