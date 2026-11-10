# op2net - the shared Outpost 2 net-client library used by OP2DummyPlayer, OP2SessionLogger and
# OP2Spectator. NO GUI dependency: it builds and links on its own and could back a CLI, tests, or
# any other front-end. Pull it in from an app with:
#   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/op2net ${CMAKE_CURRENT_BINARY_DIR}/op2net)
add_library(op2net STATIC
  op2proto.cpp
  op2client.cpp
)
target_include_directories(op2net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(op2net PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(op2net PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(op2net PUBLIC ws2_32)
  # We intentionally use the simple, portable BSD-style APIs (gethostbyname/inet_addr/strncpy).
  target_compile_definitions(op2net PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()
