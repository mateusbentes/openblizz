#pragma once

#include "openblizz/formats.hpp"
#include "openblizz/http.hpp"
#include "openblizz/types.hpp"

#include <string>
#include <vector>

namespace openblizz {

class Catalog {
public:
    explicit Catalog(HttpClient& http);

    [[nodiscard]] std::vector<ProductDescriptor> products() const;
    [[nodiscard]] VersionInfo version(const std::string& product,
                                      const std::string& region) const;
    [[nodiscard]] std::vector<CdnInfo> cdns(const std::string& product,
                                            const std::string& region) const;
    [[nodiscard]] CdnInfo select_cdn(const std::string& product,
                                     const std::string& region) const;

    [[nodiscard]] std::vector<std::uint8_t> fetch_config(const CdnInfo& cdn,
                                                         const std::string& hash) const;
    [[nodiscard]] std::vector<std::uint8_t> fetch_data(const CdnInfo& cdn,
                                                       const std::string& hash) const;
    [[nodiscard]] std::vector<std::uint8_t> fetch_decoded_data(const CdnInfo& cdn,
                                                               const std::string& hash) const;
    [[nodiscard]] std::vector<std::uint8_t> fetch_archive_index(const CdnInfo& cdn,
                                                                const std::string& hash) const;
    [[nodiscard]] std::vector<std::uint8_t> fetch_archive_range(const CdnInfo& cdn,
                                                                const std::string& hash,
                                                                std::uint64_t offset,
                                                                std::uint32_t size) const;

private:
    [[nodiscard]] std::string object_url(const CdnInfo& cdn,
                                         const std::string& kind,
                                         const std::string& hash,
                                         const std::string& suffix = {}) const;
    [[nodiscard]] HttpResponse get_with_cdn_failover(const CdnInfo& cdn,
                                                     const std::string& kind,
                                                     const std::string& hash,
                                                     const std::string& suffix = {}) const;

    HttpClient& http_;
};

[[nodiscard]] const ProductDescriptor& find_product(const std::vector<ProductDescriptor>& products,
                                                    const std::string& id);

} // namespace openblizz
