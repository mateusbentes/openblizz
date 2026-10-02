#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

// Interactive Battle.net account login driven through the user's browser.
//
// Supported engines:
//   * Chromium family (Chromium, Chrome, Brave, Edge, Vivaldi, Opera) through
//     the Chrome DevTools Protocol;
//   * Firefox family (Firefox, Firefox ESR/Developer Edition, LibreWolf,
//     Waterfox) through WebDriver BiDi.
//
// OpenBlizz never sees or stores the password: it launches the browser with a
// dedicated, isolated profile, waits until the official Battle.net login
// (including MFA / captcha) has completed, reads the resulting session cookies
// through the browser's local automation endpoint, saves them as a Netscape
// cookies.txt with owner-only permissions, and closes that browser window.
struct BrowserLoginOptions {
    std::string browser;                     // explicit executable or flatpak id; empty = auto-detect
    std::filesystem::path profile_dir;       // isolated profile; empty = default for that browser
    std::filesystem::path cookie_jar;        // destination; empty = LibraryManager default
    std::string account_host{"account.battle.net"};
    int timeout_seconds{600};
    bool keep_browser_open{false};
};

enum class BrowserEngine { Chromium, Firefox };

struct BrowserInfo {
    BrowserEngine engine{BrowserEngine::Chromium};
    std::vector<std::string> command;        // argv prefix, e.g. {"/usr/bin/firefox"} or {"flatpak","run","org.mozilla.firefox"}
    std::string display_name;
    std::filesystem::path profile_dir;       // sandbox-aware default profile directory
    std::string packaging;                   // "native", "snap" or "flatpak"
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
    // Detection order: explicit argument, OPENBLIZZ_BROWSER, the desktop
    // default browser (xdg-settings), then well-known executables, snaps and
    // flatpaks. Returns false when nothing usable is installed.
    [[nodiscard]] static bool find_browser(const std::string& preferred, BrowserInfo& out);

    // Runs the whole interactive flow. Returns the path of the saved jar.
    static std::filesystem::path run(const BrowserLoginOptions& options);

    // True when the saved jar still yields an authenticated account.battle.net session.
    [[nodiscard]] static bool session_authenticated(const std::filesystem::path& jar, const std::string& account_host);

    // Pure helpers, exposed for unit tests.
    [[nodiscard]] static std::string netscape_jar(const std::vector<BrowserCookie>& cookies,
                                                  const std::string& domain_suffix);
    [[nodiscard]] static std::vector<BrowserCookie> parse_devtools_cookies(const std::string& json);
    [[nodiscard]] static std::vector<BrowserCookie> parse_bidi_cookies(const std::string& json);
    [[nodiscard]] static bool looks_logged_in(const std::string& url, const std::string& account_host);
    [[nodiscard]] static BrowserEngine engine_for_name(const std::string& name);
};

} // namespace openblizz
