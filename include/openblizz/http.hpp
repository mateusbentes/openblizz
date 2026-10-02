#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace openblizz {

struct HttpResponse {
    long status{0};
    std::string effective_url;
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    [[nodiscard]] HttpResponse get(const std::string& url,
                                   const std::vector<std::string>& headers = {}) const;
    [[nodiscard]] HttpResponse get_range(const std::string& url,
                                         std::uint64_t offset,
                                         std::uint32_t size,
                                         const std::vector<std::string>& headers = {}) const;
    [[nodiscard]] HttpResponse post(const std::string& url, const std::string& body,
                                    const std::vector<std::string>& headers = {}) const;
    void get_to_file(const std::string& url, const std::string& path,
                     const std::vector<std::string>& headers = {}) const;

private:
    struct Impl;
    Impl* impl_;
};

// A browser-like session: persistent connection handle with libcurl's cookie
// engine enabled. Cookies are loaded from a Netscape cookies.txt file into
// memory and are never written back to disk.
class CookieSession {
public:
    explicit CookieSession(const std::string& netscape_cookie_file);
    ~CookieSession();
    CookieSession(const CookieSession&) = delete;
    CookieSession& operator=(const CookieSession&) = delete;

    // Follows redirects (up to 15). Does not throw on HTTP error status.
    [[nodiscard]] HttpResponse get(const std::string& url,
                                   const std::vector<std::string>& headers = {});

    // Writes the current (possibly rotated) cookies to a Netscape file with
    // owner-only permissions. Opt-in; nothing is persisted otherwise.
    void save_jar(const std::string& path);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace openblizz
