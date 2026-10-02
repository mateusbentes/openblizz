#include "openblizz/library.hpp"
#include "openblizz/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string response_text(const HttpResponse& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

std::string lower(std::string value) {
    for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool is_local_http(const std::string& url) {
    return url.rfind("http://localhost/", 0) == 0 ||
           url.rfind("http://localhost:", 0) == 0 ||
           url.rfind("http://127.0.0.1/", 0) == 0 ||
           url.rfind("http://127.0.0.1:", 0) == 0 ||
           url.rfind("http://[::1]/", 0) == 0;
}

void require_safe_endpoint(const std::string& url) {
    const auto normalized = lower(url);
    if (normalized.find(".example") != std::string::npos ||
        normalized.find("your-authorized-service") != std::string::npos) {
        throw std::runtime_error(
            "the entitlement URL is an example placeholder; omit --entitlement-url until you have a real authorized endpoint");
    }
    if (url.rfind("https://", 0) != 0 && !is_local_http(url)) {
        throw std::runtime_error(
            "experimental entitlement URL must use HTTPS; plain HTTP is allowed only for localhost");
    }
}

const ProductDescriptor* known_product(const std::vector<ProductDescriptor>& products,
                                       const std::string& candidate) {
    const auto needle = lower(candidate);
    for (const auto& product : products) {
        if (needle == lower(product.id) || needle == lower(product.agent_product)) return &product;
    }
    return nullptr;
}

std::optional<std::string> object_product_id(const json& value) {
    if (!value.is_object()) return std::nullopt;
    for (const auto& key : {"product_id", "productId", "product", "uid", "code", "slug", "id"}) {
        if (!value.contains(key)) continue;
        const auto& item = value.at(key);
        if (item.is_string()) return item.get<std::string>();
    }
    return std::nullopt;
}

std::optional<bool> object_owned(const json& value) {
    if (!value.is_object()) return std::nullopt;
    for (const auto& key : {"owned", "entitled", "has_entitlement", "hasEntitlement", "active"}) {
        if (!value.contains(key) || !value.at(key).is_boolean()) continue;
        return value.at(key).get<bool>();
    }
    return std::nullopt;
}

void collect_item(const json& item, const std::vector<ProductDescriptor>& products,
                  std::vector<EntitlementRecord>& result) {
    if (item.is_string()) {
        if (const auto* product = known_product(products, item.get<std::string>()); product != nullptr) {
            result.push_back({product->id, true, true, "experimental-oauth", "listed by configured endpoint"});
        }
        return;
    }
    if (!item.is_object()) return;

    const auto candidate = object_product_id(item);
    if (candidate) {
        if (const auto* product = known_product(products, *candidate); product != nullptr) {
            const auto owned = object_owned(item).value_or(true);
            result.push_back({product->id, owned, true, "experimental-oauth",
                              owned ? "endpoint reported entitlement" : "endpoint reported no entitlement"});
        }
    }

    for (const auto& key : {"products", "entitlements", "owned_products", "ownedProducts", "games", "items"}) {
        if (!item.contains(key) || !item.at(key).is_array()) continue;
        for (const auto& nested : item.at(key)) collect_item(nested, products, result);
    }
}

std::vector<LibraryEntry> catalog_entries(const Catalog& catalog,
                                          const std::vector<LibraryEntry>& existing) {
    std::map<std::string, LibraryEntry> by_id;
    for (const auto& entry : existing) by_id[entry.product_id] = entry;

    std::vector<LibraryEntry> result;
    for (const auto& product : catalog.products()) {
        const auto it = by_id.find(product.id);
        if (it != by_id.end()) {
            auto entry = it->second;
            if (entry.name.empty()) entry.name = product.name;
            result.push_back(std::move(entry));
        } else {
            result.push_back({product.id, product.name, OwnershipState::Unknown,
                              "catalog", "not scanned", now_seconds()});
        }
        by_id.erase(product.id);
    }
    for (const auto& [id, entry] : by_id) result.push_back(entry);  // dynamic (summary) products
    return result;
}

void print_entries(const std::vector<LibraryEntry>& entries) {
    for (const auto& entry : entries) {
        std::cout << entry.product_id << '\t'
                  << ownership_state_name(entry.ownership) << '\t'
                  << "source=" << (entry.source.empty() ? "unknown" : entry.source);
        if (!entry.reason.empty()) std::cout << "\t" << entry.reason;
        std::cout << '\n';
    }
}

} // namespace

std::filesystem::path LibraryManager::default_file() {
    if (const auto* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
        return std::filesystem::path(state) / "openblizz/library.json";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local/state/openblizz/library.json";
    }
    return std::filesystem::current_path() / ".openblizz-library.json";
}

std::vector<LibraryEntry> LibraryManager::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read library file: " + path.string());
    const auto document = json::parse(input, nullptr, true, true);
    if (!document.is_object() || !document.contains("products") || !document.at("products").is_array()) {
        throw std::runtime_error("library file has an invalid schema: " + path.string());
    }

    std::vector<LibraryEntry> result;
    for (const auto& item : document.at("products")) {
        if (!item.is_object() || !item.contains("product_id") || !item.at("product_id").is_string()) continue;
        LibraryEntry entry;
        entry.product_id = item.at("product_id").get<std::string>();
        entry.name = item.value("name", std::string{});
        entry.ownership = ownership_state_from_name(item.value("ownership", std::string{"unknown"}));
        entry.source = item.value("source", std::string{});
        entry.reason = item.value("reason", std::string{});
        entry.updated_at = item.value("updated_at", std::int64_t{0});
        result.push_back(std::move(entry));
    }
    return result;
}

void LibraryManager::save(const std::filesystem::path& path,
                          const std::vector<LibraryEntry>& entries) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    json document;
    document["schema"] = 1;
    document["products"] = json::array();
    for (const auto& entry : entries) {
        document["products"].push_back({
            {"product_id", entry.product_id},
            {"name", entry.name},
            {"ownership", ownership_state_name(entry.ownership)},
            {"source", entry.source},
            {"reason", entry.reason},
            {"updated_at", entry.updated_at},
        });
    }

    const auto temporary = path.string() + ".part";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create library file: " + path.string());
    output << document.dump(2) << '\n';
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
    if (error) throw std::runtime_error("cannot install library file: " + error.message());
#if !defined(_WIN32)
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

std::vector<EntitlementRecord> LibraryManager::parse_entitlement_response(
    const std::string& body, const std::vector<ProductDescriptor>& products) {
    const auto document = json::parse(body, nullptr, true, true);
    std::vector<EntitlementRecord> result;

    if (document.is_array()) {
        for (const auto& item : document) collect_item(item, products, result);
    } else if (document.is_object()) {
        for (const auto& product : products) {
            for (const auto& key : {product.id, product.agent_product}) {
                if (!document.contains(key) || !document.at(key).is_boolean()) continue;
                result.push_back({product.id, document.at(key).get<bool>(), true,
                                  "experimental-oauth", "endpoint returned a product boolean"});
            }
        }
        collect_item(document, products, result);
        for (const auto& key : {"products", "entitlements", "owned_products", "ownedProducts", "games", "items"}) {
            if (!document.contains(key) || !document.at(key).is_array()) continue;
            for (const auto& item : document.at(key)) collect_item(item, products, result);
        }
    } else {
        throw std::runtime_error("entitlement endpoint returned JSON that is not an object or array");
    }

    std::map<std::string, EntitlementRecord> unique;
    for (const auto& record : result) unique[record.product_id] = record;
    result.clear();
    for (auto& [id, record] : unique) result.push_back(std::move(record));
    return result;
}

std::filesystem::path LibraryManager::default_cookie_jar() {
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz" / "battlenet-cookies.txt";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "openblizz" / "battlenet-cookies.txt";
    }
    return std::filesystem::current_path() / "battlenet-cookies.txt";
}

bool LibraryManager::auto_refresh(const Catalog& catalog, const std::filesystem::path& library_path,
                                  const std::filesystem::path& cookie_jar, std::int64_t max_age_seconds) {
    if (cookie_jar.empty() || !std::filesystem::is_regular_file(cookie_jar)) return false;
    const auto entries = load(library_path);
    std::int64_t newest = 0;
    for (const auto& entry : entries) {
        if (entry.source == "account-web") newest = std::max(newest, entry.updated_at);
    }
    if (newest != 0 && now_seconds() - newest < max_age_seconds) return false;

    LibraryScanOptions options;
    options.web_session.cookie_file = cookie_jar;
    options.web_session.cookie_jar = cookie_jar;
    options.quiet = true;
    try {
        std::cout << "Refreshing account library from the saved session.\n";
        (void)scan(catalog, AuthOptions{}, library_path, options);
        return true;
    } catch (const std::exception& error) {
        std::cerr << "Warning: account library refresh failed: " << error.what() << '\n';
        return false;
    }
}

OwnershipState LibraryManager::ownership_of(const std::filesystem::path& library_path,
                                            const std::string& product_id) {
    for (const auto& entry : load(library_path)) {
        if (entry.product_id == product_id) return entry.ownership;
    }
    return OwnershipState::Unknown;
}

int LibraryManager::list(const Catalog& catalog, const std::filesystem::path& path, bool show_all) {
    auto entries = catalog_entries(catalog, load(path));
    if (!show_all) {
        std::vector<LibraryEntry> mine;
        for (const auto& entry : entries) {
            if (entry.ownership == OwnershipState::Owned || entry.ownership == OwnershipState::Manual) mine.push_back(entry);
        }
        if (mine.empty()) {
            std::cout << "No owned products known yet. Run `openblizz library scan --cookie-file cookies.txt` "
                         "once, or use --all to see the full catalog.\n";
        }
        entries = std::move(mine);
    }
    print_entries(entries);
    std::cout << "library-file: " << path << '\n';
    return 0;
}

int LibraryManager::add(const Catalog& catalog, const std::filesystem::path& path,
                        const std::string& product_id) {
    const auto products = catalog.products();
    const auto& product = find_product(products, product_id);
    auto entries = catalog_entries(catalog, load(path));
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const LibraryEntry& entry) {
        return entry.product_id == product.id;
    });
    if (it == entries.end()) throw std::runtime_error("product is not in the public catalog: " + product_id);
    it->ownership = OwnershipState::Manual;
    it->source = "manual";
    it->reason = "explicitly selected by the user; ownership is not independently verified";
    it->updated_at = now_seconds();
    save(path, entries);
    std::cout << "Added " << product.id << " as manual/unknown ownership.\n";
    return 0;
}

int LibraryManager::remove(const Catalog& catalog, const std::filesystem::path& path,
                           const std::string& product_id) {
    const auto products = catalog.products();
    const auto& product = find_product(products, product_id);
    auto entries = load(path);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const LibraryEntry& entry) {
        return entry.product_id == product.id;
    }), entries.end());
    save(path, entries);
    std::cout << "Removed " << product.id << " from the local library.\n";
    return 0;
}

namespace {

// Title identifiers observed in the account.battle.net "games-and-subs"
// response. The mapping is documented in SOURCES.md and derived from the
// MIT-licensed galaxy-integration-blizzard project and public account pages.
std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Title identifiers whose program code differs from the NGDP product code.
// Everything else is resolved by decoding the FourCC (see decode_title_id).
// Sources: the user's own account responses and the MIT-licensed
// galaxy-integration-blizzard definitions, documented in SOURCES.md.
const std::map<std::int64_t, std::vector<std::string>>& title_id_overrides() {
    static const std::map<std::int64_t, std::vector<std::string>> table{
        {22323, {"w3", "w3-legacy-tft"}},      // "W3"   Warcraft III (Reforged + legacy)
        {1465140039, {"hsb"}},                 // "WTCG" Hearthstone
        {1214607983, {"hero"}},                // "Hero" Heroes of the Storm
        {5272175, {"pro"}},                    // "Pro"  Overwatch
        {1329875278, {"odin"}},                // "OdIn" Call of Duty: Modern Warfare
        {1279351378, {"lazr"}},                // "LAZR" Call of Duty: Modern Warfare II
    };
    return table;
}

std::vector<std::string> products_for_title(std::int64_t title_id, const std::vector<ProductDescriptor>& products) {
    const auto override = title_id_overrides().find(title_id);
    if (override != title_id_overrides().end()) return override->second;
    const auto code = lower_copy(decode_title_id(title_id));
    if (code.empty()) return {};
    std::vector<std::string> result;
    for (const auto& product : products) {
        if (lower_copy(product.id) == code) result.push_back(product.id);
    }
    return result;
}
// Program codes that create a Battle.net *game account* and therefore show up
// in games-and-subs. Titles sold as a plain license (Warcraft I/II Remastered,
// Blizzard Arcade Collection, ...) do not, so their absence there proves
// nothing; those are resolved through the purchase history instead.
const std::set<std::string>& game_account_products() {
    static const std::set<std::string> table{
        "w3", "w3-legacy-tft", "s1", "s2", "wow", "d3", "hsb", "hero", "pro", "anbs", "osi",
        "fenris", "odin", "lazr", "zeus", "viper", "fore", "auks",
    };
    return table;
}

// Normalised purchase titles (see normalize_title) and the products they
// unlock. Checked by substring so editions/bundles ("... Battle Chest") match.
const std::vector<std::pair<std::string, std::vector<std::string>>>& purchase_title_products() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> table{
        {"warcraftiiiremastered", {"w1r", "w2r"}},
        {"warcraftiandiiremastered", {"w1r", "w2r"}},
        {"warcraftremasteredbattlechest", {"w1r", "w2r"}},
        {"warcraftiremastered", {"w1r"}},
        {"warcraftiiremastered", {"w2r"}},
        {"warcraftiiireforged", {"w3"}},
        {"warcraft3reforged", {"w3"}},
        {"warcraftiiibattlechest", {"w3-legacy-tft"}},
        {"warcraftiibattleneedition", {"w2bn"}},
        {"warcraftiibattlenetedition", {"w2bn"}},
        {"warcraftorcshumans", {"war1"}},
        {"warcraftorcsandhumans", {"war1"}},
        {"starcraftremastered", {"s1"}},
        {"starcraftii", {"s2"}},
        {"starcraft2", {"s2"}},
        {"diabloiiresurrected", {"osi"}},
        {"diabloiilordofdestruction", {"d2-lod"}},
        {"diabloiv", {"fenris"}},
        {"diabloiii", {"d3"}},
        {"diabloimmortal", {"anbs"}},
        {"worldofwarcraft", {"wow"}},
        {"overwatch", {"pro"}},
        {"hearthstone", {"hsb"}},
        {"heroesofthestorm", {"hero"}},
        {"blizzardarcadecollection", {"rtro"}},
        {"crashbandicoot4", {"wlby"}},
    };
    return table;
}

std::string normalize_title(std::string value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto ch = static_cast<unsigned char>(value[i]);
        // Drop the UTF-8 sequences for ® (c2 ae), ™ (e2 84 a2) and nbsp (c2 a0).
        if (ch == 0xc2 && i + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[i + 1]);
            if (next == 0xae || next == 0xa0) { ++i; continue; }
        }
        if (ch == 0xe2 && i + 2 < value.size() &&
            static_cast<unsigned char>(value[i + 1]) == 0x84 &&
            static_cast<unsigned char>(value[i + 2]) == 0xa2) { i += 2; continue; }
        if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

// Classic (CD key) titles listed by "classic-games" and the NGDP products
// they correspond to in the OpenBlizz catalog.
const std::vector<std::pair<std::string, std::string>>& classic_title_products() {
    static const std::vector<std::pair<std::string, std::string>> table{
        {"warcraftiiireignofchaos", "w3-legacy-tft"},
        {"warcraftiiithefrozenthrone", "w3-legacy-tft"},
        {"warcraftiibattleneedition", "w2bn"},
        {"warcraftiibattlenetedition", "w2bn"},
        {"warcraftorcshumans", "war1"},
        {"warcraftorcsandhumans", "war1"},
        {"starcraftanthology", "s1"},
        {"starcraft", "s1"},
        {"diabloiilordofdestruction", "d2-lod"},
        {"diabloii", "d2-classic"},
    };
    return table;
}

bool product_in_catalog(const std::vector<ProductDescriptor>& products, const std::string& id) {
    return known_product(products, id) != nullptr;
}

std::string status_text(const json& account) {
    if (account.contains("gameAccountStatus") && account.at("gameAccountStatus").is_string()) {
        return account.at("gameAccountStatus").get<std::string>();
    }
    return {};
}

void write_private_file(const std::filesystem::path& path, const std::string& content) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create dump file: " + path.string());
    output << content;
    output.close();
#if !defined(_WIN32)
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

std::string http_error_hint(const std::string& what) {
    if (what.find("status 401") != std::string::npos || what.find("status 403") != std::string::npos) {
        return "the account.battle.net session cookies were rejected (expired or incomplete); "
               "log in again in the browser and re-export cookies.txt";
    }
    return what;
}

void apply_records(std::vector<LibraryEntry>& entries, const std::vector<EntitlementRecord>& records,
                   const std::string& default_source) {
    std::map<std::string, EntitlementRecord> by_id;
    for (const auto& record : records) {
        // Prefer an owned record over a not-owned one for the same product.
        const auto existing = by_id.find(record.product_id);
        if (existing == by_id.end() || (!existing->second.owned && record.owned)) by_id[record.product_id] = record;
    }
    for (auto& entry : entries) {
        const auto it = by_id.find(entry.product_id);
        if (it == by_id.end()) {
            if (entry.ownership == OwnershipState::Manual) continue;
            entry.ownership = OwnershipState::Unknown;
            entry.source = default_source;
            entry.reason = "endpoint did not return this product; absence is not treated as not-owned";
        } else {
            entry.ownership = it->second.owned ? OwnershipState::Owned : OwnershipState::NotOwned;
            entry.source = it->second.source;
            entry.reason = it->second.reason;
        }
        entry.updated_at = now_seconds();
    }
    // Products recognised from the account but outside the curated catalog
    // (matched through the Ribbit summary) are appended so they show up too.
    for (const auto& [product_id, record] : by_id) {
        const bool present = std::any_of(entries.begin(), entries.end(),
                                         [&](const LibraryEntry& e) { return e.product_id == product_id; });
        if (present) continue;
        entries.push_back({product_id, "NGDP product " + product_id,
                           record.owned ? OwnershipState::Owned : OwnershipState::NotOwned,
                           record.source, record.reason, now_seconds()});
    }
}

} // namespace

std::string decode_title_id(std::int64_t title_id) {
    if (title_id <= 0 || title_id > 0xFFFFFFFFLL) return {};
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto byte = static_cast<unsigned char>((title_id >> shift) & 0xFF);
        if (byte == 0 && out.empty()) continue;  // codes shorter than four characters
        if (byte < 0x21 || byte > 0x7E) return {};
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

std::int64_t encode_title_id(const std::string& code) {
    if (code.empty() || code.size() > 4) return -1;
    std::int64_t value = 0;
    for (const unsigned char c : code) value = (value << 8) | c;
    return value;
}

std::int64_t expected_title_id(const std::string& product_id) {
    for (const auto& [title_id, products] : title_id_overrides()) {
        if (std::find(products.begin(), products.end(), product_id) != products.end()) return title_id;
    }
    // Program codes are upper-case except for the historical "WoW" spelling.
    if (product_id == "wow") return encode_title_id("WoW");
    std::string code = product_id;
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return encode_title_id(code);
}

AccountWebResult LibraryManager::parse_account_web(const std::string& games_and_subs_body,
                                                   const std::string& classic_games_body,
                                                   const std::vector<ProductDescriptor>& products) {
    AccountWebResult result;

    if (!games_and_subs_body.empty()) {
        const auto document = json::parse(games_and_subs_body, nullptr, true, true);
        if (!document.is_object() || !document.contains("gameAccounts") || !document.at("gameAccounts").is_array()) {
            throw std::runtime_error("games-and-subs response does not contain a gameAccounts array");
        }
        for (const auto& account : document.at("gameAccounts")) {
            if (!account.is_object()) continue;
            std::int64_t title_id = -1;
            if (account.contains("titleId") && account.at("titleId").is_number_integer()) {
                title_id = account.at("titleId").get<std::int64_t>();
            } else if (account.contains("gameAccountUniqueId") && account.at("gameAccountUniqueId").is_object() &&
                       account.at("gameAccountUniqueId").value("programId", json{}).is_number_integer()) {
                title_id = account.at("gameAccountUniqueId").at("programId").get<std::int64_t>();
            }
            const auto status = status_text(account);
            const auto mapped = products_for_title(title_id, products);
            if (mapped.empty()) {
                const auto code = decode_title_id(title_id);
                result.unknown_titles.push_back("titleId=" + std::to_string(title_id) +
                                                (code.empty() ? "" : " (\"" + code + "\")") +
                                                (status.empty() ? "" : " status=" + status));
                continue;
            }
            // Statuses observed on the account page. "Trial" is the only one that
            // does not represent a purchased or free license.
            const bool owned = status != "Trial";
            for (const auto& product_id : mapped) {
                if (!product_in_catalog(products, product_id)) continue;
                result.records.push_back({product_id, owned, true, "account-web",
                                          "games-and-subs titleId " + std::to_string(title_id) +
                                          " status " + (status.empty() ? "unknown" : status)});
            }
        }
    }

    if (!classic_games_body.empty()) {
        const auto document = json::parse(classic_games_body, nullptr, true, true);
        if (!document.is_object() || !document.contains("classicGames") || !document.at("classicGames").is_array()) {
            throw std::runtime_error("classic-games response does not contain a classicGames array");
        }
        for (const auto& game : document.at("classicGames")) {
            if (!game.is_object() || !game.contains("localizedGameName") || !game.at("localizedGameName").is_string()) continue;
            const auto name = game.at("localizedGameName").get<std::string>();
            const auto normalized = normalize_title(name);
            bool matched = false;
            for (const auto& [needle, product_id] : classic_title_products()) {
                if (normalized != needle) continue;
                matched = true;
                if (product_in_catalog(products, product_id)) {
                    result.records.push_back({product_id, true, true, "account-web",
                                              "classic-games entry \"" + name + "\""});
                }
                break;
            }
            if (!matched) result.unknown_titles.push_back("classic=\"" + name + "\"");
        }
    }

    // The account page enumerates every game account and every classic CD key
    // of the logged-in account. When a query succeeded, a curated product that
    // did not appear in it is therefore not owned (as opposed to unknown).
    std::set<std::string> seen;
    for (const auto& record : result.records) seen.insert(record.product_id);
    const bool have_accounts = !games_and_subs_body.empty();
    const bool have_classic = !classic_games_body.empty();
    for (const auto& product : products) {
        if (seen.count(product.id) != 0 || product.family == "ngdp") continue;
        bool classic_only = false;
        for (const auto& [needle, product_id] : classic_title_products()) {
            if (product_id == product.id) classic_only = true;
        }
        if (classic_only && !product.supported) {
            if (have_classic) {
                result.records.push_back({product.id, false, true, "account-web",
                                          "classic-games lists no CD key for this title"});
            }
            continue;
        }
        const auto title_id = expected_title_id(product.id);
        if (title_id < 0 || !have_accounts) continue;
        if (!classic_only && game_account_products().count(product.id) == 0) {
            // License-only title: games-and-subs never lists it. Leave it for
            // the purchase-history provider instead of guessing.
            continue;
        }
        if (classic_only && have_classic) {
            result.records.push_back({product.id, false, true, "account-web",
                                      "no game account (titleId " + std::to_string(title_id) + ") and no classic CD key"});
            continue;
        }
        if (classic_only) continue;
        result.records.push_back({product.id, false, true, "account-web",
                                  "games-and-subs lists no game account for titleId " + std::to_string(title_id) +
                                  " (\"" + decode_title_id(title_id) + "\")"});
    }
    return result;
}

LibraryManager::PurchaseResult LibraryManager::parse_purchases(
    const std::vector<std::string>& transaction_bodies, const std::vector<ProductDescriptor>& products) {
    PurchaseResult result;
    std::set<std::string> seen_titles;
    for (const auto& body : transaction_bodies) {
        if (body.empty()) continue;
        const auto document = json::parse(body, nullptr, false, true);
        if (document.is_discarded() || !document.is_object()) continue;
        std::vector<json> items;
        if (document.contains("purchases") && document.at("purchases").is_array()) {
            for (const auto& purchase : document.at("purchases")) items.push_back(purchase);
        }
        if (document.contains("giftClaims") && document.at("giftClaims").is_array()) {
            for (const auto& claim : document.at("giftClaims")) items.push_back(claim);
        }
        for (const auto& item : items) {
            if (!item.is_object()) continue;
            std::vector<std::string> titles;
            if (item.contains("productTitle") && item.at("productTitle").is_string()) {
                titles.push_back(item.at("productTitle").get<std::string>());
            }
            if (item.contains("lineItems") && item.at("lineItems").is_array()) {
                for (const auto& line : item.at("lineItems")) {
                    if (line.is_object() && line.contains("productTitle") && line.at("productTitle").is_string()) {
                        titles.push_back(line.at("productTitle").get<std::string>());
                    }
                }
            }
            std::string status;
            if (item.contains("localizedStatus") && item.at("localizedStatus").is_string()) status = item.at("localizedStatus").get<std::string>();
            else if (item.contains("status")) status = item.at("status").dump();
            const auto status_norm = normalize_title(status);
            // Refunds and chargebacks revoke the license; skip those orders.
            const bool revoked = status_norm.find("refund") != std::string::npos ||
                                 status_norm.find("chargeback") != std::string::npos ||
                                 status_norm.find("cancel") != std::string::npos ||
                                 status_norm.find("revers") != std::string::npos;
            for (const auto& title : titles) {
                if (title.empty() || !seen_titles.insert(title).second) continue;
                if (revoked) continue;
                const auto normalized = normalize_title(title);
                bool matched = false;
                for (const auto& [needle, product_ids] : purchase_title_products()) {
                    if (normalized.find(needle) == std::string::npos) continue;
                    for (const auto& product_id : product_ids) {
                        if (!product_in_catalog(products, product_id)) continue;
                        result.records.push_back({product_id, true, true, "account-purchases",
                                                  "purchase \"" + title + "\"" + (status.empty() ? "" : " status " + status)});
                        matched = true;
                    }
                    if (matched) break;
                }
                if (!matched) {
                    // Fall back to the catalog names themselves.
                    for (const auto& product : products) {
                        if (product.family == "ngdp") continue;
                        const auto name = normalize_title(product.name);
                        if (name.size() >= 6 && normalized.find(name) != std::string::npos) {
                            result.records.push_back({product.id, true, true, "account-purchases",
                                                      "purchase \"" + title + "\""});
                            matched = true;
                        }
                    }
                }
                if (!matched) result.unmatched_titles.push_back(title);
            }
        }
    }
    return result;
}

std::vector<LibraryManager::ShopCard> LibraryManager::parse_shop_cards(const std::string& html) {
    // The shop is a Next.js app; its server payload is pushed as JS string
    // literals via self.__next_f.push([1,"..."]). Un-escape them and look for
    // product cards (productPageName + slug).
    std::string flight;
    const std::string marker = "self.__next_f.push([1,\"";
    for (auto pos = html.find(marker); pos != std::string::npos; pos = html.find(marker, pos)) {
        pos += marker.size();
        while (pos < html.size()) {
            const char c = html[pos];
            if (c == '\\' && pos + 1 < html.size()) {
                const char n = html[pos + 1];
                if (n == 'n') flight.push_back('\n');
                else if (n == 't') flight.push_back('\t');
                else if (n == 'u' && pos + 5 < html.size()) { flight.push_back('?'); pos += 4; }
                else flight.push_back(n);
                pos += 2;
                continue;
            }
            if (c == '"') break;
            flight.push_back(c);
            ++pos;
        }
    }
    std::vector<ShopCard> cards;
    std::set<std::string> seen;
    const std::string key = "\"productPageName\":\"";
    const auto field = [&](const std::string& segment, const std::string& name) -> std::string {
        const auto k = "\"" + name + "\":\"";
        const auto at = segment.find(k);
        if (at == std::string::npos) return {};
        const auto end = segment.find('"', at + k.size());
        return end == std::string::npos ? std::string{} : segment.substr(at + k.size(), end - at - k.size());
    };
    for (auto pos = flight.find(key); pos != std::string::npos; pos = flight.find(key, pos + key.size())) {
        const auto begin = pos > 600 ? pos - 600 : 0;
        const auto segment = flight.substr(begin, 1400);
        ShopCard card{field(segment, "productPageName"), field(segment, "slug"), field(segment, "franchise"),
                      field(segment, "appGameCode")};
        while (!card.name.empty() && std::isspace(static_cast<unsigned char>(card.name.back()))) card.name.pop_back();
        if (card.name.empty() || !seen.insert(card.name).second) continue;
        cards.push_back(card);
    }
    return cards;
}

std::string LibraryManager::cookie_header_from_netscape_file(const std::filesystem::path& path,
                                                             const std::string& host) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read cookie file: " + path.string());
    std::string line;
    std::string header;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        bool http_only = false;
        if (line.rfind("#HttpOnly_", 0) == 0) { http_only = true; line = line.substr(10); }
        else if (line[0] == '#') continue;
        (void)http_only;
        std::vector<std::string> fields;
        std::string field;
        for (const auto ch : line) {
            if (ch == '\t') { fields.push_back(field); field.clear(); }
            else field.push_back(ch);
        }
        fields.push_back(field);
        if (fields.size() < 7) continue;
        auto domain = lower(fields[0]);
        if (!domain.empty() && domain[0] == '.') domain = domain.substr(1);
        const auto target = lower(host);
        const bool matches = target == domain ||
            (target.size() > domain.size() && target.compare(target.size() - domain.size() - 1, domain.size() + 1, "." + domain) == 0);
        if (!matches) continue;
        if (!header.empty()) header += "; ";
        header += fields[5] + "=" + fields[6];
    }
    if (header.empty()) throw std::runtime_error("no cookies for " + host + " found in " + path.string());
    return header;
}

int LibraryManager::scan(const Catalog& catalog, const AuthOptions& auth,
                         const std::filesystem::path& path, const LibraryScanOptions& options) {
    HttpClient http;
    const bool has_web_session =
        !options.web_session.cookie_header.empty() || !options.web_session.cookie_file.empty();
    std::string token;
    try {
        token = AuthManager::oauth_access_token(auth);
        if (!token.empty()) {
            const auto identity_response = http.get("https://oauth.battle.net/userinfo", {
                "Authorization: Bearer " + token,
                "Accept: application/json",
                "User-Agent: OpenBlizz/0.1",
            });
            const auto identity = json::parse(response_text(identity_response));
            if (!identity.is_object()) throw std::runtime_error("OAuth /userinfo returned an invalid identity response");
            if (!options.quiet) std::cout << "OAuth identity verified for library scan.\n";
        }
    } catch (const std::exception& error) {
        // The OAuth identity is informational when an account web session is
        // supplied; the web session is the actual ownership source.
        if (!has_web_session) throw;
        token.clear();
        if (!options.quiet) std::cerr << "Warning: OAuth identity unavailable (" << error.what() << "); continuing with the account web session.\n";
    }
    if (token.empty() && !has_web_session) {
        throw std::runtime_error("library scan requires an OAuth token or account web session cookies");
    }

    auto entries = catalog_entries(catalog, load(path));

    if (!options.web_session.cookie_header.empty() || !options.web_session.cookie_file.empty()) {
        const auto& session = options.web_session;
        if (session.host.find('/') != std::string::npos || session.host.empty()) {
            throw std::runtime_error("invalid account host: " + session.host);
        }
        const std::string base = "https://" + session.host;
        std::vector<std::string> headers{
            "Accept: application/json",
            "Referer: " + base + "/games",
        };
        std::string games_body;
        std::string classic_body;
        std::vector<std::string> transaction_bodies;
        const auto is_json = [](const HttpResponse& response) {
            return response.status == 200 && !response.body.empty() && response.body.front() != '<';
        };
        if (!options.quiet) std::cout << "Account web session provider: querying " << base << "/api/games-and-subs\n";

        if (!session.cookie_file.empty()) {
            // Browser-like session: the cookie engine keeps the exported cookies in
            // memory. If the account sub-site session expired, the site's own
            // login redirect chain renews it using the persistent battle.net
            // cookies (remember/login.key), exactly as a browser would.
            CookieSession browser(session.cookie_file.string());
            auto games = browser.get(base + "/api/games-and-subs", headers);
            if (!is_json(games)) {
                if (!options.quiet) std::cout << "Account session expired; renewing it through the site login flow.\n";
                const auto renew = browser.get(base + "/oauth2/authorization/account-settings");
                if (renew.status != 200) {
                    throw std::runtime_error("session renewal ended with HTTP " + std::to_string(renew.status) +
                                             "; log in again in the browser and re-export cookies.txt");
                }
                if (renew.effective_url.find("/login/") != std::string::npos) {
                    throw std::runtime_error(
                        "battle.net asked for a password: the persistent login cookies in this export were "
                        "rejected (they rotate on every use and expire). Log in again at "
                        "https://account.battle.net/games in the browser, export a fresh cookies.txt for the "
                        "whole battle.net domain, and run the scan with --cookie-jar so the rotated cookies are kept.");
                }
                games = browser.get(base + "/api/games-and-subs", headers);
            }
            if (!is_json(games)) {
                throw std::runtime_error(http_error_hint("status " + std::to_string(games.status) +
                                                         " from games-and-subs"));
            }
            games_body = response_text(games);
            const auto classic = browser.get(base + "/api/classic-games", headers);
            if (is_json(classic)) classic_body = response_text(classic);
            else std::cerr << "Warning: classic-games query returned HTTP " << classic.status << '\n';
            // Purchase history per Battle.net region (1 = Americas, 2 = Europe, 3 = Asia).
            if (!options.quiet) std::cout << "Account web session provider: querying " << base << "/api/transactions\n";
            for (const int region_id : {1, 2, 3}) {
                const auto tx = browser.get(base + "/api/transactions?regionId=" + std::to_string(region_id), headers);
                if (is_json(tx)) transaction_bodies.push_back(response_text(tx));
                else if (region_id == 1) std::cerr << "Warning: transactions query returned HTTP " << tx.status << '\n';
            }
            if (!session.cookie_jar.empty()) {
                browser.save_jar(session.cookie_jar.string());
                if (!options.quiet) std::cout << "Updated session cookies saved with owner-only permissions at " << session.cookie_jar << '\n';
            }
        } else {
            headers.push_back("Cookie: " + session.cookie_header);
            headers.push_back("User-Agent: Mozilla/5.0 (X11; Linux x86_64) OpenBlizz/0.1");
            try {
                games_body = response_text(http.get(base + "/api/games-and-subs", headers));
            } catch (const std::exception& error) {
                throw std::runtime_error(http_error_hint(error.what()));
            }
            if (games_body.empty() || games_body.front() == '<') {
                throw std::runtime_error("account.battle.net returned HTML instead of JSON; the session cookies are not authenticated");
            }
            try {
                classic_body = response_text(http.get(base + "/api/classic-games", headers));
            } catch (const std::exception& error) {
                std::cerr << "Warning: classic-games query failed: " << http_error_hint(error.what()) << '\n';
            }
            if (!classic_body.empty() && classic_body.front() == '<') classic_body.clear();
            for (const int region_id : {1, 2, 3}) {
                try {
                    auto body = response_text(http.get(base + "/api/transactions?regionId=" + std::to_string(region_id), headers));
                    if (!body.empty() && body.front() != '<') transaction_bodies.push_back(std::move(body));
                } catch (const std::exception&) {
                }
            }
        }

        if (!options.dump_path.empty()) {
            json dump;
            dump["games_and_subs"] = json::parse(games_body, nullptr, false, true);
            dump["classic_games"] = classic_body.empty() ? json{} : json::parse(classic_body, nullptr, false, true);
            dump["transactions"] = json::array();
            for (const auto& body : transaction_bodies) dump["transactions"].push_back(json::parse(body, nullptr, false, true));
            write_private_file(options.dump_path, dump.dump(2) + "\n");
            std::cout << "Raw account responses saved with owner-only permissions at " << options.dump_path << '\n';
        }

        std::vector<ProductDescriptor> products;
        try {
            products = catalog.all_products();
        } catch (const std::exception& error) {
            std::cerr << "Warning: could not load the Ribbit product summary (" << error.what()
                      << "); matching against the curated catalog only.\n";
            products = catalog.products();
        }
        auto web = parse_account_web(games_body, classic_body, products);
        const auto purchases = parse_purchases(transaction_bodies, products);
        // Purchases prove ownership; they take precedence over a not_owned
        // derived from games-and-subs absence.
        for (const auto& record : purchases.records) {
            web.records.erase(std::remove_if(web.records.begin(), web.records.end(),
                                             [&](const EntitlementRecord& r) { return r.product_id == record.product_id && !r.owned; }),
                              web.records.end());
            web.records.push_back(record);
        }
        apply_records(entries, web.records, "account-web");
        save(path, entries);
        if (options.quiet) return 0;
        std::cout << "Recognized account entries: " << web.records.size()
                  << " (purchases matched: " << purchases.records.size() << ")\n";
        if (!purchases.unmatched_titles.empty()) {
            std::cout << "Purchases not mapped to an installable product (DLC, services, third-party titles):\n";
            for (const auto& title : purchases.unmatched_titles) std::cout << "  " << title << '\n';
        }
        if (!web.unknown_titles.empty()) {
            std::cout << "Unmapped account entries (no NGDP product with this program code):\n";
            for (const auto& title : web.unknown_titles) std::cout << "  " << title << '\n';
        }
        print_entries(entries);
        return 0;
    }

    if (options.entitlement_url.empty()) {
        for (auto& entry : entries) {
            if (entry.ownership == OwnershipState::Manual) continue;
            entry.ownership = OwnershipState::Unknown;
            entry.source = "oauth-identity";
            entry.reason = "public OAuth identity does not expose owned products";
            entry.updated_at = now_seconds();
        }
        save(path, entries);
        std::cout << "No account session or entitlement endpoint configured; ownership remains unknown.\n"
                  << "Hint: export your account.battle.net cookies and run library scan --cookie-file cookies.txt\n";
        print_entries(entries);
        return 0;
    }

    require_safe_endpoint(options.entitlement_url);
    std::cout << "Experimental entitlement probe enabled for the configured endpoint.\n";
    const auto response = http.get(options.entitlement_url, {
        "Authorization: Bearer " + token,
        "Accept: application/json",
        "User-Agent: OpenBlizz/0.1",
    });
    const auto records = parse_entitlement_response(response_text(response), catalog.products());
    apply_records(entries, records, "experimental-oauth");
    save(path, entries);
    std::cout << "Recognized entitlement records: " << records.size() << '\n';
    print_entries(entries);
    return 0;
}

} // namespace openblizz
