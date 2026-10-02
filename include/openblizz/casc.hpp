#pragma once

#include "openblizz/types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openblizz {

// Local CASC storage ("static storage" in CascLib terms): `Data/data/data.NNN`
// archives holding 30-byte headers followed by the encoded (BLTE) object, and
// 16 bucketed `Data/data/<bucket><version>.idx` journals mapping the first 9
// bytes of an EKey to (archive, offset, size). Format reference:
// https://wowdev.wiki/CASC and CascLib's CascIndexFiles.cpp / CascReadFile.cpp.
constexpr std::size_t kCascKeyBytes = 9;
constexpr std::size_t kCascBuckets = 16;
constexpr std::size_t kCascDataHeaderSize = 30;
constexpr std::uint64_t kCascMaxArchiveSize = 0x40000000ull;   // 30-bit offsets

using CascKey = std::array<std::uint8_t, kCascKeyBytes>;

struct CascEntry {
    CascKey key{};
    std::uint16_t archive{};
    std::uint32_t offset{};
    std::uint32_t size{};   // header + encoded object, as stored in the .idx
};

[[nodiscard]] CascKey casc_key(const std::string& encoding_key_hex);
[[nodiscard]] std::uint8_t casc_bucket(const CascKey& key);

// Encodes/decodes a single .idx journal (version 7, guarded blocks).
[[nodiscard]] std::vector<std::uint8_t> encode_casc_index(std::uint8_t bucket, std::vector<CascEntry> entries);
[[nodiscard]] std::vector<CascEntry> decode_casc_index(const std::vector<std::uint8_t>& bytes, std::uint8_t bucket);

// Builds the 30-byte header that precedes an encoded object inside data.NNN.
[[nodiscard]] std::array<std::uint8_t, kCascDataHeaderSize> encode_casc_data_header(
    const std::array<std::uint8_t, 16>& encoding_key, std::uint32_t encoded_size,
    std::uint16_t archive, std::uint32_t offset);

class CascStorage {
public:
    // `data_dir` is the game's `Data` directory; the storage lives in `Data/data`.
    explicit CascStorage(std::filesystem::path data_dir);

    // Loads the newest .idx of every bucket and sizes the existing archives.
    void open();

    [[nodiscard]] bool contains(const std::string& encoding_key_hex) const;
    [[nodiscard]] std::optional<CascEntry> find(const std::string& encoding_key_hex) const;
    [[nodiscard]] std::size_t size() const;

    // Appends an encoded object. Thread-safe. Returns false when already present.
    bool append(const std::string& encoding_key_hex, const std::vector<std::uint8_t>& encoded);

    // Forgets a journal entry so the object can be appended again (the old
    // bytes stay in the archive as unreferenced space). Returns false if absent.
    bool erase(const std::string& encoding_key_hex);

    // Writes new .idx journals for every bucket touched since open()/commit().
    void commit();

    // Reads the encoded object (without the 30-byte header) back from the archives.
    [[nodiscard]] std::vector<std::uint8_t> read(const CascEntry& entry) const;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    [[nodiscard]] std::filesystem::path archive_path(std::uint16_t archive) const;
    void select_archive_locked(std::uint64_t needed);

    std::filesystem::path directory_;
    std::map<CascKey, CascEntry> entries_;
    std::array<std::uint32_t, kCascBuckets> versions_{};
    std::array<bool, kCascBuckets> dirty_{};
    std::uint16_t current_archive_{};
    std::uint64_t current_offset_{};
    bool opened_{false};
    mutable std::mutex mutex_;
};

// `.build.info` and `Data/config` helpers: the executable discovers the build
// through the CSV `.build.info` in the install root and reads the build/cdn
// configs from `Data/config/xx/yy/<hash>` (CascLib: CascFiles.cpp).
struct BuildInfoRecord {
    std::string branch;          // region, e.g. "us"
    std::string build_key;
    std::string cdn_key;
    std::string install_key;
    std::uint64_t install_size{};
    std::string cdn_path;
    std::vector<std::string> cdn_hosts;
    std::vector<std::string> cdn_servers;
    std::string tags;            // "Windows x86_64 US? enUS speech?:Windows x86_64 US enUS text?"
    std::string version;
    std::string product;
};

[[nodiscard]] std::string format_build_info(const BuildInfoRecord& record);
[[nodiscard]] std::string build_info_tags(const std::string& region, const std::string& locale);
void write_casc_config(const std::filesystem::path& data_dir, const std::string& hash,
                       const std::vector<std::uint8_t>& bytes);

} // namespace openblizz
