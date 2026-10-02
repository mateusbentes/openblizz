#include "openblizz/browser_login.hpp"
#include "openblizz/http.hpp"
#include "openblizz/library.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace openblizz {
namespace {

using json = nlohmann::json;

std::filesystem::path home_dir() {
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') return home;
    return std::filesystem::current_path();
}

std::filesystem::path config_home() {
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz";
    }
    return home_dir() / ".config" / "openblizz";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string find_in_path(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return (std::filesystem::exists(name) && access(name.c_str(), X_OK) == 0) ? name : std::string{};
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

// Runs a small helper command and returns its stdout (first 4 KiB).
std::string capture(const std::string& command) {
    std::array<char, 4096> buffer{};
    std::string out;
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen((command + " 2>/dev/null").c_str(), "r"), pclose);
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        out += buffer.data();
        if (out.size() > 65536) break;
    }
    return out;
}

// Ubuntu ships /usr/bin/firefox and /usr/bin/chromium as tiny shell wrappers
// around the snap; the snap sandbox cannot read ~/.config, so detect it.
std::string snap_name_for(const std::string& executable) {
    if (executable.rfind("/snap/bin/", 0) == 0) return std::filesystem::path(executable).filename().string();
    std::ifstream in(executable, std::ios::binary);
    if (!in) return {};
    std::string head(4096, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    if (head.rfind("#!", 0) != 0) return {};
    const auto pos = head.find("/snap/bin/");
    if (pos == std::string::npos) {
        if (head.find("snap") != std::string::npos) return std::filesystem::path(executable).filename().string();
        return {};
    }
    std::string name;
    for (auto i = pos + 10; i < head.size() && (std::isalnum(static_cast<unsigned char>(head[i])) || head[i] == '-' || head[i] == '_'); ++i) {
        name.push_back(head[i]);
    }
    return name;
}

const std::vector<std::string>& chromium_names() {
    static const std::vector<std::string> names{
        "chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "google-chrome-beta",
        "brave-browser", "brave", "microsoft-edge", "microsoft-edge-stable", "vivaldi", "vivaldi-stable",
        "opera", "thorium-browser", "ungoogled-chromium"};
    return names;
}

const std::vector<std::string>& firefox_names() {
    static const std::vector<std::string> names{
        "firefox", "firefox-esr", "firefox-developer-edition", "firefox-nightly", "librewolf", "waterfox",
        "floorp", "zen-browser", "zen"};
    return names;
}

struct FlatpakApp {
    const char* id;
    BrowserEngine engine;
};

const std::vector<FlatpakApp>& flatpak_apps() {
    static const std::vector<FlatpakApp> apps{
        {"org.mozilla.firefox", BrowserEngine::Firefox},
        {"io.gitlab.librewolf-community", BrowserEngine::Firefox},
        {"org.chromium.Chromium", BrowserEngine::Chromium},
        {"com.google.Chrome", BrowserEngine::Chromium},
        {"com.brave.Browser", BrowserEngine::Chromium},
        {"com.microsoft.Edge", BrowserEngine::Chromium},
        {"com.vivaldi.Vivaldi", BrowserEngine::Chromium},
    };
    return apps;
}

bool have_flatpak(const std::string& app_id) {
    static const std::string installed = capture("flatpak list --app --columns=application");
    std::stringstream stream(installed);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line == app_id) return true;
    }
    return false;
}

bool make_native(const std::string& name_or_path, BrowserInfo& out) {
    const auto resolved = find_in_path(name_or_path);
    if (resolved.empty()) return false;
    out.engine = BrowserLogin::engine_for_name(std::filesystem::path(resolved).filename().string());
    out.command = {resolved};
    out.display_name = resolved;
    const auto snap = snap_name_for(resolved);
    if (!snap.empty()) {
        out.packaging = "snap";
        out.profile_dir = home_dir() / "snap" / snap / "common" / "openblizz-profile";
    } else {
        out.packaging = "native";
        out.profile_dir = config_home() / "browser-profile";
    }
    return true;
}

bool make_flatpak(const std::string& app_id, BrowserEngine engine, BrowserInfo& out) {
    if (find_in_path("flatpak").empty() || !have_flatpak(app_id)) return false;
    out.engine = engine;
    out.command = {"flatpak", "run", app_id};
    out.display_name = "flatpak:" + app_id;
    out.packaging = "flatpak";
    out.profile_dir = home_dir() / ".var" / "app" / app_id / "openblizz-profile";
    return true;
}

// Resolves the desktop default browser (xdg-settings) to an engine family and
// a likely executable / flatpak id. Only used to order the candidate list.
void default_browser_hints(std::vector<std::string>& natives, std::vector<std::string>& flatpaks) {
    if (find_in_path("xdg-settings").empty()) return;
    auto desktop = capture("xdg-settings get default-web-browser");
    while (!desktop.empty() && std::isspace(static_cast<unsigned char>(desktop.back()))) desktop.pop_back();
    if (desktop.empty()) return;
    const auto id = lower(desktop);
    for (const auto& app : flatpak_apps()) {
        if (id.rfind(lower(app.id), 0) == 0) flatpaks.push_back(app.id);
    }
    // Typical ids: firefox.desktop, firefox_firefox.desktop (snap), google-chrome.desktop,
    // brave-browser.desktop, microsoft-edge.desktop, chromium_chromium.desktop, org.mozilla.firefox.desktop.
    auto base = id.substr(0, id.find(".desktop"));
    if (const auto underscore = base.find('_'); underscore != std::string::npos) base = base.substr(0, underscore);
    natives.push_back(base);
    if (base.find("firefox") != std::string::npos) natives.insert(natives.end(), firefox_names().begin(), firefox_names().end());
    else if (base.find("chrome") != std::string::npos) natives.insert(natives.end(), {"google-chrome", "google-chrome-stable"});
    else if (base.find("brave") != std::string::npos) natives.insert(natives.end(), {"brave-browser", "brave"});
    else if (base.find("edge") != std::string::npos) natives.insert(natives.end(), {"microsoft-edge", "microsoft-edge-stable"});
    else if (base.find("chromium") != std::string::npos) natives.insert(natives.end(), {"chromium", "chromium-browser"});
    else if (base.find("vivaldi") != std::string::npos) natives.insert(natives.end(), {"vivaldi", "vivaldi-stable"});
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
// DevTools / WebDriver BiDi protocols: text frames, masking, fragmentation,
// ping/pong.
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
            throw std::runtime_error("could not connect to the browser automation port");
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
            if (n <= 0) throw std::runtime_error("browser automation handshake failed");
            response.append(buffer, static_cast<std::size_t>(n));
            if (response.size() > 65536) throw std::runtime_error("browser automation handshake too large");
        }
        if (response.rfind("HTTP/1.1 101", 0) != 0) throw std::runtime_error("the browser refused the WebSocket upgrade");
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
            if (length > (64ULL << 20)) throw std::runtime_error("browser automation frame too large");
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
            if (opcode == 0x8) throw std::runtime_error("the browser closed the automation connection");
            if (opcode == 0xA) continue;  // pong
            message += payload;
            if (fin) return message;
        }
    }

private:
    void send_all(const unsigned char* data, std::size_t size) {
        while (size > 0) {
            const auto n = ::send(fd_, data, size, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("browser automation send failed");
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }

    unsigned char read_byte() {
        if (pending_pos_ >= pending_.size()) {
            pending_.resize(65536);
            pending_pos_ = 0;
            const auto n = ::recv(fd_, pending_.data(), pending_.size(), 0);
            if (n <= 0) throw std::runtime_error("browser automation receive failed or timed out");
            pending_.resize(static_cast<std::size_t>(n));
        }
        return static_cast<unsigned char>(pending_[pending_pos_++]);
    }

    int fd_{-1};
    std::string pending_;
    std::size_t pending_pos_{0};
};

// JSON-RPC style client shared by CDP and BiDi (both use {id, method, params}).
class AutomationClient {
public:
    AutomationClient(int port, const std::string& ws_path) : socket_(port, ws_path) {}

    json call(const std::string& method, json params = json::object()) {
        const int id = ++next_id_;
        socket_.send_text(json{{"id", id}, {"method", method}, {"params", std::move(params)}}.dump());
        for (;;) {
            const auto message = json::parse(socket_.receive_text(), nullptr, false);
            if (message.is_discarded() || !message.is_object()) continue;
            if (!message.contains("id") || !message["id"].is_number_integer() || message["id"].get<int>() != id) {
                continue;  // events / other replies
            }
            if (message.contains("error")) {  // CDP: error object; BiDi: type=error + error/message strings
                const std::string detail = message["error"].is_string()
                    ? message["error"].get<std::string>() + ": " + message.value("message", "")
                    : message["error"].dump();
                throw std::runtime_error(method + " failed: " + detail);
            }
            return message.value("result", json::object());
        }
    }

private:
    LoopbackWebSocket socket_;
    int next_id_{0};
};

struct Endpoint {
    int port{0};
    std::string ws_path;  // CDP: /devtools/browser/<id>; BiDi: /session
};

void check_child_alive(pid_t child) {
    int status = 0;
    if (waitpid(child, &status, WNOHANG) == child) {
        throw std::runtime_error("the browser exited before the login completed");
    }
}

Endpoint wait_for_endpoint(const BrowserInfo& browser, const std::filesystem::path& profile_dir, pid_t child,
                           int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        check_child_alive(child);
        Endpoint endpoint;
        if (browser.engine == BrowserEngine::Chromium) {
            std::ifstream in(profile_dir / "DevToolsActivePort");
            std::string port_line;
            if (in && std::getline(in, port_line) && std::getline(in, endpoint.ws_path) && !port_line.empty()) {
                endpoint.port = std::stoi(port_line);
                if (endpoint.port > 0) return endpoint;
            }
        } else {
            std::ifstream in(profile_dir / "WebDriverBiDiServer.json");
            if (in) {
                const auto parsed = json::parse(in, nullptr, false);
                if (parsed.is_object() && parsed.value("ws_port", 0) > 0) {
                    endpoint.port = parsed["ws_port"].get<int>();
                    endpoint.ws_path = "/session";
                    return endpoint;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error("timed out waiting for the browser automation endpoint");
}

void write_firefox_prefs(const std::filesystem::path& profile_dir) {
    const auto file = profile_dir / "user.js";
    if (std::filesystem::exists(file)) return;
    std::ofstream out(file);
    out << "// Written by OpenBlizz for its isolated login profile.\n"
           "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
           "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
           "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
           "user_pref(\"browser.startup.page\", 0);\n"
           "user_pref(\"browser.sessionstore.resume_from_crash\", false);\n"
           "user_pref(\"datareporting.policy.dataSubmissionPolicyBypassNotification\", true);\n"
           "user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);\n"
           "user_pref(\"remote.active-protocols\", 1);\n";
}

pid_t launch_browser(const BrowserInfo& browser, const std::filesystem::path& profile_dir, const std::string& url) {
    std::vector<std::string> args = browser.command;
    if (browser.engine == BrowserEngine::Chromium) {
        args.insert(args.end(), {
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
        });
    } else {
        write_firefox_prefs(profile_dir);
        args.insert(args.end(), {
            "--no-remote",
            "--profile", profile_dir.string(),
            "--remote-debugging-port", "0",
            "--new-window", url,
        });
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        setsid();
        execvp(argv[0], argv.data());
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

std::string ws_path_from_url(const std::string& value) {
    if (value.rfind("ws://", 0) == 0) {
        const auto slash = value.find('/', 5);
        return slash == std::string::npos ? "/" : value.substr(slash);
    }
    return value;
}

} // namespace

BrowserEngine BrowserLogin::engine_for_name(const std::string& name) {
    const auto id = lower(name);
    for (const char* needle : {"firefox", "librewolf", "waterfox", "floorp", "zen", "mozilla", "gecko"}) {
        if (id.find(needle) != std::string::npos) return BrowserEngine::Firefox;
    }
    return BrowserEngine::Chromium;
}

bool BrowserLogin::session_authenticated(const std::filesystem::path& jar, const std::string& account_host) {
    return std::filesystem::is_regular_file(jar) && session_is_authenticated(jar, account_host);
}

bool BrowserLogin::find_browser(const std::string& preferred, BrowserInfo& out) {
    std::vector<std::string> natives;
    std::vector<std::string> flatpaks;
    if (!preferred.empty()) natives.push_back(preferred);
    if (const auto* env = std::getenv("OPENBLIZZ_BROWSER"); env != nullptr && *env != '\0') natives.emplace_back(env);
    default_browser_hints(natives, flatpaks);
    natives.insert(natives.end(), firefox_names().begin(), firefox_names().end());
    natives.insert(natives.end(), chromium_names().begin(), chromium_names().end());

    for (const auto& candidate : natives) {
        if (candidate.find('.') != std::string::npos && candidate.find('/') == std::string::npos) {
            // Looks like a flatpak id (e.g. org.mozilla.firefox).
            if (make_flatpak(candidate, engine_for_name(candidate), out)) return true;
        }
        if (make_native(candidate, out)) return true;
    }
    for (const auto& id : flatpaks) {
        if (make_flatpak(id, engine_for_name(id), out)) return true;
    }
    for (const auto& app : flatpak_apps()) {
        if (make_flatpak(app.id, app.engine, out)) return true;
    }
    for (const char* snap : {"firefox", "chromium", "brave"}) {
        if (make_native(std::string("/snap/bin/") + snap, out)) return true;
    }
    return false;
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

std::vector<BrowserCookie> BrowserLogin::parse_bidi_cookies(const std::string& text) {
    const auto parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) throw std::runtime_error("invalid WebDriver BiDi cookie payload");
    const json& list = parsed.is_array() ? parsed : parsed.value("cookies", json::array());
    std::vector<BrowserCookie> cookies;
    for (const auto& item : list) {
        BrowserCookie cookie;
        cookie.domain = item.value("domain", "");
        cookie.path = item.value("path", "/");
        cookie.name = item.value("name", "");
        if (item.contains("value")) {
            const auto& value = item["value"];
            if (value.is_string()) {
                cookie.value = value.get<std::string>();
            } else if (value.is_object() && value.value("type", "") == "string") {
                cookie.value = value.value("value", "");
            } else {
                continue;  // base64 binary cookie values are not usable in cookies.txt
            }
        }
        cookie.expires = item.contains("expiry") && item["expiry"].is_number() ? item["expiry"].get<double>() : -1.0;
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
        long long expires = cookie.expires > 0 ? static_cast<long long>(cookie.expires) : 2147483647LL;
        if (cookie.http_only) out += "#HttpOnly_";
        out += domain + '\t' + (domain_wide ? "TRUE" : "FALSE") + '\t' + cookie.path + '\t' +
               (cookie.secure ? "TRUE" : "FALSE") + '\t' + std::to_string(expires) + '\t' + cookie.name + '\t' +
               cookie.value + '\n';
    }
    return out;
}

std::filesystem::path BrowserLogin::run(const BrowserLoginOptions& options) {
    BrowserInfo browser;
    if (!find_browser(options.browser, browser)) {
        throw std::runtime_error(
            "no supported browser found. OpenBlizz can drive Firefox-family browsers (firefox, librewolf, waterfox) "
            "and Chromium-family browsers (chromium, google-chrome, brave-browser, microsoft-edge, vivaldi), "
            "installed natively, as snap or as flatpak. Install one, set OPENBLIZZ_BROWSER=/path/to/browser "
            "(or a flatpak id such as org.mozilla.firefox), or fall back to "
            "`openblizz library scan --cookie-file cookies.txt`.");
    }
    const auto profile_dir = options.profile_dir.empty() ? browser.profile_dir : options.profile_dir;
    const auto jar = options.cookie_jar.empty() ? LibraryManager::default_cookie_jar() : options.cookie_jar;
    std::filesystem::create_directories(profile_dir);
    ::chmod(profile_dir.c_str(), 0700);
    std::error_code ec;
    std::filesystem::remove(profile_dir / "DevToolsActivePort", ec);
    std::filesystem::remove(profile_dir / "WebDriverBiDiServer.json", ec);

    const std::string start_url = "https://" + options.account_host + "/games";
    std::cout << "Opening " << browser.display_name << " (" << browser.packaging << ", "
              << (browser.engine == BrowserEngine::Firefox ? "WebDriver BiDi" : "DevTools") << ") with an isolated OpenBlizz profile.\n"
              << "Complete the Battle.net login there (password, authenticator/MFA, captcha).\n"
              << "OpenBlizz never receives your password; it only keeps the resulting session cookies.\n";
    const pid_t child = launch_browser(browser, profile_dir, start_url);

    HttpClient http;
    const auto endpoint = wait_for_endpoint(browser, profile_dir, child, 45);
    std::unique_ptr<AutomationClient> bidi;
    if (browser.engine == BrowserEngine::Firefox) {
        bidi = std::make_unique<AutomationClient>(endpoint.port, endpoint.ws_path);
        bidi->call("session.new", json{{"capabilities", json::object()}});
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    bool logged_in = false;
    bool announced_check = false;
    std::string last_url;

    const auto close_browser = [&]() {
        if (options.keep_browser_open) return;
        try {
            if (browser.engine == BrowserEngine::Firefox) {
                bidi->call("browser.close");
            } else {
                AutomationClient devtools(endpoint.port, ws_path_from_url(endpoint.ws_path));
                devtools.call("Browser.close");
            }
        } catch (const std::exception&) {
            killpg(child, SIGTERM);
        }
    };

    while (std::chrono::steady_clock::now() < deadline) {
        check_child_alive(child);
        std::vector<std::string> urls;
        try {
            if (browser.engine == BrowserEngine::Firefox) {
                const auto tree = bidi->call("browsingContext.getTree");
                for (const auto& context : tree.value("contexts", json::array())) urls.push_back(context.value("url", ""));
            } else {
                const auto list = http.get("http://127.0.0.1:" + std::to_string(endpoint.port) + "/json/list");
                const auto pages = json::parse(std::string(list.body.begin(), list.body.end()), nullptr, false);
                if (pages.is_array()) {
                    for (const auto& page : pages) {
                        if (page.value("type", "") == "page") urls.push_back(page.value("url", ""));
                    }
                }
            }
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        bool candidate = false;
        for (const auto& url : urls) {
            if (url != last_url) {
                last_url = url;
                std::cout << "  browser: " << url.substr(0, 96) << '\n';
            }
            if (looks_logged_in(url, options.account_host)) candidate = true;
        }
        if (candidate) {
            if (!announced_check) {
                std::cout << "  login page left; verifying the session...\n";
                announced_check = true;
            }
            // The /games URL also appears for a moment before the redirect to
            // the login page, so confirm against the account API before trusting it.
            std::vector<BrowserCookie> cookies;
            if (browser.engine == BrowserEngine::Firefox) {
                cookies = parse_bidi_cookies(bidi->call("storage.getCookies").dump());
            } else {
                AutomationClient devtools(endpoint.port, ws_path_from_url(endpoint.ws_path));
                cookies = parse_devtools_cookies(devtools.call("Storage.getCookies").dump());
            }
            const auto probe = jar.string() + ".probe";
            write_private_file(probe, netscape_jar(cookies, "battle.net"));
            if (session_is_authenticated(probe, options.account_host)) {
                std::filesystem::rename(probe, jar);
                ::chmod(jar.c_str(), 0600);
                logged_in = true;
                close_browser();
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
    for (int i = 0; i < 50 && waitpid(child, &status, WNOHANG) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Battle.net session captured and saved with owner-only permissions at " << jar << '\n';
    return jar;
}

} // namespace openblizz
