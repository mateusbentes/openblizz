#pragma once

#include <filesystem>
#include <string>

namespace openblizz {

struct AuthOptions {
    std::filesystem::path prefix;
    std::string proton_path{"GE-Proton"};
    std::string backend{"auto"};
    std::filesystem::path installer;
    std::string oauth_token;
};

class AuthManager {
public:
    static int login(const AuthOptions& options);
    static int status(const AuthOptions& options);
    static int account(const AuthOptions& options);
    static int agent_info(const AuthOptions& options, const std::string& product);
    static bool authenticated(const AuthOptions& options, const std::string& product,
                              std::string& reason);
    static void require_authenticated(const AuthOptions& options, const std::string& product);
};

} // namespace openblizz
