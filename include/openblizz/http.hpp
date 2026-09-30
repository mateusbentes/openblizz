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
    [[nodiscard]] HttpResponse post(const std::string& url, const std::string& body,
                                    const std::vector<std::string>& headers = {}) const;
    void get_to_file(const std::string& url, const std::string& path,
                     const std::vector<std::string>& headers = {}) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace openblizz
