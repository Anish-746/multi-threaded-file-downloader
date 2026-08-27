#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <openssl/ssl.h>   // SSL_CTX*
#include "URLParser.h"

// Byte range assigned to one worker thread. Zero-indexed, inclusive on both ends.
// Matches "Range: bytes=startByte-endByte" (RFC 7233).
struct ChunkInfo {
    int       threadId;
    long long startByte;
    long long endByte;
};

// Orchestrates the full download pipeline: HEAD → pre-alloc → partition →
// spawn threads → join. Single-use: call run() exactly once.
class Downloader {
public:
    // `ctx` may be nullptr when downloading plain HTTP only.
    // For HTTPS URLs, a valid SSL_CTX* (created by the caller) must be passed;
    // it is shared read-only across all worker threads (OpenSSL 1.1+ is thread-safe).
    Downloader(const std::string& url, int threadCount, SSL_CTX* ctx = nullptr);
    void run();

private:
    std::string m_url;
    int         m_threadCount;
    SSL_CTX*    m_sslCtx;        // shared, not owned — caller manages lifetime
    std::mutex  m_consoleMutex;  // guards std::cout only; no file I/O mutex needed

    long long fetchContentLength(const ParsedURL& parsed);
    void preallocateFile(const std::string& outputPath, long long fileSize);
    std::vector<ChunkInfo> partitionChunks(long long fileSize, int threadCount);
    void downloadChunk(const ParsedURL& parsed,
                       const ChunkInfo& chunk,
                       const std::string& outputPath);
};
