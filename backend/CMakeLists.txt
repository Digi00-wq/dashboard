cmake_minimum_required(VERSION 3.20)
project(dashboard_backend VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. System dependencies required by uSockets & Protobuf
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)


# 2. Fetch uWebSockets & uSockets via CMake FetchContent
include(FetchContent)

# First: Fetch uSockets (the underlying C event/networking library)
FetchContent_Declare(
    uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(uSockets)

# Build uSockets target manually since upstream uSockets does not provide a CMakeLists.txt
if(NOT TARGET uSockets)
    message(STATUS "usockets_SOURCE_DIR is: ${usockets_SOURCE_DIR}")
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
    message(STATUS "USOCKETS_SRCS: ${USOCKETS_SRCS}")
    add_library(uSockets STATIC ${USOCKETS_SRCS})
    target_compile_definitions(uSockets PUBLIC LIBUS_USE_OPENSSL LIBUS_USE_EPOLL)
    target_include_directories(uSockets PUBLIC "${usockets_SOURCE_DIR}/src")
    target_link_libraries(uSockets PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
endif()




# Second: Fetch uWebSockets (the C++ header abstraction layer)
FetchContent_Declare(
    uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.70.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(uWebSockets)


# 3. Compile your Protobuf schema from the shared proto/ directory
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ../proto/dashboard.proto)

# 4. Define your executable target
add_executable(dashboard_backend
    src/main.cpp 
    ${PROTO_SRCS} 
    ${PROTO_HDRS}
)

# 5. Include header directories
target_include_directories(dashboard_backend PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}       # For generated protobuf headers (dashboard.pb.h)
    ${uwebsockets_SOURCE_DIR}/src      # <App.h>
    ${usockets_SOURCE_DIR}/src         # <libusockets.h>
)

# 6. Link dependencies
target_link_libraries(dashboard_backend PRIVATE
    uSockets
    ${PROTOBUF_LDFLAGS}
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    Threads::Threads
)


