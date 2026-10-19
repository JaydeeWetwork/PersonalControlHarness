// sockets.cpp: POSIX implementation of the minimal TCP server socket
// abstraction (see sockets.h). Clean-room; written from the POSIX socket
// API, not from any existing implementation.
//
// The _WIN32 branch below is structural only (UNVERIFIED — no Windows build
// in this tree; the Windows builder owns compile-checking it).
#include "litholattice/mcp/sockets.h"

#include <cstring>

#ifdef _WIN32
// ---- UNVERIFIED Windows branch (builder to verify) ------------------------
// Winsock shape: WSAStartup once per process (refcounted), closesocket,
// ioctlsocket for timeouts is NOT used — timeouts go through SO_RCVTIMEO /
// SO_SNDTIMEO like POSIX. WSAPoll mirrors poll() for the accept wait.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace litholattice::mcp {
namespace {
int g_wsaRefs = 0;
bool wsaEnsure(std::string& error) {
  if (g_wsaRefs++ == 0) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      --g_wsaRefs;
      error = "WSAStartup failed";
      return false;
    }
  }
  return true;
}
void wsaRelease() {
  if (--g_wsaRefs == 0) WSACleanup();
}
void setTimeouts(NativeSocket s, int seconds) {
  DWORD ms = static_cast<DWORD>(seconds) * 1000u;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
             sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
             sizeof(ms));
}
}  // namespace

TcpConn::TcpConn() = default;
TcpConn::TcpConn(NativeSocket sock) : sock_(sock) { setTimeouts(sock_, kDefaultIoTimeoutSec); }
TcpConn::~TcpConn() { close(); }
TcpConn::TcpConn(TcpConn&& other) noexcept : sock_(other.sock_) {
  other.sock_ = kInvalidSocketValue;
}
TcpConn& TcpConn::operator=(TcpConn&& other) noexcept {
  if (this != &other) {
    close();
    sock_ = other.sock_;
    other.sock_ = kInvalidSocketValue;
  }
  return *this;
}
void TcpConn::close() {
  if (sock_ != kInvalidSocketValue) {
    closesocket(sock_);
    sock_ = kInvalidSocketValue;
  }
}
void TcpConn::setTimeout(int seconds) {
  if (valid()) setTimeouts(sock_, seconds);
}
std::string TcpConn::readSome(std::size_t maxBytes) {
  std::string out;
  out.resize(maxBytes > 0 ? maxBytes : 1);
  int n = recv(sock_, out.data(), static_cast<int>(out.size()), 0);
  if (n <= 0) return {};
  out.resize(static_cast<std::size_t>(n));
  return out;
}
bool TcpConn::sendAll(std::string_view data) {
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    int n = send(sock_, p, static_cast<int>(left), 0);
    if (n <= 0) return false;
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  return true;
}

TcpListener::TcpListener() = default;
TcpListener::~TcpListener() { close(); }
TcpListener::TcpListener(TcpListener&& other) noexcept
    : sock_(other.sock_),
      boundAddress_(std::move(other.boundAddress_)),
      boundPort_(other.boundPort_) {
  other.sock_ = kInvalidSocketValue;
  other.boundPort_ = 0;
}
TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    sock_ = other.sock_;
    boundAddress_ = std::move(other.boundAddress_);
    boundPort_ = other.boundPort_;
    other.sock_ = kInvalidSocketValue;
    other.boundPort_ = 0;
  }
  return *this;
}
void TcpListener::close() {
  if (sock_ != kInvalidSocketValue) {
    closesocket(sock_);
    sock_ = kInvalidSocketValue;
    wsaRelease();
  }
}
bool TcpListener::bind(const std::string& address, std::uint16_t port,
                       std::string& error) {
  std::string wsaErr;
  if (!wsaEnsure(wsaErr)) {
    error = wsaErr;
    return false;
  }
  NativeSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocketValue) {
    error = "socket() failed";
    wsaRelease();
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (InetPtonA(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
    error = "bad bind address '" + address + "'";
    closesocket(s);
    wsaRelease();
    return false;
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "bind(" + address + ":" + std::to_string(port) +
            ") failed (in use?)";
    closesocket(s);
    wsaRelease();
    return false;
  }
  if (listen(s, 16) != 0) {
    error = "listen() failed";
    closesocket(s);
    wsaRelease();
    return false;
  }
  sockaddr_in actual{};
  int actualLen = sizeof(actual);
  std::uint16_t actualPort = port;
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &actualLen) == 0)
    actualPort = ntohs(actual.sin_port);
  sock_ = s;
  boundAddress_ = address;
  boundPort_ = actualPort;
  return true;
}
bool TcpListener::accept(TcpConn& conn, int timeoutMs, std::string& error) {
  WSAPOLLFD pfd{};
  pfd.fd = sock_;
  pfd.events = POLLRDNORM;
  int r = WSAPoll(&pfd, 1, timeoutMs);
  if (r < 0) {
    error = "WSAPoll failed";
    return false;
  }
  if (r == 0) return false;  // timeout: not an error
  NativeSocket c = ::accept(sock_, nullptr, nullptr);
  if (c == kInvalidSocketValue) {
    error = "accept() failed";
    return false;
  }
  conn = TcpConn(c);
  return true;
}
}  // namespace litholattice::mcp

#else
// ---- POSIX implementation (built and tested on Linux) ----------------------
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace litholattice::mcp {
namespace {
void setTimeouts(int fd, int seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
}  // namespace

TcpConn::TcpConn() = default;
TcpConn::TcpConn(NativeSocket sock) : sock_(sock) {
  setTimeouts(sock_, kDefaultIoTimeoutSec);
}
TcpConn::~TcpConn() { close(); }
TcpConn::TcpConn(TcpConn&& other) noexcept : sock_(other.sock_) {
  other.sock_ = kInvalidSocketValue;
}
TcpConn& TcpConn::operator=(TcpConn&& other) noexcept {
  if (this != &other) {
    close();
    sock_ = other.sock_;
    other.sock_ = kInvalidSocketValue;
  }
  return *this;
}
void TcpConn::close() {
  if (sock_ != kInvalidSocketValue) {
    ::close(sock_);
    sock_ = kInvalidSocketValue;
  }
}
void TcpConn::setTimeout(int seconds) {
  if (valid()) setTimeouts(sock_, seconds);
}
std::string TcpConn::readSome(std::size_t maxBytes) {
  std::string out;
  out.resize(maxBytes > 0 ? maxBytes : 1);
  // MSG_NOSIGNAL: a peer that went away mid-write must not SIGPIPE the game.
  ssize_t n = ::recv(sock_, out.data(), out.size(), 0);
  if (n <= 0) return {};
  out.resize(static_cast<std::size_t>(n));
  return out;
}
bool TcpConn::sendAll(std::string_view data) {
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::send(sock_, p, left, MSG_NOSIGNAL);
    if (n <= 0) return false;
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  return true;
}

TcpListener::TcpListener() = default;
TcpListener::~TcpListener() { close(); }
TcpListener::TcpListener(TcpListener&& other) noexcept
    : sock_(other.sock_),
      boundAddress_(std::move(other.boundAddress_)),
      boundPort_(other.boundPort_) {
  other.sock_ = kInvalidSocketValue;
  other.boundPort_ = 0;
}
TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    sock_ = other.sock_;
    boundAddress_ = std::move(other.boundAddress_);
    boundPort_ = other.boundPort_;
    other.sock_ = kInvalidSocketValue;
    other.boundPort_ = 0;
  }
  return *this;
}
void TcpListener::close() {
  if (sock_ != kInvalidSocketValue) {
    ::close(sock_);
    sock_ = kInvalidSocketValue;
  }
}
bool TcpListener::bind(const std::string& address, std::uint16_t port,
                       std::string& error) {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    error = "socket() failed";
    return false;
  }
  int reuse = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
    error = "bad bind address '" + address + "'";
    ::close(s);
    return false;
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "bind(" + address + ":" + std::to_string(port) +
            ") failed: " + std::strerror(errno) + " (already in use?)";
    ::close(s);
    return false;
  }
  if (::listen(s, 16) != 0) {
    error = "listen() failed: " + std::string(std::strerror(errno));
    ::close(s);
    return false;
  }
  sockaddr_in actual{};
  socklen_t actualLen = sizeof(actual);
  std::uint16_t actualPort = port;
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &actualLen) == 0)
    actualPort = ntohs(actual.sin_port);
  sock_ = s;
  boundAddress_ = address;
  boundPort_ = actualPort;
  return true;
}
bool TcpListener::accept(TcpConn& conn, int timeoutMs, std::string& error) {
  pollfd pfd{};
  pfd.fd = sock_;
  pfd.events = POLLIN;
  int r = ::poll(&pfd, 1, timeoutMs);
  if (r < 0) {
    if (errno == EINTR) return false;  // interrupted: treat as a timeout tick
    error = "poll() failed: " + std::string(std::strerror(errno));
    return false;
  }
  if (r == 0) return false;  // timeout: not an error
  int c = ::accept(sock_, nullptr, nullptr);
  if (c < 0) {
    if (errno == EINTR || errno == EAGAIN) return false;
    error = "accept() failed: " + std::string(std::strerror(errno));
    return false;
  }
  conn = TcpConn(c);
  return true;
}
}  // namespace litholattice::mcp
#endif
