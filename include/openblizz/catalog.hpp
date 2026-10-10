#pragma once

#include "openblizz/formats.hpp"
#include "openblizz/http.hpp"
#include "openblizz/types.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace openblizz {

class Catalog {
public:
    // Optional transport/metadata base are dependency injection for offline
    // tests, not CLI overrides. size == 0 means GET, otherwise an HTTP range.
    using Transport = std::function<HttpResponse(const std::string&, std::uint64_t, std::uint32_t)>;
    explicit Catalog(HttpClient& http, std::vector<LibraryEntry> library = {},
                     std::string version_base = {}, Transport transport = {});

    [[nodiscard]] std::vector<ProductDescriptor> products() const;
    // Keep the local library id separate from the actual NGDP product code.
    [[nodiscard]] ProductDescriptor resolve_product(const std::string& id) const;
    [[nodiscard]] static bool valid_product_code(const std::string& code);
    void set_library(std::vector<LibraryEntry> library) { library_ = std::move(library); }

    // One row of the public Ribbit "summary" endpoint: every NGDP product code
    // currently published by Blizzard (retail, PTR, beta, internal), with its
    // sequence number and flags ("cdn", "bgdl" or empty for versions).
    struct SummaryEntry {
        std::string product;
        std::string seqn;
        std::string flags;
    };
    [[nodiscard]] std::vector<SummaryEntry> summary(const std::string& region = "us") const;

    // Curated products plus a generic descriptor for every other product code
    // in the Ribbit summary, so account entries for games outside the curated
    // list can still be recognised and installed.
    [[nodiscard]] std::vector<ProductDescriptor> all_products(const std::string& region = "us") const;
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
    [[nodiscard]] std::vector<std::uint8_t> fetch_decoded_data(const CdnInfo& cdn,
                                                               const std::string& hash,
                                                               const KeyRing& keyring) const;
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
    [[nodiscard]] HttpResponse request(const std::string& url,
                                       std::uint64_t offset = 0, std::uint32_t size = 0) const;
    [[nodiscard]] std::string metadata_base(const std::string& region) const;

    HttpClient& http_;
    std::vector<LibraryEntry> library_;
    std::string version_base_;
    Transport transport_;
};

[[nodiscard]] const ProductDescriptor& find_product(const std::vector<ProductDescriptor>& products,
                                                    const std::string& id);

} // namespace openblizz
