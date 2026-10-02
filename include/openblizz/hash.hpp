#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openblizz {

[[nodiscard]] std::string md5_hex(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::string md5_hex(const std::string& data);
[[nodiscard]] std::string sha256_hex(const std::vector<std::uint8_t>& data);
[[nodiscard]] bool is_hex_hash(const std::string& value, std::size_t bytes);
[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(const std::string& hex);
[[nodiscard]] std::string bytes_to_hex(const std::uint8_t* data, std::size_t size);

// Bob Jenkins' lookup3 hashes, used by CASC index and data file headers.
[[nodiscard]] std::uint32_t jenkins_hashlittle(const void* data, std::size_t size, std::uint32_t init);
void jenkins_hashlittle2(const void* data, std::size_t size, std::uint32_t& pc, std::uint32_t& pb);

} // namespace openblizz
