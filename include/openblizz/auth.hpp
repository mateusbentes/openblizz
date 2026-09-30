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
    std::string oauth_client_id;
    std::string oauth_client_secret;
    std::string oauth_redirect_uri;
    std::string oauth_scope{"openid"};
    std::filesystem::path oauth_token_file;
};

class AuthManager {
public:
    static int agent_login(const AuthOptions& options);
    static int oauth_login(const AuthOptions& options);
    static int status(const AuthOptions& options);
    static int account(const AuthOptions& options);
    static int agent_info(const AuthOptions& options, const std::string& product);
    static bool authenticated(const AuthOptions& options, const std::string& product,
                              std::string& reason);
    static void require_authenticated(const AuthOptions& options, const std::string& product);
};

} // namespace openblizz
