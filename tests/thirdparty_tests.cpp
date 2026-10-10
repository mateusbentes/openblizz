// Keep assert() active even in Release builds: these tests intentionally use it
// for both validation and executable specifications.
#undef NDEBUG

#include "openblizz/catalog.hpp"
#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"
#include "openblizz/http.hpp"
#include "openblizz/installer.hpp"
#include "openblizz/types.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_u16(Bytes& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u32(Bytes& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 24));
    data.push_back(static_cast<std::uint8_t>(value >> 16));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u40_at(Bytes& data, const std::size_t offset, std::uint64_t value) {
    assert(offset + 5 <= data.size());
    data[offset] = static_cast<std::uint8_t>(value >> 32);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 24);
    data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    data[offset + 3] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 4] = static_cast<std::uint8_t>(value);
}

Bytes as_bytes(const std::string& value) {
    return Bytes(value.begin(), value.end());
}

Bytes blte_single(const Bytes& decoded) {
    Bytes encoded{'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N'};
    encoded.insert(encoded.end(), decoded.begin(), decoded.end());
    return encoded;
}

std::string object_url(const std::string& kind, const std::string& hash,
                       const std::string& suffix = {}) {
    return "https://cdn.test/tpr/exg/" + kind + "/" + hash.substr(0, 2) + "/" +
           hash.substr(2, 2) + "/" + hash + suffix;
}

struct OfflineTransport {
    std::map<std::string, Bytes> responses;
    std::map<std::string, std::size_t> calls;
    std::vector<std::string> unexpected;
    mutable std::mutex mutex;

    [[nodiscard]] openblizz::Catalog::Transport function() {
        return [this](const std::string& url, const std::uint64_t offset,
                      const std::uint32_t size) {
            std::lock_guard lock(mutex);
            ++calls[url];
            const auto it = responses.find(url);
            if (it == responses.end()) {
                unexpected.push_back(url);
                throw std::runtime_error("unexpected offline URL: " + url);
            }

            openblizz::HttpResponse response;
            response.status = 200;
            response.effective_url = url;
            if (size == 0) {
                response.body = it->second;
                return response;
            }
            if (offset > it->second.size() || size > it->second.size() - offset) {
                unexpected.push_back(url + " [invalid range]");
                throw std::runtime_error("invalid offline range: " + url);
            }
            response.body.assign(it->second.begin() + static_cast<std::ptrdiff_t>(offset),
                                 it->second.begin() + static_cast<std::ptrdiff_t>(offset + size));
            return response;
        };
    }

    [[nodiscard]] std::size_t count(const std::string& url) const {
        std::lock_guard lock(mutex);
        const auto it = calls.find(url);
        return it == calls.end() ? 0 : it->second;
    }

    [[nodiscard]] std::size_t total() const {
        std::lock_guard lock(mutex);
        std::size_t result = 0;
        for (const auto& [url, count] : calls) {
            (void)url;
            result += count;
        }
        return result;
    }
};

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("openblizz-thirdparty-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct Fixture {
    Bytes payload{0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65, 0x0a};
    std::string content_key;
    Bytes game_object;
    std::string game_encoding_key;
    Bytes encoding_manifest;
    Bytes encoding_object;
    std::string encoding_content_key;
    std::string encoding_key;
    Bytes normal_install;
    std::string normal_install_content_key;
    std::string normal_install_key;
    Bytes empty_install;
    std::string empty_install_content_key;
    std::string empty_install_key;
    Bytes empty_selected_install;
    std::string empty_selected_content_key;
    std::string empty_selected_key;
    Bytes conflicting_install;
    std::string conflicting_install_content_key;
    std::string conflicting_install_key;
    std::string cdn_config;
    bool encrypted = false;

    explicit Fixture(bool encrypted_content = false) : encrypted(encrypted_content) {
        content_key = openblizz::md5_hex(payload);
        game_object = blte_single(payload);
        if (encrypted) {
            game_object = openblizz::hex_to_bytes(
                "424c5445000000240f00000100000029000000182cb0e1a35a4c7b5d6d66c0e6b6a8b35b"
                "45080011223344556677040102030453f782272bfd9eb7dee502470f4f27c7ec6886fc23e674fa3524");
            const auto keys = openblizz::parse_keyring(openblizz::parse_config(
                "key-0011223344556677 = 00112233445566778899aabbccddeeff\n"));
            payload = openblizz::BlteDecoder::decode(game_object, keys);
            content_key = openblizz::md5_hex(payload);
            // A multi-chunk BLTE EKey hashes its header, not the entire object.
            game_encoding_key = openblizz::md5_hex(Bytes(game_object.begin(), game_object.begin() + 36));
        } else {
            game_encoding_key = openblizz::md5_hex(game_object);
        }

        encoding_manifest = make_encoding(content_key, game_encoding_key, payload.size());
        encoding_object = blte_single(encoding_manifest);
        encoding_content_key = openblizz::md5_hex(encoding_manifest);
        encoding_key = openblizz::md5_hex(encoding_object);

        normal_install = make_install({{"game.bin", content_key}}, false, payload.size());
        normal_install_content_key = openblizz::md5_hex(normal_install);
        const auto normal_object = blte_single(normal_install);
        normal_install_key = openblizz::md5_hex(normal_object);

        empty_install = make_install({}, false, payload.size());
        empty_install_content_key = openblizz::md5_hex(empty_install);
        const auto empty_object = blte_single(empty_install);
        empty_install_key = openblizz::md5_hex(empty_object);

        empty_selected_install = make_install({{"game.bin", content_key}}, true, payload.size());
        empty_selected_content_key = openblizz::md5_hex(empty_selected_install);
        const auto empty_selected_object = blte_single(empty_selected_install);
        empty_selected_key = openblizz::md5_hex(empty_selected_object);

        conflicting_install = make_install({{"game.bin", content_key},
                                            {"./game.bin", openblizz::md5_hex(as_bytes("other!!!"))}}, false, payload.size());
        conflicting_install_content_key = openblizz::md5_hex(conflicting_install);
        const auto conflicting_object = blte_single(conflicting_install);
        conflicting_install_key = openblizz::md5_hex(conflicting_object);

        cdn_config = "archives =\n";
    }

    static Bytes make_install(const std::vector<std::pair<std::string, std::string>>& entries,
                              const bool exclude_entries, const std::size_t decoded_size = 8) {
        Bytes result{'I', 'N', 1, 16};
        put_u16(result, exclude_entries ? 1 : 0);
        put_u32(result, static_cast<std::uint32_t>(entries.size()));
        if (exclude_entries) {
            const std::string tag = "enUS";
            result.insert(result.end(), tag.begin(), tag.end());
            result.push_back(0);
            put_u16(result, 0);
            result.push_back(0); // One entry, selected bitmap is deliberately empty.
        }
        for (const auto& [path, key] : entries) {
            result.insert(result.end(), path.begin(), path.end());
            result.push_back(0);
            const auto key_bytes = openblizz::hex_to_bytes(key);
            result.insert(result.end(), key_bytes.begin(), key_bytes.end());
            put_u32(result, static_cast<std::uint32_t>(decoded_size));
        }
        return result;
    }

    static Bytes make_encoding(const std::string& ckey, const std::string& ekey,
                               const std::uint64_t decoded_size) {
        // This is the same minimal EN layout used by core_tests: a 22-byte
        // header, 32-byte CKey index, one 1024-byte CKey page, 32-byte EKey
        // index and one 1024-byte EKey page.
        Bytes result{'E', 'N', 1, 16, 16};
        put_u16(result, 1); // CKey page size in KiB.
        put_u16(result, 1); // EKey page size in KiB.
        put_u32(result, 1); // One CKey index page.
        put_u32(result, 1); // One EKey index page.
        result.push_back(0); // Reserved/flags byte in the 22-byte header.
        put_u32(result, 0); // No ESpec block.
        result.resize(22 + 32 + 1024 + 32 + 1024, 0);

        auto offset = static_cast<std::size_t>(22 + 32);
        result[offset++] = 2; // Two equivalent EKeys, exercising mapping fallback order.
        put_u40_at(result, offset, decoded_size);
        offset += 5;
        const auto ckey_bytes = openblizz::hex_to_bytes(ckey);
        std::copy(ckey_bytes.begin(), ckey_bytes.end(), result.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += ckey_bytes.size();
        const auto ekey_bytes = openblizz::hex_to_bytes(ekey);
        std::copy(ekey_bytes.begin(), ekey_bytes.end(), result.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += ekey_bytes.size();
        const auto fallback = openblizz::hex_to_bytes("00112233445566778899aabbccddeeff");
        std::copy(fallback.begin(), fallback.end(), result.begin() + static_cast<std::ptrdiff_t>(offset));
        return result;
    }

    void add_response(OfflineTransport& transport, const std::string& url, const Bytes& body) const {
        transport.responses[url] = body;
    }

    void configure(OfflineTransport& transport, const std::string& install_kind,
                   const bool bad_install_hash = false, const bool bad_encoding_hash = false) const {
        const Bytes* install = &normal_install;
        std::string install_key = normal_install_key;
        std::string install_content = normal_install_content_key;
        if (install_kind == "empty") {
            install = &empty_install;
            install_key = empty_install_key;
            install_content = empty_install_content_key;
        } else if (install_kind == "empty-selected") {
            install = &empty_selected_install;
            install_key = empty_selected_key;
            install_content = empty_selected_content_key;
        } else if (install_kind == "conflict") {
            install = &conflicting_install;
            install_key = conflicting_install_key;
            install_content = conflicting_install_content_key;
        }

        const auto wrong_key = std::string(32, '0');
        const auto configured_install_content = bad_install_hash ? wrong_key : install_content;
        const auto configured_encoding_content = bad_encoding_hash ? wrong_key : encoding_content_key;
        const auto build_config_text =
            "install = " + configured_install_content + " " + install_key + "\n" +
            "install-size = 0 " + std::to_string(install->size()) + "\n" +
            "encoding = " + configured_encoding_content + " " + encoding_key + "\n" +
            "encoding-size = 0 " + std::to_string(encoding_object.size()) + "\n";
        const auto build_config = as_bytes(build_config_text);
        const auto build_hash = openblizz::md5_hex(build_config);
        const auto cdn_bytes = as_bytes(cdn_config);
        const auto cdn_hash = openblizz::md5_hex(cdn_bytes);

        const auto key_config = as_bytes("key-0011223344556677 = 00112233445566778899aabbccddeeff\n");
        const auto key_hash = encrypted ? openblizz::md5_hex(key_config) : std::string{};
        if (encrypted) add_response(transport, object_url("config", key_hash), key_config);
        const auto versions_url = "https://metadata.test/v2/products/exg/versions";
        add_response(transport, versions_url, as_bytes(
            std::string{"Region!STRING:0|BuildConfig!HEX:16|CDNConfig!HEX:16|KeyRing!HEX:16|"} +
            "BuildId!STRING:0|VersionsName!STRING:0|ProductConfig!HEX:16\n" +
            "us|" + build_hash + "|" + cdn_hash + "|" + key_hash + "||1.0|\n"));
        add_response(transport, "https://metadata.test/v2/products/exg/cdns", as_bytes(
            "Name!STRING:0|Path!STRING:0|Hosts!STRING:0|Servers!STRING:0|ConfigPath!STRING:0\n"
            "us|tpr/exg|cdn.test||\n"));
        add_response(transport, object_url("config", build_hash), build_config);
        add_response(transport, object_url("config", cdn_hash), cdn_bytes);
        add_response(transport, object_url("data", install_key), blte_single(*install));
        add_response(transport, object_url("data", encoding_key), encoding_object);
        add_response(transport, object_url("data", game_encoding_key), game_object);
    }
};

openblizz::LibraryEntry dynamic_entry(const std::string& id, const std::string& name,
                                      const openblizz::OwnershipState ownership,
                                      const std::string& source,
                                      const std::string& ngdp_product,
                                      const std::string& shop_slug = "/product/example-game") {
    openblizz::LibraryEntry entry;
    entry.product_id = id;
    entry.name = name;
    entry.ownership = ownership;
    entry.source = source;
    entry.reason = "synthetic third-party fixture";
    entry.updated_at = 1;
    entry.family = "thirdparty";
    entry.ngdp_product = ngdp_product;
    entry.shop_slug = shop_slug;
    return entry;
}

template <typename Function>
bool throws(Function&& function, const std::string& expected = {}) {
    try {
        function();
    } catch (const std::exception& error) {
        return expected.empty() || std::string(error.what()).find(expected) != std::string::npos;
    }
    return false;
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    assert(output);
}

void test_product_resolution(openblizz::HttpClient& http, const Fixture& fixture) {
    OfflineTransport transport;
    transport.responses["https://metadata.test/unused"] = {};
    const auto dynamic = dynamic_entry("thirdparty-exg", "Example Game",
                                       openblizz::OwnershipState::Owned, "account-purchases", "exg");
    openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());

    const auto fixed = catalog.resolve_product("w3");
    assert(fixed.id == "w3");
    assert(fixed.agent_product == "w3");
    const auto legacy = catalog.resolve_product("w3-legacy-tft");
    assert(legacy.agent_product == "w3-legacy-tft");
    assert(openblizz::Catalog::valid_product_code(legacy.agent_product));
    assert(catalog.resolve_product("wlby").family == "thirdparty");

    const auto resolved = catalog.resolve_product("thirdparty-exg");
    assert(resolved.id == "thirdparty-exg");
    assert(resolved.name == "Example Game");
    assert(resolved.agent_product == "exg");
    assert(resolved.family == "thirdparty");

    for (const auto& blocked_code : {std::string("odin"), std::string("lazr")}) {
        const auto blocked = dynamic_entry("thirdparty-" + blocked_code, "Blocked redirect",
                                           openblizz::OwnershipState::Owned, "account-purchases", blocked_code);
        openblizz::Catalog blocked_catalog(http, {blocked}, "https://metadata.test", transport.function());
        assert(throws([&] { (void)blocked_catalog.resolve_product(blocked.product_id); }));
    }

    const auto invalid_code = dynamic_entry("thirdparty-invalid", "Invalid code",
                                            openblizz::OwnershipState::Owned, "account-purchases", "../w3");
    openblizz::Catalog invalid_catalog(http, {invalid_code}, "https://metadata.test", transport.function());
    assert(throws([&] { (void)invalid_catalog.resolve_product(invalid_code.product_id); }));

    const auto slug_only = dynamic_entry("thirdparty-slug-only", "Slug only",
                                         openblizz::OwnershipState::Owned, "account-purchases", std::string{});
    openblizz::Catalog slug_catalog(http, {slug_only}, "https://metadata.test", transport.function());
    assert(throws([&] { (void)slug_catalog.resolve_product(slug_only.product_id); }));

    const auto unowned = dynamic_entry("thirdparty-unowned", "Unowned",
                                       openblizz::OwnershipState::NotOwned, "account-purchases", "exg");
    openblizz::Catalog unowned_catalog(http, {unowned}, "https://metadata.test", transport.function());
    assert(throws([&] { (void)unowned_catalog.resolve_product(unowned.product_id); }));

    const auto wrong_source = dynamic_entry("thirdparty-wrong-source", "Wrong source",
                                            openblizz::OwnershipState::Owned, "manual", "exg");
    openblizz::Catalog source_catalog(http, {wrong_source}, "https://metadata.test", transport.function());
    assert(throws([&] { (void)source_catalog.resolve_product(wrong_source.product_id); }));

    const auto web_without_code = dynamic_entry("thirdparty-missing-code", "Missing code",
        openblizz::OwnershipState::Owned, "account-web", "");
    openblizz::Catalog web_catalog(http, {web_without_code}, "https://metadata.test", transport.function());
    assert(throws([&] { (void)web_catalog.resolve_product(web_without_code.product_id); }, "no explicit NGDP"));
    const auto generic_web = dynamic_entry("exg", "NGDP product exg",
        openblizz::OwnershipState::Owned, "account-web", "exg");
    openblizz::Catalog generic_catalog(http, {generic_web}, "https://metadata.test", transport.function());
    assert(generic_catalog.resolve_product("exg").agent_product == "exg");

    (void)fixture;
}

void test_real_pipeline(openblizz::HttpClient& http, const Fixture& fixture,
                        const std::filesystem::path& root) {
    OfflineTransport transport;
    fixture.configure(transport, "normal");
    const auto dynamic = dynamic_entry("thirdparty-exg", "Example Game",
                                       openblizz::OwnershipState::Owned, "account-purchases", "exg");
    openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
    openblizz::Installer installer(catalog, root / "cache");

    const auto plan = installer.plan("thirdparty-exg", "us", "enUS");
    assert(plan.product.id == "thirdparty-exg");
    assert(plan.product.name == "Example Game");
    assert(plan.version.product == "exg");
    assert(plan.cdn.path == "tpr/exg");
    assert(plan.cdn.hosts.size() == 1 && plan.cdn.hosts.front() == "cdn.test");
    assert(plan.install_manifest.entries.size() == 1);
    assert(plan.selected_entries.size() == 1);
    assert(plan.selected_entries.front().path == "game.bin");
    assert(plan.selected_entries.front().content_key == fixture.content_key);
    assert(plan.total_bytes == fixture.payload.size());

    const auto version_url = "https://metadata.test/v2/products/exg/versions";
    const auto cdns_url = "https://metadata.test/v2/products/exg/cdns";
    assert(transport.count(version_url) == 1);
    assert(transport.count(cdns_url) == 1);
    assert(transport.count(object_url("data", fixture.game_encoding_key)) == 0);
    assert(transport.unexpected.empty());
    const auto after_plan = transport.total();
    assert(after_plan == (fixture.encrypted ? 7u : 6u)); // encrypted case adds a public KeyRing config.

    const auto install_root = root / "installed";
    assert(installer.install(plan, install_root, "enUS", 1) == 1);
    assert(transport.total() == after_plan + 1);
    assert(transport.count(object_url("data", fixture.game_encoding_key)) == 1);
    assert(std::filesystem::exists(install_root / "game.bin"));
    assert(!installer.verify(plan, install_root, "enUS").size());

    const auto before_second_install = transport.total();
    assert(installer.install(plan, install_root, "enUS", 1) == plan.selected_entries.size());
    assert(transport.total() == before_second_install);

    write_file(install_root / "game.bin", Bytes{'b', 'r', 'o', 'k', 'e', 'n'});
    const auto failures = installer.verify(plan, install_root, "enUS");
    assert(!failures.empty());
    assert(failures.front().find("game.bin") != std::string::npos);

    const auto before_repair = transport.total();
    assert(installer.repair(plan, install_root, "enUS", 1) == 1);
    assert(transport.total() == before_repair); // decoded game object is reused from the cache.
    assert(!installer.verify(plan, install_root, "enUS").size());
    std::ifstream input(install_root / "game.bin", std::ios::binary);
    Bytes repaired((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(repaired == fixture.payload);
    assert(transport.unexpected.empty());
}

void test_plan_rejections(openblizz::HttpClient& http, const Fixture& fixture,
                          const std::filesystem::path& root) {
    const auto dynamic = dynamic_entry("thirdparty-exg", "Example Game",
                                       openblizz::OwnershipState::Owned, "account-purchases", "exg");
    const auto encoding_url = object_url("data", fixture.encoding_key);
    const auto game_url = object_url("data", fixture.game_encoding_key);

    {
        OfflineTransport transport;
        fixture.configure(transport, "empty");
        openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
        openblizz::Installer installer(catalog, root / "empty-cache");
        assert(throws([&] { (void)installer.plan("thirdparty-exg", "us", "enUS"); }));
        assert(transport.count(encoding_url) == 0);
        assert(transport.count(game_url) == 0);
        assert(transport.unexpected.empty());
    }

    {
        OfflineTransport transport;
        fixture.configure(transport, "normal", true, false);
        openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
        openblizz::Installer installer(catalog, root / "bad-install-cache");
        assert(throws([&] { (void)installer.plan("thirdparty-exg", "us", "enUS"); }));
        assert(transport.count(encoding_url) == 0);
        assert(transport.count(game_url) == 0);
        assert(transport.unexpected.empty());
    }

    {
        OfflineTransport transport;
        fixture.configure(transport, "normal", false, true);
        openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
        openblizz::Installer installer(catalog, root / "bad-encoding-cache");
        assert(throws([&] { (void)installer.plan("thirdparty-exg", "us", "enUS"); }));
        assert(transport.count(encoding_url) == 1);
        assert(transport.count(game_url) == 0);
        assert(transport.unexpected.empty());
    }

    {
        OfflineTransport transport;
        fixture.configure(transport, "empty-selected");
        openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
        openblizz::Installer installer(catalog, root / "empty-selected-cache");
        assert(throws([&] { (void)installer.plan("thirdparty-exg", "us", "enUS"); }));
        assert(transport.count(game_url) == 0);
        assert(transport.unexpected.empty());
    }

    {
        OfflineTransport transport;
        fixture.configure(transport, "conflict");
        openblizz::Catalog catalog(http, {dynamic}, "https://metadata.test", transport.function());
        openblizz::Installer installer(catalog, root / "conflict-cache");
        assert(throws([&] { (void)installer.plan("thirdparty-exg", "us", "enUS"); }));
        assert(transport.count(game_url) == 0);
        assert(transport.unexpected.empty());
    }
}

void test_archive_pipeline(openblizz::HttpClient& http, const std::filesystem::path& root) {
    Fixture fixture;
    const auto archive_hash = std::string(32, 'a');
    fixture.cdn_config = "archives = " + archive_hash + "\n";
    OfflineTransport transport;
    fixture.configure(transport, "normal");
    Bytes index(4096 + 16 + 8 + 28, 0);
    const auto key = openblizz::hex_to_bytes(fixture.game_encoding_key);
    std::copy(key.begin(), key.end(), index.begin());
    const auto encoded_size = static_cast<std::uint32_t>(fixture.game_object.size());
    for (unsigned i = 0; i < 4; ++i) index[16 + i] = static_cast<std::uint8_t>(encoded_size >> (24 - i * 8));
    index[23] = 8; // archive offset, big-endian
    const auto footer = index.size() - 28;
    index[footer + 8] = 1;
    index[footer + 11] = 4;
    index[footer + 12] = 4;
    index[footer + 13] = 4;
    index[footer + 14] = 16;
    index[footer + 15] = 8;
    index[footer + 16] = 1; // entry count, little-endian
    transport.responses[object_url("data", archive_hash, ".index")] = index;
    Bytes archive(8, 0);
    archive.insert(archive.end(), fixture.game_object.begin(), fixture.game_object.end());
    const auto archive_url = object_url("data", archive_hash);
    const auto loose_url = object_url("data", fixture.game_encoding_key);
    transport.responses[archive_url] = archive;
    transport.responses.erase(loose_url); // no loose object; range must work
    const auto entry = dynamic_entry("thirdparty-exg", "Example Game", openblizz::OwnershipState::Owned,
                                     "account-purchases", "exg");
    openblizz::Catalog catalog(http, {entry}, "https://metadata.test", transport.function());
    openblizz::Installer installer(catalog, root / "cache");
    const auto plan = installer.plan(entry.product_id, "us", "enUS");
    assert(plan.archive_entries.size() == 1);
    assert(installer.install(plan, root / "range", "enUS", 1) == 1);
    assert(transport.count(archive_url) == 1 && transport.count(loose_url) == 0);
    assert(installer.verify(plan, root / "range", "enUS").empty());
    // A later range failure falls back to a loose object in an independent cache.
    transport.responses.erase(archive_url);
    transport.responses[loose_url] = fixture.game_object;
    openblizz::Installer fallback(catalog, root / "fallback-cache");
    assert(fallback.install(plan, root / "fallback", "enUS", 1) == 1);
    assert(transport.count(loose_url) == 1);
    assert(fallback.verify(plan, root / "fallback", "enUS").empty());
}

void test_archived_manifests(openblizz::HttpClient& http, const std::filesystem::path& root) {
    Fixture fixture(true);
    const std::string archive_hash(32, 'b');
    fixture.cdn_config = "archives = " + archive_hash + "\n";
    OfflineTransport transport;
    fixture.configure(transport, "normal");
    const auto index_url = object_url("data", archive_hash, ".index");
    const auto archive_url = object_url("data", archive_hash);
    const auto install_url = object_url("data", fixture.normal_install_key);
    const auto encoding_url = object_url("data", fixture.encoding_key);
    Bytes archive;
    Bytes index(4096 + 16 + 8 + 28, 0);
    std::size_t record = 0;
    for (const auto& key : {fixture.normal_install_key, fixture.encoding_key}) {
        const auto url = object_url("data", key);
        const auto object = transport.responses.at(url);
        const auto bytes = openblizz::hex_to_bytes(key);
        std::copy(bytes.begin(), bytes.end(), index.begin() + static_cast<std::ptrdiff_t>(record));
        for (unsigned i = 0; i < 4; ++i) {
            index[record + 16 + i] = static_cast<std::uint8_t>(object.size() >> (24 - i * 8));
            index[record + 20 + i] = static_cast<std::uint8_t>(archive.size() >> (24 - i * 8));
        }
        record += 24;
        archive.insert(archive.end(), object.begin(), object.end());
        transport.responses.erase(url);
    }
    const auto footer = index.size() - 28;
    index[footer + 8] = 1;
    index[footer + 11] = 4;
    index[footer + 12] = 4;
    index[footer + 13] = 4;
    index[footer + 14] = 16;
    index[footer + 15] = 8;
    index[footer + 16] = 2;
    transport.responses[index_url] = index;
    transport.responses[archive_url] = archive;
    const auto entry = dynamic_entry("thirdparty-exg", "Example Game", openblizz::OwnershipState::Owned,
                                     "account-purchases", "exg");
    openblizz::Catalog catalog(http, {entry}, "https://metadata.test", transport.function());
    openblizz::Installer installer(catalog, root / "cache");
    const auto plan = installer.plan(entry.product_id, "us", "enUS");
    assert(plan.selected_entries.size() == 1 && plan.archive_entries.size() == 2);
    assert(transport.count(index_url) == 1 && transport.count(archive_url) == 2);
    assert(transport.count(install_url) == 1 && transport.count(encoding_url) == 1);
    assert(installer.install(plan, root / "game", "enUS", 1) == 1);
    assert(installer.verify(plan, root / "game", "enUS").empty());
    (void)installer.plan(entry.product_id, "us", "enUS");
    assert(transport.count(index_url) == 1); // fallback and normal load share the same cached indexes

    // No suitable archive entry: keep the original loose-object diagnostic.
    OfflineTransport missing;
    fixture.configure(missing, "normal");
    missing.responses.erase(install_url);
    auto unrelated_index = index;
    std::fill(unrelated_index.begin(), unrelated_index.begin() + 16, 0);
    missing.responses[index_url] = unrelated_index;
    openblizz::Catalog missing_catalog(http, {entry}, "https://metadata.test", missing.function());
    openblizz::Installer missing_installer(missing_catalog, root / "missing-cache");
    assert(throws([&] { (void)missing_installer.plan(entry.product_id, "us", "enUS"); },
                  "unavailable as a loose object or in the advertised archives"));

    // A usable loose response with an empty IN must NOT load hundreds of indexes.
    OfflineTransport empty;
    fixture.configure(empty, "empty");
    openblizz::Catalog empty_catalog(http, {entry}, "https://metadata.test", empty.function());
    openblizz::Installer empty_installer(empty_catalog, root / "empty-cache");
    assert(throws([&] { (void)empty_installer.plan(entry.product_id, "us", "enUS"); }, "empty install manifest"));
    assert(empty.count(index_url) == 0 && empty.count(encoding_url) == 0);
    assert(empty.unexpected.empty());

    // An archived object must still match the advertised decoded CKey.
    auto corrupted = archive;
    corrupted[9] ^= 1; // change decoded IN without changing the valid BLTE N wrapper
    transport.responses[archive_url] = corrupted;
    assert(throws([&] { (void)installer.plan(entry.product_id, "us", "enUS"); },
                  "install manifest content hash mismatch"));
}

void test_casc_metadata(openblizz::HttpClient& http, const Fixture& fixture,
                        const std::filesystem::path& root) {
    OfflineTransport transport;
    fixture.configure(transport, "normal");
    const auto entry = dynamic_entry("thirdparty-exg", "Example Game", openblizz::OwnershipState::Owned,
                                     "account-purchases", "exg");
    openblizz::Catalog catalog(http, {entry}, "https://metadata.test", transport.function());
    openblizz::Installer installer(catalog, root / "cache");
    auto plan = installer.plan(entry.product_id, "us", "enUS");
    // Exercise the storage writer with a synthetic CASC plan; no TVFS/game files.
    plan.casc = true;
    plan.data_objects.push_back({fixture.game_encoding_key, fixture.game_object.size(), "fixture"});
    plan.data_bytes = fixture.game_object.size();
    const auto installed = root / "game";
    assert(installer.install(plan, installed, "enUS", 2) == 2);
    std::ifstream input(installed / ".build.info");
    const std::string info((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(info.find("thirdparty-exg") == std::string::npos);
    assert(info.find("||exg\n") != std::string::npos);
    assert(installer.verify(plan, installed, "enUS", true).empty());
    const auto requests = transport.total();
    assert(installer.install(plan, installed, "enUS", 2) == 1);
    assert(transport.total() == requests); // completed file and CASC object reused
}

void test_shared_cache(openblizz::HttpClient& http, const Fixture& fixture,
                        const std::filesystem::path& root) {
    OfflineTransport transport;
    fixture.configure(transport, "normal");
    const auto entry = dynamic_entry("thirdparty-exg", "Example Game", openblizz::OwnershipState::Owned,
                                     "account-purchases", "exg");
    openblizz::Catalog catalog(http, {entry}, "https://metadata.test", transport.function());
    openblizz::Installer installer(catalog, root / "cache");
    auto plan = installer.plan(entry.product_id, "us", "enUS");
    const auto original = plan.selected_entries.front();
    plan.selected_entries.clear();
    for (unsigned i = 0; i < 16; ++i) {
        auto duplicate = original;
        duplicate.path = "copies/" + std::to_string(i) + ".bin";
        plan.selected_entries.push_back(std::move(duplicate));
    }
    assert(installer.install(plan, root / "game", "enUS", 8) == 16);
    assert(installer.verify(plan, root / "game", "enUS").empty());
    assert(transport.unexpected.empty());
}

} // namespace

int main() {
    openblizz::HttpClient http;
    const Fixture fixture;
    TemporaryDirectory temporary;

    test_product_resolution(http, fixture);
    test_real_pipeline(http, fixture, temporary.path);
    test_plan_rejections(http, fixture, temporary.path);
    const Fixture encrypted_fixture(true);
    test_real_pipeline(http, encrypted_fixture, temporary.path / "encrypted");
    test_archive_pipeline(http, temporary.path / "archive");
    test_archived_manifests(http, temporary.path / "archived-manifests");
    test_casc_metadata(http, encrypted_fixture, temporary.path / "casc");
    test_shared_cache(http, fixture, temporary.path / "shared-cache");

    std::cout << "OpenBlizz third-party tests passed\n";
    return 0;
}
