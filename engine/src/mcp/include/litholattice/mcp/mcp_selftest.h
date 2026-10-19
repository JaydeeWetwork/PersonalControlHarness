// Headless checks of the MCP Phase 0 JSON-RPC core: strict JSON parser
// (valid documents plus every malformed class rejected fail-fast),
// canonical serializer round-trips, and JSON-RPC 2.0 framing (requests,
// notifications, all error codes, router dispatch).
//
// Phase 2 (mcp_http_selftest.cpp): strict HTTP/1.1 parser, SSE framing,
// bearer-token auth, handshake-era session lifecycle, and a live-loopback
// integration test of the accept/dispatch thread.
#pragma once

namespace litholattice::mcp {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runMcpSelfTest();
/// Phase 2 HTTP transport checks (same reporting convention).
int runMcpHttpSelfTest();
}  // namespace litholattice::mcp
