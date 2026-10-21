#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat {

struct Conn {
  int fd = -1;
  std::string error;
};

void ignore_sigpipe();

// port 为 0 时由系统分配端口。成功时 fd >= 0。
Conn listen_on(std::uint16_t port);
std::uint16_t bound_port(int fd);

// timeout_ms 只作用于连接建立，连接成功后发送不再使用这个时限。
Conn connect_to(const std::string& host, std::uint16_t port, int timeout_ms);

void close_conn(int fd);
bool send_all(int fd, const void* data, std::size_t size);

// >0 为读到的字节数，0 为对端关闭，-1 为错误（含超时，errno 为 EAGAIN 或 EWOULDBLOCK）。
int recv_some(int fd, void* data, std::size_t size);

}  // namespace chat
