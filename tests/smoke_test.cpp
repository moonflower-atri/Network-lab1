#include "net/socket.hpp"
#include "proto/frame.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "失败: %s\n", message);
    ++g_failures;
  }
}

void test_codec() {
  std::string bytes;
  expect(chat::encode_frame(7, chat::FrameType::Text, "你好", bytes), "编码中文帧");
  chat::FrameReader reader;
  std::vector<chat::Frame> frames;
  expect(reader.feed(bytes.data(), 4, frames), "半个帧头可以留下");
  expect(frames.empty() && reader.has_partial(), "未收满时不产生消息");
  expect(reader.feed(bytes.data() + 4, bytes.size() - 4, frames), "补齐后解帧");
  expect(frames.size() == 1 && frames[0].seq == 7 && frames[0].body == "你好", "帧内容正确");
  expect(!reader.has_partial(), "完整帧不留下半包");

  frames.clear();
  std::string bad = bytes;
  bad[0] = 0;
  bad[1] = 0;
  bad[2] = 0x20;
  bad[3] = 0;
  chat::FrameReader too_long;
  expect(!too_long.feed(bad.data(), bad.size(), frames), "超长正文被拒绝");

  expect(!chat::utf8_ok("\xE4\xBD"), "截断的 UTF-8 无效");
  expect(chat::valid_nickname("张三"), "中文昵称有效");
  expect(!chat::valid_nickname("a\nb"), "昵称不能换行");
}

struct SeqWatch {
  std::uint32_t expect_seq = 1;
  std::vector<std::uint32_t> gaps;
  void note(std::uint32_t seq) {
    if (seq == 0) {
      return;
    }
    if (seq == expect_seq) {
      ++expect_seq;
    } else if (seq > expect_seq) {
      for (std::uint32_t missing = expect_seq; missing < seq; ++missing) {
        gaps.push_back(missing);
      }
      expect_seq = seq + 1;
    }
  }
};

struct Peer {
  int fd = -1;
  chat::FrameReader reader;
  std::vector<chat::Frame> frames;
  SeqWatch seq;
  ~Peer() { chat::close_conn(fd); }
};

struct ServerProc {
  pid_t pid = -1;
  int port = -1;
  ~ServerProc() { stop(); }
  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      ::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
};

void set_recv_timeout(int fd) {
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 100000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool pump(Peer& peer) {
  char buffer[4096];
  const int n = chat::recv_some(peer.fd, buffer, sizeof(buffer));
  if (n <= 0) {
    return false;
  }
  std::vector<chat::Frame> got;
  if (!peer.reader.feed(buffer, static_cast<std::size_t>(n), got)) {
    return false;
  }
  for (chat::Frame& frame : got) {
    peer.seq.note(frame.seq);
    peer.frames.push_back(std::move(frame));
  }
  return true;
}

template <typename Pred>
bool until(Peer& peer, Pred pred, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    pump(peer);
  }
  return pred();
}

bool has_text(const Peer& peer, const std::string& body) {
  for (const chat::Frame& frame : peer.frames) {
    if (frame.type == chat::FrameType::Text && frame.body == body) {
      return true;
    }
  }
  return false;
}

bool members_have(const Peer& peer, const std::string& name) {
  for (const chat::Frame& frame : peer.frames) {
    if (frame.type != chat::FrameType::Members) {
      continue;
    }
    for (const std::string& line : chat::split_lines(frame.body)) {
      if (line == name) {
        return true;
      }
    }
  }
  return false;
}

bool start_server(ServerProc& proc, const char* path, int drop) {
  int pipes[2];
  if (::pipe(pipes) < 0) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipes[0]);
    ::close(pipes[1]);
    return false;
  }
  if (pid == 0) {
    ::dup2(pipes[1], STDOUT_FILENO);
    ::close(pipes[0]);
    ::close(pipes[1]);
    char drop_text[16];
    std::snprintf(drop_text, sizeof(drop_text), "%d", drop);
    ::execl(path, path, "--port", "0", "--drop", drop_text, static_cast<char*>(nullptr));
    std::perror("execl");
    _exit(127);
  }
  ::close(pipes[1]);
  proc.pid = pid;

  std::string output;
  char buffer[128];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (output.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    pollfd ready{pipes[0], POLLIN, 0};
    if (::poll(&ready, 1, 200) <= 0) {
      int status = 0;
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        proc.pid = -1;
        break;
      }
      continue;
    }
    const ssize_t n = ::read(pipes[0], buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    output.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(pipes[0]);
  if (output.rfind("LISTENING ", 0) != 0) {
    return false;
  }
  proc.port = std::atoi(output.c_str() + std::strlen("LISTENING "));
  return proc.port > 0;
}

bool connect_peer(Peer& peer, int port) {
  chat::Conn conn = chat::connect_to("127.0.0.1", static_cast<std::uint16_t>(port), 3000);
  if (conn.fd < 0) {
    std::fprintf(stderr, "%s\n", conn.error.c_str());
    return false;
  }
  peer.fd = conn.fd;
  set_recv_timeout(peer.fd);
  return true;
}

bool send_text(Peer& peer, chat::FrameType type, const std::string& body, std::uint32_t& seq) {
  return chat::send_frame(peer.fd, ++seq, type, body);
}

void test_room(const char* server_path) {
  ServerProc server;
  expect(start_server(server, server_path, 0), "服务器启动");
  if (server.port < 0) {
    return;
  }

  Peer alice;
  Peer bob;
  std::uint32_t alice_seq = 0;
  std::uint32_t bob_seq = 0;
  expect(connect_peer(alice, server.port), "Alice 连接");
  expect(send_text(alice, chat::FrameType::Join, "Alice", alice_seq), "Alice 加入");
  expect(until(alice, [&] { return members_have(alice, "Alice") && has_text(alice, "* Alice 加入了聊天"); }, 3000),
         "Alice 收到自己的在线名单");

  expect(connect_peer(bob, server.port), "Bob 连接");
  expect(send_text(bob, chat::FrameType::Join, "Bob", bob_seq), "Bob 加入");
  expect(until(bob, [&] { return members_have(bob, "Alice") && members_have(bob, "Bob"); }, 3000),
         "Bob 看到两个人");
  expect(until(alice, [&] { return members_have(alice, "Bob"); }, 3000), "Alice 看到 Bob");

  expect(send_text(alice, chat::FrameType::Text, "hello 你好", alice_seq), "Alice 发言");
  expect(until(bob, [&] { return has_text(bob, "Alice: hello 你好"); }, 3000), "Bob 收到中文消息");
  expect(bob.seq.gaps.empty(), "正常收发没有序号缺口");

  Peer partial;
  expect(connect_peer(partial, server.port), "半帧连接");
  char head[10] = {};
  head[3] = 30;
  expect(chat::send_all(partial.fd, head, sizeof(head)), "发送不完整的帧头和声明长度");
  chat::close_conn(partial.fd);
  partial.fd = -1;
  expect(send_text(alice, chat::FrameType::Text, "still", alice_seq), "半帧之后还能发言");
  expect(until(bob, [&] { return has_text(bob, "Alice: still"); }, 3000), "半帧没有破坏其他连接");

  server.stop();

  ServerProc lossy;
  expect(start_server(lossy, server_path, 100), "启动丢弃测试服务器");
  if (lossy.port < 0) {
    return;
  }
  Peer a2;
  Peer b2;
  Peer c2;
  std::uint32_t a2_seq = 0;
  std::uint32_t b2_seq = 0;
  std::uint32_t c2_seq = 0;
  expect(connect_peer(a2, lossy.port) && send_text(a2, chat::FrameType::Join, "Alice", a2_seq), "丢弃测试 Alice");
  expect(connect_peer(b2, lossy.port) && send_text(b2, chat::FrameType::Join, "Bob", b2_seq), "丢弃测试 Bob");
  expect(until(b2, [&] { return has_text(b2, "* Bob 加入了聊天"); }, 3000), "Bob 收到加入提示");
  expect(send_text(a2, chat::FrameType::Text, "hello", a2_seq), "被丢弃的文本");
  expect(connect_peer(c2, lossy.port) && send_text(c2, chat::FrameType::Join, "Charlie", c2_seq), "Charlie 加入");
  expect(until(b2, [&] { return has_text(b2, "* Charlie 加入了聊天"); }, 3000), "后续消息仍然到达");
  expect(!has_text(b2, "Alice: hello"), "注入丢弃的消息不会显示");
  expect(!b2.seq.gaps.empty(), "注入丢弃造成序号缺口");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("用法: chat_smoke <chat_server 路径>\n", stderr);
    return 2;
  }
  chat::ignore_sigpipe();
  test_codec();
  test_room(argv[1]);
  if (g_failures != 0) {
    std::fprintf(stderr, "%d 项失败\n", g_failures);
    return 1;
  }
  std::puts("smoke ok");
  return 0;
}
