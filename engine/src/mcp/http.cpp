// http.cpp: strict HTTP/1.1 request parser, response serializer, and SSE
// framing for the MCP Streamable HTTP transport. Clean-room; written from
// the HTTP/1.1 wire format, not from any existing implementation.
#include "litholattice/mcp/http.h"

#include <cctype>
#include <cstdint>

namespace litholattice::mcp {
namespace {

// --- tiny ASCII helpers (HTTP tokens are ASCII; never locale-dependent) ---
bool isTokenChar(char c) {
  // RFC 9110 token chars.
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool isValidToken(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!isTokenChar(c)) return false;
  return true;
}

std::string toLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(static_cast<char>(std::tolower(
                       static_cast<unsigned char>(c))));
  return out;
}

std::string_view trimOws(std::string_view s) {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
  std::size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool icontains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    if (iequals(haystack.substr(i, needle.size()), needle)) return true;
  return false;
}

}  // namespace

std::string_view HttpRequest::headerValue(std::string_view name) const {
  for (const HttpHeader& h : headers)
    if (iequals(h.name, name)) return h.value;
  return {};
}

bool HttpRequest::wantsEventStream() const {
  return icontains(headerValue("accept"), "text/event-stream");
}

bool HttpRequest::keepAlive() const {
  std::string_view conn = headerValue("connection");
  bool close = icontains(conn, "close");
  bool keep = icontains(conn, "keep-alive");
  if (version == "HTTP/1.0") return keep && !close;
  return !close;  // HTTP/1.1 default: keep alive
}

HttpParseOutcome parseHttpHeaders(std::string_view buffer) {
  HttpParseOutcome out;
  // Find the end of the header block.
  std::size_t end = buffer.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (buffer.size() > kMaxHttpHeaderBytes + 4) {
      out.status = HttpParseStatus::kError;
      out.error = "header block exceeds 64 KiB";
    }
    return out;  // kNeedMore (or kError as set above)
  }
  std::size_t blockLen = end + 4;
  if (blockLen > kMaxHttpHeaderBytes + 4) {
    out.status = HttpParseStatus::kError;
    out.error = "header block exceeds 64 KiB";
    return out;
  }

  // block = request line + header lines, each CRLF-terminated (the blank
  // line itself is excluded).
  std::string_view block = buffer.substr(0, end + 2);
  std::size_t lineEnd = block.find("\r\n");
  if (lineEnd == std::string_view::npos) {
    out.status = HttpParseStatus::kError;
    out.error = "empty request";
    return out;
  }

  // --- request line: METHOD SP TARGET SP VERSION ---
  std::string_view requestLine = block.substr(0, lineEnd);
  std::size_t sp1 = requestLine.find(' ');
  std::size_t sp2 = sp1 == std::string_view::npos
                        ? std::string_view::npos
                        : requestLine.find(' ', sp1 + 1);
  auto fail = [&](const char* why) {
    out.status = HttpParseStatus::kError;
    out.error = why;
    return out;
  };
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
      requestLine.find(' ', sp2 + 1) != std::string_view::npos)
    return fail("malformed request line");
  std::string_view method = requestLine.substr(0, sp1);
  std::string_view target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string_view version = requestLine.substr(sp2 + 1);
  if (!isValidToken(method)) return fail("bad method token");
  if (target.empty() || target[0] != '/') return fail("bad request target");
  for (char c : target)
    if (c <= ' ' || c == 0x7f) return fail("bad request target");
  if (version != "HTTP/1.0" && version != "HTTP/1.1")
    return fail("unsupported HTTP version");

  HttpRequest req;
  req.method = std::string(method);
  req.target = std::string(target);
  req.version = std::string(version);

  // --- headers ---
  std::size_t pos = lineEnd + 2;
  bool sawContentLength = false;
  while (pos < block.size()) {
    std::size_t eol = block.find("\r\n", pos);
    if (eol == std::string_view::npos) return fail("truncated header line");
    std::string_view line = block.substr(pos, eol - pos);
    pos = eol + 2;
    if (line.empty()) return fail("blank line inside header block");
    // obs-fold (line starting with SP/HT) is rejected, never unfolded.
    if (line[0] == ' ' || line[0] == '\t') return fail("obsolete line folding");
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return fail("header without colon");
    std::string_view name = line.substr(0, colon);
    std::string_view value = trimOws(line.substr(colon + 1));
    if (!isValidToken(name)) return fail("bad header name");
    // No whitespace between field-name and colon (strict).
    std::string lname = toLower(name);
    if (lname == "content-length") {
      if (sawContentLength) return fail("duplicate Content-Length");
      sawContentLength = true;
    }
    for (char c : value)
      if (c == '\r' || c == '\n' || c == '\0') return fail("bad header value");
    req.headers.push_back({std::move(lname), std::string(value)});
  }

  out.status = HttpParseStatus::kOk;
  out.request = std::move(req);
  out.consumed = blockLen;
  return out;
}

bool parseContentLength(const HttpRequest& req, std::uint64_t& out) {
  int seen = 0;
  std::uint64_t value = 0;
  for (const HttpHeader& h : req.headers) {
    if (h.name != "content-length") continue;
    ++seen;
    if (seen > 1) return false;  // duplicate (defense in depth; parser rejects)
    if (h.value.empty() || h.value.size() > 19) return false;
    for (char c : h.value) {
      if (c < '0' || c > '9') return false;
      std::uint64_t d = static_cast<std::uint64_t>(c - '0');
      if (value > (UINT64_MAX - d) / 10) return false;  // overflow
      value = value * 10 + d;
    }
  }
  if (seen == 0) return false;
  out = value;
  return true;
}

void HttpResponse::setHeader(std::string_view name, std::string_view value) {
  std::string lname = toLower(name);
  for (HttpHeader& h : headers) {
    if (h.name == lname) {
      h.value = std::string(value);
      return;
    }
  }
  headers.push_back({std::move(lname), std::string(value)});
}

const char* httpReason(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Unknown";
  }
}

std::string_view HttpResponse::headerValue(std::string_view name) const {
  for (const HttpHeader& h : headers)
    if (iequals(h.name, name)) return h.value;
  return {};
}

std::string HttpResponse::serialize() const {
  std::string out;
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += httpReason(status);
  out += "\r\n";
  for (const HttpHeader& h : headers) {
    out += h.name;
    out += ": ";
    out += h.value;
    out += "\r\n";
  }
  out += "content-length: ";
  out += std::to_string(body.size());
  out += "\r\n\r\n";
  out += body;
  return out;
}

HttpResponse makeErrorResponse(int status, std::string_view message) {
  HttpResponse r;
  r.status = status;
  r.setHeader("Content-Type", "text/plain; charset=utf-8");
  r.setHeader("Connection", "close");
  r.body = std::string(message);
  return r;
}

std::string SseStream::formatEvent(std::string_view event,
                                   std::string_view data) {
  std::string out;
  out += "event: ";
  out += event;
  out += "\n";
  // SSE: one "data:" line per line of data. A single trailing newline is
  // stripped first so it does not emit a phantom empty line.
  std::string_view d = data;
  if (!d.empty() && d.back() == '\n') d.remove_suffix(1);
  if (d.empty()) {
    out += "data:\n";
  } else {
    std::size_t pos = 0;
    while (true) {
      std::size_t nl = d.find('\n', pos);
      std::string_view line =
          (nl == std::string_view::npos) ? d.substr(pos) : d.substr(pos, nl - pos);
      out += "data: ";
      out += line;
      out += "\n";
      if (nl == std::string_view::npos) break;
      pos = nl + 1;
    }
  }
  out += "\n";
  return out;
}

void SseStream::pushEvent(std::string_view event, std::string_view data) {
  bytes_ += formatEvent(event, data);
}

HttpResponse SseStream::streamResponse(std::string_view sseBody) {
  HttpResponse r;
  r.status = 200;
  r.setHeader("Content-Type", "text/event-stream");
  r.setHeader("Cache-Control", "no-cache");
  r.body = std::string(sseBody);
  return r;
}

}  // namespace litholattice::mcp
