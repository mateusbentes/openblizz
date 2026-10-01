#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

void put_u32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 24));
    data.push_back(static_cast<std::uint8_t>(value >> 16));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u32le_at(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>(value);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    data[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void put_u16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u40(std::vector<std::uint8_t>& data, std::uint64_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 32));
    put_u32(data, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> single_chunk(const std::string& payload) {
    std::vector<std::uint8_t> encoded{'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N'};
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

std::vector<std::uint8_t> multi_chunk(const std::string& payload) {
    std::vector<std::uint8_t> encoded{'B', 'L', 'T', 'E'};
    put_u32(encoded, 36); // 12-byte preamble + one 24-byte chunk record
    encoded.push_back(0x0f);
    encoded.push_back(0);
    encoded.push_back(0);
    encoded.push_back(1);
    put_u32(encoded, static_cast<std::uint32_t>(payload.size() + 1));
    put_u32(encoded, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> compressed{'N'};
    compressed.insert(compressed.end(), payload.begin(), payload.end());
    const auto digest = openblizz::md5_hex(compressed);
    for (std::size_t i = 0; i < digest.size(); i += 2) {
        encoded.push_back(static_cast<std::uint8_t>(std::stoul(digest.substr(i, 2), nullptr, 16)));
    }
    encoded.push_back('N');
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

} // namespace

int main() {
    assert(openblizz::md5_hex(std::string("")) == "d41d8cd98f00b204e9800998ecf8427e");
    assert(openblizz::sha256_hex(std::vector<std::uint8_t>{'a'}) ==
           "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");

    const auto plain = std::string("openblizz");
    assert(openblizz::BlteDecoder::decode(single_chunk(plain)) ==
           std::vector<std::uint8_t>(plain.begin(), plain.end()));
    assert(openblizz::BlteDecoder::decode(multi_chunk(plain)) ==
           std::vector<std::uint8_t>(plain.begin(), plain.end()));

    const auto config = openblizz::parse_config("root = aa bb\n# comment\nname = Warcraft\n");
    assert(config.get("root").size() == 2);
    assert(config.get("name").front() == "Warcraft");

    const std::vector<std::uint8_t> install{
        'I','N',1,16, 0,1, 0,0,0,1,
        'e','n','U','S',0, 0,1, 0x80,
        'a','.', 'b', 0,
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        0,0,0,4
    };
    const auto parsed = openblizz::parse_install_manifest(install);
    assert(parsed.entries.size() == 1);
    assert(parsed.entries.front().path == "a.b");
    assert(parsed.entries.front().file_size == 4);

    std::vector<std::uint8_t> encoding{'E','N',1,16,16};
    put_u16(encoding, 1);
    put_u16(encoding, 1);
    put_u32(encoding, 1);
    put_u32(encoding, 1);
    encoding.push_back(0); // flags
    put_u32(encoding, 0); // ESpec block size
    encoding.resize(22 + 32 + 1024 + 32 + 1024, 0);
    auto offset = static_cast<std::size_t>(22 + 32);
    encoding[offset++] = 2; // two equivalent EKeys
    put_u40(encoding, 4);
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = i;
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = static_cast<std::uint8_t>(0x10 + i);
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = static_cast<std::uint8_t>(0x20 + i);
    const auto parsed_encoding = openblizz::EncodingIndex::parse(encoding);
    assert(parsed_encoding.mappings().size() == 1);
    assert(parsed_encoding.mappings().begin()->second.encoding_keys.size() == 2);

    std::vector<std::uint8_t> archive(4096 + 16 + 8 + 28, 0);
    for (std::size_t i = 0; i < 16; ++i) archive[i] = static_cast<std::uint8_t>(0xa0 + i);
    archive[18] = 0x12;
    archive[19] = 0x34;
    archive[20] = 1;
    archive[21] = 2;
    archive[22] = 3;
    archive[23] = 4;
    const auto footer = archive.size() - 28;
    archive[footer + 8] = 1;
    archive[footer + 11] = 4;
    archive[footer + 12] = 4;
    archive[footer + 13] = 4;
    archive[footer + 14] = 16;
    archive[footer + 15] = 8;
    put_u32le_at(archive, footer + 16, 1);
    const auto parsed_archive = openblizz::ArchiveIndex::parse(archive);
    const auto* archive_entry = parsed_archive.find("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    assert(archive_entry != nullptr);
    assert(archive_entry->offset == 0x01020304);
    assert(archive_entry->encoded_size == 0x1234);

    std::cout << "OpenBlizz core tests passed\n";
    return 0;
}
