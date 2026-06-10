cmake_minimum_required(VERSION 3.16)
project(OP2SessionLogger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Statically link the MSVC runtime so the Windows build is a single self-contained .exe to
# share - no VC++ redistributable needed (it then only depends on opengl32.dll, which ships
# with Windows). CMAKE_MSVC_RUNTIME_LIBRARY drives our targets (CMP0091, default NEW here);
# USE_MSVC_RUNTIME_LIBRARY_DLL=OFF makes the fetched GLFW link the static CRT to match.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# op2net - the net-client library. NO GUI dependency: this builds and links on
# its own and could back a CLI, tests, or any other front-end.
# ---------------------------------------------------------------------------
add_library(op2net STATIC
  src/net/op2proto.cpp
  src/net/op2client.cpp
)
target_include_directories(op2net PUBLIC src/net)
find_package(Threads REQUIRED)
target_link_libraries(op2net PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(op2net PUBLIC ws2_32)
  # We intentionally use the simple, portable BSD-style APIs (gethostbyname/inet_addr/strncpy).
  target_compile_definitions(op2net PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Dependencies for the GUI (fetched so it builds out-of-the-box on Win + Linux).
# Linux note: GLFW needs system dev packages (e.g. xorg-dev / libwayland-dev).
# ---------------------------------------------------------------------------
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5)
FetchContent_MakeAvailable(imgui)

find_package(OpenGL REQUIRED)

# ImGui has no CMake of its own - compile the core + the GLFW/OpenGL3 backends.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# The GUI executable.
# ---------------------------------------------------------------------------
add_executable(OP2SessionLogger src/gui/main.cpp)
target_link_libraries(OP2SessionLogger PRIVATE op2net imgui glfw OpenGL::GL)

# Windows app/window icon (Structure.ico). Configure the .rc with an absolute icon path so the RC
# compiler always resolves it; GLFW picks up the "GLFW_ICON" resource for the window.
if(WIN32)
  set(OP2_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/Structure.ico")
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
  target_sources(OP2SessionLogger PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()
if(WIN32)
  # Use the GUI subsystem (no console window) for release builds. The GUI subsystem defaults
  # to a WinMain entry point, so redirect it to our standard main() via mainCRTStartup.
  set_target_properties(OP2SessionLogger PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
  if(MSVC)
    target_link_options(OP2SessionLogger PRIVATE $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
  endif()
endif()
