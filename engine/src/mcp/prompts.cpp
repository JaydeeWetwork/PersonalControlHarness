// prompts.cpp: Phase 3 prompt templates — agent_onboarding,
// build_contraption, diagnose. Clean-room; every line written for this file.
//
// Prompts are read-only templates (requiredPerms 0 = open): they render
// static guidance text grounded in the ACTUAL tool/resource names the
// server registers. They execute nothing and reveal no more than
// tools/list already does, so no permission gate is needed. The renderers
// substitute the validated arguments into the text.
#include "litholattice/mcp/server.h"

#include <map>
#include <string>

namespace litholattice::mcp {
namespace {

// Strict decimal-integer parse for the diagnose tick window.
bool parseTicks(const std::string& s, int64_t& out) {
  if (s.empty() || s.size() > 7) return false;
  int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v < 1 || v > 1000000) return false;
  out = v;
  return true;
}

std::string renderOnboarding(const std::map<std::string, std::string>& args) {
  const std::string name = args.at("name");
  return "You are " + name +
         ", an agent inside LithoKraft — a voxel game running on the "
         "LithoLattice engine. You talk to the game through this MCP server.\n"
         "\n"
         "HOW YOU SEE (query tools — all read-only, results are JSON text):\n"
         "- world_query_blocks: scan an inclusive box for one block id. "
         "Bounds are mandatory, the cell count is capped, order is "
         "deterministic (x, then y, then z).\n"
         "- world_get_block: the block id at one cell. Unloaded cells read "
         "as air (0).\n"
         "- entity_list / entity_get: entities within a radius of a point "
         "(radius mandatory, 1..512) or one entity by id.\n"
         "- player_get_state: the local player's position, velocity, look "
         "direction, dimension, health, and named stats (Kharge lives here).\n"
         "- fragment_search: full-text search over the player's Fragment "
         "journal — doc ids, scores, page text.\n"
         "- sim_status: the simulation clock — world tick, measured TPS, "
         "target TPS, paused flag.\n"
         "- mod_list / defrag_status: installed mods, and the forever-world "
         "maintenance service (running, phase, progress).\n"
         "\n"
         "HOW YOU ACT (tools): v1 is READ-ONLY. Every tool above is "
         "annotated readOnlyHint. There is no block-placement or "
         "console-exec tool yet — write tools land in a later phase, "
         "approval-gated. A permission denial comes back as an isError "
         "result naming the missing permission, never as a protocol error.\n"
         "\n"
         "HOW YOU LEARN (resources — read-only data, zero tool-call "
         "latency):\n"
         "- lithoscript://commands and lithoscript://cvars: the live console "
         "surface. This is how you learn lithoscript, the game's scripting "
         "language.\n"
         "- world://dimensions: dimension list, bounds, and which one is "
         "current.\n"
         "- game://lore-glossary: the world's terms (Kharge, the Fragment, "
         "the Daemon) as markdown.\n"
         "- mods://list and net://version: installed mods, and the "
         "engine/SDK/protocol versions.\n"
         "\n"
         "HOW YOU BUILD (the lithoscript + lithography loop): survey with "
         "the query tools, plan the build as explicit coordinates, and — "
         "until write tools land — hand the placement plan to the player. "
         "The endgame fabrication path is lithography (the LithographOR "
         "block); learn its interface through lithoscript://commands.\n"
         "\n"
         "PERMISSIONS: your connection starts deny-by-default (mask 0). Ask "
         "the player to approve you in the debug console: `mcp grant "
         "world_read console` (stdio) or the session variant over HTTP. "
         "Grants apply to future calls on the same connection.";
}

std::string renderBuildContraption(
    const std::map<std::string, std::string>& args) {
  const std::string what = args.at("what");
  return "Build workflow for: " + what + "\n"
         "\n"
         "1. SURVEY the area first. Use world_query_blocks to map the "
         "footprint for existing blocks, and entity_list (radius 1..512) to "
         "confirm the space is clear. Use player_get_state to note the "
         "dimension and your coordinates.\n"
         "2. PLAN before touching anything: the bounding box, the material "
         "palette (block ids), and the placement order as an explicit "
         "coordinate list. Keep the first version small — extend it later.\n"
         "3. PLACE blocks — HONEST LIMIT (v1): this server is read-only; "
         "there is no placement tool yet (world_set_block lands in a later "
         "phase, approval-gated). Produce the placement plan as a checklist "
         "of (x, y, z, blockId) steps the player — or a later write-enabled "
         "agent — can execute. Do not claim you placed anything.\n"
         "4. VERIFY with query. Once the blocks exist, re-run "
         "world_query_blocks over the finished region and diff it against "
         "the plan: every planned cell filled, no extras.\n"
         "\n"
         "Tips: respect the query cell caps (shrink the survey box or tile "
         "it); unloaded cells read as air (0), so confirm the area is "
         "loaded before trusting a survey; deterministic x/y/z order makes "
         "diffs easy.";
}

std::string renderDiagnose(const std::map<std::string, std::string>& args) {
  int64_t ticks = 600;
  auto it = args.find("ticks");
  if (it != args.end() && !parseTicks(it->second, ticks)) {
    // Unreachable: onPromptsGet rejects non-string args, and the
    // descriptor marks ticks required=false; but fail fast anyway.
    throw RpcError(kRpcInvalidParams,
                   "diagnose: 'ticks' must be a decimal integer 1..1000000");
  }
  const std::string t = std::to_string(ticks);
  return "World-health check over a " + t + "-tick window.\n"
         "\n"
         "1. Pull sim_status now: record tick, measured TPS, target TPS, "
         "and the paused flag.\n"
         "2. Wait " + t +
         " ticks of wall-clock time, then pull sim_status again.\n"
         "3. Read the numbers:\n"
         "   - Tick advanced by ~" + t +
         " and measured TPS ~= target TPS → HEALTHY.\n"
         "   - Tick advanced but measured TPS is well under target → "
         "DEGRADED (the sim is overloaded; check defrag_status — a running "
         "defrag phase steals time).\n"
         "   - Tick did not advance and paused is false → STALLED (the sim "
         "thread is stuck; report it, do not try to fix it).\n"
         "   - paused is true → PAUSED (expected if the player paused; not "
         "a fault).\n"
         "4. Pull defrag_status: is the forever-world maintenance running, "
         "which phase, what progress.\n"
         "5. Engine log: not yet exposed over MCP (no log tool in v1) — ask "
         "the player to paste recent lines from the debug console, "
         "especially lines starting with `mcp:`.\n"
         "\n"
         "Summarize: one verdict line (HEALTHY / DEGRADED / STALLED / "
         "PAUSED) plus the numbers that justify it.";
}

}  // namespace

void McpServer::registerPhase3Prompts() {
  prompts_.push_back(PromptDescriptor{
      "agent_onboarding",
      "Briefing for an agent inside LithoKraft: how you see (query tools), "
      "act (read-only v1 tools), learn (resources), and build (lithoscript "
      "+ lithography loop).",
      {{"name", "The agent's name, used to personalize the briefing.", true}},
      0,
      renderOnboarding,
  });
  prompts_.push_back(PromptDescriptor{
      "build_contraption",
      "Guided build workflow: survey area, plan, place blocks, verify with "
      "query. Honest about v1: placement is not yet exposed (read-only "
      "server), so the workflow produces an executable placement plan.",
      {{"what", "What to build (e.g. \"a 5x5 piston door\").", true}},
      0,
      renderBuildContraption,
  });
  prompts_.push_back(PromptDescriptor{
      "diagnose",
      "World-health check: pull sim_status twice over a tick window plus "
      "defrag_status, then summarize HEALTHY / DEGRADED / STALLED / PAUSED. "
      "Honest about v1: the engine log is not yet exposed over MCP.",
      {{"ticks",
        "Tick window as a decimal integer 1..1000000 (default \"600\").",
        false}},
      0,
      renderDiagnose,
  });
}

}  // namespace litholattice::mcp
