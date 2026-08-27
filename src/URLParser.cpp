#include "URLParser.h"
#include <stdexcept>
#include <string>

ParsedURL URLParser::parse(const std::string& url) {
    ParsedURL result;

    // Detect scheme: "https://" takes priority over "http://"
    const std::string httpsPrefix = "https://";
    const std::string httpPrefix  = "http://";

    if (url.rfind(httpsPrefix, 0) == 0) {
        result.scheme = "https";
    } else if (url.rfind(httpPrefix, 0) == 0) {
        result.scheme = "http";
    } else {
        throw std::invalid_argument("Only 'http://' and 'https://' URLs are supported: " + url);
    }

    const std::string& prefix = result.isHttps() ? httpsPrefix : httpPrefix;

    // Split authority (host[:port]) from path at the first '/'
    const std::string rest     = url.substr(prefix.size());
    const size_t      slashPos = rest.find('/');

    std::string authority;
    if (slashPos == std::string::npos) {
        authority   = rest;
        result.path = "/";  // implicit root per HTTP/1.1
    } else {
        authority   = rest.substr(0, slashPos);
        result.path = rest.substr(slashPos);
    }

    if (authority.empty())
        throw std::invalid_argument("Missing host in URL: " + url);

    // RFC 3986 §3.2.2: IPv6 literals are wrapped in brackets — e.g. [::1] or [::1]:8080.
    if (!authority.empty() && authority[0] == '[') {
        const size_t closeBracket = authority.find(']');
        if (closeBracket == std::string::npos)
            throw std::invalid_argument("Malformed IPv6 literal (missing ']') in URL: " + url);
        result.host = authority.substr(1, closeBracket - 1);  // strip '[' and ']'
        if (result.host.empty())
            throw std::invalid_argument("Empty IPv6 address in URL: " + url);

        if (closeBracket + 1 < authority.size()) {
            // Characters follow the ']' — must be ':port'
            if (authority[closeBracket + 1] != ':')
                throw std::invalid_argument(
                    "Unexpected character after IPv6 literal in URL: " + url);
            const std::string portStr = authority.substr(closeBracket + 2);
            try {
                const int p = std::stoi(portStr);
                if (p < 1 || p > 65535)
                    throw std::out_of_range("port out of range");
                result.port = p;
            } catch (...) {
                throw std::invalid_argument("Invalid port '" + portStr + "' in: " + url);
            }
        } else {
            result.port = result.isHttps() ? 443 : 80;  // IANA defaults
        }
    } else {
        // Hostname or IPv4: split on the first (and only) colon
        const size_t colonPos = authority.find(':');
        if (colonPos == std::string::npos) {
            result.host = authority;
            result.port = result.isHttps() ? 443 : 80;  // IANA defaults
        } else {
            result.host = authority.substr(0, colonPos);
            const std::string portStr = authority.substr(colonPos + 1);
            try {
                const int p = std::stoi(portStr);
                if (p < 1 || p > 65535)
                    throw std::out_of_range("port out of range");
                result.port = p;
            } catch (...) {
                throw std::invalid_argument("Invalid port '" + portStr + "' in: " + url);
            }
        }
    }

    if (result.host.empty())
        throw std::invalid_argument("Empty host in URL: " + url);

    return result;
}
