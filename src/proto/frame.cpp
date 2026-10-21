#include "proto/frame.hpp"

#include "net/socket.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace chat {
namespace {

void append_be32(std::string& out, std::uint32_t value) {
  const std::uint32_t network = htonl(value);
  out.append(reinterpret_cast<const char*>(&network), sizeof(network));
}

void append_be16(std::string& out, std::uint16_t value) {
  const std::uint16_t network = htons(value);
  out.append(reinterpret_cast<const char*>(&network), sizeof(network));
}

std::uint32_t read_be32(const char* data) {
  std::uint32_t network = 0;
  std::memcpy(&network, data, sizeof(network));
  return ntohl(network);
}

std::uint16_t read_be16(const char* data) {
  std::uint16_t network = 0;
  std::memcpy(&network, data, sizeof(network));
  return ntohs(network);
}

}  // namespace

bool utf8_ok(const std::string& text) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = cursor + text.size();
  while (cursor < end) {
    if (*cursor <= 0x7F) {
      ++cursor;
      continue;
    }
    int need = 0;
    if ((*cursor & 0xE0) == 0xC0) {
      need = 2;
    } else if ((*cursor & 0xF0) == 0xE0) {
      need = 3;
    } else if ((*cursor & 0xF8) == 0xF0) {
      need = 4;
    } else {
      return false;
    }
    if (need == 2 && *cursor < 0xC2) {
      return false;
    }
    if (end - cursor < need) {
      return false;
    }
    if (need == 3 && cursor[0] == 0xE0 && cursor[1] < 0xA0) {
      return false;
    }
    if (need == 3 && cursor[0] == 0xED && cursor[1] >= 0xA0) {
      return false;
    }
    if (need == 4 && cursor[0] == 0xF0 && cursor[1] < 0x90) {
      return false;
    }
    if (need == 4 && (cursor[0] > 0xF4 || (cursor[0] == 0xF4 && cursor[1] > 0x8F))) {
      return false;
    }
    for (int i = 1; i < need; ++i) {
      if ((cursor[i] & 0xC0) != 0x80) {
        return false;
      }
    }
    cursor += need;
  }
  return true;
}

std::string trim_ascii_space(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  return text.substr(begin);
}

bool valid_nickname(const std::string& name) {
  if (name.empty() || name.size() > kMaxNickname) {
    return false;
  }
  if (!utf8_ok(name)) {
    return false;
  }
  for (unsigned char ch : name) {
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\0') {
      return false;
    }
  }
  return true;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (char ch : text) {
    if (ch == '\n') {
      if (!current.empty()) {
        lines.push_back(current);
      }
      current.clear();
    } else if (ch != '\r') {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

bool encode_frame(std::uint32_t seq, FrameType type, const std::string& body, std::string& out) {
  if (body.size() > kMaxBody) {
    return false;
  }
  out.clear();
  out.reserve(kHeaderSize + body.size());
  append_be32(out, static_cast<std::uint32_t>(body.size()));
  append_be32(out, seq);
  append_be16(out, static_cast<std::uint16_t>(type));
  out.append(body);
  return true;
}

bool send_frame(int fd, std::uint32_t seq, FrameType type, const std::string& body) {
  std::string bytes;
  if (!encode_frame(seq, type, body, bytes)) {
    return false;
  }
  return send_all(fd, bytes.data(), bytes.size());
}

bool FrameReader::feed(const char* data, std::size_t size, std::vector<Frame>& out) {
  buffer_.append(data, size);
  while (buffer_.size() >= kHeaderSize) {
    const std::uint32_t length = read_be32(buffer_.data());
    if (length > kMaxBody) {
      return false;
    }
    if (buffer_.size() < kHeaderSize + length) {
      return true;
    }
    const std::uint32_t seq = read_be32(buffer_.data() + 4);
    const std::uint16_t raw_type = read_be16(buffer_.data() + 8);
    std::string body = buffer_.substr(kHeaderSize, length);
    buffer_.erase(0, kHeaderSize + length);
    if (raw_type < 1 || raw_type > 3 || !utf8_ok(body)) {
      return false;
    }
    Frame frame;
    frame.seq = seq;
    frame.type = static_cast<FrameType>(raw_type);
    frame.body = std::move(body);
    out.push_back(std::move(frame));
  }
  return true;
}

}  // namespace chat
