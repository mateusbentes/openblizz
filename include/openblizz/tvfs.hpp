#pragma once

#include "openblizz/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openblizz {

// TACT Virtual File System (TVFS) manifest. The format is documented at
// https://wowdev.wiki/TVFS and implemented by CascLib's
// CascRootFile_TVFS.cpp. Warcraft III: Reforged uses it as its ROOT: the
// build config exposes `vfs-root` plus ~110 `vfs-N` manifests that are
// mounted as nested virtual file systems (CascLib joins them with ':').
constexpr std::uint32_t kTvfsIncludeCKey = 0x1;
constexpr std::uint32_t kTvfsWriteSupport = 0x2;
constexpr std::uint32_t kTvfsPatchSupport = 0x4;
constexpr std::uint32_t kTvfsLowercaseManifest = 0x8;

enum class TvfsEntryKind : std::uint8_t {
    File,
    Deleted,
    Inline,
    Link,
};

struct TvfsEntry {
    std::string path;
    TvfsEntryKind kind{TvfsEntryKind::File};
    std::vector<VfsSpanRef> spans;
    std::vector<std::uint8_t> inline_data;
};

struct TvfsManifest {
    std::uint8_t version{};
    std::uint8_t ekey_size{};
    std::uint8_t pkey_size{};
    std::uint32_t flags{};
    std::uint16_t max_depth{};
    std::vector<TvfsEntry> entries;
};

[[nodiscard]] bool is_tvfs(const std::vector<std::uint8_t>& decoded);
[[nodiscard]] TvfsManifest parse_tvfs(const std::vector<std::uint8_t>& decoded);

// Build-config `vfs-root` / `vfs-N = <ckey> <ekey>` entries with their sizes.
struct VfsManifestRef {
    std::string name;
    std::string content_key;
    std::string encoding_key;
    std::uint64_t content_size{};
    std::uint64_t encoded_size{};
};

[[nodiscard]] std::vector<VfsManifestRef> vfs_manifest_refs(const ConfigFile& build_config);

// Walks vfs-root and every nested manifest it references and returns the flat
// list of virtual files. `load` must return the decoded bytes of a manifest.
// Nested manifests are detected like CascLib does: a single-span entry whose
// truncated EKey matches the prefix of one of the `vfs-N` encoding keys.
class VfsResolver {
public:
    using Loader = std::function<std::vector<std::uint8_t>(const VfsManifestRef&)>;

    [[nodiscard]] static std::vector<VfsFile> resolve(const ConfigFile& build_config, const Loader& load);
};

} // namespace openblizz
