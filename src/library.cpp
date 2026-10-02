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
    }
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

int LibraryManager::list(const Catalog& catalog, const std::filesystem::path& path) {
    print_entries(catalog_entries(catalog, load(path)));
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
const std::map<std::int64_t, std::vector<std::string>>& title_id_products() {
    static const std::map<std::int64_t, std::vector<std::string>> table{
        {21297, {"s1"}},
        {22323, {"w3", "w3-legacy-tft"}},
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
}

} // namespace

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
            const auto mapped = title_id_products().find(title_id);
            if (mapped == title_id_products().end()) {
                result.unknown_titles.push_back("titleId=" + std::to_string(title_id) +
                                                (status.empty() ? "" : " status=" + status));
                continue;
            }
            // Statuses observed on the account page. "Trial" is the only one that
            // does not represent a purchased or free license.
            const bool owned = status != "Trial";
            for (const auto& product_id : mapped->second) {
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
    return result;
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
            std::cout << "OAuth identity verified for library scan.\n";
        }
    } catch (const std::exception& error) {
        // The OAuth identity is informational when an account web session is
        // supplied; the web session is the actual ownership source.
        if (!has_web_session) throw;
        token.clear();
        std::cerr << "Warning: OAuth identity unavailable (" << error.what() << "); continuing with the account web session.\n";
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
        const auto is_json = [](const HttpResponse& response) {
            return response.status == 200 && !response.body.empty() && response.body.front() != '<';
        };
        std::cout << "Account web session provider: querying " << base << "/api/games-and-subs\n";

        if (!session.cookie_file.empty()) {
            // Browser-like session: the cookie engine keeps the exported cookies in
            // memory. If the account sub-site session expired, the site's own
            // login redirect chain renews it using the persistent battle.net
            // cookies (remember/login.key), exactly as a browser would.
            CookieSession browser(session.cookie_file.string());
            auto games = browser.get(base + "/api/games-and-subs", headers);
            if (!is_json(games)) {
                std::cout << "Account session expired; renewing it through the site login flow.\n";
                const auto renew = browser.get(base + "/oauth2/authorization/account-settings");
                if (renew.status != 200) {
                    throw std::runtime_error("session renewal ended with HTTP " + std::to_string(renew.status) +
                                             "; log in again in the browser and re-export cookies.txt");
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
        }

        if (!options.dump_path.empty()) {
            json dump;
            dump["games_and_subs"] = json::parse(games_body, nullptr, false, true);
            dump["classic_games"] = classic_body.empty() ? json{} : json::parse(classic_body, nullptr, false, true);
            write_private_file(options.dump_path, dump.dump(2) + "\n");
            std::cout << "Raw account responses saved with owner-only permissions at " << options.dump_path << '\n';
        }

        const auto web = parse_account_web(games_body, classic_body, catalog.products());
        apply_records(entries, web.records, "account-web");
        save(path, entries);
        std::cout << "Recognized account entries: " << web.records.size() << '\n';
        if (!web.unknown_titles.empty()) {
            std::cout << "Unmapped account entries (not in the OpenBlizz catalog yet):\n";
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
