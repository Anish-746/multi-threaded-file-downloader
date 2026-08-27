cmake_minimum_required(VERSION 3.16)

project(MultiThreadedDownloader
    VERSION     2.0.0
    DESCRIPTION "Multi-Threaded HTTP/HTTPS File Downloader — raw POSIX sockets + OpenSSL"
    LANGUAGES   CXX
)

# ─── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─── Compiler Warning Flags ───────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wshadow
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

# ─── Build Type Defaults ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ─── Source Files ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/URLParser.cpp
    src/SocketUtils.cpp
    src/Downloader.cpp
)

# ─── Executable Target ────────────────────────────────────────────────────────
add_executable(downloader ${SOURCES})

# ─── Include Directories ──────────────────────────────────────────────────────
target_include_directories(downloader
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

# ─── Link Libraries ───────────────────────────────────────────────────────────

# POSIX Threads — std::thread is backed by pthreads on Linux
find_package(Threads REQUIRED)

# OpenSSL — provides libssl (TLS) and libcrypto (algorithms, X.509)
# FindOpenSSL exports two imported targets:
#   OpenSSL::SSL    → -lssl    (TLS record layer, handshake, SSL_* API)
#   OpenSSL::Crypto → -lcrypto (RSA, AES, SHA, X.509, BN, etc.)
# Both are required: SSL depends on Crypto internally.
find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL version : ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include : ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libs    : ${OPENSSL_LIBRARIES}")

target_link_libraries(downloader
    PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

# ─── Installation ─────────────────────────────────────────────────────────────
install(TARGETS downloader RUNTIME DESTINATION bin)

# ─── Build Summary ────────────────────────────────────────────────────────────
message(STATUS "Configuring ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  C++ Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "  Build with:")
message(STATUS "    cmake -B build && cmake --build build")
message(STATUS "  Run:")
message(STATUS "    ./build/downloader https://example.com/file.zip 4")
