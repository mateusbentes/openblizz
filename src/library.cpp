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

int LibraryManager::scan(const Catalog& catalog, const AuthOptions& auth,
                         const std::filesystem::path& path, const std::string& entitlement_url) {
    const auto token = AuthManager::oauth_access_token(auth);
    if (token.empty()) throw std::runtime_error("library scan requires an OAuth token; run openblizz login first");

    HttpClient http;
    const auto identity_response = http.get("https://oauth.battle.net/userinfo", {
        "Authorization: Bearer " + token,
        "Accept: application/json",
        "User-Agent: OpenBlizz/0.1",
    });
    const auto identity = json::parse(response_text(identity_response));
    if (!identity.is_object()) throw std::runtime_error("OAuth /userinfo returned an invalid identity response");
    std::cout << "OAuth identity verified for library scan.\n";

    auto entries = catalog_entries(catalog, load(path));
    if (entitlement_url.empty()) {
        for (auto& entry : entries) {
            if (entry.ownership == OwnershipState::Manual) continue;
            entry.ownership = OwnershipState::Unknown;
            entry.source = "oauth-identity";
            entry.reason = "public OAuth identity does not expose owned products";
            entry.updated_at = now_seconds();
        }
        save(path, entries);
        std::cout << "No entitlement endpoint configured; ownership remains unknown.\n";
        print_entries(entries);
        return 0;
    }

    require_safe_endpoint(entitlement_url);
    std::cout << "Experimental entitlement probe enabled for the configured endpoint.\n";
    const auto response = http.get(entitlement_url, {
        "Authorization: Bearer " + token,
        "Accept: application/json",
        "User-Agent: OpenBlizz/0.1",
    });
    const auto records = parse_entitlement_response(response_text(response), catalog.products());
    std::map<std::string, EntitlementRecord> by_id;
    for (const auto& record : records) by_id[record.product_id] = record;

    for (auto& entry : entries) {
        const auto it = by_id.find(entry.product_id);
        if (it == by_id.end()) {
            if (entry.ownership == OwnershipState::Manual) continue;
            entry.ownership = OwnershipState::Unknown;
            entry.source = "experimental-oauth";
            entry.reason = "endpoint did not return this product; absence is not treated as not-owned";
        } else {
            entry.ownership = it->second.owned ? OwnershipState::Owned : OwnershipState::NotOwned;
            entry.source = it->second.source;
            entry.reason = it->second.reason;
        }
        entry.updated_at = now_seconds();
    }
    save(path, entries);
    std::cout << "Recognized entitlement records: " << records.size() << '\n';
    print_entries(entries);
    return 0;
}

} // namespace openblizz
