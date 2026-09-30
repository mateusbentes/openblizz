#include "openblizz/http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <fstream>
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

size_t write_file(char* ptr, size_t size, size_t count, void* userdata) {
    const auto bytes = size * count;
    auto* output = static_cast<std::ofstream*>(userdata);
    output->write(ptr, static_cast<std::streamsize>(bytes));
    return output->good() ? bytes : 0;
}

size_t header_callback(char* ptr, size_t size, size_t count, void* userdata) {
    const auto bytes = size * count;
    std::string line(ptr, bytes);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
        auto key = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
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
        throw std::runtime_error("HTTP POST returned status " + std::to_string(response.status) +
                                 " for " + url);
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
