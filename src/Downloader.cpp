#include "Downloader.h"
#include "SocketUtils.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <chrono>
#include <algorithm>   // std::min
#include <cstring>     // strerror
#include <sys/resource.h>

static long getVmRSS_KB() {
    std::ifstream stat_stream("/proc/self/status");
    std::string line;
    while (std::getline(stat_stream, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            try { return std::stol(line.substr(6)); } catch(...) {}
        }
    }
    return 0;
}

static long getVmHWM_KB() {
    std::ifstream stat_stream("/proc/self/status");
    std::string line;
    while (std::getline(stat_stream, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            try { return std::stol(line.substr(6)); } catch(...) {}
        }
    }
    return 0;
}

Downloader::Downloader(const std::string& url, int threadCount, SSL_CTX* ctx)
    : m_url(url), m_threadCount(threadCount), m_sslCtx(ctx)
{
    if (threadCount < 1)
        throw std::invalid_argument("Thread count must be >= 1.");
}

void Downloader::run() {
    const auto t0 = std::chrono::steady_clock::now();
    const long baselineRSS = getVmRSS_KB();

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              << "║       Multi-Threaded HTTP/S File Downloader          ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    std::cout << "[1/5] Parsing URL...\n";
    const ParsedURL parsed = URLParser::parse(m_url);
    std::cout << "      Scheme : " << parsed.scheme << "\n"
              << "      Host   : " << parsed.host   << "\n"
              << "      Port   : " << parsed.port   << "\n"
              << "      Path   : " << parsed.path   << "\n\n";

    if (parsed.isHttps() && !m_sslCtx)
        throw std::runtime_error("HTTPS URL requires a valid SSL_CTX.");

    std::cout << "[2/5] Fetching resource metadata (HTTP HEAD)...\n";
    const long long fileSize = fetchContentLength(parsed);
    std::cout << "      Content-Length: " << fileSize << " bytes ("
              << (static_cast<double>(fileSize) / 1024.0 / 1024.0) << " MiB)\n\n";

    std::string outputPath = parsed.path;
    const size_t lastSlash = outputPath.rfind('/');
    outputPath = (lastSlash != std::string::npos && lastSlash + 1 < outputPath.size())
                 ? outputPath.substr(lastSlash + 1)
                 : "downloaded_file";

    std::cout << "[3/5] Pre-allocating '" << outputPath << "' (" << fileSize << " bytes)...\n";
    preallocateFile(outputPath, fileSize);
    std::cout << "      Done.\n\n";

    const int actualThreads = static_cast<int>(
        std::min(static_cast<long long>(m_threadCount), fileSize));
    if (actualThreads < m_threadCount)
        std::cout << "      [WARN] Reduced to " << actualThreads << " thread(s) (file too small).\n";

    std::cout << "[4/5] Partitioning into " << actualThreads << " chunk(s)...\n";
    const std::vector<ChunkInfo> chunks = partitionChunks(fileSize, actualThreads);
    for (const auto& c : chunks)
        std::cout << "      T" << c.threadId
                  << " → [" << c.startByte << " – " << c.endByte << "] "
                  << "(" << (c.endByte - c.startByte + 1) << " bytes)\n";
    std::cout << "\n";

    std::cout << "[5/5] Starting " << actualThreads << " thread(s)...\n\n";

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(actualThreads));

    for (const auto& chunk : chunks) {
        // Capture by value — each thread owns its data; catch here to avoid std::terminate()
        threads.emplace_back([this, parsed, chunk, outputPath]() {
            try {
                downloadChunk(parsed, chunk, outputPath);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(m_consoleMutex);
                std::cerr << "[T" << chunk.threadId << "] ERROR: " << ex.what() << "\n";
            }
        });
    }

    for (auto& t : threads)
        t.join();

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    const double speed = (static_cast<double>(fileSize) / 1024.0 / 1024.0) / elapsed;

    long finalRSS = getVmRSS_KB();
    long peakRSS = getVmHWM_KB();
    if (peakRSS == 0) {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            peakRSS = usage.ru_maxrss;
        }
    }

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              << "║                  Download Complete!                  ║\n"
              << "╚══════════════════════════════════════════════════════╝\n"
              << "  File    : " << outputPath    << "\n"
              << "  Size    : " << fileSize      << " bytes\n"
              << "  Time    : " << elapsed       << " s\n"
              << "  Speed   : " << speed         << " MiB/s\n"
              << "  Threads : " << actualThreads << "\n\n"
              << "  --- Memory Instrumentation (/proc/self/status) ---\n"
              << "  Buffer per worker: 8192 bytes\n"
              << "  Baseline RSS     : " << ((double) baselineRSS / 1024.0) << " MB\n"
              << "  Peak RSS (VmHWM) : " << ((double) peakRSS / 1024.0) << " MB\n"
              << "  Final RSS        : " << ((double) finalRSS / 1024.0) << " MB\n"
              << "  Additional RSS   : " << ((double) (peakRSS - baselineRSS) / 1024.0) << " MB\n\n";
}

long long Downloader::fetchContentLength(const ParsedURL& parsed) {
    Connection conn = SocketUtils::createConnection(
        parsed.host, parsed.port, parsed.isHttps(), m_sslCtx);

    const std::string req =
        "HEAD " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "User-Agent: MultiThreadedDownloader/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SocketUtils::sendAll(conn, req);

    std::string response;
    response.reserve(4096);
    char buf[4096];

    while (true) {
        const ssize_t n = SocketUtils::recvFrom(conn, buf, sizeof(buf));
        if (n < 0) { throw std::runtime_error("recv failed during HEAD"); }
        if (n == 0) break;
        response.append(buf, static_cast<size_t>(n));
        if (response.find("\r\n\r\n") != std::string::npos) break;
    }
    // conn goes out of scope here — destructor runs SSL_shutdown → SSL_free → close(fd).

    if (response.find("HTTP/1.1 200") == std::string::npos &&
        response.find("HTTP/1.0 200") == std::string::npos) {
        const size_t eol = response.find("\r\n");
        throw std::runtime_error("Non-200 HEAD response: " +
            (eol != std::string::npos ? response.substr(0, eol) : response.substr(0, 80)));
    }

    auto findHeader = [&](const std::string& name) -> long long {
        size_t pos = response.find(name);
        if (pos == std::string::npos) return -1LL;
        pos += name.size();
        while (pos < response.size() && response[pos] == ' ') ++pos;
        const size_t start = pos;
        while (pos < response.size() && response[pos] != '\r' && response[pos] != '\n') ++pos;
        try { return std::stoll(response.substr(start, pos - start)); } catch (...) { return -1LL; }
    };

    long long len = findHeader("Content-Length: ");
    if (len < 0) len = findHeader("content-length: ");
    if (len < 0) throw std::runtime_error("Server did not send Content-Length.");
    if (len == 0) throw std::runtime_error("Content-Length is 0; nothing to download.");
    return len;
}

void Downloader::preallocateFile(const std::string& path, long long size) {
    std::fstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        throw std::runtime_error("Cannot create file: " + path);

    f.seekp(size - 1);   // sparse write: sets inode i_size without filling zeros
    f.write("\0", 1);

    if (!f.good())
        throw std::runtime_error("Pre-allocation failed for '" + path + "'. Disk full?");
    f.close();
}

std::vector<ChunkInfo> Downloader::partitionChunks(long long fileSize, int n) {
    std::vector<ChunkInfo> chunks;
    chunks.reserve(static_cast<size_t>(n));
    const long long chunkSize = fileSize / n;

    for (int i = 0; i < n; ++i) {
        ChunkInfo c;
        c.threadId  = i;
        c.startByte = static_cast<long long>(i) * chunkSize;
        c.endByte   = (i == n - 1) ? fileSize - 1 : c.startByte + chunkSize - 1;  // last absorbs remainder
        chunks.push_back(c);
    }
    return chunks;
}

void Downloader::downloadChunk(const ParsedURL& parsed,
                               const ChunkInfo& chunk,
                               const std::string& outputPath) {
    {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        std::cout << "  [T" << chunk.threadId << "] Starting → "
                  << "[" << chunk.startByte << " – " << chunk.endByte << "]\n";
    }

    const auto t0 = std::chrono::steady_clock::now();

    // Own TCP fd + own SSL* — sharing would serialise all I/O across threads
    Connection conn = SocketUtils::createConnection(
        parsed.host, parsed.port, parsed.isHttps(), m_sslCtx);

    const std::string range = "bytes=" + std::to_string(chunk.startByte) +
                              "-"      + std::to_string(chunk.endByte);
    const std::string req =
        "GET " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "User-Agent: MultiThreadedDownloader/1.0\r\n"
        "Range: " + range + "\r\n"  // RFC 7233: zero-indexed, inclusive
        "Connection: close\r\n"
        "\r\n";
    SocketUtils::sendAll(conn, req);

    // std::ios::in prevents truncating the pre-allocated file on open;
    // each thread's fstream has its own kernel file offset — no mutex needed
    std::fstream outFile(outputPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!outFile.is_open()) {
        throw std::runtime_error("[T" + std::to_string(chunk.threadId) +
                                 "] Cannot open output file: " + outputPath);
        // conn destructor will close the socket on the throw path.
    }
    outFile.seekp(chunk.startByte);

    static constexpr size_t BUF = 8192;
    char recvBuf[BUF];
    std::string headerBuf;
    headerBuf.reserve(4096);
    bool headerDone = false;

    long long written = 0;
    const long long total = chunk.endByte - chunk.startByte + 1;

    while (true) {
        const ssize_t n = SocketUtils::recvFrom(conn, recvBuf, BUF);

        if (n < 0) {
            // Capture error BEFORE the Connection destructor runs: SSL_free()
            // clears the OpenSSL error queue, so networkError() must be called
            // first. closeConnection() is called explicitly here to control the
            // order; the destructor then becomes a no-op (sentinels are nulled).
            const std::string errMsg = SocketUtils::networkError(conn);
            SocketUtils::closeConnection(conn);
            throw std::runtime_error("[T" + std::to_string(chunk.threadId) +
                                     "] recv error: " + errMsg);
        }
        if (n == 0) break;  // TCP FIN or TLS close_notify

        if (!headerDone) {
            headerBuf.append(recvBuf, static_cast<size_t>(n));
            const size_t sep = headerBuf.find("\r\n\r\n");
            if (sep == std::string::npos) continue;

            headerDone = true;

            const bool is206 = headerBuf.find("HTTP/1.1 206") != std::string::npos ||
                               headerBuf.find("HTTP/1.0 206") != std::string::npos;
            const bool is200 = headerBuf.find("HTTP/1.1 200") != std::string::npos ||
                               headerBuf.find("HTTP/1.0 200") != std::string::npos;

            if (!is206 && !is200) {
                const size_t eol = headerBuf.find("\r\n");
                throw std::runtime_error("[T" + std::to_string(chunk.threadId) +
                    "] Unexpected status: " + headerBuf.substr(0, eol));
                // conn destructor closes the socket on the throw path.
            }
            if (is200 && !is206) {
                std::lock_guard<std::mutex> lock(m_consoleMutex);
                std::cerr << "  [T" << chunk.threadId
                          << "] WARNING: server returned 200, not 206 — range ignored.\n";
            }

            // Body bytes that arrived in the same recv() call as the headers
            const size_t bodyStart = sep + 4;
            if (bodyStart < headerBuf.size()) {
                const size_t avail = headerBuf.size() - bodyStart;
                const size_t toWrite = static_cast<size_t>(
                    std::min(static_cast<long long>(avail), total - written));
                outFile.write(headerBuf.c_str() + bodyStart,
                              static_cast<std::streamsize>(toWrite));
                written += static_cast<long long>(toWrite);
            }
        } else {
            const size_t toWrite = static_cast<size_t>(
                std::min(static_cast<long long>(n), total - written));
            outFile.write(recvBuf, static_cast<std::streamsize>(toWrite));
            written += static_cast<long long>(toWrite);
        }

        if (written >= total) break;
    }

    outFile.flush();
    outFile.close();
    // conn destructor runs here: SSL_shutdown → SSL_free → close(fd).

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        std::cout << "  [T" << chunk.threadId << "] Done → "
                  << written << "/" << total << " bytes"
                  << "  (" << elapsed << "s"
                  << "  ~" << (static_cast<double>(written) / 1024.0 / 1024.0 / elapsed)
                  << " MiB/s)\n";
    }
}
