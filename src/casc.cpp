#include "openblizz/casc.hpp"
#include "openblizz/hash.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openblizz {
namespace {

constexpr std::uint16_t kIndexVersion = 7;
constexpr std::size_t kIndexEntrySize = kCascKeyBytes + 5 + 4;
constexpr std::size_t kIndexHeaderSize = 0x10;
constexpr std::size_t kIndexEntriesOffset = 0x28;
constexpr std::size_t kIndexMinFileSize = 0x8000;   // lets CascLib accept empty journals
constexpr std::uint32_t kDataHeaderSeed = 0x3D6BE971u;

// Extracted from Agent.exe (see wowdev.wiki/CASC, "ChecksumB").
constexpr std::uint32_t kChecksumTable[16] = {
    0x049396b8, 0x72a82a9b, 0xee626cca, 0x9917754f, 0x15de40b1, 0xf5a8a9b6, 0x421eac7e, 0xa9d55c9a,
    0x317fd40c, 0x04faf80d, 0x3d6be971, 0x52933cfd, 0x27f64b7d, 0xc6f5c11b, 0xd5757e3a, 0x6c388745,
};

void put_u16le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void put_u32le_at(std::uint8_t* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put_u64le(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t get_u32le(const std::vector<std::uint8_t>& in, std::size_t offset) {
    if (offset + 4 > in.size()) throw std::runtime_error("truncated CASC index");
    return static_cast<std::uint32_t>(in[offset]) | (static_cast<std::uint32_t>(in[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(in[offset + 2]) << 16) | (static_cast<std::uint32_t>(in[offset + 3]) << 24);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read file: " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(std::max<std::streamoff>(size, 0)));
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!input && !data.empty()) throw std::runtime_error("short read: " + path.string());
    return data;
}

void write_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    const auto part = path.string() + ".part";
    {
        std::ofstream output(part, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write temporary file: " + part);
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output) throw std::runtime_error("failed writing temporary file: " + part);
    }
    std::error_code error;
    std::filesystem::rename(part, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(part, path, error);
        if (error) throw std::runtime_error("cannot commit file: " + path.string());
    }
}

std::string index_file_name(std::uint8_t bucket, std::uint32_t version) {
    char name[32];
    std::snprintf(name, sizeof(name), "%02x%08x.idx", bucket, version);
    return name;
}

std::string archive_file_name(std::uint16_t archive) {
    char name[32];
    std::snprintf(name, sizeof(name), "data.%03u", static_cast<unsigned>(archive));
    return name;
}

std::uint32_t entries_hash(const std::uint8_t* data, std::size_t size) {
    // The Agent hashes the entries one by one, carrying (pc, pb) across them.
    std::uint32_t pc = 0;
    std::uint32_t pb = 0;
    for (std::size_t offset = 0; offset + kIndexEntrySize <= size; offset += kIndexEntrySize) {
        jenkins_hashlittle2(data + offset, kIndexEntrySize, pc, pb);
    }
    return pc;
}

std::uint32_t entries_hash_blizzget(const std::uint8_t* data, std::size_t size) {
    std::uint32_t hash = 0;
    for (std::size_t offset = 0; offset + kIndexEntrySize <= size; offset += kIndexEntrySize) {
        hash = jenkins_hashlittle(data + offset, kIndexEntrySize, hash);
    }
    return hash;
}

} // namespace

CascKey casc_key(const std::string& encoding_key_hex) {
    if (encoding_key_hex.size() < kCascKeyBytes * 2) {
        throw std::runtime_error("encoding key too short for CASC: " + encoding_key_hex);
    }
    const auto bytes = hex_to_bytes(encoding_key_hex.substr(0, kCascKeyBytes * 2));
    CascKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::uint8_t casc_bucket(const CascKey& key) {
    std::uint8_t folded = 0;
    for (const auto byte : key) folded ^= byte;
    return static_cast<std::uint8_t>((folded & 0x0f) ^ (folded >> 4));
}

std::vector<std::uint8_t> encode_casc_index(std::uint8_t bucket, std::vector<CascEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const CascEntry& a, const CascEntry& b) { return a.key < b.key; });

    std::vector<std::uint8_t> header;
    put_u16le(header, kIndexVersion);
    header.push_back(bucket);
    header.push_back(0);                          // extra bytes
    header.push_back(4);                          // size bytes
    header.push_back(5);                          // offset bytes
    header.push_back(static_cast<std::uint8_t>(kCascKeyBytes));
    header.push_back(30);                         // offset bits
    put_u64le(header, 0x4000000000ull);           // maximum storage size
    if (header.size() != kIndexHeaderSize) throw std::logic_error("bad CASC index header size");

    std::vector<std::uint8_t> body;
    body.reserve(entries.size() * kIndexEntrySize);
    for (const auto& entry : entries) {
        if (entry.offset >= kCascMaxArchiveSize || entry.archive >= (1u << 10)) {
            throw std::runtime_error("CASC entry location does not fit the 5-byte offset field");
        }
        body.insert(body.end(), entry.key.begin(), entry.key.end());
        const std::uint64_t packed = (static_cast<std::uint64_t>(entry.archive) << 30) | entry.offset;
        for (int shift = 32; shift >= 0; shift -= 8) body.push_back(static_cast<std::uint8_t>(packed >> shift));
        put_u32le(body, entry.size);
    }

    std::vector<std::uint8_t> out;
    out.reserve(std::max(kIndexMinFileSize, kIndexEntriesOffset + body.size()));
    put_u32le(out, static_cast<std::uint32_t>(kIndexHeaderSize));
    put_u32le(out, jenkins_hashlittle(header.data(), header.size(), 0));
    out.insert(out.end(), header.begin(), header.end());
    out.resize(0x20, 0);
    put_u32le(out, static_cast<std::uint32_t>(body.size()));
    put_u32le(out, entries_hash(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    const auto padded = std::max(kIndexMinFileSize, (out.size() + 0xfff) & ~static_cast<std::size_t>(0xfff));
    out.resize(padded, 0);
    return out;
}

std::vector<CascEntry> decode_casc_index(const std::vector<std::uint8_t>& bytes, std::uint8_t bucket) {
    if (bytes.size() < kIndexEntriesOffset) throw std::runtime_error("CASC index too small");
    const auto header_size = get_u32le(bytes, 0);
    if (header_size != kIndexHeaderSize) throw std::runtime_error("unsupported CASC index header size");
    if (get_u32le(bytes, 4) != jenkins_hashlittle(bytes.data() + 8, kIndexHeaderSize, 0)) {
        throw std::runtime_error("CASC index header hash mismatch");
    }
    const auto version = static_cast<std::uint16_t>(bytes[8] | (bytes[9] << 8));
    if (version != kIndexVersion) throw std::runtime_error("unsupported CASC index version");
    if (bytes[10] != bucket) throw std::runtime_error("CASC index bucket mismatch");
    if (bytes[12] != 4 || bytes[13] != 5 || bytes[14] != kCascKeyBytes || bytes[15] != 30) {
        throw std::runtime_error("unsupported CASC index entry layout");
    }
    const auto block_size = get_u32le(bytes, 0x20);
    const auto block_hash = get_u32le(bytes, 0x24);
    if (kIndexEntriesOffset + block_size > bytes.size()) throw std::runtime_error("CASC index entries exceed file");
    const auto* data = bytes.data() + kIndexEntriesOffset;
    if (block_size != 0 && block_hash != entries_hash(data, block_size) &&
        block_hash != entries_hash_blizzget(data, block_size)) {
        throw std::runtime_error("CASC index entries hash mismatch");
    }
    std::vector<CascEntry> entries;
    entries.reserve(block_size / kIndexEntrySize);
    for (std::size_t offset = 0; offset + kIndexEntrySize <= block_size; offset += kIndexEntrySize) {
        CascEntry entry;
        std::copy(data + offset, data + offset + kCascKeyBytes, entry.key.begin());
        std::uint64_t packed = 0;
        for (std::size_t i = 0; i < 5; ++i) packed = (packed << 8) | data[offset + kCascKeyBytes + i];
        entry.archive = static_cast<std::uint16_t>(packed >> 30);
        entry.offset = static_cast<std::uint32_t>(packed & (kCascMaxArchiveSize - 1));
        entry.size = get_u32le(bytes, kIndexEntriesOffset + offset + kCascKeyBytes + 5);
        entries.push_back(entry);
    }
    return entries;
}

std::array<std::uint8_t, kCascDataHeaderSize> encode_casc_data_header(
    const std::array<std::uint8_t, 16>& encoding_key, std::uint32_t encoded_size,
    std::uint16_t archive, std::uint32_t offset) {
    std::array<std::uint8_t, kCascDataHeaderSize> header{};
    for (std::size_t i = 0; i < 16; ++i) header[i] = encoding_key[15 - i];
    put_u32le_at(header.data() + 0x10, encoded_size + static_cast<std::uint32_t>(kCascDataHeaderSize));
    header[0x14] = 0;
    header[0x15] = 0;
    put_u32le_at(header.data() + 0x16, jenkins_hashlittle(header.data(), 0x16, kDataHeaderSeed));

    const std::uint32_t location = (offset & 0x3fffffffu) | (static_cast<std::uint32_t>(archive & 3) << 30);
    const std::uint32_t encoded_offset = kChecksumTable[(location + 0x1e) & 0x0f] ^ (location + 0x1e);
    std::uint8_t encoded_offset_bytes[4];
    put_u32le_at(encoded_offset_bytes, encoded_offset);
    std::uint8_t hashed[4] = {0, 0, 0, 0};
    for (std::uint32_t i = 0; i < 0x1a; ++i) hashed[(i + location) & 3] ^= header[i];
    for (std::uint32_t j = 0; j < 4; ++j) {
        const auto i = j + 0x1a + location;
        header[0x1a + j] = static_cast<std::uint8_t>(hashed[i & 3] ^ encoded_offset_bytes[i & 3]);
    }
    return header;
}

CascStorage::CascStorage(std::filesystem::path data_dir) : directory_(std::move(data_dir) / "data") {}

std::filesystem::path CascStorage::archive_path(std::uint16_t archive) const {
    return directory_ / archive_file_name(archive);
}

void CascStorage::open() {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);
    entries_.clear();
    versions_.fill(0);
    dirty_.fill(false);
    std::array<std::optional<std::filesystem::path>, kCascBuckets> newest;
    std::optional<std::uint16_t> last_archive;
    for (const auto& item : std::filesystem::directory_iterator(directory_)) {
        if (!item.is_regular_file()) continue;
        const auto name = item.path().filename().string();
        if (name.size() == 14 && name.compare(10, 4, ".idx") == 0 && is_hex_hash(name.substr(0, 10), 5)) {
            const auto bucket = static_cast<std::uint8_t>(std::stoul(name.substr(0, 2), nullptr, 16));
            const auto version = static_cast<std::uint32_t>(std::stoul(name.substr(2, 8), nullptr, 16));
            if (bucket < kCascBuckets && version >= versions_[bucket]) {
                versions_[bucket] = version;
                newest[bucket] = item.path();
            }
        } else if (name.size() == 8 && name.rfind("data.", 0) == 0 &&
                   std::all_of(name.begin() + 5, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            const auto number = static_cast<std::uint16_t>(std::stoul(name.substr(5)));
            if (!last_archive || number > *last_archive) last_archive = number;
        }
    }
    for (std::size_t bucket = 0; bucket < kCascBuckets; ++bucket) {
        if (!newest[bucket]) {
            dirty_[bucket] = true;   // make sure every bucket gets a journal on commit
            continue;
        }
        for (auto& entry : decode_casc_index(read_file(*newest[bucket]), static_cast<std::uint8_t>(bucket))) {
            entries_.insert_or_assign(entry.key, entry);
        }
    }
    current_archive_ = last_archive.value_or(0);
    current_offset_ = 0;
    salvaged_ = 0;
    // Walk every archive and re-index objects whose journal entry was lost
    // (the run was interrupted before commit()). Each object starts with a
    // 30-byte header that carries the reversed EKey, the total size and a
    // Jenkins checksum of its first 0x16 bytes, which is enough to tell a
    // complete object from a partial tail. A partial tail is cut off so the
    // next append reuses the space instead of leaving garbage behind.
    if (last_archive) {
        for (std::uint16_t archive = 0; archive <= *last_archive; ++archive) {
            const auto path = archive_path(archive);
            std::error_code error;
            if (!std::filesystem::exists(path, error)) continue;
            const std::uint64_t file_size = std::filesystem::file_size(path, error);
            if (error) continue;
            std::ifstream input(path, std::ios::binary);
            std::uint64_t offset = 0;
            std::vector<std::uint8_t> header(kCascDataHeaderSize);
            while (offset + kCascDataHeaderSize <= file_size) {
                input.seekg(static_cast<std::streamoff>(offset));
                input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
                if (!input) break;
                const std::uint32_t total = get_u32le(header, 0x10);
                const std::uint32_t checksum = get_u32le(header, 0x16);
                if (total < kCascDataHeaderSize || offset + total > file_size ||
                    checksum != jenkins_hashlittle(header.data(), 0x16, kDataHeaderSeed)) {
                    break;   // partial or foreign data: everything from here on is unusable
                }
                CascEntry entry;
                for (std::size_t i = 0; i < kCascKeyBytes; ++i) entry.key[i] = header[15 - i];
                entry.archive = archive;
                entry.offset = static_cast<std::uint32_t>(offset);
                entry.size = total;
                if (entries_.emplace(entry.key, entry).second) {
                    dirty_[casc_bucket(entry.key)] = true;
                    ++salvaged_;
                }
                offset += total;
            }
            input.close();
            if (offset < file_size) std::filesystem::resize_file(path, offset, error);
            if (archive == *last_archive) current_offset_ = offset;
        }
    }
    opened_ = true;
}

bool CascStorage::contains(const std::string& encoding_key_hex) const {
    std::lock_guard lock(mutex_);
    return entries_.contains(casc_key(encoding_key_hex));
}

std::optional<CascEntry> CascStorage::find(const std::string& encoding_key_hex) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(casc_key(encoding_key_hex));
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::size_t CascStorage::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void CascStorage::select_archive_locked(std::uint64_t needed) {
    if (needed > kCascMaxArchiveSize) throw std::runtime_error("encoded object exceeds the CASC archive size limit");
    if (current_offset_ + needed > kCascMaxArchiveSize) {
        ++current_archive_;
        current_offset_ = 0;
        if (current_archive_ >= (1u << 10)) throw std::runtime_error("CASC storage exhausted the archive numbering");
    }
}

bool CascStorage::append(const std::string& encoding_key_hex, const std::vector<std::uint8_t>& encoded) {
    if (!is_hex_hash(encoding_key_hex, 16)) throw std::runtime_error("CASC append needs a full 16-byte EKey");
    const auto key = casc_key(encoding_key_hex);
    const auto ekey_bytes = hex_to_bytes(encoding_key_hex);
    std::array<std::uint8_t, 16> ekey{};
    std::copy(ekey_bytes.begin(), ekey_bytes.end(), ekey.begin());

    std::lock_guard lock(mutex_);
    if (!opened_) throw std::logic_error("CascStorage::open() was not called");
    if (entries_.contains(key)) return false;
    const std::uint64_t needed = kCascDataHeaderSize + encoded.size();
    select_archive_locked(needed);
    const auto offset = static_cast<std::uint32_t>(current_offset_);
    const auto header = encode_casc_data_header(ekey, static_cast<std::uint32_t>(encoded.size()),
                                                current_archive_, offset);

    const auto path = archive_path(current_archive_);
    std::FILE* file = std::fopen(path.c_str(), std::filesystem::exists(path) ? "r+b" : "w+b");
    if (file == nullptr) throw std::runtime_error("cannot open CASC archive: " + path.string());
    bool ok = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
              std::fwrite(header.data(), 1, header.size(), file) == header.size() &&
              (encoded.empty() || std::fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size()) &&
              std::fflush(file) == 0;
    ok = (std::fclose(file) == 0) && ok;
    if (!ok) {
        // Leave the partially written tail unreferenced; it will be overwritten
        // by the next append at the same offset.
        throw std::runtime_error("failed writing CASC archive: " + path.string());
    }

    CascEntry entry;
    entry.key = key;
    entry.archive = current_archive_;
    entry.offset = offset;
    entry.size = static_cast<std::uint32_t>(needed);
    entries_.emplace(key, entry);
    dirty_[casc_bucket(key)] = true;
    current_offset_ += needed;
    return true;
}

bool CascStorage::erase(const std::string& encoding_key_hex) {
    std::lock_guard lock(mutex_);
    const auto key = casc_key(encoding_key_hex);
    if (entries_.erase(key) == 0) return false;
    dirty_[casc_bucket(key)] = true;
    return true;
}

void CascStorage::commit() {
    std::lock_guard lock(mutex_);
    if (!opened_) throw std::logic_error("CascStorage::open() was not called");
    std::array<std::vector<CascEntry>, kCascBuckets> buckets;
    for (const auto& [key, entry] : entries_) {
        const auto bucket = casc_bucket(key);
        if (dirty_[bucket]) buckets[bucket].push_back(entry);
    }
    for (std::size_t bucket = 0; bucket < kCascBuckets; ++bucket) {
        if (!dirty_[bucket]) continue;
        const auto version = versions_[bucket] + 1;
        const auto bytes = encode_casc_index(static_cast<std::uint8_t>(bucket), std::move(buckets[bucket]));
        write_atomic(directory_ / index_file_name(static_cast<std::uint8_t>(bucket), version), bytes);
        // Drop superseded journals of this bucket.
        for (const auto& item : std::filesystem::directory_iterator(directory_)) {
            const auto name = item.path().filename().string();
            if (name.size() == 14 && name.compare(10, 4, ".idx") == 0 && is_hex_hash(name.substr(0, 10), 5) &&
                std::stoul(name.substr(0, 2), nullptr, 16) == bucket &&
                std::stoul(name.substr(2, 8), nullptr, 16) != version) {
                std::error_code error;
                std::filesystem::remove(item.path(), error);
            }
        }
        versions_[bucket] = version;
        dirty_[bucket] = false;
    }
}

std::vector<std::uint8_t> CascStorage::read(const CascEntry& entry) const {
    if (entry.size < kCascDataHeaderSize) throw std::runtime_error("CASC entry smaller than its header");
    std::ifstream input(archive_path(entry.archive), std::ios::binary);
    if (!input) throw std::runtime_error("cannot open CASC archive " + archive_file_name(entry.archive));
    input.seekg(entry.offset);
    std::vector<std::uint8_t> block(entry.size);
    input.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
    if (!input) throw std::runtime_error("short read from CASC archive " + archive_file_name(entry.archive));
    for (std::size_t i = 0; i < kCascKeyBytes; ++i) {
        if (block[15 - i] != entry.key[i]) throw std::runtime_error("CASC data header key mismatch");
    }
    if (get_u32le(block, 0x10) != entry.size) throw std::runtime_error("CASC data header size mismatch");
    block.erase(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(kCascDataHeaderSize));
    return block;
}

std::string format_build_info(const BuildInfoRecord& record) {
    std::ostringstream out;
    out << "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16|Install Key!HEX:16|IM Size!DEC:4|"
           "CDN Path!STRING:0|CDN Hosts!STRING:0|CDN Servers!STRING:0|Tags!STRING:0|Armadillo!STRING:0|"
           "Last Activated!STRING:0|Version!STRING:0|KeyRing!HEX:16|Product!STRING:0\n";
    const auto join = [](const std::vector<std::string>& values) {
        std::string joined;
        for (const auto& value : values) {
            if (!joined.empty()) joined.push_back(' ');
            joined += value;
        }
        return joined;
    };
    out << record.branch << "|1|" << record.build_key << '|' << record.cdn_key << '|' << record.install_key << '|'
        << record.install_size << '|' << record.cdn_path << '|' << join(record.cdn_hosts) << '|'
        << join(record.cdn_servers) << '|' << record.tags << "|||" << record.version << "||" << record.product << '\n';
    return out.str();
}

std::string build_info_tags(const std::string& region, const std::string& locale) {
    std::string upper = region;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    return "Windows x86_64 " + upper + "? " + locale + " speech?:Windows x86_64 " + upper + " " + locale + " text?";
}

void write_casc_config(const std::filesystem::path& data_dir, const std::string& hash,
                       const std::vector<std::uint8_t>& bytes) {
    if (!is_hex_hash(hash, 16)) throw std::runtime_error("config hash is not a 16-byte hex key: " + hash);
    write_atomic(data_dir / "config" / hash.substr(0, 2) / hash.substr(2, 2) / hash, bytes);
}

} // namespace openblizz
