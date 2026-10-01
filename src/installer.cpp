#include "openblizz/installer.hpp"
#include "openblizz/hash.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace openblizz {
namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read file: " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < 0) throw std::runtime_error("cannot determine file size: " + path.string());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!input && !data.empty()) throw std::runtime_error("short read: " + path.string());
    return data;
}

void write_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    const auto part = path.string() + ".part";
    {
        std::ofstream output(part, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write temporary file: " + part);
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output) throw std::runtime_error("failed writing temporary file: " + part);
    }
    std::error_code error;
    std::filesystem::rename(part, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(part, path, error);
        if (error) throw std::runtime_error("cannot commit file: " + path.string());
    }
}

bool tag_has_file(const InstallTag& tag, std::size_t index) {
    const auto byte = index / 8;
    const auto bit = index % 8;
    return byte < tag.bitmap.size() && (tag.bitmap[byte] & (0x80u >> bit)) != 0;
}

std::filesystem::path safe_relative_path(const std::string& raw) {
    std::string normalized = raw;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path path(normalized);
    if (path.is_absolute()) throw std::runtime_error("manifest contains an absolute path: " + raw);
    path = path.lexically_normal();
    for (const auto& component : path) {
        if (component == "..") throw std::runtime_error("manifest path escapes install directory: " + raw);
    }
    return path;
}

} // namespace

Installer::Installer(Catalog& catalog, std::filesystem::path cache_root)
    : catalog_(catalog), cache_root_(std::move(cache_root)) {}

std::vector<InstallEntry> Installer::select_entries(const InstallManifest& manifest,
                                                    const std::string& locale) const {
    std::vector<InstallEntry> selected;
    const std::vector<std::string> required_tags{locale, "Windows", "Release"};
    for (std::size_t i = 0; i < manifest.entries.size(); ++i) {
        bool include = true;
        for (const auto& required : required_tags) {
            const auto tag = std::find_if(manifest.tags.begin(), manifest.tags.end(),
                                          [&](const auto& candidate) {
                                              return candidate.name == required;
                                          });
            // A product may omit a category, in which case the absence of the
            // selector is not a reason to discard every file.
            if (tag != manifest.tags.end() && !tag_has_file(*tag, i)) {
                include = false;
                break;
            }
        }
        if (include) selected.push_back(manifest.entries[i]);
    }
    return selected;
}

InstallPlan Installer::plan(const std::string& product, const std::string& region,
                            const std::string& locale) const {
    InstallPlan result;
    result.product = find_product(catalog_.products(), product);
    result.version = catalog_.version(product, region);
    result.cdn = catalog_.select_cdn(product, region);

    const auto build_bytes = catalog_.fetch_config(result.cdn, result.version.build_config);
    const auto cdn_bytes = catalog_.fetch_config(result.cdn, result.version.cdn_config);
    result.build_config = parse_config(std::string(reinterpret_cast<const char*>(build_bytes.data()), build_bytes.size()));
    result.cdn_config = parse_config(std::string(reinterpret_cast<const char*>(cdn_bytes.data()), cdn_bytes.size()));

    const auto archive_hashes = result.cdn_config.get("archives");
    if (!archive_hashes.empty()) {
        std::cout << "Loading " << archive_hashes.size() << " archive indexes.\n";
        for (const auto& archive_hash : archive_hashes) {
            const auto index_bytes = catalog_.fetch_archive_index(result.cdn, archive_hash);
            const auto index = ArchiveIndex::parse(index_bytes);
            for (const auto& [encoding_key, location] : index.entries()) {
                auto archive_location = location;
                archive_location.archive_key = archive_hash;
                result.archive_entries.try_emplace(encoding_key, std::move(archive_location));
            }
        }
        std::cout << "Archive index entries: " << result.archive_entries.size() << "\n";
    }

    const auto install_pair = result.build_config.pair("install");
    if (!install_pair || install_pair->encoding_key.empty()) {
        throw std::runtime_error("product " + product + " has no install manifest in the current build");
    }
    const auto encoding_pair = result.build_config.pair("encoding");
    if (!encoding_pair || encoding_pair->encoding_key.empty()) {
        throw std::runtime_error("product " + product + " has no encoding manifest in the current build");
    }

    const auto install_decoded = catalog_.fetch_decoded_data(result.cdn, install_pair->encoding_key);
    result.install_manifest = parse_install_manifest(install_decoded);

    const auto encoding_decoded = catalog_.fetch_decoded_data(result.cdn, encoding_pair->encoding_key);
    const auto encoding = EncodingIndex::parse(encoding_decoded);
    result.mappings = encoding.mappings();
    result.selected_entries = select_entries(result.install_manifest, locale);
    for (const auto& entry : result.selected_entries) result.total_bytes += entry.file_size;
    return result;
}

std::filesystem::path Installer::cache_path(const std::string& hash) const {
    return cache_root_ / "objects" / hash.substr(0, 2) / hash.substr(2, 2) / hash;
}

std::vector<std::uint8_t> Installer::content(
    const CdnInfo& cdn, const std::unordered_map<std::string, ArchiveLocation>& archives,
    const std::string& encoding_key) const {
    const auto path = cache_path(encoding_key);
    if (std::filesystem::exists(path)) return read_file(path);

    std::vector<std::uint8_t> encoded;
    const auto location = archives.find(encoding_key);
    if (location != archives.end()) {
        try {
            encoded = catalog_.fetch_archive_range(cdn, location->second.archive_key,
                                                   location->second.offset, location->second.encoded_size);
        } catch (const std::exception& archive_error) {
            try {
                encoded = catalog_.fetch_data(cdn, encoding_key);
            } catch (const std::exception& direct_error) {
                throw std::runtime_error("archive object failed (" + std::string(archive_error.what()) +
                                         "); direct object failed (" + direct_error.what() + ")");
            }
        }
    } else {
        encoded = catalog_.fetch_data(cdn, encoding_key);
    }
    const auto decoded = BlteDecoder::decode(encoded);
    write_atomic(path, decoded);
    return decoded;
}

bool Installer::install_one(const InstallPlan& plan, const InstallEntry& entry,
                            const std::filesystem::path& directory) const {
    const auto output = directory / safe_relative_path(entry.path);
    if (std::filesystem::exists(output)) {
        try {
            const auto existing = read_file(output);
            if (existing.size() == entry.file_size && md5_hex(existing) == entry.content_key) {
                return false;
            }
        } catch (const std::exception&) {
            // Treat unreadable or incomplete destinations as missing and repair them.
        }
    }

    const auto mapping = plan.mappings.find(entry.content_key);
    if (mapping == plan.mappings.end()) {
        throw std::runtime_error("encoding index has no entry for content key " + entry.content_key);
    }
    auto encoding_keys = mapping->second.encoding_keys;
    if (encoding_keys.empty()) encoding_keys.push_back(mapping->second.encoding_key);

    std::vector<std::uint8_t> data;
    std::string last_error;
    bool found = false;
    for (const auto& encoding_key : encoding_keys) {
        try {
            auto candidate = content(plan.cdn, plan.archive_entries, encoding_key);
            if (candidate.size() == entry.file_size && md5_hex(candidate) == entry.content_key) {
                data = std::move(candidate);
                found = true;
                break;
            }
            std::error_code error;
            std::filesystem::remove(cache_path(encoding_key), error);
            last_error = "decoded size or content hash mismatch for EKey " + encoding_key;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    if (!found) {
        throw std::runtime_error("no usable encoding object for " + entry.path +
                                 " after trying " + std::to_string(encoding_keys.size()) +
                                 " EKeys: " + last_error);
    }

    write_atomic(output, data);
    return true;
}

std::size_t Installer::install(const InstallPlan& plan, const std::filesystem::path& directory,
                               const std::string& locale, std::size_t jobs) const {
    (void)locale;
    if (jobs == 0) jobs = 1;
    std::filesystem::create_directories(directory);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> completed{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto worker = [&]() {
        try {
            while (true) {
                const auto index = next.fetch_add(1);
                if (index >= plan.selected_entries.size()) break;
                const auto downloaded = install_one(plan, plan.selected_entries[index], directory);
                const auto done = completed.fetch_add(1) + 1;
                std::lock_guard lock(failure_mutex);
                std::cout << "[" << done << "/" << plan.selected_entries.size() << "] "
                          << plan.selected_entries[index].path
                          << (downloaded ? "" : " (already verified)") << '\n';
            }
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
        }
    };

    const auto count = std::min(jobs, std::max<std::size_t>(1, plan.selected_entries.size()));
    std::vector<std::thread> workers;
    workers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
    if (failure) std::rethrow_exception(failure);
    return completed.load();
}

std::vector<std::string> Installer::verify(const InstallPlan& plan,
                                           const std::filesystem::path& directory,
                                           const std::string& locale) const {
    (void)locale;
    std::vector<std::string> failures;
    for (const auto& entry : plan.selected_entries) {
        const auto path = directory / safe_relative_path(entry.path);
        if (!std::filesystem::exists(path)) {
            failures.push_back(entry.path + ": missing");
            continue;
        }
        try {
            const auto data = read_file(path);
            if (data.size() != entry.file_size) {
                failures.push_back(entry.path + ": size mismatch");
            } else if (md5_hex(data) != entry.content_key) {
                failures.push_back(entry.path + ": content hash mismatch");
            }
        } catch (const std::exception& error) {
            failures.push_back(entry.path + ": " + error.what());
        }
    }
    return failures;
}

std::size_t Installer::repair(const InstallPlan& plan, const std::filesystem::path& directory,
                              const std::string& locale, std::size_t jobs) const {
    const auto failures = verify(plan, directory, locale);
    if (failures.empty()) return 0;
    std::set<std::string> broken;
    for (const auto& failure : failures) broken.insert(failure.substr(0, failure.find(':')));

    InstallPlan repair_plan = plan;
    repair_plan.selected_entries.clear();
    for (const auto& entry : plan.selected_entries) {
        if (broken.contains(entry.path)) repair_plan.selected_entries.push_back(entry);
    }
    return install(repair_plan, directory, locale, jobs);
}

} // namespace openblizz
