#include "openblizz/hash.hpp"

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

} // namespace openblizz
