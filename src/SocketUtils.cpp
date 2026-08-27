#include "SocketUtils.h"
#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>
#include <climits>     // INT_MAX — caps SSL_read length argument
#include <algorithm>   // std::min
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static std::string sslErrorString() {
    const unsigned long e = ERR_get_error();
    if (e == 0) return "unknown SSL error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(buf);
}

Connection SocketUtils::createConnection(const std::string& host, int port,
                                         bool isHttps, SSL_CTX* ctx) {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;    // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP

    const std::string portStr = std::to_string(port);
    struct addrinfo* results  = nullptr;

    const int gaierr = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &results);
    if (gaierr != 0)
        throw std::runtime_error("DNS failed for '" + host + "': " + gai_strerror(gaierr));

    Connection conn;
    struct addrinfo* rp = nullptr;

    for (rp = results; rp != nullptr; rp = rp->ai_next) {
        conn.fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (conn.fd < 0) continue;
        if (::connect(conn.fd, rp->ai_addr, rp->ai_addrlen) == 0) break;  // TCP handshake
        ::close(conn.fd);
        conn.fd = -1;
    }
    freeaddrinfo(results);

    if (conn.fd < 0)
        throw std::runtime_error("Cannot connect to '" + host + ":" + portStr +
                                 "': " + strerror(errno));

    if (!isHttps) return conn;

    if (!ctx) {
        ::close(conn.fd);
        throw std::runtime_error("HTTPS requested but no SSL_CTX provided.");
    }

    conn.ssl = SSL_new(ctx);  // per-thread SSL object; ctx is shared read-only
    if (!conn.ssl) { ::close(conn.fd); throw std::runtime_error("SSL_new() failed: " + sslErrorString()); }

    if (SSL_set_fd(conn.ssl, conn.fd) != 1) {
        SSL_free(conn.ssl); ::close(conn.fd);
        throw std::runtime_error("SSL_set_fd() failed: " + sslErrorString());
    }

    // SNI: required by CDNs and virtual-hosted servers to select the right certificate
    if (SSL_set_tlsext_host_name(conn.ssl, host.c_str()) != 1) {
        SSL_free(conn.ssl); ::close(conn.fd);
        throw std::runtime_error("SSL_set_tlsext_host_name() failed: " + sslErrorString());
    }

    if (SSL_connect(conn.ssl) != 1) {  // TLS handshake
        const std::string err = sslErrorString();
        SSL_free(conn.ssl); ::close(conn.fd);
        throw std::runtime_error("TLS handshake failed with '" + host + "': " + err);
    }

    return conn;
}

void SocketUtils::sendAll(const Connection& conn, const std::string& data) {
    const char* ptr    = data.c_str();
    size_t      remain = data.size();

    while (remain > 0) {
        int sent = 0;
        if (conn.isSecure()) {
            sent = SSL_write(conn.ssl, ptr, static_cast<int>(remain));
            if (sent <= 0) throw std::runtime_error("SSL_write() failed: " + sslErrorString());
        } else {
            const ssize_t s = ::send(conn.fd, ptr, remain, MSG_NOSIGNAL);  // MSG_NOSIGNAL: avoid SIGPIPE
            if (s < 0) throw std::runtime_error(std::string("send() failed: ") + strerror(errno));
            sent = static_cast<int>(s);
        }
        ptr    += sent;
        remain -= static_cast<size_t>(sent);
    }
}

ssize_t SocketUtils::recvFrom(const Connection& conn, void* buf, size_t len) {
    if (conn.isSecure()) {
        // cap len to INT_MAX to prevent silent truncation
        const int safeLen = static_cast<int>(std::min(len, static_cast<size_t>(INT_MAX)));
        const int n = SSL_read(conn.ssl, buf, safeLen);
        return static_cast<ssize_t>(n);  // 0 = peer close_notify, < 0 = error
    }
    return ::recv(conn.fd, buf, len, 0);
}

void SocketUtils::closeConnection(Connection& conn) {
    if (conn.ssl) {
        const int rc = SSL_shutdown(conn.ssl);  // send TLS close_notify
        if (rc < 0) {
            // Shutdown error — clear the OpenSSL error queue so stale errors
            // don't leak into subsequent SSL operations on other connections.
            ERR_clear_error();
        }
        SSL_free(conn.ssl);  // frees SSL object + BIO (does NOT close fd)
        conn.ssl = nullptr;
    }
    if (conn.fd >= 0) {
        ::close(conn.fd);    // TCP FIN; SSL_free does not close the fd
        conn.fd = -1;
    }
}

std::string SocketUtils::networkError(const Connection& conn) {
    if (conn.isSecure()) {
        // OpenSSL errors are queued internally — errno is meaningless after SSL_read.
        // Must be called BEFORE closeConnection(): SSL_free() clears this queue.
        return sslErrorString();
    }
    return strerror(errno);
}
