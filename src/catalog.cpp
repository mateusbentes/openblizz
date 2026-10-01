#include "openblizz/catalog.hpp"
#include "openblizz/hash.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
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

} // namespace

Catalog::Catalog(HttpClient& http) : http_(http) {}

std::vector<ProductDescriptor> Catalog::products() const {
    return {
        {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
        {"w3-legacy-tft", "Warcraft III: legacy/TFT", "warcraft", "w3", true},
        {"w2r", "Warcraft II: Remastered", "warcraft", "w2r", true},
        {"w2bn", "Warcraft II: Battle.net Edition", "warcraft", "w2bn", true},
        {"w1r", "Warcraft I: Remastered", "warcraft", "w1r", true},
        {"war1", "Warcraft I: legacy", "warcraft", "war1", true},
        {"s1", "StarCraft: Remastered", "starcraft", "s1", true},
    };
}

VersionInfo Catalog::version(const std::string& product, const std::string& region) const {
    const auto url = "https://" + region + ".version.battle.net/v2/products/" + product + "/versions";
    const auto response = http_.get(url);
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
        result.product = product;
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
    const auto url = "https://" + region + ".version.battle.net/v2/products/" + product + "/cdns";
    const auto response = http_.get(url);
    std::vector<std::string> header;
    const auto rows = parse_bpsv(body_text(response), &header);
    const auto region_index = field_index(header, "Name");
    const auto path_index = field_index(header, "Path");
    const auto hosts_index = field_index(header, "Hosts");
    const auto servers_index = field_index(header, "Servers");
    const auto config_index = field_index(header, "ConfigPath");
    std::vector<CdnInfo> result;
    for (const auto& row : rows) {
        CdnInfo cdn;
        cdn.product = product;
        cdn.region = region;
        cdn.path = cell(row, path_index);
        cdn.hosts = split_hosts(cell(row, hosts_index));
        cdn.servers = split_hosts(cell(row, servers_index));
        cdn.config_path = cell(row, config_index);
        if (!cdn.path.empty() && !cdn.hosts.empty()) result.push_back(std::move(cdn));
        (void)region_index;
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
            return http_.get(object_url(candidate, kind, hash, suffix));
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
            return http_.get_range(object_url(candidate, "data", hash), offset, size).body;
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
    return *it;
}

} // namespace openblizz
