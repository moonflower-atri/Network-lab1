#include "net/socket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace chat {
namespace {

void set_timeout(int fd, int timeout_ms) {
  timeval tv{};
  if (timeout_ms > 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
  }
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool enable_nodelay(int fd) {
  int yes = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}

}  // namespace

void ignore_sigpipe() {
  struct sigaction action {};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPIPE, &action, nullptr);
}

Conn listen_on(std::uint16_t port) {
  Conn conn;
  conn.fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (conn.fd < 0) {
    conn.error = std::string("创建套接字失败: ") + std::strerror(errno);
    return conn;
  }

  int yes = 1;
  ::setsockopt(conn.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(conn.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    conn.error = std::string("绑定端口失败: ") + std::strerror(errno);
    close_conn(conn.fd);
    conn.fd = -1;
    return conn;
  }
  if (::listen(conn.fd, 16) < 0) {
    conn.error = std::string("监听失败: ") + std::strerror(errno);
    close_conn(conn.fd);
    conn.fd = -1;
    return conn;
  }
  return conn;
}

std::uint16_t bound_port(int fd) {
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

Conn connect_to(const std::string& host, std::uint16_t port, int timeout_ms) {
  Conn conn;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
  if (gai != 0) {
    conn.error = std::string("解析地址失败: ") + gai_strerror(gai);
    return conn;
  }

  std::string last_error = "无法连接";
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_error = std::string("创建套接字失败: ") + std::strerror(errno);
      continue;
    }
    set_timeout(fd, timeout_ms);
    if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      set_timeout(fd, 0);
      enable_nodelay(fd);
      conn.fd = fd;
      conn.error.clear();
      break;
    }
    last_error = std::string("连接失败: ") + std::strerror(errno);
    ::close(fd);
  }
  ::freeaddrinfo(result);
  if (conn.fd < 0) {
    conn.error = last_error;
  }
  return conn;
}

void close_conn(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool send_all(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::send(fd, bytes + sent, size - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

int recv_some(int fd, void* data, std::size_t size) {
  while (true) {
    const ssize_t n = ::recv(fd, data, size, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return static_cast<int>(n);
  }
}

}  // namespace chat
