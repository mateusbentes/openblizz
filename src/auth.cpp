#include "openblizz/auth.hpp"
#include "openblizz/http.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

std::string response_text(const HttpResponse& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
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
    if (battle_net_executable(options.prefix).empty()) {
        reason = "Battle.net.exe was not found in the supplied prefix";
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

    reason = "no authenticated Battle.net Agent session was found; run login and complete MFA first";
    return std::nullopt;
}

} // namespace

int AuthManager::login(const AuthOptions& options) {
    if (options.prefix.empty()) throw std::runtime_error("login requires --prefix");
    std::filesystem::create_directories(options.prefix);
    const auto executable = battle_net_executable(options.prefix);
    const auto target = executable.empty() ? options.installer : executable;
    if (target.empty() || !std::filesystem::exists(target)) {
        throw std::runtime_error(
            "Battle.net is not installed in this prefix; pass the official installer with --installer");
    }

    const auto command = "WINEPREFIX=" + shell_quote(options.prefix.string()) +
                         " PROTONPATH=" + shell_quote(options.proton_path) +
                         " GAMEID='umu-battlenet' umu-run " + shell_quote(target.string());
    std::cout << "Opening the official Battle.net UI. Complete login and MFA there.\n";
    std::cout << "OpenBlizz does not receive or store your password.\n";
    return std::system(command.c_str());
}

int AuthManager::account(const AuthOptions& options) {
    if (options.oauth_token.empty()) {
        throw std::runtime_error("no OAuth token supplied; set OPENBLIZZ_OAUTH_TOKEN or use --token-env");
    }
    HttpClient http;
    const auto response = http.get("https://oauth.battle.net/userinfo", {
        "Authorization: Bearer " + options.oauth_token,
        "User-Agent: OpenBlizz/0.1",
    });
    const auto user = json::parse(response_text(response));
    std::cout << redacted(user).dump(2) << '\n';
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
    if (!authenticated(options, product, reason)) {
        throw std::runtime_error("account authentication required: " + reason);
    }
    std::cout << "Authentication: " << reason << '\n';
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
