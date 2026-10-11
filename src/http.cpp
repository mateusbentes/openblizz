#include "openblizz/http.hpp"

#include <curl/curl.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace openblizz {
namespace {

struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("curl_global_init failed");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal& curl_global() {
    static CurlGlobal instance;
    return instance;
}

size_t write_memory(char* ptr, size_t size, size_t count, void* userdata) {
    const auto bytes = size * count;
    auto* output = static_cast<std::vector<std::uint8_t>*>(userdata);
    output->insert(output->end(), reinterpret_cast<std::uint8_t*>(ptr),
                   reinterpret_cast<std::uint8_t*>(ptr) + bytes);
    return bytes;
}

struct RangeBuffer {
    std::vector<std::uint8_t>* output;
    std::size_t limit;
};

size_t write_range(char* ptr, size_t size, size_t count, void* userdata) {
    auto& buffer = *static_cast<RangeBuffer*>(userdata);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto bytes = size * count;
    if (bytes > buffer.limit - buffer.output->size()) return 0;
    buffer.output->insert(buffer.output->end(), reinterpret_cast<std::uint8_t*>(ptr),
                          reinterpret_cast<std::uint8_t*>(ptr) + bytes);
    return bytes;
}

bool valid_content_range(const std::string& value, std::uint64_t offset, std::uint32_t size) {
    if (value.rfind("bytes ", 0) != 0) return false;
    const auto dash = value.find('-', 6);
    const auto slash = value.find('/', dash == std::string::npos ? 6 : dash + 1);
    if (dash == std::string::npos || slash == std::string::npos) return false;
    const auto number = [&](std::size_t first, std::size_t last, std::uint64_t& result) {
        const auto parsed = std::from_chars(value.data() + first, value.data() + last, result);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + last;
    };
    std::uint64_t begin{}, end{}, total{};
    if (!number(6, dash, begin) || !number(dash + 1, slash, end)) return false;
    if (begin != offset || end != offset + size - 1) return false;
    if (value.substr(slash + 1) == "*") return true;
    return number(slash + 1, value.size(), total) && total > end;
}

size_t write_file(char* ptr, size_t size, size_t count, void* userdata) {
    const auto bytes = size * count;
    auto* output = static_cast<std::ofstream*>(userdata);
    output->write(ptr, static_cast<std::streamsize>(bytes));
    return output->good() ? bytes : 0;
}

size_t header_callback(char* ptr, size_t size, size_t count, void* userdata) {
    const auto bytes = size * count;
    std::string line(ptr, bytes);
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    // Intermediate redirects/100 responses must not supply headers for the
    // final response. HTTP header names are case-insensitive.
    if (line.rfind("HTTP/", 0) == 0) headers->clear();
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        auto key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        (*headers)[key] = value;
    }
    return bytes;
}

void configure(CURL* curl, const std::string& url,
               const std::vector<std::string>& headers) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenBlizz/0.1 (+independent-client)");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (!headers.empty()) {
        auto* list = curl_slist_append(nullptr, headers.front().c_str());
        for (std::size_t i = 1; i < headers.size(); ++i) {
            list = curl_slist_append(list, headers[i].c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl, CURLOPT_PRIVATE, list);
    }
}

void free_headers(CURL* curl) {
    char* private_data = nullptr;
    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &private_data);
    if (private_data != nullptr) {
        curl_slist_free_all(reinterpret_cast<curl_slist*>(private_data));
    }
}

} // namespace

struct HttpClient::Impl {};

HttpClient::HttpClient() : impl_(new Impl()) {
    (void)curl_global();
}

HttpClient::~HttpClient() { delete impl_; }

HttpResponse HttpClient::get(const std::string& url,
                             const std::vector<std::string>& headers) const {
    (void)impl_;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

    HttpResponse response;
    configure(curl, url, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        free_headers(curl);
        const std::string message = curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP GET failed for " + url + ": " + message);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective != nullptr) response.effective_url = effective;
    free_headers(curl);
    curl_easy_cleanup(curl);

    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("HTTP GET returned status " + std::to_string(response.status) +
                                 " for " + url);
    }
    return response;
}

HttpResponse HttpClient::get_range(const std::string& url, std::uint64_t offset,
                                   std::uint32_t size,
                                   const std::vector<std::string>& headers) const {
    if (size == 0) throw std::runtime_error("HTTP Range request has zero size");
    if (offset > std::numeric_limits<std::uint64_t>::max() - (size - 1)) {
        throw std::runtime_error("HTTP Range request overflows its end offset");
    }
    (void)impl_;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

    HttpResponse response;
    configure(curl, url, headers);
    // Range offsets identify encoded archive bytes, not decoded HTTP content.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    const auto range = std::to_string(offset) + "-" + std::to_string(offset + size - 1);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    RangeBuffer buffer{&response.body, size};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_range);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        free_headers(curl);
        const std::string message = curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP Range GET failed for " + url + ": " + message);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective != nullptr) response.effective_url = effective;
    free_headers(curl);
    curl_easy_cleanup(curl);

    if (response.status != 206) {
        throw std::runtime_error("HTTP Range GET returned status " + std::to_string(response.status) +
                                 "; expected 206 for " + url);
    }
    const auto content_range = response.headers.find("content-range");
    if (content_range == response.headers.end() || !valid_content_range(content_range->second, offset, size)) {
        throw std::runtime_error("HTTP Range GET has missing or mismatched Content-Range for " + url);
    }
    const auto encoding = response.headers.find("content-encoding");
    if (encoding != response.headers.end() && encoding->second != "identity") {
        throw std::runtime_error("HTTP Range GET has non-identity Content-Encoding for " + url);
    }
    if (response.body.size() != size) {
        throw std::runtime_error("HTTP Range GET returned " + std::to_string(response.body.size()) +
                                 " bytes, expected " + std::to_string(size) + " for " + url);
    }
    return response;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body,
                              const std::vector<std::string>& headers) const {
    (void)impl_;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

    HttpResponse response;
    configure(curl, url, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        free_headers(curl);
        const std::string message = curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP POST failed for " + url + ": " + message);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective != nullptr) response.effective_url = effective;
    free_headers(curl);
    curl_easy_cleanup(curl);

    if (response.status < 200 || response.status >= 300) {
        auto detail = std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        if (detail.size() > 512) detail.resize(512);
        throw std::runtime_error("HTTP POST returned status " + std::to_string(response.status) +
                                 " for " + url + (detail.empty() ? std::string{} : "; response: " + detail));
    }
    return response;
}

void HttpClient::get_to_file(const std::string& url, const std::string& path,
                             const std::vector<std::string>& headers) const {
    (void)impl_;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("cannot open output file: " + path);
    }

    configure(curl, url, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        free_headers(curl);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP file download failed for " + url + ": " +
                                 curl_easy_strerror(result));
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    free_headers(curl);
    curl_easy_cleanup(curl);
    output.close();
    if (status < 200 || status >= 300) {
        throw std::runtime_error("HTTP file download returned status " + std::to_string(status));
    }
}

} // namespace openblizz

namespace openblizz {

struct CookieSession::Impl {
    CURL* curl{nullptr};
};

CookieSession::CookieSession(const std::string& netscape_cookie_file) : impl_(new Impl()) {
    (void)curl_global();
    impl_->curl = curl_easy_init();
    if (impl_->curl == nullptr) {
        delete impl_;
        throw std::runtime_error("curl_easy_init failed");
    }
    // Enables the cookie engine and loads the file into memory. No CURLOPT_COOKIEJAR
    // is set, so updated cookies are never written to disk.
    curl_easy_setopt(impl_->curl, CURLOPT_COOKIEFILE, netscape_cookie_file.c_str());
}

CookieSession::~CookieSession() {
    if (impl_ != nullptr) {
        if (impl_->curl != nullptr) curl_easy_cleanup(impl_->curl);
        delete impl_;
    }
}

void CookieSession::save_jar(const std::string& path) {
    // Create the file with owner-only permissions before libcurl writes it.
    {
        std::ofstream create(path, std::ios::app);
        if (!create) throw std::runtime_error("cannot create cookie jar: " + path);
    }
#if !defined(_WIN32)
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    curl_easy_setopt(impl_->curl, CURLOPT_COOKIEJAR, path.c_str());
    // CURLOPT_COOKIEJAR is written at handle cleanup; flush explicitly now.
    curl_easy_setopt(impl_->curl, CURLOPT_COOKIELIST, "FLUSH");
}

HttpResponse CookieSession::get(const std::string& url, const std::vector<std::string>& headers) {
    CURL* curl = impl_->curl;
    // Do not curl_easy_reset(): the cookie file is read lazily at the first
    // transfer and a reset before that would discard it.
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, nullptr);

    HttpResponse response;
    configure(curl, url, headers);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0 OpenBlizz/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    const auto result = curl_easy_perform(curl);
    const std::string message = result == CURLE_OK ? "" : curl_easy_strerror(result);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective != nullptr) response.effective_url = effective;
    free_headers(curl);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    if (result != CURLE_OK) throw std::runtime_error("HTTP GET failed for " + url + ": " + message);
    return response;
}

} // namespace openblizz
