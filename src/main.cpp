/**
 * @file main.cpp
 * @brief CLI entry point — initialises OpenSSL once, runs the downloader.
 *
 * Usage:
 *   ./downloader <URL> [thread_count]
 *
 * Examples:
 *   ./downloader http://speedtest.tele2.net/1MB.zip 4
 *   ./downloader https://releases.ubuntu.com/noble/ubuntu-24.04.2-live-server-amd64.iso 8
 */

#include <iostream>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "Downloader.h"

static void printUsage(const char* prog) {
    std::cerr << "\nUsage:\n"
              << "  " << prog << " <URL> [thread_count]\n\n"
              << "Arguments:\n"
              << "  URL           http:// or https:// URL to download\n"
              << "  thread_count  Parallel download threads (default: 4, max: 256)\n\n"
              << "Examples:\n"
              << "  " << prog << " http://speedtest.tele2.net/1MB.zip\n"
              << "  " << prog << " https://releases.ubuntu.com/.../ubuntu.iso 8\n\n";
}

int main(int argc, char* argv[]) {

    if (argc < 2) {
        std::cerr << "[ERROR] Missing required argument: URL\n";
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string url = argv[1];
    int threadCount = 4;

    if (argc >= 3) {
        try {
            threadCount = std::stoi(argv[2]);
            if (threadCount < 1 || threadCount > 256)
                throw std::out_of_range("must be 1–256");
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Invalid thread count '" << argv[2]
                      << "': " << e.what() << "\n";
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Init OpenSSL once — loads TLS algorithms and error strings before any threads spawn
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    // SSL_CTX: shared read-only factory for all SSL* objects; thread-safe in OpenSSL 1.1+
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        std::cerr << "[FATAL] SSL_CTX_new() failed.\n";
        return EXIT_FAILURE;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);  // reject invalid/expired certs
    SSL_CTX_set_default_verify_paths(ctx);               // use system CA store
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);  // reject TLS < 1.2 (POODLE/BEAST)

    int exitCode = EXIT_SUCCESS;
    try {
        Downloader downloader(url, threadCount, ctx);
        downloader.run();
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL] Download failed: " << e.what() << "\n\n";
        exitCode = EXIT_FAILURE;
    }

    // Must run after all SSL* objects from this CTX are freed (guaranteed by join())
    SSL_CTX_free(ctx);

    return exitCode;
}
