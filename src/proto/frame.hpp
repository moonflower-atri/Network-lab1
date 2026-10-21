#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chat {

constexpr std::size_t kHeaderSize = 10;
constexpr std::uint32_t kMaxBody = 4096;
constexpr std::size_t kMaxNickname = 64;

enum class FrameType : std::uint16_t {
  Join = 1,
  Text = 2,
  Members = 3,
};

struct Frame {
  std::uint32_t seq = 0;
  FrameType type = FrameType::Text;
  std::string body;
};

bool utf8_ok(const std::string& text);
bool valid_nickname(const std::string& name);
std::string trim_ascii_space(std::string text);
std::vector<std::string> split_lines(const std::string& text);

bool encode_frame(std::uint32_t seq, FrameType type, const std::string& body, std::string& out);
bool send_frame(int fd, std::uint32_t seq, FrameType type, const std::string& body);

// 凑齐一帧再输出。半帧留在内部缓冲区，连接断开时用 has_partial() 判断并丢弃。
class FrameReader {
 public:
  bool feed(const char* data, std::size_t size, std::vector<Frame>& out);
  bool has_partial() const { return !buffer_.empty(); }
  void clear() { buffer_.clear(); }

 private:
  std::string buffer_;
};

}  // namespace chat
