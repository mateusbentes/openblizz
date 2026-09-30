#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openblizz {

[[nodiscard]] std::string md5_hex(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::string md5_hex(const std::string& data);
[[nodiscard]] std::string sha256_hex(const std::vector<std::uint8_t>& data);
[[nodiscard]] bool is_hex_hash(const std::string& value, std::size_t bytes);

} // namespace openblizz
