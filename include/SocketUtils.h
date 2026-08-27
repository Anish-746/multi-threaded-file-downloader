#pragma once
#include <string>
#include <openssl/ssl.h>   // SSL*, SSL_CTX*
#include <sys/socket.h>    // socket(), connect(), SOCK_STREAM
#include <netdb.h>         // getaddrinfo(), freeaddrinfo()
#include <arpa/inet.h>     // htons()
#include <unistd.h>        // close()
#include <sys/types.h>     // ssize_t

// Unified connection handle for both plain HTTP and HTTPS.
// For HTTP:  fd >= 0, ssl == nullptr
// For HTTPS: fd >= 0, ssl != nullptr (wraps the same fd)
//
// RAII: the destructor performs the full teardown sequence:
//   SSL_shutdown → SSL_free → close(fd)
// Resources are therefore released on scope exit, even through exceptions.
// Connection objects are move-only — copying a raw fd or SSL* is never safe.
struct Connection {
    int  fd  = -1;
    SSL* ssl = nullptr;

    // Default-construct an empty (closed) connection.
    Connection() = default;

    // RAII destructor: graceful TLS shutdown, then close the socket.
    ~Connection() {
        if (ssl) {
            SSL_shutdown(ssl);  // send TLS close_notify (best-effort)
            SSL_free(ssl);      // frees SSL object + BIO; does NOT close fd
            ssl = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    // Move constructor — transfers ownership; leaves `other` in a closed state.
    Connection(Connection&& other) noexcept
        : fd(other.fd), ssl(other.ssl)
    {
        other.fd  = -1;
        other.ssl = nullptr;
    }

    // Move assignment.
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            // Release any resource we already own.
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
            if (fd >= 0) { ::close(fd); fd = -1; }
            // Take ownership from other.
            fd        = other.fd;
            ssl       = other.ssl;
            other.fd  = -1;
            other.ssl = nullptr;
        }
        return *this;
    }

    // Copying a raw fd/SSL* handle is never safe — deleted.
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    bool isSecure() const { return ssl != nullptr; }
};

// Stateless POSIX + OpenSSL socket helpers.
// Each worker thread calls these independently — connections are never shared.
class SocketUtils {
public:
    // Creates a TCP connection (DNS → connect) and, when isHttps == true,
    // wraps the socket in a TLS session using the provided SSL_CTX.
    // SNI is configured automatically from `host`.
    // Throws std::runtime_error on DNS failure, connect() failure, or TLS handshake failure.
    static Connection createConnection(const std::string& host, int port,
                                       bool isHttps, SSL_CTX* ctx);

    // Sends all bytes of `data`, looping on partial writes.
    // Dispatches to SSL_write (HTTPS) or send() (HTTP) based on conn.isSecure().
    static void sendAll(const Connection& conn, const std::string& data);

    // Receives up to `len` bytes into `buf`.
    // Returns bytes read (> 0), 0 on clean connection close, or -1 on error.
    // Dispatches to SSL_read (HTTPS) or recv() (HTTP).
    static ssize_t recvFrom(const Connection& conn, void* buf, size_t len);

    // Explicit early release: gracefully shuts down TLS (SSL_shutdown → SSL_free)
    // and closes the raw socket fd, then nulls the sentinels so the destructor
    // becomes a no-op. Useful when you need to release the connection before the
    // Connection object goes out of scope (e.g. to free the port sooner).
    // Safe to call on both HTTP and HTTPS connections.
    static void closeConnection(Connection& conn);

    // Returns a human-readable description of the most recent network error.
    // For HTTPS: drains the OpenSSL error queue (errno is meaningless after SSL_read).
    // For HTTP:  returns strerror(errno).
    // Call this BEFORE closeConnection() — SSL_free() clears the error queue.
    static std::string networkError(const Connection& conn);
};
