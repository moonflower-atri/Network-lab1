cmake_minimum_required(VERSION 3.20)

project(lan_chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 配置：cmake -S . -B build
# 构建：cmake --build build
# 服务器：./build/chat_server
# 客户端：./build/chat_client
#
# Linux 上需要 OpenGL、Fontconfig、X11 开发包，以及字体包 fonts-noto-cjk。

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Fontconfig REQUIRED)

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# 实验室环境通常有 X11 或 XWayland。关掉 Wayland 后，配置时不需要 wayland 开发包。
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.5.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

# Dear ImGui 没有自己的 CMakeLists.txt，只取出源码再由下面的目标编译。
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  # ImGui 不提供 CMakeLists.txt，只能把源码下载下来再手工列进目标。
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(imgui)
endif()

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

add_library(chat_core
  src/net/socket.cpp
  src/proto/frame.cpp
)
target_include_directories(chat_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(chat_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chat_server src/server/main.cpp)
target_link_libraries(chat_server PRIVATE chat_core Threads::Threads)
target_compile_options(chat_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chat_client src/client/main.cpp)
target_link_libraries(chat_client PRIVATE
  chat_core
  imgui_lib
  Fontconfig::Fontconfig
  Threads::Threads
)
target_compile_options(chat_client PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chat_smoke tests/smoke_test.cpp)
target_link_libraries(chat_smoke PRIVATE chat_core Threads::Threads)
target_compile_options(chat_smoke PRIVATE -Wall -Wextra -Wpedantic)
