#pragma once

#include "openblizz/auth.hpp"
#include "openblizz/catalog.hpp"
#include "openblizz/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

struct EntitlementRecord {
    std::string product_id;
    bool owned{false};
    bool explicit_state{false};
    std::string source;
    std::string reason;
};

class LibraryManager {
public:
    [[nodiscard]] static std::filesystem::path default_file();
    [[nodiscard]] static std::vector<LibraryEntry> load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path,
                     const std::vector<LibraryEntry>& entries);

    // Parse only deliberately supported response shapes. Absence is never treated as not-owned.
    [[nodiscard]] static std::vector<EntitlementRecord> parse_entitlement_response(
        const std::string& body, const std::vector<ProductDescriptor>& products);

    static int list(const Catalog& catalog, const std::filesystem::path& path);
    static int add(const Catalog& catalog, const std::filesystem::path& path,
                   const std::string& product_id);
    static int remove(const Catalog& catalog, const std::filesystem::path& path,
                      const std::string& product_id);
    static int scan(const Catalog& catalog, const AuthOptions& auth,
                    const std::filesystem::path& path, const std::string& entitlement_url);
};

} // namespace openblizz
