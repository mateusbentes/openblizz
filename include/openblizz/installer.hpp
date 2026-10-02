#pragma once

#include "openblizz/catalog.hpp"
#include "openblizz/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

class Installer {
public:
    Installer(Catalog& catalog, std::filesystem::path cache_root);

    // Resolves version, CDN and both config files without loading manifests.
    [[nodiscard]] BuildContext context(const std::string& product,
                                       const std::string& region) const;

    // Mounts vfs-root and every nested TVFS manifest of the build. Manifests
    // are cached decoded in the content cache like any other object.
    [[nodiscard]] std::vector<VfsFile> vfs_files(const BuildContext& context) const;

    [[nodiscard]] InstallPlan plan(const std::string& product,
                                   const std::string& region,
                                   const std::string& locale) const;
    [[nodiscard]] std::size_t install(const InstallPlan& plan,
                                      const std::filesystem::path& directory,
                                      const std::string& locale,
                                      std::size_t jobs = 4) const;
    [[nodiscard]] std::vector<std::string> verify(const InstallPlan& plan,
                                                  const std::filesystem::path& directory,
                                                  const std::string& locale) const;
    [[nodiscard]] std::size_t repair(const InstallPlan& plan,
                                     const std::filesystem::path& directory,
                                     const std::string& locale,
                                     std::size_t jobs = 4) const;

private:
    [[nodiscard]] std::vector<InstallEntry> select_entries(const InstallManifest& manifest,
                                                            const std::string& locale) const;
    [[nodiscard]] std::filesystem::path cache_path(const std::string& hash) const;
    [[nodiscard]] std::vector<std::uint8_t> content(const CdnInfo& cdn,
                                                    const std::unordered_map<std::string, ArchiveLocation>& archives,
                                                    const std::string& encoding_key) const;
    [[nodiscard]] bool install_one(const InstallPlan& plan, const InstallEntry& entry,
                                   const std::filesystem::path& directory) const;

    Catalog& catalog_;
    std::filesystem::path cache_root_;
};

} // namespace openblizz
