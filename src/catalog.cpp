#include "openblizz/catalog.hpp"
#include "openblizz/hash.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <set>
#include <stdexcept>

namespace openblizz {
namespace {

std::string body_text(const HttpResponse& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

std::vector<std::string> bpsv_header(const std::string& line) {
    std::vector<std::string> fields;
    for (auto field : split(line, '|')) {
        const auto marker = field.find('!');
        if (marker != std::string::npos) field = field.substr(0, marker);
        fields.push_back(field);
    }
    return fields;
}

std::vector<std::vector<std::string>> parse_bpsv(const std::string& text,
                                                  std::vector<std::string>* header_out = nullptr) {
    std::istringstream input(text);
    std::string line;
    std::vector<std::string> fields;
    std::vector<std::vector<std::string>> rows;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("##", 0) == 0) continue;
        if (fields.empty()) {
            fields = bpsv_header(line);
            if (header_out != nullptr) *header_out = fields;
            continue;
        }
        rows.push_back(split(line, '|'));
    }
    return rows;
}

std::size_t field_index(const std::vector<std::string>& header, const std::string& name) {
    const auto it = std::find(header.begin(), header.end(), name);
    return it == header.end() ? header.size() : static_cast<std::size_t>(std::distance(header.begin(), it));
}

std::string cell(const std::vector<std::string>& row, std::size_t index) {
    return index < row.size() ? row[index] : std::string{};
}

std::vector<std::string> split_hosts(const std::string& value) {
    auto hosts = split(value, ' ');
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [](const std::string& value) { return value.empty(); }), hosts.end());
    return hosts;
}

const char* const kCodReason =
    "Call of Duty remains metadata-only: its complete content/key/authentication/runtime combination "
    "has not been validated. Public KeyRing support does not establish a working installation "
    "or anti-cheat compatibility; this change does not enable its downloader.";

} // namespace

Catalog::Catalog(HttpClient& http, std::vector<LibraryEntry> library,
                 std::string version_base, Transport transport)
    : http_(http), library_(std::move(library)), version_base_(std::move(version_base)),
      transport_(std::move(transport)) {}

bool Catalog::valid_product_code(const std::string& code) {
    return !code.empty() && code.size() <= 64 &&
           std::all_of(code.begin(), code.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

ProductDescriptor Catalog::resolve_product(const std::string& id) const {
    const auto curated = products();
    const auto known = std::find_if(curated.begin(), curated.end(), [&](const auto& p) { return p.id == id; });
    if (known != curated.end()) {
        auto product = find_product(curated, id);
        // agent_product historically also names account/launcher aliases (w3
        // for Legacy/TFT). NGDP uses the curated id, including w3-legacy-tft.
        product.agent_product = product.id;
        return product;
    }
    const auto entry = std::find_if(library_.begin(), library_.end(), [&](const auto& e) { return e.product_id == id; });
    if (entry == library_.end()) throw std::runtime_error("unsupported catalog product: " + id);
    if (entry->ownership != OwnershipState::Owned ||
        (entry->source != "account-purchases" && entry->source != "account-web")) {
        throw std::runtime_error("dynamic product " + id + " needs account-derived ownership; run `openblizz library scan`");
    }
    const auto& code = entry->ngdp_product;
    if (code.empty()) {
        throw std::runtime_error("dynamic product " + id + " has no explicit NGDP appGameCode; ownership-only. "
                                 "Run `openblizz library scan` to refresh its metadata");
    }
    if (!valid_product_code(code)) throw std::runtime_error("invalid NGDP product code for " + id + ": " + code);
    const auto target = std::find_if(curated.begin(), curated.end(), [&](const auto& p) { return p.id == code; });
    if (target != curated.end()) (void)find_product(curated, code);  // do not bypass a curated block through an alias
    return {id, entry->name.empty() ? id : entry->name,
            entry->family.empty() ? "ngdp" : entry->family, code, true};
}

HttpResponse Catalog::request(const std::string& url, std::uint64_t offset, std::uint32_t size) const {
    if (transport_) return transport_(url, offset, size);
    return size == 0 ? http_.get(url) : http_.get_range(url, offset, size);
}

std::string Catalog::metadata_base(const std::string& region) const {
    if (!valid_product_code(region)) throw std::runtime_error("invalid NGDP region: " + region);
    return version_base_.empty() ? "https://" + region + ".version.battle.net" : version_base_;
}

std::vector<ProductDescriptor> Catalog::products() const {
    return {
        {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
        {"w3-legacy-tft", "Warcraft III: legacy/TFT", "warcraft", "w3", true},
        {"w2r", "Warcraft II: Remastered", "warcraft", "w2r", true},
        {"w2bn", "Warcraft II: Battle.net Edition", "warcraft", "w2bn", true},
        {"w1r", "Warcraft I: Remastered", "warcraft", "w1r", true},
        {"war1", "Warcraft I: legacy", "warcraft", "war1", true},
        {"s1", "StarCraft: Remastered", "starcraft", "s1", true},
        {"s2", "StarCraft II", "starcraft", "s2", true},
        {"wow", "World of Warcraft", "warcraft", "wow", true},
        // Mobile-origin titles: NGDP ships their Windows build only; the phone
        // builds come from the app stores and are out of reach by design.
        {"anbs", "Diablo Immortal (PC build)", "diablo", "anbs", true},
        {"rtro", "Blizzard Arcade Collection", "arcade", "rtro", true},
        {"wow_classic", "World of Warcraft Classic", "warcraft", "wow_classic", true},
        {"gryphon", "Warcraft Rumble (PC build)", "warcraft", "gryphon", true},
        {"osi", "Diablo II: Resurrected", "diablo", "osi", true},
        {"d3", "Diablo III", "diablo", "d3", true},
        {"fenris", "Diablo IV", "diablo", "fenris", true},
        {"pro", "Overwatch", "overwatch", "pro", true},
        {"hsb", "Hearthstone (PC build)", "hearthstone", "hsb", true},
        {"hero", "Heroes of the Storm", "heroes", "hero", true},
        {"lyra", "The Witcher 3: Wild Hunt Remastered", "thirdparty", "lyra", true},
        // Public product config identifies wlby as CrashBandicoot4.exe with
        // containerless NGDP, not Call of Duty. Download support is experimental;
        // its online/runtime requirements are not bypassed by this installer.
        {"wlby", "Crash Bandicoot 4: It's About Time", "thirdparty", "wlby", true},
        // Classic CD-key titles still listed by the account page. They are not
        // distributed through NGDP, so they are catalogued for ownership only.
        {"d2-classic", "Diablo II (classic, legacy installer)", "diablo", "", false},
        {"d2-lod", "Diablo II: Lord of Destruction (classic, legacy installer)", "diablo", "", false},
        // Call of Duty titles expose public Ribbit/NGDP metadata, but the
        // full content/key/runtime combination has not been validated.
        // Public KeyRing decoding alone does not establish installation or
        // anti-cheat compatibility. They are catalogued for ownership
        // and metadata (`versions`, `cdns`) only.
        {"odin", "Call of Duty: Modern Warfare (2019) / Warzone", "callofduty", "odin", false, kCodReason},
        {"zeus", "Call of Duty: Black Ops Cold War", "callofduty", "zeus", false, kCodReason},
        {"fore", "Call of Duty: Vanguard", "callofduty", "fore", false, kCodReason},
        {"lazr", "Call of Duty: Modern Warfare II (2022)", "callofduty", "lazr", false, kCodReason},
        {"nina", "Call of Duty (NGDP code nina)", "callofduty", "nina", false, kCodReason},
        {"auks", "Call of Duty (NGDP code auks)", "callofduty", "auks", false, kCodReason},
    };
}

std::vector<Catalog::SummaryEntry> Catalog::summary(const std::string& region) const {
    const auto url = metadata_base(region) + "/v2/summary";
    const auto response = request(url);
    std::vector<std::string> header;
    const auto rows = parse_bpsv(body_text(response), &header);
    const auto product_index = field_index(header, "Product");
    const auto seqn_index = field_index(header, "Seqn");
    const auto flags_index = field_index(header, "Flags");
    std::vector<SummaryEntry> result;
    for (const auto& row : rows) {
        SummaryEntry entry{cell(row, product_index), cell(row, seqn_index), cell(row, flags_index)};
        if (!entry.product.empty()) result.push_back(std::move(entry));
    }
    if (result.empty()) throw std::runtime_error("Ribbit summary returned no products");
    return result;
}

std::vector<ProductDescriptor> Catalog::all_products(const std::string& region) const {
    auto result = products();
    std::set<std::string> known;
    for (const auto& product : result) known.insert(product.id);
    for (const auto& entry : summary(region)) {
        if (!entry.flags.empty()) continue;  // only "versions" rows name a product
        if (!known.insert(entry.product).second) continue;
        result.push_back({entry.product, "NGDP product " + entry.product, "ngdp", entry.product, true});
    }
    return result;
}

VersionInfo Catalog::version(const std::string& product, const std::string& region) const {
    const auto code = product.rfind("thirdparty-", 0) == 0 ? resolve_product(product).agent_product : product;
    if (!valid_product_code(code)) throw std::runtime_error("invalid NGDP product code: " + code);
    const auto url = metadata_base(region) + "/v2/products/" + code + "/versions";
    const auto response = request(url);
    std::vector<std::string> header;
    const auto rows = parse_bpsv(body_text(response), &header);
    const auto region_index = field_index(header, "Region");
    const auto build_index = field_index(header, "BuildConfig");
    const auto cdn_index = field_index(header, "CDNConfig");
    const auto keyring_index = field_index(header, "KeyRing");
    const auto build_id_index = field_index(header, "BuildId");
    const auto version_index = field_index(header, "VersionsName");
    const auto product_index = field_index(header, "ProductConfig");
    for (const auto& row : rows) {
        if (region_index < row.size() && row[region_index] != region) continue;
        VersionInfo result;
        result.product = code;
        result.region = region;
        result.build_config = cell(row, build_index);
        result.cdn_config = cell(row, cdn_index);
        result.keyring = cell(row, keyring_index);
        result.build_id = cell(row, build_id_index);
        result.version_name = cell(row, version_index);
        result.product_config = cell(row, product_index);
        if (result.build_config.empty() || result.cdn_config.empty()) {
            throw std::runtime_error("NGDP returned an incomplete version row for " + product);
        }
        return result;
    }
    throw std::runtime_error("product " + product + " has no version in region " + region);
}

std::vector<CdnInfo> Catalog::cdns(const std::string& product, const std::string& region) const {
    const auto code = product.rfind("thirdparty-", 0) == 0 ? resolve_product(product).agent_product : product;
    if (!valid_product_code(code)) throw std::runtime_error("invalid NGDP product code: " + code);
    const auto url = metadata_base(region) + "/v2/products/" + code + "/cdns";
    const auto response = request(url);
    std::vector<std::string> header;
    const auto rows = parse_bpsv(body_text(response), &header);
    const auto region_index = field_index(header, "Name");
    const auto path_index = field_index(header, "Path");
    const auto hosts_index = field_index(header, "Hosts");
    const auto servers_index = field_index(header, "Servers");
    const auto config_index = field_index(header, "ConfigPath");
    std::vector<CdnInfo> result;
    for (const auto& row : rows) {
        if (region_index < row.size() && row[region_index] != region) continue;
        CdnInfo cdn;
        cdn.product = code;
        cdn.region = region;
        cdn.path = cell(row, path_index);
        cdn.hosts = split_hosts(cell(row, hosts_index));
        cdn.servers = split_hosts(cell(row, servers_index));
        cdn.config_path = cell(row, config_index);
        if (!cdn.path.empty() && !cdn.hosts.empty()) result.push_back(std::move(cdn));
    }
    if (result.empty()) throw std::runtime_error("NGDP returned no usable CDN for " + product);
    return result;
}

CdnInfo Catalog::select_cdn(const std::string& product, const std::string& region) const {
    const auto entries = cdns(product, region);
    return entries.front();
}

std::string Catalog::object_url(const CdnInfo& cdn, const std::string& kind,
                                const std::string& hash, const std::string& suffix) const {
    if (!is_hex_hash(hash, 16)) throw std::runtime_error("invalid TACT object hash: " + hash);
    return "https://" + cdn.hosts.front() + "/" + cdn.path + "/" + kind + "/" +
           hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash + suffix;
}

HttpResponse Catalog::get_with_cdn_failover(const CdnInfo& cdn, const std::string& kind,
                                            const std::string& hash, const std::string& suffix) const {
    std::string last_error;
    for (const auto& host : cdn.hosts) {
        CdnInfo candidate = cdn;
        candidate.hosts = {host};
        try {
            return request(object_url(candidate, kind, hash, suffix));
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    throw std::runtime_error("all CDN hosts failed for " + hash + ": " + last_error);
}

std::vector<std::uint8_t> Catalog::fetch_config(const CdnInfo& cdn, const std::string& hash) const {
    const auto response = get_with_cdn_failover(cdn, "config", hash);
    if (md5_hex(response.body) != hash) throw std::runtime_error("config hash mismatch for " + hash);
    return response.body;
}

std::vector<std::uint8_t> Catalog::fetch_data(const CdnInfo& cdn, const std::string& hash) const {
    const auto response = get_with_cdn_failover(cdn, "data", hash);
    // TACT EKeys identify the encoded object according to the encoding table;
    // they are not the MD5 of the complete BLTE wrapper. BLTE chunk checksums
    // and the install entry's decoded CKey provide the integrity checks.
    return response.body;
}

std::vector<std::uint8_t> Catalog::fetch_decoded_data(const CdnInfo& cdn, const std::string& hash) const {
    return BlteDecoder::decode(fetch_data(cdn, hash));
}

std::vector<std::uint8_t> Catalog::fetch_decoded_data(const CdnInfo& cdn, const std::string& hash,
                                                     const KeyRing& keyring) const {
    return BlteDecoder::decode(fetch_data(cdn, hash), keyring);
}

std::vector<std::uint8_t> Catalog::fetch_archive_index(const CdnInfo& cdn,
                                                       const std::string& hash) const {
    return get_with_cdn_failover(cdn, "data", hash, ".index").body;
}

std::vector<std::uint8_t> Catalog::fetch_archive_range(const CdnInfo& cdn,
                                                       const std::string& hash,
                                                       std::uint64_t offset,
                                                       std::uint32_t size) const {
    std::string last_error;
    for (const auto& host : cdn.hosts) {
        CdnInfo candidate = cdn;
        candidate.hosts = {host};
        try {
            return request(object_url(candidate, "data", hash), offset, size).body;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    throw std::runtime_error("all CDN hosts failed for archive " + hash + ": " + last_error);
}

const ProductDescriptor& find_product(const std::vector<ProductDescriptor>& products,
                                      const std::string& id) {
    const auto it = std::find_if(products.begin(), products.end(), [&](const auto& product) {
        return product.id == id;
    });
    if (it == products.end()) throw std::runtime_error("unsupported catalog product: " + id);
    if (!it->supported) {
        if (!it->unsupported_reason.empty()) throw std::runtime_error(it->name + ": " + it->unsupported_reason);
        throw std::runtime_error(it->name + " is not distributed through NGDP; OpenBlizz can only report its ownership. "
                                 "Use the legacy installer from the Battle.net account page.");
    }
    return *it;
}

} // namespace openblizz
