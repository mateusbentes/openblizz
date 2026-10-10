#pragma once

#include "openblizz/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace openblizz {

[[nodiscard]] std::vector<std::string> split(const std::string& value, char delimiter);
[[nodiscard]] std::string trim(std::string value);
[[nodiscard]] ConfigFile parse_config(const std::string& text);
[[nodiscard]] KeyRing parse_keyring(const ConfigFile& config);

class BlteDecoder {
public:
    [[nodiscard]] static std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& encoded);
    [[nodiscard]] static std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& encoded,
                                                           const KeyRing& keyring);
};

[[nodiscard]] InstallManifest parse_install_manifest(const std::vector<std::uint8_t>& decoded);
[[nodiscard]] DownloadManifest parse_download_manifest(const std::vector<std::uint8_t>& decoded);

class EncodingIndex {
public:
    [[nodiscard]] static EncodingIndex parse(const std::vector<std::uint8_t>& decoded);
    [[nodiscard]] const FileMapping* find(const std::string& content_key) const;
    [[nodiscard]] const std::unordered_map<std::string, FileMapping>& mappings() const noexcept {
        return mappings_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return mappings_.size(); }
    // EKey -> encoded size, from the EKey spec pages. Lists every encoded
    // object of the build with its full 16-byte key.
    [[nodiscard]] const std::unordered_map<std::string, std::uint64_t>& encoded_sizes() const noexcept {
        return encoded_sizes_;
    }

private:
    std::unordered_map<std::string, FileMapping> mappings_;
    std::unordered_map<std::string, std::uint64_t> encoded_sizes_;
};

class ArchiveIndex {
public:
    [[nodiscard]] static ArchiveIndex parse(const std::vector<std::uint8_t>& bytes);
    [[nodiscard]] const ArchiveLocation* find(const std::string& encoding_key) const;
    [[nodiscard]] const std::unordered_map<std::string, ArchiveLocation>& entries() const noexcept {
        return entries_;
    }

private:
    std::unordered_map<std::string, ArchiveLocation> entries_;
};

} // namespace openblizz
