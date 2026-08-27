#pragma once
#include <string>

// Holds the decomposed components of an HTTP or HTTPS URL.
struct ParsedURL {
    std::string scheme;  // "http" or "https"
    std::string host;    // e.g. "example.com"
    int         port;    // 80 for http, 443 for https (or explicit override)
    std::string path;    // e.g. "/file.zip"

    bool isHttps() const { return scheme == "https"; }
};

// Stateless utility: parses "http[s]://host[:port]/path" into a ParsedURL.
// Throws std::invalid_argument on malformed input or unknown scheme.
class URLParser {
public:
    static ParsedURL parse(const std::string& url);
};
