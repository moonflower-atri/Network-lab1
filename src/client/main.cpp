#include "net/socket.hpp"
#include "proto/frame.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <fontconfig/fontconfig.h>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

enum class Phase { Login, Chat };

enum class CloseKind { None, Eof, EofPartial, Protocol, Local };

struct Line {
  std::uint32_t seq = 0;
  std::string text;
};

struct App {
  Phase phase = Phase::Login;
  char host[128] = "127.0.0.1";
  int port = 9000;
  char nick[chat::kMaxNickname + 1] = "";
  char draft[2001] = "";
  std::string notice;
  std::string font_warning;

  int fd = -1;
  pthread_t recv_thread{};
  bool recv_started = false;
  pthread_mutex_t mu{};
  std::vector<chat::Frame> inbox;
  chat::FrameReader reader;
  bool peer_closed = false;
  CloseKind close_kind = CloseKind::None;

  std::vector<std::string> users;
  int selected = -1;
  std::vector<Line> lines;
  std::uint32_t expect = 1;
  std::vector<std::uint32_t> gaps;
  std::uint32_t out_seq = 0;
};

struct FoundFont {
  std::string path;
  int index = 0;
  bool ok = false;
};

FoundFont find_noto_sans_cjk_sc() {
  FoundFont found;
  if (!FcInit()) {
    return found;
  }
  FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("Noto Sans CJK SC"));
  FcObjectSet* objects = pattern == nullptr ? nullptr : FcObjectSetBuild(FC_FILE, FC_INDEX, nullptr);
  FcFontSet* fonts = pattern == nullptr || objects == nullptr ? nullptr : FcFontList(nullptr, pattern, objects);
  if (fonts != nullptr && fonts->nfont > 0) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(fonts->fonts[0], FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
      found.path = reinterpret_cast<char*>(file);
      int index = 0;
      if (FcPatternGetInteger(fonts->fonts[0], FC_INDEX, 0, &index) == FcResultMatch && index > 0) {
        found.index = index;
      }
      found.ok = !found.path.empty();
    }
  }
  if (fonts != nullptr) {
    FcFontSetDestroy(fonts);
  }
  if (objects != nullptr) {
    FcObjectSetDestroy(objects);
  }
  if (pattern != nullptr) {
    FcPatternDestroy(pattern);
  }
  return found;
}

void note_seq(App& app, std::uint32_t seq) {
  if (seq == 0) {
    return;
  }
  if (seq == app.expect) {
    ++app.expect;
  } else if (seq > app.expect) {
    for (std::uint32_t missing = app.expect; missing < seq; ++missing) {
      app.gaps.push_back(missing);
    }
    app.expect = seq + 1;
  }
}

void apply_frame(App& app, const chat::Frame& frame) {
  note_seq(app, frame.seq);
  if (frame.type == chat::FrameType::Members) {
    app.users = chat::split_lines(frame.body);
    if (app.selected >= static_cast<int>(app.users.size())) {
      app.selected = -1;
    }
    return;
  }
  if (frame.type == chat::FrameType::Text) {
    app.lines.push_back(Line{frame.seq, frame.body});
  }
}

const char* close_message(CloseKind kind) {
  switch (kind) {
    case CloseKind::EofPartial:
      return "连接断开，未收完的半帧已丢弃";
    case CloseKind::Protocol:
      return "收到无法识别的数据，连接已断开";
    case CloseKind::Local:
      return "已断开连接";
    case CloseKind::Eof:
    case CloseKind::None:
      return "与服务器的连接已断开";
  }
  return "与服务器的连接已断开";
}

void teardown(App& app) {
  if (app.fd >= 0) {
    ::shutdown(app.fd, SHUT_RDWR);
  }
  if (app.recv_started) {
    pthread_join(app.recv_thread, nullptr);
    app.recv_started = false;
  }
  if (app.fd >= 0) {
    chat::close_conn(app.fd);
    app.fd = -1;
  }
  pthread_mutex_lock(&app.mu);
  app.inbox.clear();
  app.peer_closed = false;
  pthread_mutex_unlock(&app.mu);
  app.reader.clear();
}

void* recv_main(void* arg) {
  auto* app = static_cast<App*>(arg);
  char buffer[8192];
  while (true) {
    const int n = chat::recv_some(app->fd, buffer, sizeof(buffer));
    std::vector<chat::Frame> frames;
    bool protocol_ok = true;
    if (n > 0) {
      protocol_ok = app->reader.feed(buffer, static_cast<std::size_t>(n), frames);
    }
    pthread_mutex_lock(&app->mu);
    app->inbox.insert(app->inbox.end(), std::make_move_iterator(frames.begin()),
                      std::make_move_iterator(frames.end()));
    if (n <= 0 || !protocol_ok) {
      app->peer_closed = true;
      if (app->close_kind == CloseKind::None) {
        if (!protocol_ok) {
          app->close_kind = CloseKind::Protocol;
        } else if (app->reader.has_partial()) {
          app->close_kind = CloseKind::EofPartial;
        } else {
          app->close_kind = CloseKind::Eof;
        }
      }
      pthread_mutex_unlock(&app->mu);
      break;
    }
    pthread_mutex_unlock(&app->mu);
  }
  return nullptr;
}

void reset_room(App& app) {
  app.users.clear();
  app.selected = -1;
  app.lines.clear();
  app.expect = 1;
  app.gaps.clear();
  app.out_seq = 0;
  app.draft[0] = '\0';
  app.reader.clear();
  pthread_mutex_lock(&app.mu);
  app.inbox.clear();
  app.peer_closed = false;
  app.close_kind = CloseKind::None;
  pthread_mutex_unlock(&app.mu);
}

void disconnect_locally(App& app) {
  pthread_mutex_lock(&app.mu);
  app.close_kind = CloseKind::Local;
  pthread_mutex_unlock(&app.mu);
  teardown(app);
  app.phase = Phase::Login;
  app.notice = close_message(CloseKind::Local);
}

bool start_chat(App& app, int fd, const std::string& nick) {
  reset_room(app);
  app.fd = fd;
  if (pthread_create(&app.recv_thread, nullptr, recv_main, &app) != 0) {
    chat::close_conn(fd);
    app.fd = -1;
    app.notice = "无法创建接收线程";
    return false;
  }
  app.recv_started = true;
  if (!chat::send_frame(fd, ++app.out_seq, chat::FrameType::Join, nick)) {
    disconnect_locally(app);
    app.notice = "发送昵称失败";
    return false;
  }
  app.phase = Phase::Chat;
  app.notice.clear();
  return true;
}

void try_connect(App& app) {
  const std::string nick = chat::trim_ascii_space(app.nick);
  if (!chat::valid_nickname(nick)) {
    app.notice = "昵称需要是 1 到 64 字节的 UTF-8，且不能包含换行";
    return;
  }
  if (app.host[0] == '\0') {
    app.notice = "请填写服务器地址";
    return;
  }
  if (app.port < 1 || app.port > 65535) {
    app.notice = "端口需要在 1 到 65535 之间";
    return;
  }
  std::snprintf(app.nick, sizeof(app.nick), "%s", nick.c_str());
  app.notice = "正在连接...";
  chat::Conn conn = chat::connect_to(app.host, static_cast<std::uint16_t>(app.port), 3000);
  if (conn.fd < 0) {
    app.notice = conn.error;
    return;
  }
  start_chat(app, conn.fd, nick);
}

void send_draft(App& app) {
  const std::string text = chat::trim_ascii_space(app.draft);
  if (text.empty()) {
    return;
  }
  if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos ||
      text.size() > 2000 || !chat::utf8_ok(text)) {
    app.notice = "这条消息无法发送";
    return;
  }
  if (!chat::send_frame(app.fd, ++app.out_seq, chat::FrameType::Text, text)) {
    disconnect_locally(app);
    app.notice = "发送失败，连接已断开";
    return;
  }
  app.draft[0] = '\0';
}

void pump(App& app) {
  std::vector<chat::Frame> frames;
  bool closed = false;
  CloseKind kind = CloseKind::None;
  pthread_mutex_lock(&app.mu);
  frames.swap(app.inbox);
  closed = app.peer_closed;
  kind = app.close_kind;
  pthread_mutex_unlock(&app.mu);

  for (const chat::Frame& frame : frames) {
    apply_frame(app, frame);
  }
  if (app.phase == Phase::Chat && closed) {
    teardown(app);
    app.phase = Phase::Login;
    app.notice = close_message(kind);
  }
}

void begin_root() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
}

std::string seq_status(const App& app) {
  if (app.gaps.empty()) {
    return "接收序号连续，下一号 " + std::to_string(app.expect);
  }
  std::string text = "序号缺口：";
  const std::size_t shown = std::min<std::size_t>(app.gaps.size(), 8);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += std::to_string(app.gaps[i]);
  }
  if (app.gaps.size() > shown) {
    text += " ...";
  }
  return text;
}

void draw_login(App& app) {
  begin_root();
  if (!app.font_warning.empty()) {
    ImGui::TextUnformatted(app.font_warning.c_str());
    ImGui::Separator();
  }
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImGui::SetCursorPosX((avail.x - 360.0f) * 0.5f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - 280.0f) * 0.35f);
  ImGui::BeginChild("login", ImVec2(360, 250), true);
  ImGui::TextUnformatted("加入全局聊天室");
  ImGui::Spacing();
  bool submit = false;
  submit |= ImGui::InputText("昵称", app.nick, sizeof(app.nick), ImGuiInputTextFlags_EnterReturnsTrue);
  submit |= ImGui::InputText("服务器", app.host, sizeof(app.host), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::InputInt("端口", &app.port);
  submit |= ImGui::Button("连接");
  if (submit) {
    try_connect(app);
  }
  if (!app.notice.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(app.notice.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
  ImGui::End();
}

void draw_chat(App& app) {
  begin_root();
  ImGui::BeginChild("users", ImVec2(220, 0), true);
  ImGui::TextUnformatted("在线");
  ImGui::Separator();
  ImGui::BeginChild("userlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
  if (app.users.empty()) {
    ImGui::TextUnformatted("还没有用户");
  }
  for (int i = 0; i < static_cast<int>(app.users.size()); ++i) {
    ImGui::PushID(i);
    if (ImGui::Selectable(app.users[static_cast<std::size_t>(i)].c_str(), app.selected == i)) {
      app.selected = i;
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (ImGui::Button("断开", ImVec2(-1, 0))) {
    disconnect_locally(app);
  }
  ImGui::EndChild();

  if (app.phase != Phase::Chat) {
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::BeginChild("room", ImVec2(0, 0), false);
  ImGui::TextUnformatted("全局聊天室");
  const std::string status = seq_status(app);
  ImGui::TextUnformatted(status.c_str());
  if (!app.notice.empty()) {
    ImGui::TextUnformatted(app.notice.c_str());
  }
  const float footer = ImGui::GetFrameHeightWithSpacing();
  ImGui::BeginChild("log", ImVec2(0, -footer), true);
  const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f;
  for (const Line& line : app.lines) {
    const std::string shown = std::to_string(line.seq) + "  " + line.text;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopTextWrapPos();
  }
  if (follow) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();

  ImGui::SetNextItemWidth(-80.0f);
  bool send = ImGui::InputText("##draft", app.draft, sizeof(app.draft), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  send |= ImGui::Button("发送");
  if (send) {
    send_draft(app);
  }
  ImGui::EndChild();
  ImGui::End();
}

void glfw_error_callback(int error, const char* description) {
  std::fprintf(stderr, "glfw %d: %s\n", error, description);
}

}  // namespace

int main() {
  chat::ignore_sigpipe();
  App app;
  pthread_mutex_init(&app.mu, nullptr);

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) {
    std::fputs("无法初始化 GLFW\n", stderr);
    return 1;
  }

  float scale = 1.0f;
  if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
    scale = ImGui_ImplGlfw_GetContentScaleForMonitor(monitor);
    if (scale < 1.0f) {
      scale = 1.0f;
    }
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  GLFWwindow* window = glfwCreateWindow(static_cast<int>(960 * scale), static_cast<int>(640 * scale), "聊天室",
                                        nullptr, nullptr);
  if (window == nullptr) {
    std::fputs("无法创建窗口\n", stderr);
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  glfwSetWindowSizeLimits(window, 720, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsLight();
  ImGuiStyle& style = ImGui::GetStyle();
  style.FrameRounding = 3.0f;
  style.WindowRounding = 0.0f;
  style.FontSizeBase = 18.0f;
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(nullptr);

  const FoundFont font = find_noto_sans_cjk_sc();
  if (!font.ok) {
    app.font_warning = "未找到 Noto Sans CJK SC。请安装 fonts-noto-cjk。Noto Sans CJK SC not found.";
    std::fputs("未找到 Noto Sans CJK SC，请安装 fonts-noto-cjk\n", stderr);
  } else {
    ImFontConfig config;
    config.FontNo = static_cast<ImU32>(font.index);
    if (io.Fonts->AddFontFromFileTTF(font.path.c_str(), 18.0f, &config) == nullptr) {
      app.font_warning = "Noto Sans CJK SC 加载失败。Failed to load Noto Sans CJK SC.";
      std::fprintf(stderr, "加载字体失败: %s\n", font.path.c_str());
    }
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    pump(app);
    if (app.phase == Phase::Chat) {
      draw_chat(app);
    } else {
      draw_login(app);
    }
    ImGui::Render();
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.94f, 0.94f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  if (app.phase == Phase::Chat) {
    disconnect_locally(app);
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  pthread_mutex_destroy(&app.mu);
  return 0;
}
