#pragma once

#include "openblizz/auth.hpp"
#include "openblizz/catalog.hpp"
#include "openblizz/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

struct EntitlementRecord {
    std::string product_id;
    bool owned{false};
    bool explicit_state{false};
    std::string source;
    std::string reason;
};

// Session cookies for the account.battle.net web application. Loaded only in
// memory from a Netscape cookies.txt export or a raw Cookie header string.
struct AccountWebSession {
    std::string cookie_header;
    std::string host{"account.battle.net"};
};

struct AccountWebResult {
    std::vector<EntitlementRecord> records;
    std::vector<std::string> unknown_titles;
};

struct LibraryScanOptions {
    std::string entitlement_url;
    AccountWebSession web_session;
    std::filesystem::path dump_path;
};

class LibraryManager {
public:
    [[nodiscard]] static std::filesystem::path default_file();
    [[nodiscard]] static std::vector<LibraryEntry> load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path,
                     const std::vector<LibraryEntry>& entries);

    // Parse only deliberately supported response shapes. Absence is never treated as not-owned.
    [[nodiscard]] static std::vector<EntitlementRecord> parse_entitlement_response(
        const std::string& body, const std::vector<ProductDescriptor>& products);

    // Parse the account.battle.net "games-and-subs" and "classic-games" JSON.
    [[nodiscard]] static AccountWebResult parse_account_web(
        const std::string& games_and_subs_body, const std::string& classic_games_body,
        const std::vector<ProductDescriptor>& products);

    [[nodiscard]] static std::string cookie_header_from_netscape_file(
        const std::filesystem::path& path, const std::string& host);

    static int list(const Catalog& catalog, const std::filesystem::path& path);
    static int add(const Catalog& catalog, const std::filesystem::path& path,
                   const std::string& product_id);
    static int remove(const Catalog& catalog, const std::filesystem::path& path,
                      const std::string& product_id);
    static int scan(const Catalog& catalog, const AuthOptions& auth,
                    const std::filesystem::path& path, const LibraryScanOptions& options);
};

} // namespace openblizz
