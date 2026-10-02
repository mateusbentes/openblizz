#include "openblizz/hash.hpp"

#include <algorithm>
#include <openssl/evp.h>

#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace openblizz {
namespace {

std::string digest_hex(const std::vector<std::uint8_t>& data, const EVP_MD* algorithm) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), algorithm, nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("OpenSSL digest initialization failed");
    }

    unsigned int length = EVP_MAX_MD_SIZE;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    if (EVP_DigestFinal_ex(context.get(), digest, &length) != 1) {
        throw std::runtime_error("OpenSSL digest finalization failed");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) output << std::setw(2) << static_cast<int>(digest[i]);
    return output.str();
}

} // namespace

std::string md5_hex(const std::vector<std::uint8_t>& data) {
    return digest_hex(data, EVP_md5());
}

std::string md5_hex(const std::string& data) {
    return md5_hex(std::vector<std::uint8_t>(data.begin(), data.end()));
}

std::string sha256_hex(const std::vector<std::uint8_t>& data) {
    return digest_hex(data, EVP_sha256());
}

bool is_hex_hash(const std::string& value, std::size_t bytes) {
    if (value.size() != bytes * 2) return false;
    for (const auto ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::vector<std::uint8_t> hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) throw std::runtime_error("odd-length hex string");
    std::vector<std::uint8_t> out(hex.size() / 2);
    const auto nibble = [&](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
        throw std::runtime_error("invalid hex digit in " + hex);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return out;
}

std::string bytes_to_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

// lookup3 by Bob Jenkins (public domain), byte-oriented variant so it is
// independent of host endianness and alignment.
namespace {

inline std::uint32_t rot(std::uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

#define OPENBLIZZ_JENKINS_MIX(a, b, c) \
    do { \
        a -= c; a ^= rot(c, 4); c += b; \
        b -= a; b ^= rot(a, 6); a += c; \
        c -= b; c ^= rot(b, 8); b += a; \
        a -= c; a ^= rot(c, 16); c += b; \
        b -= a; b ^= rot(a, 19); a += c; \
        c -= b; c ^= rot(b, 4); b += a; \
    } while (0)

#define OPENBLIZZ_JENKINS_FINAL(a, b, c) \
    do { \
        c ^= b; c -= rot(b, 14); \
        a ^= c; a -= rot(c, 11); \
        b ^= a; b -= rot(a, 25); \
        c ^= b; c -= rot(b, 16); \
        a ^= c; a -= rot(c, 4); \
        b ^= a; b -= rot(a, 14); \
        c ^= b; c -= rot(b, 24); \
    } while (0)

inline std::uint32_t load_le(const std::uint8_t* k, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= static_cast<std::uint32_t>(k[i]) << (8 * i);
    return value;
}

} // namespace

void jenkins_hashlittle2(const void* data, std::size_t size, std::uint32_t& pc, std::uint32_t& pb) {
    const auto* k = static_cast<const std::uint8_t*>(data);
    std::uint32_t a = 0xdeadbeef + static_cast<std::uint32_t>(size) + pc;
    std::uint32_t b = a;
    std::uint32_t c = a + pb;
    while (size > 12) {
        a += load_le(k, 4);
        b += load_le(k + 4, 4);
        c += load_le(k + 8, 4);
        OPENBLIZZ_JENKINS_MIX(a, b, c);
        size -= 12;
        k += 12;
    }
    if (size == 0) {
        pc = c;
        pb = b;
        return;
    }
    a += load_le(k, std::min<std::size_t>(size, 4));
    if (size > 4) b += load_le(k + 4, std::min<std::size_t>(size - 4, 4));
    if (size > 8) c += load_le(k + 8, size - 8);
    OPENBLIZZ_JENKINS_FINAL(a, b, c);
    pc = c;
    pb = b;
}

std::uint32_t jenkins_hashlittle(const void* data, std::size_t size, std::uint32_t init) {
    std::uint32_t pc = init;
    std::uint32_t pb = 0;
    jenkins_hashlittle2(data, size, pc, pb);
    return pc;
}

#undef OPENBLIZZ_JENKINS_MIX
#undef OPENBLIZZ_JENKINS_FINAL

} // namespace openblizz
