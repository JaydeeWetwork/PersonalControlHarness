// http.h: minimal strict HTTP/1.1 request parser + response serializer for
// the MCP Streamable HTTP transport (Phase 2). Clean-room; written from the
// HTTP/1.1 wire format and the MCP Streamable HTTP transport rules, not from
// any existing implementation.
//
// Scope is deliberately tiny: exactly what POST /mcp needs. Everything else
// is rejected fail-fast:
//
//   - Only POST /mcp is served. Any other target -> 404; any other method on
//     /mcp -> 405 (GET is reserved for a future SSE stream endpoint).
//   - Content-Length is REQUIRED on POST (missing -> 411). Bodies are capped
//     at kMaxHttpBodyBytes (16 MiB, same posture as the stdio transport);
//     larger -> 413 and the connection is closed.
//   - Transfer-Encoding (chunked) is NOT supported in v1 -> 501.
//   - Content-Type must be application/json (parameters such as
//     "; charset=utf-8" are tolerated) -> else 415.
//   - Any malformed request line or header -> 400. Duplicate Content-Length
//     -> 400 (request-smuggling defense: never guess which one wins).
//   - Header block capped at kMaxHttpHeaderBytes (64 KiB) -> 431.
//
// The parser works on a header block (through the blank line); the body is
// read separately using Content-Length, so the parser never has to guess
// framing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice::mcp {

/// Max header-block bytes accepted (request line + headers, through the
/// blank line). Larger -> 431 Request Header Fields Too Large.
inline constexpr std::size_t kMaxHttpHeaderBytes = 64u * 1024u;
/// Max POST body bytes accepted. Same 16 MiB posture as the stdio transport
/// (kMaxStdioLineBytes); larger -> 413 and the connection is closed.
inline constexpr std::size_t kMaxHttpBodyBytes = 16u * 1024u * 1024u;

/// One parsed header. `name` is stored lowercased (header names are
/// case-insensitive); `value` has optional whitespace trimmed.
struct HttpHeader {
  std::string name;
  std::string value;
};

/// A parsed HTTP request: request line + headers + body. The body is filled
/// by the connection reader (http_server.cpp), not by parseHttpHeaders.
struct HttpRequest {
  std::string method;   ///< e.g. "POST"
  std::string target;   ///< e.g. "/mcp"
  std::string version;  ///< "HTTP/1.0" or "HTTP/1.1"
  std::vector<HttpHeader> headers;
  std::string body;

  /// Case-insensitive header lookup. Empty view when absent.
  std::string_view headerValue(std::string_view name) const;
  /// True when the client will accept an SSE response stream.
  bool wantsEventStream() const;
  /// Keep-alive decision for the parsed request (HTTP/1.1 default on,
  /// HTTP/1.0 default off, "Connection:" overrides either way).
  bool keepAlive() const;
};

enum class HttpParseStatus {
  kOk,        ///< full header block parsed; `consumed` bytes used
  kNeedMore,  ///< no complete header block yet; feed more bytes
  kError,     ///< malformed; `error` says why — never trust it
};

struct HttpParseOutcome {
  HttpParseStatus status = HttpParseStatus::kNeedMore;
  HttpRequest request;      ///< valid when status == kOk
  std::string error;        ///< valid when status == kError
  std::size_t consumed = 0;  ///< header-block bytes when kOk
};

/// Parse one header block from `buffer` (which must contain bytes through the
/// terminating blank line). Returns kNeedMore if the block is incomplete,
/// kError on any malformation, kOk with `consumed` set otherwise.
HttpParseOutcome parseHttpHeaders(std::string_view buffer);

/// Strict Content-Length extraction: digits only, no duplicates allowed.
/// False when missing, malformed, or duplicated.
bool parseContentLength(const HttpRequest& req, std::uint64_t& out);

struct HttpResponse {
  int status = 200;
  std::vector<HttpHeader> headers;
  std::string body;

  void setHeader(std::string_view name, std::string_view value);
  /// Case-insensitive header lookup. Empty view when absent.
  std::string_view headerValue(std::string_view name) const;
  std::string serialize() const;
};

/// Reason phrase for the statuses this server emits.
const char* httpReason(int status);
/// text/plain error response with a short message body (for transport-level
/// failures: 400/401/404/405/411/413/415/431/500/501).
HttpResponse makeErrorResponse(int status, std::string_view message);

// ---------------------------------------------------------------------------
// SSE (Server-Sent Events) framing for Streamable HTTP server->client
// messages. Phase 1 has no server-initiated notifications, so this is the
// seam: SseStream formats spec-correct frames today (proven by self-test),
// and a later phase will push live notification events through it. The
// Streamable HTTP transport also allows a single JSON-RPC response to be
// delivered as an SSE `message` event when the client sends
// `Accept: text/event-stream`; handleRequest does that via streamResponse().
// ---------------------------------------------------------------------------
class SseStream {
 public:
  /// Format one event frame: "event: <event>\n" + one "data: <line>\n" per
  /// line of data + "\n". Multi-line data becomes multiple data: lines per
  /// the SSE spec; a trailing newline in data does not emit an empty line.
  static std::string formatEvent(std::string_view event, std::string_view data);

  /// Append an event frame to this stream's bytes.
  void pushEvent(std::string_view event, std::string_view data);
  const std::string& bytes() const { return bytes_; }
  void clear() { bytes_.clear(); }

  /// 200 response carrying an SSE body (Content-Type: text/event-stream,
  /// Cache-Control: no-cache).
  static HttpResponse streamResponse(std::string_view sseBody);

 private:
  std::string bytes_;
};

}  // namespace litholattice::mcp
