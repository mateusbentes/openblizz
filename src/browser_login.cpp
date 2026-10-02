#include "openblizz/browser_login.hpp"
#include "openblizz/http.hpp"
#include "openblizz/library.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace openblizz {
namespace {

using json = nlohmann::json;

std::filesystem::path config_home() {
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "openblizz";
    }
    return std::filesystem::current_path() / ".openblizz";
}

std::string find_in_path(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return std::filesystem::exists(name) ? name : std::string{};
    }
    const auto* path = std::getenv("PATH");
    if (path == nullptr) return {};
    std::stringstream stream(path);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        if (dir.empty()) continue;
        const auto candidate = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
    }
    return {};
}

std::string base64(const unsigned char* data, std::size_t size) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned v = (data[i] << 16) | (i + 1 < size ? data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[v & 63] : '=');
    }
    return out;
}

// Minimal RFC 6455 client, loopback only (ws://127.0.0.1). Enough for the
// DevTools protocol: text frames, masking, fragmentation, ping/pong.
class LoopbackWebSocket {
public:
    LoopbackWebSocket(int port, const std::string& path) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            throw std::runtime_error("could not connect to the browser DevTools port");
        }
        timeval tv{};
        tv.tv_sec = 30;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        unsigned char nonce[16];
        RAND_bytes(nonce, sizeof(nonce));
        const auto key = base64(nonce, sizeof(nonce));
        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                    "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        send_all(reinterpret_cast<const unsigned char*>(request.data()), request.size());
        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos) {
            char buffer[1024];
            const auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (n <= 0) throw std::runtime_error("DevTools handshake failed");
            response.append(buffer, static_cast<std::size_t>(n));
            if (response.size() > 65536) throw std::runtime_error("DevTools handshake too large");
        }
        if (response.rfind("HTTP/1.1 101", 0) != 0) throw std::runtime_error("DevTools refused the WebSocket upgrade");
        const auto header_end = response.find("\r\n\r\n") + 4;
        pending_.assign(response.begin() + static_cast<long>(header_end), response.end());
    }

    ~LoopbackWebSocket() {
        if (fd_ >= 0) ::close(fd_);
    }

    void send_text(const std::string& payload) {
        std::vector<unsigned char> frame;
        frame.push_back(0x81);
        const auto size = payload.size();
        if (size < 126) {
            frame.push_back(static_cast<unsigned char>(0x80 | size));
        } else if (size <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<unsigned char>(size >> 8));
            frame.push_back(static_cast<unsigned char>(size));
        } else {
            frame.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<unsigned char>(size >> shift));
        }
        unsigned char mask[4];
        RAND_bytes(mask, sizeof(mask));
        frame.insert(frame.end(), mask, mask + 4);
        for (std::size_t i = 0; i < size; ++i) {
            frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
        send_all(frame.data(), frame.size());
    }

    // Returns the next complete text message.
    std::string receive_text() {
        std::string message;
        for (;;) {
            const auto header0 = read_byte();
            const auto header1 = read_byte();
            const bool fin = (header0 & 0x80) != 0;
            const int opcode = header0 & 0x0F;
            const bool masked = (header1 & 0x80) != 0;
            std::uint64_t length = header1 & 0x7F;
            if (length == 126) {
                length = (static_cast<std::uint64_t>(read_byte()) << 8) | read_byte();
            } else if (length == 127) {
                length = 0;
                for (int i = 0; i < 8; ++i) length = (length << 8) | read_byte();
            }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked) for (auto& m : mask) m = read_byte();
            if (length > (64ULL << 20)) throw std::runtime_error("DevTools frame too large");
            std::string payload(static_cast<std::size_t>(length), '\0');
            for (std::uint64_t i = 0; i < length; ++i) {
                payload[static_cast<std::size_t>(i)] = static_cast<char>(read_byte() ^ mask[i % 4]);
            }
            if (opcode == 0x9) {  // ping -> pong
                std::vector<unsigned char> pong{0x8A, static_cast<unsigned char>(0x80 | (payload.size() & 0x7F))};
                unsigned char m[4] = {0, 0, 0, 0};
                pong.insert(pong.end(), m, m + 4);
                pong.insert(pong.end(), payload.begin(), payload.end());
                send_all(pong.data(), pong.size());
                continue;
            }
            if (opcode == 0x8) throw std::runtime_error("the browser closed the DevTools connection");
            if (opcode == 0xA) continue;  // pong
            message += payload;
            if (fin) return message;
        }
    }

private:
    void send_all(const unsigned char* data, std::size_t size) {
        while (size > 0) {
            const auto n = ::send(fd_, data, size, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("DevTools send failed");
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }

    unsigned char read_byte() {
        if (pending_pos_ >= pending_.size()) {
            pending_.resize(65536);
            pending_pos_ = 0;
            const auto n = ::recv(fd_, pending_.data(), pending_.size(), 0);
            if (n <= 0) throw std::runtime_error("DevTools receive failed or timed out");
            pending_.resize(static_cast<std::size_t>(n));
        }
        return static_cast<unsigned char>(pending_[pending_pos_++]);
    }

    int fd_{-1};
    std::string pending_;
    std::size_t pending_pos_{0};
};

class DevToolsClient {
public:
    DevToolsClient(int port, const std::string& ws_path) : socket_(port, ws_path) {}

    json call(const std::string& method, json params = json::object()) {
        const int id = ++next_id_;
        socket_.send_text(json{{"id", id}, {"method", method}, {"params", std::move(params)}}.dump());
        for (;;) {
            const auto message = json::parse(socket_.receive_text(), nullptr, false);
            if (message.is_discarded()) continue;
            if (message.value("id", -1) != id) continue;  // events / other replies
            if (message.contains("error")) {
                throw std::runtime_error("DevTools " + method + " failed: " + message["error"].dump());
            }
            return message.value("result", json::object());
        }
    }

private:
    LoopbackWebSocket socket_;
    int next_id_{0};
};

struct DevToolsEndpoint {
    int port{0};
    std::string browser_ws_path;
};

DevToolsEndpoint wait_for_devtools(const std::filesystem::path& profile_dir, pid_t child, int timeout_seconds) {
    const auto file = profile_dir / "DevToolsActivePort";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) {
            throw std::runtime_error("the browser exited before exposing its DevTools endpoint");
        }
        std::ifstream in(file);
        if (in) {
            DevToolsEndpoint endpoint;
            std::string port_line;
            if (std::getline(in, port_line) && std::getline(in, endpoint.browser_ws_path) && !port_line.empty()) {
                endpoint.port = std::stoi(port_line);
                if (endpoint.port > 0) return endpoint;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error("timed out waiting for the browser DevTools endpoint");
}

pid_t launch_browser(const std::string& executable, const std::filesystem::path& profile_dir, const std::string& url) {
    std::vector<std::string> args{
        executable,
        "--user-data-dir=" + profile_dir.string(),
        "--remote-debugging-port=0",
        "--remote-allow-origins=http://127.0.0.1",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-sync",
        "--disable-background-networking",
        "--disable-component-update",
        "--new-window",
        "--window-size=1100,900",
        "--app=" + url,
    };
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        // Keep the browser quiet; its stderr is very chatty.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        setsid();
        execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

void write_private_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        out << content;
    }
    ::chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path);
    ::chmod(path.c_str(), 0600);
}

bool session_is_authenticated(const std::filesystem::path& jar, const std::string& host) {
    try {
        CookieSession session(jar.string());
        const auto response = session.get("https://" + host + "/api/", {"Accept: application/json"});
        const std::string body(response.body.begin(), response.body.end());
        if (response.status != 200) return false;
        const auto parsed = json::parse(body, nullptr, false);
        return !parsed.is_discarded() && parsed.value("authenticated", false);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

bool BrowserLogin::session_authenticated(const std::filesystem::path& jar, const std::string& account_host) {
    return std::filesystem::is_regular_file(jar) && session_is_authenticated(jar, account_host);
}

std::filesystem::path BrowserLogin::default_profile_dir() {
    return config_home() / "browser-profile";
}

std::string BrowserLogin::find_browser(const std::string& preferred) {
    std::vector<std::string> candidates;
    if (!preferred.empty()) candidates.push_back(preferred);
    if (const auto* env = std::getenv("OPENBLIZZ_BROWSER"); env != nullptr && *env != '\0') candidates.emplace_back(env);
    for (const char* name : {"chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "brave-browser",
                             "brave", "microsoft-edge", "microsoft-edge-stable", "vivaldi", "vivaldi-stable"}) {
        candidates.emplace_back(name);
    }
    for (const auto& candidate : candidates) {
        const auto resolved = find_in_path(candidate);
        if (!resolved.empty()) return resolved;
    }
    return {};
}

bool BrowserLogin::looks_logged_in(const std::string& url, const std::string& account_host) {
    const std::string prefix = "https://" + account_host + "/";
    if (url.rfind(prefix, 0) != 0) return false;
    const auto rest = url.substr(prefix.size());
    if (rest.rfind("login", 0) == 0) return false;
    if (rest.rfind("oauth2/", 0) == 0 || rest.rfind("callback/", 0) == 0) return false;
    return true;
}

std::vector<BrowserCookie> BrowserLogin::parse_devtools_cookies(const std::string& text) {
    const auto parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) throw std::runtime_error("invalid DevTools cookie payload");
    const json& list = parsed.is_array() ? parsed : parsed.value("cookies", json::array());
    std::vector<BrowserCookie> cookies;
    for (const auto& item : list) {
        BrowserCookie cookie;
        cookie.domain = item.value("domain", "");
        cookie.path = item.value("path", "/");
        cookie.name = item.value("name", "");
        cookie.value = item.value("value", "");
        cookie.expires = item.value("expires", -1.0);
        cookie.secure = item.value("secure", false);
        cookie.http_only = item.value("httpOnly", false);
        if (!cookie.domain.empty() && !cookie.name.empty()) cookies.push_back(std::move(cookie));
    }
    return cookies;
}

std::string BrowserLogin::netscape_jar(const std::vector<BrowserCookie>& cookies, const std::string& domain_suffix) {
    std::string out = "# Netscape HTTP Cookie File\n# Written by OpenBlizz browser login. Keep this file private.\n\n";
    for (const auto& cookie : cookies) {
        const auto& domain = cookie.domain;
        const bool matches = domain == domain_suffix || domain == "." + domain_suffix ||
                             (domain.size() > domain_suffix.size() &&
                              domain.compare(domain.size() - domain_suffix.size() - 1, std::string::npos,
                                             "." + domain_suffix) == 0);
        if (!matches) continue;
        const bool domain_wide = !domain.empty() && domain.front() == '.';
        // Session cookies (expires < 0) get a far-future expiry so curl keeps them.
        long long expires = cookie.expires > 0 ? static_cast<long long>(cookie.expires) : 2147483647LL;
        if (cookie.http_only) out += "#HttpOnly_";
        out += domain + '\t' + (domain_wide ? "TRUE" : "FALSE") + '\t' + cookie.path + '\t' +
               (cookie.secure ? "TRUE" : "FALSE") + '\t' + std::to_string(expires) + '\t' + cookie.name + '\t' +
               cookie.value + '\n';
    }
    return out;
}

std::filesystem::path BrowserLogin::run(const BrowserLoginOptions& options) {
    const auto executable = find_browser(options.browser);
    if (executable.empty()) {
        throw std::runtime_error(
            "no Chromium-family browser found (chromium, google-chrome, brave-browser, microsoft-edge, vivaldi). "
            "Install one, set OPENBLIZZ_BROWSER=/path/to/browser, or fall back to "
            "`openblizz library scan --cookie-file cookies.txt`.");
    }
    const auto profile_dir = options.profile_dir.empty() ? default_profile_dir() : options.profile_dir;
    const auto jar = options.cookie_jar.empty() ? LibraryManager::default_cookie_jar() : options.cookie_jar;
    std::filesystem::create_directories(profile_dir);
    ::chmod(profile_dir.c_str(), 0700);
    std::error_code ec;
    std::filesystem::remove(profile_dir / "DevToolsActivePort", ec);

    const std::string start_url = "https://" + options.account_host + "/games";
    std::cout << "Opening " << executable << " with an isolated OpenBlizz profile.\n"
              << "Complete the Battle.net login there (password, authenticator/MFA, captcha).\n"
              << "OpenBlizz never receives your password; it only keeps the resulting session cookies.\n";
    const pid_t child = launch_browser(executable, profile_dir, start_url);

    HttpClient http;
    const auto endpoint = wait_for_devtools(profile_dir, child, 30);
    const std::string base = "http://127.0.0.1:" + std::to_string(endpoint.port);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    bool logged_in = false;
    bool announced_check = false;
    std::string last_url;

    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) {
            throw std::runtime_error("the browser window was closed before the login completed");
        }
        HttpResponse list;
        try {
            list = http.get(base + "/json/list");
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        const auto pages = json::parse(std::string(list.body.begin(), list.body.end()), nullptr, false);
        bool candidate = false;
        if (pages.is_array()) {
            for (const auto& page : pages) {
                if (page.value("type", "") != "page") continue;
                const auto url = page.value("url", "");
                if (url != last_url) {
                    last_url = url;
                    std::cout << "  browser: " << url.substr(0, 96) << '\n';
                }
                if (looks_logged_in(url, options.account_host)) candidate = true;
            }
        }
        if (candidate) {
            if (!announced_check) {
                std::cout << "  login page left; verifying the session...\n";
                announced_check = true;
            }
            // The /games URL also appears for a moment before the redirect to
            // the login page, so confirm against the account API before trusting it.
            const std::string ws_path = endpoint.browser_ws_path.rfind("ws://", 0) == 0
                ? endpoint.browser_ws_path.substr(endpoint.browser_ws_path.find('/', 5))
                : endpoint.browser_ws_path;
            DevToolsClient devtools(endpoint.port, ws_path);
            const auto result = devtools.call("Storage.getCookies");
            const auto cookies = parse_devtools_cookies(result.dump());
            const auto probe = jar.string() + ".probe";
            write_private_file(probe, netscape_jar(cookies, "battle.net"));
            if (session_is_authenticated(probe, options.account_host)) {
                std::filesystem::rename(probe, jar);
                ::chmod(jar.c_str(), 0600);
                logged_in = true;
                if (!options.keep_browser_open) {
                    try {
                        devtools.call("Browser.close");
                    } catch (const std::exception&) {
                        killpg(child, SIGTERM);
                    }
                }
                break;
            }
            std::filesystem::remove(probe, ec);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    if (!logged_in) {
        killpg(child, SIGTERM);
        throw std::runtime_error("timed out waiting for the Battle.net login to complete");
    }
    int status = 0;
    for (int i = 0; i < 40 && waitpid(child, &status, WNOHANG) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Battle.net session captured and saved with owner-only permissions at " << jar << '\n';
    return jar;
}

} // namespace openblizz
