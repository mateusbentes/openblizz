#include "openblizz/tvfs.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace openblizz {
namespace {

constexpr std::uint32_t kFolderNode = 0x80000000u;
constexpr std::uint32_t kFolderSizeMask = 0x7fffffffu;
constexpr std::uint8_t kMaxSpanCount = 0xe0;
constexpr std::uint8_t kEntryLink = 0xfd;
constexpr std::uint8_t kEntryInline = 0xfe;
constexpr std::uint8_t kEntryDeleted = 0xff;

std::uint32_t be32(const std::vector<std::uint8_t>& data, std::size_t offset, const char* what) {
    if (offset + 4 > data.size()) throw std::runtime_error(std::string("truncated TVFS ") + what);
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

std::uint16_t be16(const std::vector<std::uint8_t>& data, std::size_t offset, const char* what) {
    if (offset + 2 > data.size()) throw std::runtime_error(std::string("truncated TVFS ") + what);
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

std::uint32_t be_n(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t count, const char* what) {
    if (offset + count > data.size()) throw std::runtime_error(std::string("truncated TVFS ") + what);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value = (value << 8) | data[offset + i];
    return value;
}

std::string hex_bytes(const std::uint8_t* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

// Offsets into the container-file and ESpec tables use only as many bytes as
// the referenced table needs (see "Note on Container file/ESpec table field
// sizes" at wowdev.wiki/TVFS and GetOffsetFieldSize() in CascLib).
std::size_t offset_field_size(std::uint32_t table_size) {
    if (table_size == 0) return 0;
    if (table_size > 0xffffff) return 4;
    if (table_size > 0xffff) return 3;
    if (table_size > 0xff) return 2;
    return 1;
}

struct Header {
    std::uint8_t version{};
    std::uint8_t header_size{};
    std::uint8_t ekey_size{};
    std::uint8_t pkey_size{};
    std::uint32_t flags{};
    std::uint32_t path_offset{};
    std::uint32_t path_size{};
    std::uint32_t vfs_offset{};
    std::uint32_t vfs_size{};
    std::uint32_t cft_offset{};
    std::uint32_t cft_size{};
    std::uint16_t max_depth{};
    std::uint32_t est_offset{};
    std::uint32_t est_size{};
    std::size_t cft_offset_size{};
    std::size_t est_offset_size{};
};

Header parse_header(const std::vector<std::uint8_t>& data) {
    if (!is_tvfs(data)) throw std::runtime_error("invalid TVFS magic");
    if (data.size() < 38) throw std::runtime_error("truncated TVFS header");
    Header header;
    header.version = data[4];
    header.header_size = data[5];
    header.ekey_size = data[6];
    header.pkey_size = data[7];
    if (header.version != 1) throw std::runtime_error("unsupported TVFS version");
    if (header.ekey_size == 0 || header.ekey_size > 16 || header.header_size < 38) {
        throw std::runtime_error("unsupported TVFS header layout");
    }
    header.flags = be32(data, 8, "flags");
    header.path_offset = be32(data, 12, "path table offset");
    header.path_size = be32(data, 16, "path table size");
    header.vfs_offset = be32(data, 20, "vfs table offset");
    header.vfs_size = be32(data, 24, "vfs table size");
    header.cft_offset = be32(data, 28, "container table offset");
    header.cft_size = be32(data, 32, "container table size");
    header.max_depth = be16(data, 36, "max depth");
    if ((header.flags & kTvfsWriteSupport) != 0 && header.header_size >= 46) {
        header.est_offset = be32(data, 38, "espec table offset");
        header.est_size = be32(data, 42, "espec table size");
    }
    const auto in_range = [&](std::uint64_t offset, std::uint64_t size) {
        return offset <= data.size() && size <= data.size() - offset;
    };
    if (!in_range(header.path_offset, header.path_size) || !in_range(header.vfs_offset, header.vfs_size) ||
        !in_range(header.cft_offset, header.cft_size) || !in_range(header.est_offset, header.est_size)) {
        throw std::runtime_error("TVFS table offsets exceed file");
    }
    header.cft_offset_size = offset_field_size(header.cft_size);
    header.est_offset_size = offset_field_size(header.est_size);
    return header;
}

class Parser {
public:
    Parser(const std::vector<std::uint8_t>& data, Header header, TvfsManifest& manifest)
        : data_(data), header_(header), manifest_(manifest) {}

    void run() {
        std::size_t begin = header_.path_offset;
        std::size_t end = begin + header_.path_size;
        // The path table usually starts with an unnamed root folder node.
        if (begin + 5 < end && data_[begin] == 0xff) {
            const auto value = be32(data_, begin + 1, "root node value");
            if ((value & kFolderNode) == 0) throw std::runtime_error("TVFS root node is not a folder");
            const auto root_end = begin + 1 + static_cast<std::size_t>(value & kFolderSizeMask);
            if (root_end > end) throw std::runtime_error("TVFS root folder exceeds path table");
            end = root_end;
            begin += 5;
        }
        std::string path;
        parse_folder(begin, end, path, 0);
    }

private:
    struct PathEntry {
        std::string name;
        bool separator_before{false};
        bool separator_after{false};
        bool has_value{false};
        std::uint32_t value{};
    };

    std::size_t capture(std::size_t offset, std::size_t end, PathEntry& entry) const {
        entry = PathEntry{};
        if (offset < end && data_[offset] == 0) {
            entry.separator_before = true;
            ++offset;
        }
        if (offset < end && data_[offset] != 0xff) {
            const std::size_t length = data_[offset++];
            if (offset + length > end) throw std::runtime_error("TVFS path fragment exceeds folder");
            entry.name.assign(reinterpret_cast<const char*>(data_.data() + offset), length);
            offset += length;
        }
        if (offset < end && data_[offset] == 0) {
            entry.separator_after = true;
            ++offset;
        }
        if (offset < end) {
            if (data_[offset] == 0xff) {
                if (offset + 5 > end) throw std::runtime_error("TVFS node value exceeds folder");
                entry.value = be32(data_, offset + 1, "node value");
                entry.has_value = true;
                offset += 5;
            } else {
                // A following fragment implies a separator after this one.
                entry.separator_after = true;
            }
        }
        return offset;
    }

    void parse_folder(std::size_t offset, std::size_t end, std::string& path, std::size_t depth) {
        if (depth > 64) throw std::runtime_error("TVFS folder nesting is too deep");
        const auto saved = path.size();
        PathEntry entry;
        while (offset < end) {
            offset = capture(offset, end, entry);
            if (entry.separator_before) path.push_back('/');
            path += entry.name;
            if (entry.separator_after) path.push_back('/');
            if (!entry.has_value) continue;
            if ((entry.value & kFolderNode) != 0) {
                const auto length = static_cast<std::size_t>(entry.value & kFolderSizeMask);
                if (length < 4 || offset + length - 4 > end) throw std::runtime_error("TVFS folder exceeds parent");
                const auto folder_end = offset + length - 4;
                parse_folder(offset, folder_end, path, depth + 1);
                offset = folder_end;
            } else {
                add_file(path, entry.value);
            }
            path.resize(saved);
        }
    }

    VfsSpanRef container_entry(std::uint32_t cft_entry_offset) const {
        std::size_t offset = header_.cft_offset + cft_entry_offset;
        const auto cft_end = static_cast<std::size_t>(header_.cft_offset) + header_.cft_size;
        if (cft_entry_offset >= header_.cft_size || offset + header_.ekey_size + 4 > cft_end) {
            throw std::runtime_error("TVFS container entry exceeds table");
        }
        VfsSpanRef span;
        span.encoding_key = hex_bytes(data_.data() + offset, header_.ekey_size);
        offset += header_.ekey_size;
        span.encoded_size = be32(data_, offset, "container encoded size");
        offset += 4;
        offset += header_.est_offset_size;
        if (offset + 4 <= cft_end) span.content_size = be32(data_, offset, "container content size");
        offset += 4;
        if ((header_.flags & kTvfsIncludeCKey) != 0 && offset + 16 <= cft_end) {
            span.content_key = hex_bytes(data_.data() + offset, 16);
        }
        return span;
    }

    void add_file(const std::string& path, std::uint32_t vfs_entry_offset) {
        std::size_t offset = header_.vfs_offset + vfs_entry_offset;
        const auto vfs_end = static_cast<std::size_t>(header_.vfs_offset) + header_.vfs_size;
        if (vfs_entry_offset >= header_.vfs_size) throw std::runtime_error("TVFS file entry exceeds table");
        TvfsEntry entry;
        entry.path = path;
        const auto span_count = data_[offset++];
        if (span_count == kEntryDeleted) {
            entry.kind = TvfsEntryKind::Deleted;
        } else if (span_count == kEntryInline) {
            entry.kind = TvfsEntryKind::Inline;
            if (offset >= vfs_end) throw std::runtime_error("TVFS inline entry exceeds table");
            const std::size_t inline_size = static_cast<std::size_t>(data_[offset++]) + 1;
            if (offset + inline_size > vfs_end) throw std::runtime_error("TVFS inline data exceeds table");
            entry.inline_data.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                                     data_.begin() + static_cast<std::ptrdiff_t>(offset + inline_size));
        } else if (span_count == kEntryLink) {
            entry.kind = TvfsEntryKind::Link;
            std::string target;
            while (offset < vfs_end) {
                const auto length = static_cast<std::int8_t>(data_[offset++]);
                if (length < 0) break;
                if (offset + static_cast<std::size_t>(length) > vfs_end) {
                    throw std::runtime_error("TVFS link component exceeds table");
                }
                if (!target.empty()) target.push_back('/');
                target.append(reinterpret_cast<const char*>(data_.data() + offset), static_cast<std::size_t>(length));
                offset += static_cast<std::size_t>(length);
            }
            entry.inline_data.assign(target.begin(), target.end());
        } else if (span_count == 0 || span_count > kMaxSpanCount) {
            return;   // 0xE1-0xFC are reserved; CascLib ignores them as well
        } else {
            const std::size_t item_size = 8 + header_.cft_offset_size;
            if (offset + item_size * span_count > vfs_end) throw std::runtime_error("TVFS spans exceed table");
            entry.spans.reserve(span_count);
            for (std::uint8_t i = 0; i < span_count; ++i) {
                const auto file_offset = be32(data_, offset, "span offset");
                const auto size = be32(data_, offset + 4, "span size");
                const auto cft_entry = be_n(data_, offset + 8, header_.cft_offset_size, "span container offset");
                auto span = container_entry(cft_entry);
                span.file_offset = file_offset;
                // The span size is authoritative; the container entry repeats it.
                span.content_size = size;
                entry.spans.push_back(std::move(span));
                offset += item_size;
            }
        }
        manifest_.entries.push_back(std::move(entry));
    }

    const std::vector<std::uint8_t>& data_;
    Header header_;
    TvfsManifest& manifest_;
};

} // namespace

bool is_tvfs(const std::vector<std::uint8_t>& decoded) {
    return decoded.size() >= 8 && decoded[0] == 'T' && decoded[1] == 'V' && decoded[2] == 'F' && decoded[3] == 'S';
}

TvfsManifest parse_tvfs(const std::vector<std::uint8_t>& decoded) {
    const auto header = parse_header(decoded);
    TvfsManifest manifest;
    manifest.version = header.version;
    manifest.ekey_size = header.ekey_size;
    manifest.pkey_size = header.pkey_size;
    manifest.flags = header.flags;
    manifest.max_depth = header.max_depth;
    Parser parser(decoded, header, manifest);
    parser.run();
    return manifest;
}

std::vector<VfsManifestRef> vfs_manifest_refs(const ConfigFile& build_config) {
    std::vector<VfsManifestRef> refs;
    for (const auto& [key, values] : build_config.values) {
        if (key.rfind("vfs-", 0) != 0 || (key.size() > 5 && key.compare(key.size() - 5, 5, "-size") == 0)) continue;
        if (values.empty()) continue;
        VfsManifestRef ref;
        ref.name = key;
        ref.content_key = values[0];
        if (values.size() > 1) ref.encoding_key = values[1];
        const auto sizes = build_config.get(key + "-size");
        if (!sizes.empty()) ref.content_size = std::stoull(sizes[0]);
        if (sizes.size() > 1) ref.encoded_size = std::stoull(sizes[1]);
        refs.push_back(std::move(ref));
    }
    // Keep vfs-root first, then vfs-1, vfs-2, ... in numeric order.
    std::sort(refs.begin(), refs.end(), [](const VfsManifestRef& a, const VfsManifestRef& b) {
        const auto rank = [](const std::string& name) -> long long {
            if (name == "vfs-root") return -1;
            const auto digits = name.substr(4);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                return 1ll << 40;
            }
            return std::stoll(digits);
        };
        const auto ra = rank(a.name);
        const auto rb = rank(b.name);
        return ra != rb ? ra < rb : a.name < b.name;
    });
    return refs;
}

std::vector<VfsFile> VfsResolver::resolve(const ConfigFile& build_config, const Loader& load) {
    const auto refs = vfs_manifest_refs(build_config);
    const auto root = std::find_if(refs.begin(), refs.end(), [](const auto& ref) { return ref.name == "vfs-root"; });
    if (root == refs.end()) throw std::runtime_error("build config has no vfs-root manifest");
    std::vector<VfsFile> files;
    std::set<std::string> visiting;

    const std::function<void(const VfsManifestRef&, const std::string&)> walk =
        [&](const VfsManifestRef& ref, const std::string& prefix) {
            if (!visiting.insert(ref.name).second) {
                throw std::runtime_error("TVFS manifest cycle through " + ref.name);
            }
            const auto manifest = parse_tvfs(load(ref));
            const std::size_t prefix_chars = static_cast<std::size_t>(manifest.ekey_size) * 2;
            std::unordered_map<std::string, const VfsManifestRef*> nested;
            for (const auto& candidate : refs) {
                if (candidate.name == "vfs-root" || candidate.encoding_key.size() < prefix_chars) continue;
                nested.emplace(candidate.encoding_key.substr(0, prefix_chars), &candidate);
            }
            for (const auto& entry : manifest.entries) {
                if (entry.kind != TvfsEntryKind::File) continue;
                VfsFile file;
                file.path = prefix + entry.path;
                file.manifest = ref.name;
                file.spans = entry.spans;
                const VfsManifestRef* child = nullptr;
                if (entry.spans.size() == 1) {
                    const auto it = nested.find(entry.spans.front().encoding_key);
                    if (it != nested.end()) child = it->second;
                }
                if (child != nullptr) file.nested_manifest = child->name;
                files.push_back(std::move(file));
                if (child != nullptr) walk(*child, files.back().path + ":");
            }
            visiting.erase(ref.name);
        };
    walk(*root, "");
    return files;
}

} // namespace openblizz
