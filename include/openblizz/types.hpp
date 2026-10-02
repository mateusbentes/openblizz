#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openblizz {

struct ProductDescriptor {
    std::string id;
    std::string name;
    std::string family;
    std::string agent_product;
    bool supported{true};
};

enum class OwnershipState {
    Unknown,
    Owned,
    NotOwned,
    Manual,
};

[[nodiscard]] inline const char* ownership_state_name(const OwnershipState state) {
    switch (state) {
    case OwnershipState::Unknown: return "unknown";
    case OwnershipState::Owned: return "owned";
    case OwnershipState::NotOwned: return "not_owned";
    case OwnershipState::Manual: return "manual";
    }
    return "unknown";
}

[[nodiscard]] inline OwnershipState ownership_state_from_name(const std::string& name) {
    if (name == "owned") return OwnershipState::Owned;
    if (name == "not_owned") return OwnershipState::NotOwned;
    if (name == "manual") return OwnershipState::Manual;
    return OwnershipState::Unknown;
}

struct LibraryEntry {
    std::string product_id;
    std::string name;
    OwnershipState ownership{OwnershipState::Unknown};
    std::string source;
    std::string reason;
    std::int64_t updated_at{};
};

struct VersionInfo {
    std::string product;
    std::string region;
    std::string build_config;
    std::string cdn_config;
    std::string keyring;
    std::string build_id;
    std::string version_name;
    std::string product_config;
};

struct CdnInfo {
    std::string product;
    std::string region;
    std::string path;
    std::vector<std::string> hosts;
    std::vector<std::string> servers;
    std::string config_path;
};

struct KeyPair {
    std::string content_key;
    std::string encoding_key;
};

struct ConfigFile {
    std::map<std::string, std::vector<std::string>> values;

    [[nodiscard]] bool contains(const std::string& key) const {
        return values.find(key) != values.end();
    }

    [[nodiscard]] std::vector<std::string> get(const std::string& key) const {
        const auto it = values.find(key);
        return it == values.end() ? std::vector<std::string>{} : it->second;
    }

    [[nodiscard]] std::optional<KeyPair> pair(const std::string& key) const {
        const auto v = get(key);
        if (v.empty()) return std::nullopt;
        KeyPair result;
        result.content_key = v[0];
        if (v.size() > 1) result.encoding_key = v[1];
        return result;
    }
};

struct InstallTag {
    std::string name;
    std::uint16_t type{};
    std::vector<std::uint8_t> bitmap;
};

struct InstallEntry {
    std::string path;
    std::string content_key;
    std::uint64_t file_size{};
    std::optional<std::uint8_t> file_type;
};

struct InstallManifest {
    std::uint8_t version{};
    std::uint8_t ckey_size{};
    std::uint32_t entry_count{};
    std::vector<InstallTag> tags;
    std::vector<InstallEntry> entries;
};

struct DownloadTag {
    std::string name;
    std::uint16_t type{};
    std::vector<std::uint8_t> bitmap;
};

struct DownloadEntry {
    std::string encoding_key;
    std::uint64_t file_size{};
    std::int8_t priority{};
    std::optional<std::uint32_t> checksum;
    std::vector<std::uint8_t> flags;
};

struct DownloadManifest {
    std::uint8_t version{};
    std::uint8_t ekey_size{};
    bool has_checksum{false};
    std::uint32_t entry_count{};
    std::uint16_t tag_count{};
    std::uint8_t flag_size{};
    std::int8_t base_priority{};
    std::vector<DownloadEntry> entries;
    std::vector<DownloadTag> tags;
};

struct FileMapping {
    std::string encoding_key;
    std::uint64_t decoded_size{};
    std::vector<std::string> encoding_keys;
};

struct ArchiveLocation {
    std::string archive_key;
    std::uint64_t offset{};
    std::uint32_t encoded_size{};
};

struct VfsSpanRef {
    std::uint32_t file_offset{};
    std::uint32_t content_size{};
    std::string encoding_key;   // truncated (usually 9 bytes) hex EKey from the TVFS
    std::uint32_t encoded_size{};
    std::string content_key;    // full CKey when the manifest includes it, else empty
};

// One file of the mounted virtual file system. Nested manifests are joined
// with ':' exactly like CascLib ("war3.w3mod:_hd.w3mod:units/...").
struct VfsFile {
    std::string path;
    std::string manifest;          // build-config key of the manifest listing it ("vfs-root", "vfs-8")
    std::string nested_manifest;   // non-empty when the entry is itself a nested TVFS ("vfs-2")
    std::vector<VfsSpanRef> spans;

    [[nodiscard]] std::uint64_t content_size() const {
        std::uint64_t total = 0;
        for (const auto& span : spans) total += span.content_size;
        return total;
    }

    [[nodiscard]] std::uint64_t encoded_size() const {
        std::uint64_t total = 0;
        for (const auto& span : spans) total += span.encoded_size;
        return total;
    }
};

struct BuildContext {
    ProductDescriptor product;
    VersionInfo version;
    CdnInfo cdn;
    ConfigFile build_config;
    ConfigFile cdn_config;
};

struct InstallPlan {
    ProductDescriptor product;
    VersionInfo version;
    CdnInfo cdn;
    ConfigFile build_config;
    ConfigFile cdn_config;
    InstallManifest install_manifest;
    std::unordered_map<std::string, FileMapping> mappings;
    std::unordered_map<std::string, ArchiveLocation> archive_entries;
    std::vector<InstallEntry> selected_entries;
    std::uint64_t total_bytes{};
};

} // namespace openblizz
