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

class BlteDecoder {
public:
    [[nodiscard]] static std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& encoded);
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

private:
    std::unordered_map<std::string, FileMapping> mappings_;
};

} // namespace openblizz
