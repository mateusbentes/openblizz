// No external network or credentials: libcurl talks to a loopback fixture.
#undef NDEBUG
#include "openblizz/http.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
class Server {
public:
    Server() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("fixture socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd_, 8) < 0) {
            ::close(fd_);
            throw std::runtime_error("fixture bind/listen failed");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
            ::close(fd_);
            throw std::runtime_error("fixture port failed");
        }
        base_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        thread_ = std::thread([this] { serve(); });
    }
    ~Server() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }
    std::string url(const std::string& path) const { return base_ + path; }
    bool request_ok() const { return request_ok_; }
private:
    void serve() {
        while (!stop_) {
            pollfd event{fd_, POLLIN, 0};
            if (::poll(&event, 1, 100) <= 0) continue;
            const auto client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) continue;
            timeval timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string request;
            char bytes[4096];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
                const auto n = ::recv(client, bytes, sizeof(bytes), 0);
                if (n <= 0) break;
                request.append(bytes, static_cast<std::size_t>(n));
            }
            request_ok_ = request_ok_ && request.find("Range: bytes=2-5\r\n") != std::string::npos &&
                request.find("Accept-Encoding: identity\r\n") != std::string::npos;
            const auto begin = request.find(' ');
            const auto end = request.find(' ', begin + 1);
            const auto path = request.substr(begin + 1, end - begin - 1);
            std::string status = "206 Partial Content";
            std::string headers = "cOnTeNt-RaNgE: bytes 2-5/10\r\n";
            std::string body = "2345";
            if (path == "/full") status = "200 OK";
            if (path == "/wrong") headers = "Content-Range: bytes 0-3/10\r\n";
            if (path == "/missing") headers.clear();
            if (path == "/total") headers = "Content-Range: bytes 2-5/5\r\n";
            if (path == "/unknown-total") headers = "Content-Range: bytes 2-5/*\r\n";
            if (path == "/short") body = "23";
            if (path == "/large") body = std::string(65536, 'x');
            if (path == "/encoding") headers += "Content-Encoding: gzip\r\n";
            if (path == "/redirect") {
                status = "302 Found";
                headers += "Location: " + url("/valid") + "\r\n";
                body.clear();
            }
            if (path == "/redirect-stale") {
                status = "302 Found";
                headers += "Location: " + url("/missing") + "\r\n";
                body.clear();
            }
            if (path == "/error") { status = "416 Range Not Satisfiable"; body.clear(); }
            const auto response = "HTTP/1.1 " + status + "\r\n" + headers +
                "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const auto n = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) break;
                sent += static_cast<std::size_t>(n);
            }
            ::close(client);
        }
    }
    int fd_{};
    std::string base_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> request_ok_{true};
    std::thread thread_;
};

template<typename Function>
bool fails(Function&& function, const std::string& reason) {
    try { function(); }
    catch (const std::exception& error) { return std::string(error.what()).find(reason) != std::string::npos; }
    return false;
}
}

int main() {
    Server server;
    openblizz::HttpClient http;
    for (const auto& path : {"/valid", "/redirect", "/unknown-total"}) {
        const auto response = http.get_range(server.url(path), 2, 4);
        assert(response.status == 206);
        assert(std::string(response.body.begin(), response.body.end()) == "2345");
    }
    assert(fails([&] { (void)http.get_range(server.url("/full"), 2, 4); }, "expected 206"));
    for (const auto& path : {"/wrong", "/missing", "/total", "/redirect-stale"}) {
        assert(fails([&] { (void)http.get_range(server.url(path), 2, 4); }, "Content-Range"));
    }
    assert(fails([&] { (void)http.get_range(server.url("/short"), 2, 4); }, "expected 4"));
    assert(fails([&] { (void)http.get_range(server.url("/large"), 2, 4); }, "HTTP Range GET failed"));
    assert(fails([&] { (void)http.get_range(server.url("/encoding"), 2, 4); }, "HTTP Range GET"));
    assert(fails([&] { (void)http.get_range(server.url("/error"), 2, 4); }, "expected 206"));
    assert(fails([&] { (void)http.get_range(server.url("/valid"), 2, 0); }, "zero size"));
    assert(fails([&] { (void)http.get_range(server.url("/valid"), std::numeric_limits<std::uint64_t>::max(), 2); }, "overflows"));
    assert(server.request_ok());
    std::cout << "OpenBlizz loopback HTTP tests passed\n";
}
