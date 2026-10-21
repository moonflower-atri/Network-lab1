#include "net/socket.hpp"
#include "proto/frame.hpp"

#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Client {
  int fd = -1;
  std::string name;
  std::uint32_t out_seq = 0;
  bool joined = false;
  bool failed = false;
  chat::FrameReader reader;
};

struct Server {
  pthread_mutex_t mu{};
  std::vector<std::unique_ptr<Client>> clients;
  int drop_percent = 0;
};

struct ThreadStart {
  Server* server = nullptr;
  Client* client = nullptr;
};

void mark_failed(Client& client) {
  if (client.failed) {
    return;
  }
  client.failed = true;
  ::shutdown(client.fd, SHUT_RDWR);
}

bool roll_drop(const Server& server) {
  if (server.drop_percent <= 0) {
    return false;
  }
  if (server.drop_percent >= 100) {
    return true;
  }
  return (std::rand() % 100) < server.drop_percent;
}

void broadcast_locked(Server& server, chat::FrameType type, const std::string& body, bool droppable) {
  for (const std::unique_ptr<Client>& client : server.clients) {
    if (!client->joined || client->failed) {
      continue;
    }
    const std::uint32_t seq = ++client->out_seq;
    if (droppable && roll_drop(server)) {
      std::fprintf(stderr, "注入丢弃 seq=%u -> %s\n", seq, client->name.c_str());
      continue;
    }
    if (!chat::send_frame(client->fd, seq, type, body)) {
      std::fprintf(stderr, "发送失败 fd=%d\n", client->fd);
      mark_failed(*client);
    }
  }
}

void broadcast_members_locked(Server& server) {
  std::string body;
  for (const std::unique_ptr<Client>& client : server.clients) {
    if (!client->joined || client->failed) {
      continue;
    }
    if (!body.empty()) {
      body.push_back('\n');
    }
    body += client->name;
  }
  broadcast_locked(server, chat::FrameType::Members, body, false);
}

void broadcast_text_locked(Server& server, const std::string& body, bool droppable) {
  broadcast_locked(server, chat::FrameType::Text, body, droppable);
}

void finish_client(Server& server, Client* target) {
  pthread_mutex_lock(&server.mu);
  const std::string name = target->name;
  const int fd = target->fd;
  const bool joined = target->joined;
  const bool partial = target->reader.has_partial();
  auto& clients = server.clients;
  clients.erase(std::remove_if(clients.begin(), clients.end(),
                               [target](const std::unique_ptr<Client>& item) {
                                 return item.get() == target;
                               }),
                clients.end());
  if (joined) {
    broadcast_text_locked(server, "* " + name + " 离开了聊天", false);
    broadcast_members_locked(server);
  }
  pthread_mutex_unlock(&server.mu);
  chat::close_conn(fd);
  if (joined) {
    std::fprintf(stderr, "离开 %s fd=%d%s\n", name.c_str(), fd, partial ? "，半帧已丢弃" : "");
  } else {
    std::fprintf(stderr, "断开未加入的连接 fd=%d%s\n", fd, partial ? "，半帧已丢弃" : "");
  }
}

bool handle_frame(Server& server, Client& client, const chat::Frame& frame) {
  pthread_mutex_lock(&server.mu);
  bool ok = true;
  if (!client.joined) {
    if (frame.type != chat::FrameType::Join || !chat::valid_nickname(frame.body)) {
      ok = false;
    } else {
      client.name = frame.body;
      client.joined = true;
      std::fprintf(stderr, "加入 %s fd=%d\n", client.name.c_str(), client.fd);
      broadcast_members_locked(server);
      broadcast_text_locked(server, "* " + client.name + " 加入了聊天", false);
    }
  } else if (frame.type == chat::FrameType::Text) {
    if (frame.body.empty() || frame.body.find('\n') != std::string::npos ||
        frame.body.find('\r') != std::string::npos) {
      ok = false;
    } else {
      const std::string line = client.name + ": " + frame.body;
      if (line.size() > chat::kMaxBody) {
        ok = false;
      } else {
        std::fprintf(stderr, "文本 %s\n", line.c_str());
        broadcast_text_locked(server, line, true);
      }
    }
  } else {
    ok = false;
  }
  pthread_mutex_unlock(&server.mu);
  return ok;
}

void read_loop(Server& server, Client& client) {
  char buffer[8192];
  while (true) {
    const int n = chat::recv_some(client.fd, buffer, sizeof(buffer));
    if (n <= 0) {
      return;
    }
    std::vector<chat::Frame> frames;
    const bool ok = client.reader.feed(buffer, static_cast<std::size_t>(n), frames);
    for (const chat::Frame& frame : frames) {
      if (!handle_frame(server, client, frame)) {
        return;
      }
    }
    if (!ok) {
      std::fprintf(stderr, "协议错误 fd=%d\n", client.fd);
      return;
    }
  }
}

void* client_thread(void* arg) {
  std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(arg));
  pthread_detach(pthread_self());
  Server& server = *start->server;
  Client* client = start->client;
  start.reset();
  read_loop(server, *client);
  finish_client(server, client);
  return nullptr;
}

void print_usage() {
  std::fputs(
      "用法: chat_server [--port 端口] [--drop 0-100]\n"
      "默认端口 9000，丢弃比例 0。\n"
      "端口写 0 时由系统分配，并在标准输出打印 LISTENING <端口>。\n"
      "--drop 只丢弃已经收完整的聊天文本，用来制造序号缺口。加入、离开和在线名单不丢。\n",
      stderr);
}

bool parse_int(const char* text, int max_value, int& out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value > max_value) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int port = 9000;
  int drop = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--port" || arg == "--drop") {
      if (i + 1 >= argc) {
        print_usage();
        return 2;
      }
      int value = 0;
      if (!parse_int(argv[++i], arg == "--port" ? 65535 : 100, value)) {
        print_usage();
        return 2;
      }
      if (arg == "--port") {
        port = value;
      } else {
        drop = value;
      }
    } else if (!parse_int(arg.c_str(), 65535, port)) {
      print_usage();
      return 2;
    }
  }

  chat::ignore_sigpipe();
  std::srand(static_cast<unsigned>(std::time(nullptr)));

  chat::Conn listener = chat::listen_on(static_cast<std::uint16_t>(port));
  if (listener.fd < 0) {
    std::fprintf(stderr, "%s\n", listener.error.c_str());
    return 1;
  }
  const std::uint16_t actual = chat::bound_port(listener.fd);
  std::printf("LISTENING %u\n", static_cast<unsigned>(actual));
  std::fflush(stdout);
  std::fprintf(stderr, "监听 0.0.0.0:%u，丢弃比例 %d%%\n", static_cast<unsigned>(actual), drop);

  Server server;
  pthread_mutex_init(&server.mu, nullptr);
  server.drop_percent = drop;

  while (true) {
    const int fd = ::accept(listener.fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("accept");
      break;
    }
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    auto client = std::make_unique<Client>();
    client->fd = fd;
    Client* raw = client.get();
    pthread_mutex_lock(&server.mu);
    server.clients.push_back(std::move(client));
    pthread_mutex_unlock(&server.mu);
    std::fprintf(stderr, "接入 fd=%d\n", fd);

    auto* start = new ThreadStart{&server, raw};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, client_thread, start) != 0) {
      delete start;
      finish_client(server, raw);
    }
  }

  chat::close_conn(listener.fd);
  return 0;
}
