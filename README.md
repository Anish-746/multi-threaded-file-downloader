# Multi-Threaded HTTP/HTTPS File Downloader

A high-performance, concurrent C++ network client designed to accelerate large file downloads. By fetching non-overlapping file segments simultaneously across independent TCP connections, it significantly improves download throughput compared to single-threaded baselines.

## Key Features

* **Concurrent Download Engine**: Utilizes `std::thread` and HTTP `Range` requests to download multiple segments of a file in parallel across separate TCP connections.
* **Application-Level Lock-Free I/O**: Eliminates application level write-lock contention by assigning threads disjoint file offsets. Each thread writes directly to its pre-allocated chunk, enabling concurrent writes without a shared mutex.
* **Secure Transfers (HTTPS)**: Hardened network transfers using OpenSSL to enforce TLS 1.2+ minimums, strict X.509 certificate validation, and SNI (Server Name Indication) for correct CDN routing.
* **Scalable Architecture**: Supports up to 256 concurrent streams with minimal memory footprint by sharing a read-only `SSL_CTX` and isolating socket and TLS states per worker thread.
* **Robust Error Handling**: Engineered a zero-leak connection lifecycle handling multiple distinct error states (e.g., network drops). Thread isolation ensures that surviving workers can complete their chunks even if one worker fails.

## Project Structure

```
multi-threaded-file-downloader/
├── CMakeLists.txt          # Build system configuration
├── include/                # Header files
│   ├── URLParser.h         
│   ├── SocketUtils.h       # TCP/TLS connection utilities
│   └── Downloader.h        # Core download engine
└── src/                    # Source files
    ├── URLParser.cpp       # RFC 3986 URL parsing
    ├── SocketUtils.cpp     # POSIX DNS, TCP, and OpenSSL integration
    ├── Downloader.cpp      # Thread management and HTTP logic
    └── main.cpp            # CLI entry point
```

## Dependencies

* C++17 compliant compiler (GCC, Clang)
* CMake 3.16+
* OpenSSL (`libssl-dev` / `openssl-devel`)
* POSIX-compliant OS (Linux/macOS)

## Build Instructions

```bash
# Configure the build directory
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile the project
cmake --build build --parallel
```

## Usage

```bash
./build/downloader <URL> [thread_count]
```

### Arguments

| Argument | Description | Default |
|---|---|---|
| `URL` | The HTTP or HTTPS URL of the file to download. | **Required** |
| `thread_count` | Number of parallel download threads (1–256). | `4` |

### Examples

```bash
# Download over HTTPS with the default 4 threads
./build/downloader https://releases.ubuntu.com/noble/ubuntu-24.04.2-live-server-amd64.iso

# Download a large file using 16 threads
./build/downloader http://speedtest.tele2.net/1GB.zip 16

# Sequential baseline test (1 thread)
./build/downloader https://example.com/largefile.tar.gz 1
```

## Architecture

### Decoupled State Management
The application is designed to avoid global state and synchronization bottlenecks. 
* A single `SSL_CTX` is instantiated by the main thread and passed as a read-only reference to the workers, minimizing memory usage (under 20MB even with 32+ threads).
* Each worker thread securely opens its own POSIX socket (`fd`) and creates a dedicated TLS session (`SSL*`).
* Because each thread manages its own network I/O and file stream, data streams directly from the kernel network buffer into the filesystem cache without serialization.

### File Pre-allocation
Before starting the download, the engine issues an HTTP `HEAD` request to probe the `Content-Length`. It then pre-allocates the output file by seeking to the final byte offset and writing a null terminator. This establishes the target inode size upfront and allows all threads to execute `seekp()` to their respective chunk offsets safely.
