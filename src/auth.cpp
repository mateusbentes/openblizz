#include "openblizz/auth.hpp"
#include "openblizz/formats.hpp"
#include "openblizz/http.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace openblizz {
namespace {

using json = nlohmann::json;

std::string shell_quote(const std::string& value) {
    std::string quoted{"'"};
    for (const char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

bool command_available(const std::string& command) {
    const auto* path_value = std::getenv("PATH");
    if (path_value == nullptr) return false;
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    for (const auto& directory : split(std::string(path_value), separator)) {
        if (directory.empty()) continue;
        if (std::filesystem::exists(std::filesystem::path(directory) / command)) return true;
    }
    return false;
}

std::string response_text(const HttpResponse& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

std::string url_encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0F]);
        }
    }
    return result;
}

int hex_value(const char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) {
            result.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const auto high = hex_value(value[i + 1]);
            const auto low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                result.push_back(value[i]);
            }
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string base64(const std::string& value) {
    std::string result(4 * ((value.size() + 2) / 3), '\0');
    const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                        reinterpret_cast<const unsigned char*>(value.data()),
                                        static_cast<int>(value.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::string random_state() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("OpenSSL could not generate OAuth state");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0F]);
    }
    return result;
}

struct CallbackData {
    std::string code;
    std::string state;
    std::string error;
    std::string error_description;
};

CallbackData parse_callback(const std::string& input) {
    const auto question = input.find('?');
    if (question == std::string::npos) return {};
    auto query = input.substr(question + 1);
    const auto fragment = query.find('#');
    if (fragment != std::string::npos) query.resize(fragment);

    CallbackData result;
    for (const auto& pair : split(query, '&')) {
        const auto equals = pair.find('=');
        const auto key = url_decode(pair.substr(0, equals));
        const auto value = equals == std::string::npos ? std::string{} : url_decode(pair.substr(equals + 1));
        if (key == "code") result.code = value;
        else if (key == "state") result.state = value;
        else if (key == "error") result.error = value;
        else if (key == "error_description") result.error_description = value;
    }
    return result;
}

std::filesystem::path token_file(const AuthOptions& options) {
    if (!options.oauth_token_file.empty()) return options.oauth_token_file;
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz/oauth-token.json";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config/openblizz/oauth-token.json";
    }
    return std::filesystem::current_path() / ".openblizz-oauth-token.json";
}

std::string load_token(const AuthOptions& options) {
    if (!options.oauth_token.empty()) return options.oauth_token;
    const auto path = token_file(options);
    if (!std::filesystem::exists(path)) return {};
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read OAuth token file: " + path.string());
    const auto saved = json::parse(input, nullptr, true, true);
    const auto token = saved.value("access_token", std::string{});
    const auto expires_at = saved.value("expires_at", std::int64_t{0});
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (token.empty()) throw std::runtime_error("OAuth token file has no access_token: " + path.string());
    if (expires_at > 0 && now >= expires_at) {
        throw std::runtime_error("OAuth access token expired; run openblizz oauth-login again");
    }
    return token;
}

void save_token(const AuthOptions& options, const json& token) {
    const auto path = token_file(options);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    json persisted;
    for (const auto& key : {"access_token", "token_type", "scope", "expires_in", "expires_at", "refresh_token"}) {
        if (token.contains(key)) persisted[key] = token.at(key);
    }
    const auto temporary = path.string() + ".part";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create OAuth token file: " + path.string());
    output << persisted.dump(2) << '\n';
    output.close();
#if !defined(_WIN32)
    ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) throw std::runtime_error("cannot install OAuth token file: " + error.message());
#if !defined(_WIN32)
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

std::filesystem::path battle_net_executable(const std::filesystem::path& prefix) {
    const std::vector<std::filesystem::path> candidates = {
        prefix / "drive_c/Program Files (x86)/Battle.net/Battle.net.exe",
        prefix / "drive_c/Program Files/Battle.net/Battle.net.exe",
        prefix / "drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

std::vector<unsigned short> listening_ports() {
#if defined(_WIN32)
    return {};
#else
    FILE* pipe = ::popen("ss -ltnH 2>/dev/null", "r");
    if (pipe == nullptr) return {};
    std::vector<unsigned short> ports;
    char buffer[512]{};
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        const std::string line(buffer);
        for (const std::string marker : {"127.0.0.1:", "[::1]:", "0.0.0.0:"}) {
            const auto position = line.find(marker);
            if (position == std::string::npos) continue;
            std::size_t end = position + marker.size();
            while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
            try {
                const auto port = std::stoul(line.substr(position + marker.size(), end - position - marker.size()));
                if (port > 0 && port <= 65535) ports.push_back(static_cast<unsigned short>(port));
            } catch (const std::exception&) {
                // Ignore malformed listener lines.
            }
            break;
        }
    }
    ::pclose(pipe);
    return ports;
#endif
}

bool json_has_agent_error(const json& value) {
    if (!value.is_object() || !value.contains("error")) return false;
    try {
        return value.at("error").get<double>() > 0.0;
    } catch (const std::exception&) {
        return false;
    }
}

bool sensitive_key(const std::string& key) {
    std::string lower;
    lower.reserve(key.size());
    for (const auto ch : key) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return lower.find("token") != std::string::npos ||
           lower.find("authorization") != std::string::npos ||
           lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos;
}

json redacted(json value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (sensitive_key(it.key())) it.value() = "[redacted]";
            else it.value() = redacted(it.value());
        }
    } else if (value.is_array()) {
        for (auto& item : value) item = redacted(item);
    }
    return value;
}

struct AgentSession {
    unsigned short port{};
    std::string token;
    json agent;
    std::optional<json> product_status;
};

std::optional<AgentSession> probe_agent(const AuthOptions& options, const std::string& product,
                                        std::string& reason) {
    if (options.prefix.empty()) {
        reason = "no Battle.net prefix was provided";
        return std::nullopt;
    }
    if (!std::filesystem::exists(options.prefix)) {
        reason = "the supplied Battle.net prefix does not exist";
        return std::nullopt;
    }

    HttpClient http;
    for (const auto port : listening_ports()) {
        const auto base = "http://127.0.0.1:" + std::to_string(port);
        try {
            const auto agent_response = http.get(base + "/agent");
            const auto agent = json::parse(response_text(agent_response));
            if (!agent.is_object() || !agent.contains("authorization")) continue;
            const auto token = agent.at("authorization").get<std::string>();
            if (token.empty()) continue;

            AgentSession session{port, token, agent, std::nullopt};
            const std::vector<std::string> headers{
                "Authorization: " + token,
                "User-Agent: OpenBlizz/0.1",
            };
            if (!product.empty()) {
                const auto version_response = http.get(base + "/version/" + product, headers);
                const auto version = json::parse(response_text(version_response));
                if (json_has_agent_error(version)) {
                    reason = "Battle.net Agent rejected the product session";
                    continue;
                }
                session.product_status = version;
            }
            return session;
        } catch (const std::exception&) {
            // Most listeners are unrelated services; continue probing.
        }
    }

    reason = "no authenticated Battle.net Agent session was found; run agent-login and complete MFA first";
    return std::nullopt;
}

json user_info(const std::string& token) {
    HttpClient http;
    const auto response = http.get("https://oauth.battle.net/userinfo", {
        "Authorization: Bearer " + token,
        "User-Agent: OpenBlizz/0.1",
    });
    return json::parse(response_text(response));
}

} // namespace

int AuthManager::agent_login(const AuthOptions& options) {
    if (options.prefix.empty()) throw std::runtime_error("agent-login requires --prefix");
    std::filesystem::create_directories(options.prefix);
    const auto executable = battle_net_executable(options.prefix);
    const auto target = executable.empty() ? options.installer : executable;
    if (target.empty() || !std::filesystem::exists(target)) {
        throw std::runtime_error(
            "Battle.net is not installed in this prefix; pass the official installer with --installer");
    }

    auto backend = options.backend;
    if (backend == "auto") {
        if (command_available("umu-run")) backend = "umu";
        else if (command_available("wine")) backend = "wine";
        else {
            throw std::runtime_error(
                "no Windows runtime found: install umu-launcher for Proton or install Wine; "
                "see https://github.com/Open-Wine-Components/umu-launcher");
        }
    }

    std::string command;
    if (backend == "umu" || backend == "proton") {
        if (!command_available("umu-run")) {
            throw std::runtime_error("umu-run was not found; install umu-launcher or use --backend wine");
        }
        command = "WINEPREFIX=" + shell_quote(options.prefix.string()) +
                  " PROTONPATH=" + shell_quote(options.proton_path) +
                  " GAMEID='umu-battlenet' umu-run " + shell_quote(target.string());
    } else if (backend == "wine") {
        if (!command_available("wine")) {
            throw std::runtime_error("wine was not found; install Wine or use --backend umu");
        }
        command = "WINEPREFIX=" + shell_quote(options.prefix.string()) +
                  " wine " + shell_quote(target.string());
    } else {
        throw std::runtime_error("unknown agent-login backend: " + backend + " (expected auto, umu, or wine)");
    }
    std::cout << "Opening the official Battle.net UI. Complete login and MFA there.\n";
    std::cout << "OpenBlizz does not receive or store your password.\n";
    return std::system(command.c_str());
}

int AuthManager::oauth_login(const AuthOptions& options) {
    if (options.oauth_client_id.empty()) {
        throw std::runtime_error("oauth-login requires --client-id or OPENBLIZZ_CLIENT_ID");
    }
    if (options.oauth_client_secret.empty()) {
        throw std::runtime_error("oauth-login requires the client secret in OPENBLIZZ_CLIENT_SECRET or --secret-env");
    }
    if (options.oauth_redirect_uri.empty()) {
        throw std::runtime_error("oauth-login requires --redirect-uri matching the registered OAuth client");
    }
    if (options.oauth_redirect_uri.rfind("https://", 0) != 0) {
        throw std::runtime_error("OAuth redirect URI must use HTTPS and match the registered client");
    }

    const auto state = random_state();
    const auto scope = options.oauth_scope.empty() ? "openid" : options.oauth_scope;
    const auto authorize_url =
        "https://oauth.battle.net/authorize?response_type=code&client_id=" + url_encode(options.oauth_client_id) +
        "&scope=" + url_encode(scope) + "&redirect_uri=" + url_encode(options.oauth_redirect_uri) +
        "&state=" + url_encode(state);

    std::cout << "Open this URL in your browser and authorize OpenBlizz:\n\n" << authorize_url << "\n\n";
    if (command_available("xdg-open")) {
        const auto command = "xdg-open " + shell_quote(authorize_url) + " >/dev/null 2>&1 &";
        (void)std::system(command.c_str());
    }
    std::cout << "Paste the complete callback URL, including its state parameter: " << std::flush;
    std::string callback;
    if (!std::getline(std::cin, callback) || callback.empty()) {
        throw std::runtime_error("OAuth callback input was empty");
    }

    const auto data = parse_callback(callback);
    if (!data.error.empty()) {
        throw std::runtime_error("OAuth authorization failed: " + data.error +
                                 (data.error_description.empty() ? std::string{} : " (" + data.error_description + ")"));
    }
    if (data.code.empty()) throw std::runtime_error("OAuth callback did not contain a code");
    if (data.state.empty()) throw std::runtime_error("OAuth callback did not contain state");
    if (data.state != state) throw std::runtime_error("OAuth state mismatch");

    const auto form = "grant_type=authorization_code&auth_flow=auth_code&code=" + url_encode(data.code) +
                      "&redirect_uri=" + url_encode(options.oauth_redirect_uri);
    const auto basic = base64(options.oauth_client_id + ":" + options.oauth_client_secret);
    HttpClient http;
    const auto token_response = http.post("https://oauth.battle.net/token", form, {
        "Authorization: Basic " + basic,
        "Content-Type: application/x-www-form-urlencoded",
        "Accept: application/json",
        "User-Agent: OpenBlizz/0.1",
    });
    auto token = json::parse(response_text(token_response));
    const auto access_token = token.value("access_token", std::string{});
    if (access_token.empty()) throw std::runtime_error("OAuth token response did not contain access_token");

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    token["expires_at"] = now + token.value("expires_in", std::int64_t{0});
    save_token(options, token);
    std::cout << "OAuth token saved with owner-only permissions at " << token_file(options) << "\n";

    try {
        std::cout << "Account identity:\n" << redacted(user_info(access_token)).dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Warning: token was saved, but /userinfo failed: " << error.what() << '\n';
    }
    return 0;
}

int AuthManager::account(const AuthOptions& options) {
    const auto token = load_token(options);
    if (token.empty()) {
        throw std::runtime_error("no OAuth token found; run oauth-login or set OPENBLIZZ_OAUTH_TOKEN");
    }
    std::cout << redacted(user_info(token)).dump(2) << '\n';
    return 0;
}

int AuthManager::agent_info(const AuthOptions& options, const std::string& product) {
    std::string reason;
    const auto session = probe_agent(options, product, reason);
    if (!session) {
        std::cout << "session: not authenticated\nreason: " << reason << '\n';
        return 1;
    }
    std::cout << "agent-port: " << session->port << '\n';
    std::cout << "agent: " << redacted(session->agent).dump(2) << '\n';
    if (session->product_status) {
        std::cout << "product: " << product << '\n';
        std::cout << "product-status: " << redacted(*session->product_status).dump(2) << '\n';
    }
    return 0;
}

bool AuthManager::authenticated(const AuthOptions& options, const std::string& product,
                                std::string& reason) {
    const auto session = probe_agent(options, product, reason);
    if (!session) return false;
    reason = "authenticated Battle.net Agent session found on localhost:" + std::to_string(session->port);
    return true;
}

void AuthManager::require_authenticated(const AuthOptions& options, const std::string& product) {
    std::string reason;
    if (authenticated(options, product, reason)) {
        std::cout << "Authentication: " << reason << '\n';
        return;
    }

    try {
        const auto token = load_token(options);
        if (!token.empty()) {
            const auto identity = user_info(token);
            std::cout << "Authentication: OAuth identity verified\n";
            std::cout << "Identity: " << redacted(identity).dump() << '\n';
            std::cout << "Warning: Blizzard does not document a public entitlement endpoint; "
                         "product ownership remains unverified by OpenBlizz.\n";
            return;
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("account authentication required: " + reason + "; OAuth: " + error.what());
    }
    throw std::runtime_error("account authentication required: " + reason +
                             "; run openblizz login to authenticate with OAuth");
}

int AuthManager::status(const AuthOptions& options) {
    if (options.prefix.empty()) throw std::runtime_error("auth-status requires --prefix");
    const auto executable = battle_net_executable(options.prefix);
    const auto agent_dir = options.prefix / "drive_c/ProgramData/Battle.net/Agent";
    std::cout << "prefix: " << options.prefix << '\n';
    std::cout << "battle.net executable: " << (executable.empty() ? "not found" : executable.string()) << '\n';
    std::cout << "agent data: " << (std::filesystem::exists(agent_dir) ? "present" : "not found") << '\n';

    std::string reason;
    const auto ok = authenticated(options, "", reason);
    std::cout << "session: " << (ok ? reason : "not authenticated") << '\n';
    std::cout << "entitlement: OpenBlizz does not infer ownership from local files; product operations are gated on the authenticated Agent session\n";
    return ok ? 0 : 1;
}

} // namespace openblizz
