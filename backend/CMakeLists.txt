cmake_minimum_required(VERSION 3.20)
project(dashboard_backend VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 1. System dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

# 2. nlohmann/json (system package if available, otherwise download)
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

# 3. uSockets (no upstream CMakeLists -> build it ourselves)
FetchContent_Declare(
    uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(uSockets)

if(NOT TARGET uSockets)
    set(USOCKETS_SRCS
        "${usockets_SOURCE_DIR}/src/bsd.c"
        "${usockets_SOURCE_DIR}/src/context.c"
        "${usockets_SOURCE_DIR}/src/loop.c"
        "${usockets_SOURCE_DIR}/src/socket.c"
        "${usockets_SOURCE_DIR}/src/udp.c"
        "${usockets_SOURCE_DIR}/src/quic.c"
        "${usockets_SOURCE_DIR}/src/eventing/epoll_kqueue.c"
        "${usockets_SOURCE_DIR}/src/crypto/openssl.c"
        "${usockets_SOURCE_DIR}/src/crypto/sni_tree.cpp"
    )
    add_library(uSockets STATIC ${USOCKETS_SRCS})
    target_compile_definitions(uSockets PUBLIC LIBUS_USE_OPENSSL LIBUS_USE_EPOLL)
    target_include_directories(uSockets PUBLIC "${usockets_SOURCE_DIR}/src")
    target_link_libraries(uSockets PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
endif()

# 4. uWebSockets (header only)
FetchContent_Declare(
    uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.70.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(uWebSockets)

# 5. Protobuf code from the shared proto/ directory
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${PROTO_DIR}/dashboard.proto")

# 6. Executable
add_executable(dashboard_backend
    src/main.cpp
    src/config.cpp
    src/http_client.cpp
    src/home_assistant.cpp
    src/ha_events.cpp
    src/system_stats.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(dashboard_backend PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}       # dashboard.pb.h
    ${uwebsockets_SOURCE_DIR}/src     # <App.h>
    ${usockets_SOURCE_DIR}/src        # <libusockets.h>
)

target_link_libraries(dashboard_backend PRIVATE
    uSockets
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${PROTOBUF_LDFLAGS}
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    Threads::Threads
)

install(TARGETS dashboard_backend RUNTIME DESTINATION bin)
