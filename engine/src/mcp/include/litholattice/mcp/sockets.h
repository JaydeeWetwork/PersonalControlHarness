// sockets.h: minimal TCP server socket abstraction for the MCP Streamable
// HTTP transport (Phase 2). Clean-room; no code taken from any socket
// implementation.
//
// Threading model (locked): the McpHttpServer runs ONE accept/dispatch
// thread. TcpListener::accept is poll-based with a timeout so the thread
// notices stop(); each accepted TcpConn is served synchronously on that same
// thread (sequential requests, keep-alive, bounded per connection). No
// connection threads and no thread pool in v1. Blocking reads use an I/O
// timeout so a stalled peer cannot wedge the loop forever.
//
// Platform: POSIX is implemented and covered by tests (Linux). The _WIN32
// branch has the Winsock shape (WSAStartup, closesocket, WSAPoll) but is
// UNVERIFIED — this tree has no Windows build; the Windows builder must
// compile-check and smoke-test that path before enabling it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace litholattice::mcp {

#ifdef _WIN32
// UNVERIFIED on Windows (see header note). Shape only.
using NativeSocket = std::uintptr_t;
inline constexpr NativeSocket kInvalidSocketValue = static_cast<NativeSocket>(~0u);
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocketValue = -1;
#endif

/// Default I/O timeout (seconds) for blocking reads/writes on an accepted
/// connection. A stalled peer wedges the single dispatch thread for at most
/// this long per read; the accept loop polls on a much shorter cadence.
inline constexpr int kDefaultIoTimeoutSec = 5;

/// RAII TCP connection (one accepted socket). Move-only.
class TcpConn {
 public:
  TcpConn();
  ~TcpConn();

  TcpConn(const TcpConn&) = delete;
  TcpConn& operator=(const TcpConn&) = delete;
  TcpConn(TcpConn&& other) noexcept;
  TcpConn& operator=(TcpConn&& other) noexcept;

  bool valid() const { return sock_ != kInvalidSocketValue; }
  void close();

  /// Blocking read of up to maxBytes, honoring the I/O timeout. Returns the
  /// bytes read; empty means the peer closed or the timeout elapsed with no
  /// data (indistinguishable by design — the caller treats both as "done").
  std::string readSome(std::size_t maxBytes);
  /// Blocking write of the full buffer. False on error or timeout.
  bool sendAll(std::string_view data);
  /// Seconds before blocking reads/writes give up.
  void setTimeout(int seconds);

 private:
  friend class TcpListener;
  explicit TcpConn(NativeSocket sock);

  NativeSocket sock_ = kInvalidSocketValue;
};

/// Listening TCP socket. Move-only.
class TcpListener {
 public:
  TcpListener();
  ~TcpListener();

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;

  /// Bind (IPv4 dotted quad) + listen. Fail-fast: false with `error` set on
  /// any failure (address in use, permission, bad address). The caller logs
  /// and never silently picks another port.
  bool bind(const std::string& address, std::uint16_t port, std::string& error);
  /// Wait up to timeoutMs for a peer. Returns true and fills `conn` on
  /// accept; false on timeout. A fatal error sets `error` and also returns
  /// false — the caller stops the loop only when told to, so a transient
  /// accept failure does not kill the server.
  bool accept(TcpConn& conn, int timeoutMs, std::string& error);
  void close();
  bool listening() const { return sock_ != kInvalidSocketValue; }

  /// What bind() actually bound (queried via getsockname, so binding port 0
  /// reports the ephemeral port the OS picked). Used by tests to assert the
  /// loopback-only default and to run live-loopback integration tests.
  std::string boundAddress() const { return boundAddress_; }
  std::uint16_t boundPort() const { return boundPort_; }

 private:
  NativeSocket sock_ = kInvalidSocketValue;
  std::string boundAddress_;
  std::uint16_t boundPort_ = 0;
};

}  // namespace litholattice::mcp
