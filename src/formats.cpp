#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"

#include <lz4.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace openblizz {
namespace {

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("truncated u32");
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

std::uint32_t u32le(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("truncated little-endian u32");
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint16_t u16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("truncated u16");
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

std::uint64_t u40(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 5 > data.size()) throw std::runtime_error("truncated u40");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 5; ++i) value = (value << 8) | data[offset + i];
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

void ensure_available(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t length,
                      const std::string& what) {
    if (offset > data.size() || length > data.size() - offset) {
        throw std::runtime_error("truncated " + what);
    }
}

std::string read_cstring(const std::vector<std::uint8_t>& data, std::size_t& offset,
                         const std::string& what) {
    const auto end = std::find(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end(), 0);
    if (end == data.end()) throw std::runtime_error("unterminated " + what);
    const auto length = static_cast<std::size_t>(std::distance(data.begin() + static_cast<std::ptrdiff_t>(offset), end));
    std::string value(reinterpret_cast<const char*>(data.data() + offset), length);
    offset += length + 1;
    return value;
}

std::vector<std::uint8_t> decompress_zlib(const std::uint8_t* bytes, std::size_t size,
                                          std::size_t expected) {
    constexpr std::size_t max_output = 1ull << 30;
    std::size_t capacity = std::max<std::size_t>(expected, std::max<std::size_t>(size * 2, 4096));
    while (capacity <= max_output) {
        std::vector<std::uint8_t> result(capacity);
        uLongf output_size = static_cast<uLongf>(capacity);
        const auto code = ::uncompress(result.data(), &output_size, bytes, static_cast<uLong>(size));
        if (code == Z_OK) {
            result.resize(output_size);
            return result;
        }
        if (code != Z_BUF_ERROR) throw std::runtime_error("BLTE zlib decompression failed");
        capacity *= 2;
    }
    throw std::runtime_error("BLTE zlib output exceeds safety limit");
}

std::uint32_t rotate_left(const std::uint32_t value, const int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::uint32_t load_le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void salsa20_xor(std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& key,
                 const std::uint8_t* iv, const std::size_t iv_size, const std::size_t block_index) {
    if (key.size() != 16) throw std::runtime_error("BLTE Salsa20 key must be 16 bytes");
    if (iv_size != 4 && iv_size != 8) throw std::runtime_error("BLTE Salsa20 IV must be 4 or 8 bytes");

    std::array<std::uint8_t, 8> nonce{};
    std::copy_n(iv, iv_size, nonce.begin());
    const auto block = static_cast<std::uint32_t>(block_index);
    for (std::size_t i = 0; i < 4; ++i) {
        nonce[i] ^= static_cast<std::uint8_t>((block >> (8 * i)) & 0xffu);
    }

    std::array<std::uint32_t, 16> state{};
    state[0] = 0x61707865u;  // "expa"
    state[5] = 0x3120646eu;  // "nd 1"
    state[10] = 0x79622d36u; // "6-by"
    state[15] = 0x6b206574u; // "te k"
    for (std::size_t i = 0; i < 4; ++i) {
        state[1 + i] = load_le32(key.data() + 4 * i);
        state[11 + i] = state[1 + i];
    }
    state[6] = load_le32(nonce.data());
    state[7] = load_le32(nonce.data() + 4);
    state[8] = 0;
    state[9] = 0;

    std::size_t position = 0;
    while (position < data.size()) {
        auto working = state;
        const auto quarter_round = [&](const std::size_t a, const std::size_t b,
                                       const std::size_t c, const std::size_t d) {
            working[b] ^= rotate_left(working[a] + working[d], 7);
            working[c] ^= rotate_left(working[b] + working[a], 9);
            working[d] ^= rotate_left(working[c] + working[b], 13);
            working[a] ^= rotate_left(working[d] + working[c], 18);
        };
        for (int round = 0; round < 10; ++round) {
            quarter_round(0, 4, 8, 12);
            quarter_round(5, 9, 13, 1);
            quarter_round(10, 14, 2, 6);
            quarter_round(15, 3, 7, 11);
            quarter_round(0, 1, 2, 3);
            quarter_round(5, 6, 7, 4);
            quarter_round(10, 11, 8, 9);
            quarter_round(15, 12, 13, 14);
        }

        const auto available = std::min<std::size_t>(64, data.size() - position);
        for (std::size_t i = 0; i < available; ++i) {
            const auto word = working[i / 4] + state[i / 4];
            const auto stream_byte = static_cast<std::uint8_t>((word >> (8 * (i % 4))) & 0xffu);
            data[position + i] ^= stream_byte;
        }
        position += available;
        state[8]++;
        if (state[8] == 0) ++state[9];
    }
}

void arc4_xor(std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& key) {
    if (key.empty() || key.size() > 256) throw std::runtime_error("BLTE ARC4 key has invalid size");
    std::array<std::uint8_t, 256> state{};
    for (std::size_t i = 0; i < state.size(); ++i) state[i] = static_cast<std::uint8_t>(i);
    std::size_t j = 0;
    for (std::size_t i = 0; i < state.size(); ++i) {
        j = (j + state[i] + key[i % key.size()]) & 0xffu;
        std::swap(state[i], state[j]);
    }
    std::size_t i = 0;
    j = 0;
    for (auto& byte : data) {
        i = (i + 1) & 0xffu;
        j = (j + state[i]) & 0xffu;
        std::swap(state[i], state[j]);
        const auto stream = state[(state[i] + state[j]) & 0xffu];
        byte ^= stream;
    }
}

std::vector<std::uint8_t> decode_chunk(const std::uint8_t* bytes, std::size_t size,
                                       std::size_t expected, const KeyRing& keyring,
                                       const std::size_t block_index, const std::size_t depth) {
    if (depth > 2) throw std::runtime_error("nested BLTE encryption is too deep");
    if (size == 0) throw std::runtime_error("empty BLTE chunk");
    const char mode = static_cast<char>(bytes[0]);
    const auto* payload = bytes + 1;
    const auto payload_size = size - 1;
    if (mode == 'N') {
        return std::vector<std::uint8_t>(payload, payload + payload_size);
    }
    if (mode == 'Z') return decompress_zlib(payload, payload_size, expected);
    if (mode == '4') {
        if (payload_size < 8) throw std::runtime_error("BLTE LZ4 chunk is missing its size header");
        std::uint64_t declared_size = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            declared_size |= static_cast<std::uint64_t>(payload[i]) << (8 * i);
        }
        if (declared_size > (1ull << 30) ||
            (expected != 0 && declared_size != expected)) {
            throw std::runtime_error("BLTE LZ4 decoded size mismatch");
        }
        std::vector<std::uint8_t> result(static_cast<std::size_t>(declared_size));
        const auto decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(payload + 8),
                                                  reinterpret_cast<char*>(result.data()),
                                                  static_cast<int>(payload_size - 8),
                                                  static_cast<int>(declared_size));
        if (decoded < 0 || static_cast<std::size_t>(decoded) != declared_size) {
            throw std::runtime_error("BLTE LZ4 decompression failed");
        }
        result.resize(static_cast<std::size_t>(decoded));
        return result;
    }
    if (mode == 'F') {
        throw std::runtime_error("BLTE frame mode is not yet supported");
    }
    if (mode == 'E') {
        if (keyring.empty()) throw std::runtime_error("encrypted BLTE content has no KeyRing entry");
        if (payload_size < 1) throw std::runtime_error("encrypted BLTE chunk is missing its key name size");
        std::size_t offset = 0;
        const auto key_name_size = static_cast<std::size_t>(payload[offset++]);
        if (key_name_size == 0 || payload_size - offset < key_name_size + 1) {
            throw std::runtime_error("encrypted BLTE chunk has a truncated key name");
        }
        const auto key_name = hex_bytes(payload + offset, key_name_size);
        offset += key_name_size;
        const auto iv_size = static_cast<std::size_t>(payload[offset++]);
        if (iv_size != 4 && iv_size != 8) throw std::runtime_error("encrypted BLTE IV must be 4 or 8 bytes");
        if (payload_size - offset < iv_size + 1) throw std::runtime_error("encrypted BLTE chunk has a truncated IV");
        const auto* iv = payload + offset;
        offset += iv_size;
        const auto encryption_type = payload[offset++];
        const auto key = keyring.find(key_name);
        if (key == keyring.end()) throw std::runtime_error("BLTE encryption key not found: " + key_name);
        std::vector<std::uint8_t> decrypted(payload + offset, payload + payload_size);
        if (encryption_type == 'S') {
            salsa20_xor(decrypted, key->second, iv, iv_size, block_index);
        } else if (encryption_type == 'A') {
            arc4_xor(decrypted, key->second);
        } else {
            throw std::runtime_error("unsupported BLTE encryption type");
        }
        if (decrypted.empty()) throw std::runtime_error("encrypted BLTE chunk has no inner payload");
        if (decrypted[0] == 'E') throw std::runtime_error("nested encrypted BLTE chunks are not supported");
        return decode_chunk(decrypted.data(), decrypted.size(), expected, keyring, block_index, depth + 1);
    }
    throw std::runtime_error(std::string("unsupported BLTE chunk mode: ") + mode);
}

} // namespace

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> result;
    std::string current;
    for (const auto ch : value) {
        if (ch == delimiter) {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    result.push_back(current);
    return result;
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

ConfigFile parse_config(const std::string& text) {
    ConfigFile config;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty()) continue;
        config.values[key] = split(value, ' ');
        auto& values = config.values[key];
        values.erase(std::remove_if(values.begin(), values.end(), [](const auto& item) { return item.empty(); }), values.end());
    }
    return config;
}

KeyRing parse_keyring(const ConfigFile& config) {
    KeyRing result;
    for (const auto& [name, values] : config.values) {
        if (name.rfind("key-", 0) != 0) continue;
        auto key_id = name.substr(4);
        std::transform(key_id.begin(), key_id.end(), key_id.begin(), [](const unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!is_hex_hash(key_id, 8) || values.size() != 1 || !is_hex_hash(values.front(), 16)) {
            throw std::runtime_error("invalid KeyRing entry: " + name);
        }
        result.emplace(key_id, hex_to_bytes(values.front()));
    }
    return result;
}

std::vector<std::uint8_t> BlteDecoder::decode(const std::vector<std::uint8_t>& encoded) {
    static const KeyRing empty_keyring;
    return decode(encoded, empty_keyring);
}

std::vector<std::uint8_t> BlteDecoder::decode(const std::vector<std::uint8_t>& encoded,
                                              const KeyRing& keyring) {
    ensure_available(encoded, 0, 8, "BLTE preamble");
    if (std::string(reinterpret_cast<const char*>(encoded.data()), 4) != "BLTE") {
        throw std::runtime_error("invalid BLTE magic");
    }
    const auto header_size = u32(encoded, 4);
    std::vector<std::uint8_t> output;

    if (header_size == 0) {
        const auto decoded = decode_chunk(encoded.data() + 8, encoded.size() - 8, encoded.size() - 8,
                                          keyring, 0, 0);
        output.insert(output.end(), decoded.begin(), decoded.end());
        return output;
    }

    ensure_available(encoded, 8, 4, "BLTE extended header");
    if (header_size < 12 || header_size > encoded.size()) throw std::runtime_error("invalid BLTE header size");
    if (encoded[8] != 0x0f) throw std::runtime_error("invalid BLTE extended header marker");
    const auto chunk_count = (static_cast<std::uint32_t>(encoded[9]) << 16) |
                             (static_cast<std::uint32_t>(encoded[10]) << 8) | encoded[11];
    const auto table_end = 12ull + static_cast<std::uint64_t>(chunk_count) * 24ull;
    if (table_end != header_size) throw std::runtime_error("BLTE chunk table size mismatch");

    struct ChunkInfo { std::uint32_t compressed{}; std::uint32_t decoded{}; std::string checksum; };
    std::vector<ChunkInfo> chunks;
    chunks.reserve(chunk_count);
    std::size_t offset = 12;
    for (std::uint32_t i = 0; i < chunk_count; ++i) {
        ChunkInfo info;
        info.compressed = u32(encoded, offset);
        info.decoded = u32(encoded, offset + 4);
        info.checksum = hex_bytes(encoded.data() + offset + 8, 16);
        chunks.push_back(info);
        offset += 24;
    }

    std::size_t data_offset = header_size;
    for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
        const auto& chunk = chunks[chunk_index];
        ensure_available(encoded, data_offset, chunk.compressed, "BLTE chunk");
        auto decoded = decode_chunk(encoded.data() + data_offset, chunk.compressed, chunk.decoded,
                                    keyring, chunk_index, 0);
        if (decoded.size() != chunk.decoded) {
            throw std::runtime_error("BLTE decoded size mismatch");
        }
        const std::vector<std::uint8_t> compressed_chunk(
            encoded.begin() + static_cast<std::ptrdiff_t>(data_offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(data_offset + chunk.compressed));
        if (md5_hex(compressed_chunk) != chunk.checksum) {
            throw std::runtime_error("BLTE chunk checksum mismatch");
        }
        output.insert(output.end(), decoded.begin(), decoded.end());
        data_offset += chunk.compressed;
    }
    return output;
}

InstallManifest parse_install_manifest(const std::vector<std::uint8_t>& decoded) {
    ensure_available(decoded, 0, 10, "install manifest header");
    if (decoded[0] != 'I' || decoded[1] != 'N') throw std::runtime_error("invalid install manifest magic");
    InstallManifest manifest;
    manifest.version = decoded[2];
    manifest.ckey_size = decoded[3];
    if (manifest.version == 0 || manifest.version > 2 || manifest.ckey_size == 0) {
        throw std::runtime_error("unsupported install manifest version or key size");
    }
    const auto tag_count = u16(decoded, 4);
    manifest.entry_count = u32(decoded, 6);
    std::size_t offset = 10;
    if (manifest.version >= 2) {
        ensure_available(decoded, offset, 6, "install manifest v2 header");
        offset += 6;
    }
    const auto mask_size = (static_cast<std::size_t>(manifest.entry_count) + 7) / 8;
    manifest.tags.reserve(tag_count);
    for (std::uint16_t i = 0; i < tag_count; ++i) {
        InstallTag tag;
        tag.name = read_cstring(decoded, offset, "install tag");
        ensure_available(decoded, offset, 2 + mask_size, "install tag data");
        tag.type = u16(decoded, offset);
        offset += 2;
        tag.bitmap.assign(decoded.begin() + static_cast<std::ptrdiff_t>(offset),
                          decoded.begin() + static_cast<std::ptrdiff_t>(offset + mask_size));
        offset += mask_size;
        manifest.tags.push_back(std::move(tag));
    }
    manifest.entries.reserve(manifest.entry_count);
    for (std::uint32_t i = 0; i < manifest.entry_count; ++i) {
        InstallEntry entry;
        entry.path = read_cstring(decoded, offset, "install path");
        ensure_available(decoded, offset, manifest.ckey_size + 4, "install file entry");
        entry.content_key = hex_bytes(decoded.data() + offset, manifest.ckey_size);
        offset += manifest.ckey_size;
        entry.file_size = u32(decoded, offset);
        offset += 4;
        if (manifest.version >= 2) {
            entry.file_type = decoded[offset++];
        }
        manifest.entries.push_back(std::move(entry));
    }
    return manifest;
}

DownloadManifest parse_download_manifest(const std::vector<std::uint8_t>& decoded) {
    ensure_available(decoded, 0, 11, "download manifest header");
    if (decoded[0] != 'D' || decoded[1] != 'L') throw std::runtime_error("invalid download manifest magic");
    DownloadManifest manifest;
    manifest.version = decoded[2];
    manifest.ekey_size = decoded[3];
    manifest.has_checksum = decoded[4] != 0;
    manifest.entry_count = u32(decoded, 5);
    manifest.tag_count = u16(decoded, 9);
    std::size_t offset = 11;
    if (manifest.version >= 2) {
        ensure_available(decoded, offset, 1, "download v2 header");
        manifest.flag_size = decoded[offset++];
    }
    if (manifest.version >= 3) {
        ensure_available(decoded, offset, 4, "download v3 header");
        manifest.base_priority = static_cast<std::int8_t>(decoded[offset]);
        offset += 4;
    }
    if (manifest.version == 0 || manifest.version > 3 || manifest.ekey_size == 0 || manifest.flag_size > 4) {
        throw std::runtime_error("unsupported download manifest header");
    }

    manifest.entries.reserve(manifest.entry_count);
    for (std::uint32_t i = 0; i < manifest.entry_count; ++i) {
        ensure_available(decoded, offset, manifest.ekey_size + 6 + (manifest.has_checksum ? 4 : 0) + manifest.flag_size,
                         "download entry");
        DownloadEntry entry;
        entry.encoding_key = hex_bytes(decoded.data() + offset, manifest.ekey_size);
        offset += manifest.ekey_size;
        entry.file_size = u40(decoded, offset);
        offset += 5;
        entry.priority = static_cast<std::int8_t>(decoded[offset++]);
        if (manifest.has_checksum) {
            entry.checksum = u32(decoded, offset);
            offset += 4;
        }
        entry.flags.assign(decoded.begin() + static_cast<std::ptrdiff_t>(offset),
                           decoded.begin() + static_cast<std::ptrdiff_t>(offset + manifest.flag_size));
        offset += manifest.flag_size;
        manifest.entries.push_back(std::move(entry));
    }

    const auto mask_size = (static_cast<std::size_t>(manifest.entry_count) + 7) / 8;
    manifest.tags.reserve(manifest.tag_count);
    for (std::uint16_t i = 0; i < manifest.tag_count; ++i) {
        DownloadTag tag;
        tag.name = read_cstring(decoded, offset, "download tag");
        ensure_available(decoded, offset, 2 + mask_size, "download tag data");
        tag.type = u16(decoded, offset);
        offset += 2;
        tag.bitmap.assign(decoded.begin() + static_cast<std::ptrdiff_t>(offset),
                          decoded.begin() + static_cast<std::ptrdiff_t>(offset + mask_size));
        offset += mask_size;
        manifest.tags.push_back(std::move(tag));
    }
    return manifest;
}

EncodingIndex EncodingIndex::parse(const std::vector<std::uint8_t>& decoded) {
    ensure_available(decoded, 0, 22, "encoding header");
    if (decoded[0] != 'E' || decoded[1] != 'N') throw std::runtime_error("invalid encoding magic");
    if (decoded[2] != 1) throw std::runtime_error("unsupported encoding version");
    const auto ckey_size = decoded[3];
    const auto ekey_size = decoded[4];
    const auto ckey_page_kb = u16(decoded, 5);
    const auto ekey_page_kb = u16(decoded, 7);
    const auto ckey_pages = u32(decoded, 9);
    const auto ekey_pages = u32(decoded, 13);
    const auto espec_size = u32(decoded, 18);
    if (ckey_size == 0 || ekey_size == 0 || ckey_page_kb == 0 || ekey_page_kb == 0) {
        throw std::runtime_error("invalid encoding page configuration");
    }

    // EN layout: header, ESpec table, CKey index, CKey pages, EKey index,
    // EKey pages, then the optional trailing self-describing ESpec.
    const std::uint64_t ckey_index_size = static_cast<std::uint64_t>(ckey_pages) * (ckey_size + 16ull);
    const std::uint64_t ekey_index_size = static_cast<std::uint64_t>(ekey_pages) * (ekey_size + 16ull);
    const std::uint64_t ckey_offset = 22ull + espec_size + ckey_index_size;
    const std::uint64_t ckey_page_size = static_cast<std::uint64_t>(ckey_page_kb) * 1024ull;
    const std::uint64_t ekey_index_offset = ckey_offset + static_cast<std::uint64_t>(ckey_pages) * ckey_page_size;
    const std::uint64_t ekey_offset = ekey_index_offset + ekey_index_size;
    const std::uint64_t ekey_page_size = static_cast<std::uint64_t>(ekey_page_kb) * 1024ull;
    const std::uint64_t encoding_pages_end = ekey_offset + static_cast<std::uint64_t>(ekey_pages) * ekey_page_size;
    if (ckey_offset > decoded.size() || ekey_offset > decoded.size() || encoding_pages_end > decoded.size()) {
        throw std::runtime_error("encoding page offsets exceed file");
    }

    EncodingIndex index;
    for (std::uint32_t page = 0; page < ckey_pages; ++page) {
        const auto page_offset = ckey_offset + static_cast<std::uint64_t>(page) * ckey_page_size;
        if (page_offset + ckey_page_size > decoded.size()) throw std::runtime_error("truncated CKey page");
        std::size_t offset = static_cast<std::size_t>(page_offset);
        const auto end = offset + static_cast<std::size_t>(ckey_page_size);
        while (offset < end) {
            const auto count = decoded[offset++];
            if (count == 0) break;
            if (offset + 5 + ckey_size + static_cast<std::size_t>(count) * ekey_size > end) break;
            const auto decoded_size = u40(decoded, offset);
            offset += 5;
            const auto ckey = hex_bytes(decoded.data() + offset, ckey_size);
            offset += ckey_size;
            std::vector<std::string> ekeys;
            ekeys.reserve(count);
            for (std::uint8_t key = 0; key < count; ++key) {
                ekeys.push_back(hex_bytes(decoded.data() + offset, ekey_size));
                offset += ekey_size;
            }
            index.mappings_.try_emplace(ckey, FileMapping{ekeys.front(), decoded_size, std::move(ekeys)});
        }
    }
    for (std::uint32_t page = 0; page < ekey_pages; ++page) {
        const auto page_offset = ekey_offset + static_cast<std::uint64_t>(page) * ekey_page_size;
        std::size_t offset = static_cast<std::size_t>(page_offset);
        const auto end = offset + static_cast<std::size_t>(ekey_page_size);
        // Entry: EKey, ESpec index (u32 BE), encoded size (u40 BE).
        while (offset + ekey_size + 9 <= end) {
            bool empty = true;
            for (std::size_t i = 0; i < ekey_size && empty; ++i) empty = decoded[offset + i] == 0;
            if (empty) break;
            const auto ekey = hex_bytes(decoded.data() + offset, ekey_size);
            index.encoded_sizes_.try_emplace(ekey, u40(decoded, offset + ekey_size + 4));
            offset += ekey_size + 9;
        }
    }
    return index;
}

const FileMapping* EncodingIndex::find(const std::string& content_key) const {
    const auto it = mappings_.find(content_key);
    return it == mappings_.end() ? nullptr : &it->second;
}

ArchiveIndex ArchiveIndex::parse(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 28) throw std::runtime_error("archive index is too small");

    const auto footer_hash_bytes = bytes[bytes.size() - 13];
    if (footer_hash_bytes == 0 || footer_hash_bytes > 16) {
        throw std::runtime_error("invalid archive index footer hash size");
    }
    const auto footer_size = static_cast<std::size_t>(20 + footer_hash_bytes);
    if (footer_size > bytes.size()) throw std::runtime_error("archive index footer exceeds file");
    const auto footer = bytes.size() - footer_size;
    const auto page_size = static_cast<std::size_t>(bytes[footer + 11]) * 1024;
    const auto offset_bytes = bytes[footer + 12];
    const auto size_bytes = bytes[footer + 13];
    const auto ekey_bytes = bytes[footer + 14];
    const auto element_count = u32le(bytes, footer + 16);
    if (bytes[footer + 8] > 1 || bytes[footer + 9] != 0 || bytes[footer + 10] != 0 ||
        page_size == 0 || page_size != 4096 || size_bytes != 4 || ekey_bytes == 0 ||
        ekey_bytes > 16 || (offset_bytes != 4 && offset_bytes != 5)) {
        throw std::runtime_error("unsupported archive index format");
    }

    const auto record_size = static_cast<std::size_t>(ekey_bytes) + size_bytes + offset_bytes;
    const auto records_per_page = page_size / record_size;
    if (records_per_page == 0) throw std::runtime_error("archive index record does not fit page");
    const auto page_count = (static_cast<std::size_t>(element_count) + records_per_page - 1) /
                            records_per_page;
    const auto toc_size = page_count * (static_cast<std::size_t>(ekey_bytes) + footer_hash_bytes);
    if (footer_size + toc_size > bytes.size()) throw std::runtime_error("archive index TOC exceeds file");
    const auto data_size = bytes.size() - footer_size - toc_size;
    if (page_count != 0 && data_size < (page_count - 1) * page_size) {
        throw std::runtime_error("archive index data pages are truncated");
    }

    ArchiveIndex result;
    for (std::size_t page = 0; page < page_count; ++page) {
        const auto page_offset = page * page_size;
        const auto page_length = std::min(page_size, data_size - page_offset);
        std::size_t offset = page_offset;
        const auto end = page_offset + page_length;
        while (offset + record_size <= end) {
            const auto key = hex_bytes(bytes.data() + offset, ekey_bytes);
            const auto size = u32(bytes, offset + ekey_bytes);
            const auto offset_field = offset + ekey_bytes + size_bytes;
            bool all_zero = size == 0;
            for (std::size_t i = 0; i < ekey_bytes; ++i) all_zero = all_zero && bytes[offset + i] == 0;
            std::uint64_t archive_offset = 0;
            for (std::size_t i = 0; i < offset_bytes; ++i) {
                archive_offset = (archive_offset << 8) | bytes[offset_field + i];
                all_zero = all_zero && bytes[offset_field + i] == 0;
            }
            if (all_zero) break;
            result.entries_.try_emplace(key, ArchiveLocation{"", archive_offset, size});
            offset += record_size;
        }
    }
    return result;
}

const ArchiveLocation* ArchiveIndex::find(const std::string& encoding_key) const {
    const auto it = entries_.find(encoding_key);
    return it == entries_.end() ? nullptr : &it->second;
}

} // namespace openblizz
