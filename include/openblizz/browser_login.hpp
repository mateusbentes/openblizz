#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

// Interactive Battle.net account login driven through a Chromium-family
// browser (Chromium, Chrome, Brave, Edge, Vivaldi).
//
// OpenBlizz never sees or stores the password: it launches the browser with a
// dedicated, isolated profile, waits until the official Battle.net login
// (including MFA / captcha) has completed, reads the resulting session cookies
// through the browser's local DevTools endpoint, saves them as a Netscape
// cookies.txt with owner-only permissions, and closes that browser window.
struct BrowserLoginOptions {
    std::string browser;                     // explicit executable; empty = auto-detect
    std::filesystem::path profile_dir;       // isolated profile; empty = default
    std::filesystem::path cookie_jar;        // destination; empty = LibraryManager default
    std::string account_host{"account.battle.net"};
    int timeout_seconds{600};
    bool keep_browser_open{false};
};

struct BrowserCookie {
    std::string domain;
    std::string path;
    std::string name;
    std::string value;
    double expires{-1};
    bool secure{false};
    bool http_only{false};
};

class BrowserLogin {
public:
    [[nodiscard]] static std::filesystem::path default_profile_dir();
    [[nodiscard]] static std::string find_browser(const std::string& preferred);

    // Runs the whole interactive flow. Returns the path of the saved jar.
    static std::filesystem::path run(const BrowserLoginOptions& options);

    // Pure helpers, exposed for unit tests.
    [[nodiscard]] static std::string netscape_jar(const std::vector<BrowserCookie>& cookies,
                                                  const std::string& domain_suffix);
    [[nodiscard]] static std::vector<BrowserCookie> parse_devtools_cookies(const std::string& json);
    [[nodiscard]] static bool looks_logged_in(const std::string& url, const std::string& account_host);

    // True when the saved jar still yields an authenticated account.battle.net session.
    [[nodiscard]] static bool session_authenticated(const std::filesystem::path& jar, const std::string& account_host);
};

} // namespace openblizz
