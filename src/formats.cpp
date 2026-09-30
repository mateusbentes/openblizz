#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"

#include <lz4.h>
#include <zlib.h>

#include <algorithm>
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

std::vector<std::uint8_t> decode_chunk(const std::uint8_t* bytes, std::size_t size,
                                       std::size_t expected) {
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
        throw std::runtime_error("encrypted BLTE content requires a user-provided keyring");
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

std::vector<std::uint8_t> BlteDecoder::decode(const std::vector<std::uint8_t>& encoded) {
    ensure_available(encoded, 0, 8, "BLTE preamble");
    if (std::string(reinterpret_cast<const char*>(encoded.data()), 4) != "BLTE") {
        throw std::runtime_error("invalid BLTE magic");
    }
    const auto header_size = u32(encoded, 4);
    std::vector<std::uint8_t> output;

    if (header_size == 0) {
        const auto decoded = decode_chunk(encoded.data() + 8, encoded.size() - 8, encoded.size() - 8);
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
    for (const auto& chunk : chunks) {
        ensure_available(encoded, data_offset, chunk.compressed, "BLTE chunk");
        auto decoded = decode_chunk(encoded.data() + data_offset, chunk.compressed, chunk.decoded);
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
            const auto ekey = hex_bytes(decoded.data() + offset, ekey_size);
            offset += static_cast<std::size_t>(count) * ekey_size;
            index.mappings_.try_emplace(ckey, FileMapping{ekey, decoded_size});
        }
    }
    return index;
}

const FileMapping* EncodingIndex::find(const std::string& content_key) const {
    const auto it = mappings_.find(content_key);
    return it == mappings_.end() ? nullptr : &it->second;
}

} // namespace openblizz
