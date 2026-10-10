#pragma once

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
    std::filesystem::path cookie_file;
    std::filesystem::path cookie_jar;   // optional write-back target
    std::string host{"account.battle.net"};
};

struct AccountWebResult {
    std::vector<EntitlementRecord> records;
    std::vector<std::string> unknown_titles;
};

// Battle.net "titleId" values are the big-endian FourCC of a program code
// (22323 == "W3", 5730135 == "WoW", 1095647827 == "ANBS"). These helpers
// convert between the two so any NGDP product can be matched.
[[nodiscard]] std::string decode_title_id(std::int64_t title_id);
[[nodiscard]] std::int64_t encode_title_id(const std::string& code);
// Returns the account titleId expected for a catalog product, or -1 when the
// product is not distributed as a Battle.net game account (classic CD keys).
[[nodiscard]] std::int64_t expected_title_id(const std::string& product_id);

struct LibraryScanOptions {
    AccountWebSession web_session;
    std::filesystem::path dump_path;
    bool quiet{false};
};

class LibraryManager {
public:
    [[nodiscard]] static std::filesystem::path default_file();
    // Default location of the persisted account web session (Netscape cookies).
    [[nodiscard]] static std::filesystem::path default_cookie_jar();

    // If a cookie jar exists, silently re-scan the account when the stored
    // account-web ownership is older than max_age_seconds. Returns true when
    // a refresh happened. Network or session errors are reported as warnings.
    static bool auto_refresh(const Catalog& catalog, const std::filesystem::path& library_path,
                             const std::filesystem::path& cookie_jar, std::int64_t max_age_seconds);

    [[nodiscard]] static OwnershipState ownership_of(const std::filesystem::path& library_path,
                                                     const std::string& product_id);
    [[nodiscard]] static std::vector<LibraryEntry> load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path,
                     const std::vector<LibraryEntry>& entries);


    // Parse the account.battle.net "games-and-subs" and "classic-games" JSON.
    // Storefront cards used to enrich purchases that are not in the curated
    // catalog. The fields are public metadata only; no payment information is
    // retained.
    struct ShopCard {
        std::string name;
        std::string slug;
        std::string franchise;
        std::string app_game_code;
    };

    struct DynamicProduct {
        std::string product_id;
        std::string name;
        std::string family;
        std::string reason;
    };

    // Purchase history (account.battle.net /api/transactions). Each purchase
    // carries a localized productTitle which is matched against the catalog;
    // storefront cards can turn otherwise unknown third-party games into
    // dynamic, ownership-only library entries.
    struct PurchaseResult {
        std::vector<EntitlementRecord> records;
        std::vector<std::string> unmatched_titles;
        std::vector<DynamicProduct> dynamic_products;
    };
    [[nodiscard]] static PurchaseResult parse_purchases(
        const std::vector<std::string>& transaction_bodies,
        const std::vector<ProductDescriptor>& products,
        const std::vector<ShopCard>& shop_cards = {});

    // Storefront highlight cards embedded in the shop home page (public).
    [[nodiscard]] static std::vector<ShopCard> parse_shop_cards(const std::string& html);
    // Complete "Games" navigation menu of the storefront (text, destination,
    // category). Present in every shop page; the category lands in `franchise`
    // and the destination path in `slug`.
    [[nodiscard]] static std::vector<ShopCard> parse_shop_games(const std::string& html);
    // Product cards of a /family/<slug> page (title + product slug).
    [[nodiscard]] static std::vector<ShopCard> parse_shop_family(const std::string& html);
    // Known storefront destination -> OpenBlizz product ids (for the Library column).
    [[nodiscard]] static std::vector<std::string> shop_destination_products(const std::string& destination);

    [[nodiscard]] static AccountWebResult parse_account_web(
        const std::string& games_and_subs_body, const std::string& classic_games_body,
        const std::vector<ProductDescriptor>& products);


    static int list(const Catalog& catalog, const std::filesystem::path& path, bool show_all);
    static int add(const Catalog& catalog, const std::filesystem::path& path,
                   const std::string& product_id);
    static int remove(const Catalog& catalog, const std::filesystem::path& path,
                      const std::string& product_id);
    static int scan(const Catalog& catalog,
                    const std::filesystem::path& path, const LibraryScanOptions& options);
};

} // namespace openblizz
