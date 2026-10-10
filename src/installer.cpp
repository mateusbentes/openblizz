#include "openblizz/installer.hpp"
#include "openblizz/hash.hpp"
#include "openblizz/tvfs.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <unistd.h>

namespace openblizz {
namespace {

constexpr std::uint64_t kRangeChunkBytes = 32ull * 1024 * 1024;   // one HTTP range request
constexpr std::uint64_t kRangeMaxGapBytes = 1ull * 1024 * 1024;   // unwanted bytes we accept inside a chunk
constexpr std::uint64_t kCommitEveryBytes = 256ull * 1024 * 1024;   // journal flush interval
constexpr int kRangeAttempts = 3;                                    // archive range retries before per-object fallback

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
    // Different output files can share an EKey/cache object. Their workers
    // must not truncate or rename the same temporary cache filename.
    static std::atomic<std::uint64_t> sequence{0};
    const auto part = path.string() + ".part." + std::to_string(::getpid()) + "." +
                      std::to_string(sequence.fetch_add(1));
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

void write_atomic(const std::filesystem::path& path, const std::string& text) {
    write_atomic(path, std::vector<std::uint8_t>(text.begin(), text.end()));
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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

// EKeys are MD5s of the encoded object: of the whole file for single-chunk
// BLTE, of the BLTE header (which carries the chunk checksums) otherwise.
bool verify_encoded(const std::string& encoding_key, const std::vector<std::uint8_t>& encoded) {
    if (encoded.size() < 8 || encoded[0] != 'B' || encoded[1] != 'L' || encoded[2] != 'T' || encoded[3] != 'E') {
        return false;
    }
    const std::uint32_t header_size = (static_cast<std::uint32_t>(encoded[4]) << 24) |
                                      (static_cast<std::uint32_t>(encoded[5]) << 16) |
                                      (static_cast<std::uint32_t>(encoded[6]) << 8) | encoded[7];
    if (header_size == 0) return md5_hex(encoded) == encoding_key;
    if (header_size > encoded.size()) return false;
    return md5_hex(std::vector<std::uint8_t>(encoded.begin(), encoded.begin() + header_size)) == encoding_key;
}

std::string format_mib(std::uint64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

template <typename Function>
void run_parallel(std::size_t jobs, std::size_t count, Function&& function) {
    if (jobs == 0) jobs = 1;
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto worker = [&]() {
        try {
            while (true) {
                const auto index = next.fetch_add(1);
                if (index >= count) break;
                {
                    std::lock_guard lock(failure_mutex);
                    if (failure) break;   // another worker failed; stop early
                }
                function(index);
            }
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
        }
    };
    std::vector<std::thread> workers;
    const auto threads = std::min(jobs, std::max<std::size_t>(1, count));
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

Installer::Installer(Catalog& catalog, std::filesystem::path cache_root)
    : catalog_(catalog), cache_root_(std::move(cache_root)) {}

std::vector<InstallEntry> Installer::select_entries(const InstallManifest& manifest,
                                                    const std::string& locale) const {
    std::vector<InstallEntry> selected;
    // Some legacy manifests publish locale-specific CASC data (for example
    // ptbr) but only ship the Windows bootstrap files under enUS. If the
    // requested locale has no install-manifest tag, selecting no locale tag
    // would include every language variant with the same output path.
    // Prefer the enUS bootstrap in that case; the requested locale remains
    // selected separately by select_data_objects().
    std::string manifest_locale = locale;
    const auto has_tag = [&](const std::string& name) {
        const auto wanted = lower(name);
        return std::find_if(manifest.tags.begin(), manifest.tags.end(),
                            [&](const auto& candidate) {
                                return lower(candidate.name) == wanted;
                            });
    };
    if (has_tag(manifest_locale) == manifest.tags.end() && has_tag("enUS") != manifest.tags.end()) {
        manifest_locale = "enUS";
    }
    const std::vector<std::string> required_tags{manifest_locale, "Windows", "Release"};
    for (std::size_t i = 0; i < manifest.entries.size(); ++i) {
        bool include = true;
        for (const auto& required : required_tags) {
            const auto tag = has_tag(required);
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

BuildContext Installer::context(const std::string& product, const std::string& region) const {
    BuildContext result;
    result.product = catalog_.resolve_product(product);
    result.version = catalog_.version(result.product.agent_product, region);
    result.cdn = catalog_.select_cdn(result.product.agent_product, region);

    if (!result.version.keyring.empty()) {
        const auto keyring_bytes = catalog_.fetch_config(result.cdn, result.version.keyring);
        result.keyring = parse_keyring(parse_config(std::string(
            reinterpret_cast<const char*>(keyring_bytes.data()), keyring_bytes.size())));
    }
    result.build_config_bytes = catalog_.fetch_config(result.cdn, result.version.build_config);
    result.cdn_config_bytes = catalog_.fetch_config(result.cdn, result.version.cdn_config);
    result.build_config = parse_config(std::string(reinterpret_cast<const char*>(result.build_config_bytes.data()),
                                                   result.build_config_bytes.size()));
    result.cdn_config = parse_config(std::string(reinterpret_cast<const char*>(result.cdn_config_bytes.data()),
                                                 result.cdn_config_bytes.size()));
    return result;
}

std::vector<VfsFile> Installer::vfs_files(const BuildContext& context) const {
    const std::unordered_map<std::string, ArchiveLocation> no_archives;
    // Manifests are small loose CDN objects; fetching ~110 of them one by one
    // is latency bound, so warm the cache with a few parallel workers first.
    const auto refs = vfs_manifest_refs(context.build_config);
    run_parallel(8, refs.size(), [&](std::size_t index) {
        if (!refs[index].encoding_key.empty()) {
            (void)content(context.cdn, no_archives, refs[index].encoding_key, context.keyring);
        }
    });
    return VfsResolver::resolve(context.build_config, [&](const VfsManifestRef& ref) {
        if (ref.encoding_key.empty()) {
            throw std::runtime_error("manifest " + ref.name + " has no encoding key in the build config");
        }
        auto decoded = content(context.cdn, no_archives, ref.encoding_key, context.keyring);
        if (md5_hex(decoded) != ref.content_key) {
            std::error_code error;
            std::filesystem::remove(cache_path(ref.encoding_key), error);
            throw std::runtime_error("content hash mismatch for manifest " + ref.name);
        }
        return decoded;
    });
}

bool Installer::vfs_file_selected(const VfsFile& file, const std::vector<std::string>& locales) {
    static const std::string marker = "_locales/";
    const auto path = lower(file.path);
    std::size_t position = 0;
    while ((position = path.find(marker, position)) != std::string::npos) {
        position += marker.size();
        const auto end = path.find(".w3mod", position);
        if (end == std::string::npos) break;
        const auto locale = path.substr(position, end - position);
        if (std::find(locales.begin(), locales.end(), locale) == locales.end()) return false;
    }
    return true;
}

void Installer::select_data_objects(InstallPlan& plan, const EncodingIndex& encoding,
                                    const PlanOptions& options) const {
    plan.selected_locales.clear();
    if (options.all_locales) {
        std::set<std::string> found;
        for (const auto& file : plan.vfs_files) {
            const auto path = lower(file.path);
            const auto marker = path.find("_locales/");
            const auto end = marker == std::string::npos ? marker : path.find(".w3mod", marker);
            if (end != std::string::npos) found.insert(path.substr(marker + 9, end - marker - 9));
        }
        plan.selected_locales.assign(found.begin(), found.end());
    } else {
        plan.selected_locales.push_back("enus");   // base assets every other locale falls back to
        const auto wanted = lower(plan.locale);
        if (wanted != "enus") plan.selected_locales.push_back(wanted);
    }

    // Full EKeys by their 9-byte prefix, so TVFS spans can be looked up in the
    // encoding table and the CDN archives.
    std::unordered_map<std::string, std::string> by_prefix;
    by_prefix.reserve(encoding.encoded_sizes().size() + plan.archive_entries.size());
    for (const auto& [ekey, size] : encoding.encoded_sizes()) by_prefix.try_emplace(ekey.substr(0, kCascKeyBytes * 2), ekey);
    for (const auto& [ekey, location] : plan.archive_entries) by_prefix.try_emplace(ekey.substr(0, kCascKeyBytes * 2), ekey);

    std::unordered_set<std::string> seen;
    const auto add = [&](const std::string& ekey, std::uint64_t size, const std::string& source) {
        if (!seen.insert(ekey).second) return;
        if (size == 0) {
            if (const auto it = encoding.encoded_sizes().find(ekey); it != encoding.encoded_sizes().end()) size = it->second;
            else if (const auto at = plan.archive_entries.find(ekey); at != plan.archive_entries.end()) size = at->second.encoded_size;
        }
        plan.data_objects.push_back(DataObject{ekey, size, source});
        plan.data_bytes += size;
    };
    const auto add_pair = [&](const std::string& key) {
        const auto pair = plan.build_config.pair(key);
        if (!pair) return;
        const auto sizes = plan.build_config.get(key + "-size");
        const std::uint64_t size = sizes.size() > 1 ? std::stoull(sizes[1]) : 0;
        if (!pair->encoding_key.empty()) {
            add(pair->encoding_key, size, key);
            return;
        }
        // Only the content key is known (e.g. "root"): go through the encoding table.
        if (const auto* mapping = encoding.find(pair->content_key); mapping != nullptr) {
            for (const auto& ekey : mapping->encoding_keys) {
                if (encoding.encoded_sizes().contains(ekey) || plan.archive_entries.contains(ekey)) {
                    add(ekey, 0, key);
                    return;
                }
            }
            if (!mapping->encoding_keys.empty()) add(mapping->encoding_keys.front(), 0, key);
        }
    };

    // System manifests the client needs before it can resolve anything else.
    for (const auto* key : {"encoding", "root", "install", "download", "size"}) add_pair(key);
    for (const auto& ref : vfs_manifest_refs(plan.build_config)) {
        if (!ref.encoding_key.empty()) add(ref.encoding_key, ref.encoded_size, ref.name);
    }

    // Platform filter for the top-level entries (executables, BlizzardBrowser):
    // keep what the install manifest selected for this locale/platform.
    std::unordered_set<std::string> install_paths;
    for (const auto& entry : plan.selected_entries) {
        auto path = lower(entry.path);
        std::replace(path.begin(), path.end(), '\\', '/');
        install_paths.insert(path);
    }

    for (const auto& file : plan.vfs_files) {
        if (options.data_limit != 0 && plan.data_bytes >= options.data_limit) break;
        if (!file.nested_manifest.empty()) continue;   // the manifest itself was added above
        if (!vfs_file_selected(file, plan.selected_locales)) continue;
        if (file.manifest == "vfs-root" && file.path != "index" && !install_paths.contains(lower(file.path))) continue;
        for (const auto& span : file.spans) {
            std::string full;
            if (!span.content_key.empty()) {
                if (const auto* mapping = encoding.find(span.content_key); mapping != nullptr) {
                    for (const auto& candidate : mapping->encoding_keys) {
                        if (candidate.compare(0, span.encoding_key.size(), span.encoding_key) == 0) {
                            full = candidate;
                            break;
                        }
                    }
                }
            }
            if (full.empty()) {
                if (const auto it = by_prefix.find(span.encoding_key); it != by_prefix.end()) full = it->second;
            }
            if (full.empty()) {
                ++plan.unresolved_spans;
                continue;
            }
            add(full, span.encoded_size, file.manifest);
        }
    }
}

InstallPlan Installer::plan(const std::string& product, const std::string& region,
                            const std::string& locale, const PlanOptions& options) const {
    InstallPlan result;
    {
        auto base = context(product, region);
        result.product = std::move(base.product);
        result.version = std::move(base.version);
        result.cdn = std::move(base.cdn);
        result.keyring = std::move(base.keyring);
        result.build_config = std::move(base.build_config);
        result.cdn_config = std::move(base.cdn_config);
        result.build_config_bytes = std::move(base.build_config_bytes);
        result.cdn_config_bytes = std::move(base.cdn_config_bytes);
    }
    result.locale = locale;

    const auto install_pair = result.build_config.pair("install");
    if (!install_pair || install_pair->encoding_key.empty()) {
        throw std::runtime_error("product " + product + " has no install manifest in the current build");
    }
    const auto encoding_pair = result.build_config.pair("encoding");
    if (!encoding_pair || encoding_pair->encoding_key.empty()) {
        throw std::runtime_error("product " + product + " has no encoding manifest in the current build");
    }

    bool indexes_loaded = false;
    const auto load_indexes = [&] {
        if (indexes_loaded) return;
        result.archive_entries = archive_entries(result.cdn, result.cdn_config);
        indexes_loaded = true;
    };
    const auto manifest = [&](const KeyPair& pair) {
        std::vector<std::uint8_t> encoded;
        try {
            encoded = catalog_.fetch_data(result.cdn, pair.encoding_key);
        } catch (const std::exception& direct_error) {
            // Some products publish the named-file manifests only inside an
            // archive. Do not mistake a loose-object 404 for missing content.
            const std::string direct_reason = direct_error.what();
            load_indexes();
            const auto location = result.archive_entries.find(pair.encoding_key);
            if (location == result.archive_entries.end()) {
                throw std::runtime_error("manifest " + pair.encoding_key +
                    " is unavailable as a loose object or in the advertised archives: " + direct_reason);
            }
            encoded = catalog_.fetch_archive_range(result.cdn, location->second.archive_key,
                                                   location->second.offset, location->second.encoded_size);
        }
        return BlteDecoder::decode(encoded, result.keyring);
    };
    const auto install_decoded = manifest(*install_pair);
    if (md5_hex(install_decoded) != install_pair->content_key) {
        throw std::runtime_error("install manifest content hash mismatch for " + product);
    }
    result.install_manifest = parse_install_manifest(install_decoded);
    if (result.install_manifest.entries.empty()) {
        throw std::runtime_error("product " + product + " has an empty install manifest; the current CDN build is metadata-only");
    }

    const auto encoding_decoded = manifest(*encoding_pair);
    if (md5_hex(encoding_decoded) != encoding_pair->content_key) {
        throw std::runtime_error("encoding manifest content hash mismatch for " + product);
    }
    const auto encoding = EncodingIndex::parse(encoding_decoded);
    result.mappings = encoding.mappings();
    std::map<std::string, InstallEntry> outputs;
    for (auto entry : select_entries(result.install_manifest, locale)) {
        entry.path = safe_relative_path(entry.path).generic_string();
        if (entry.path.empty() || entry.path == ".") throw std::runtime_error("empty install file path for " + product);
        const auto [it, inserted] = outputs.emplace(lower(entry.path), entry);
        if (!inserted) {
            if (it->second.content_key != entry.content_key || it->second.file_size != entry.file_size) {
                throw std::runtime_error("conflicting install entries for " + entry.path +
                                         "; refusing concurrent writes to the same file");
            }
            continue; // identical output: process once, including case aliases on Wine
        }
        result.selected_entries.push_back(std::move(entry));
    }
    if (result.selected_entries.empty()) {
        throw std::runtime_error("product " + product + " has no selected Windows install files for locale " + locale);
    }
    for (const auto& entry : result.selected_entries) result.total_bytes += entry.file_size;

    load_indexes();

    if (result.build_config.contains("vfs-root") && !options.skip_data) {
        result.casc = true;
        std::cout << "Mounting TVFS manifests.\n";
        BuildContext ctx;
        ctx.cdn = result.cdn;
        ctx.keyring = result.keyring;
        ctx.build_config = result.build_config;
        result.vfs_files = vfs_files(ctx);
        select_data_objects(result, encoding, options);
    }
    return result;
}

std::filesystem::path Installer::cache_path(const std::string& hash) const {
    return cache_root_ / "objects" / hash.substr(0, 2) / hash.substr(2, 2) / hash;
}

std::filesystem::path Installer::index_cache_path(const std::string& hash) const {
    return cache_root_ / "indices" / (hash + ".index");
}

std::vector<std::uint8_t> Installer::archive_index_bytes(const CdnInfo& cdn, const std::string& hash) const {
    const auto path = index_cache_path(hash);
    if (std::filesystem::exists(path)) return read_file(path);
    auto bytes = catalog_.fetch_archive_index(cdn, hash);
    write_atomic(path, bytes);
    return bytes;
}

std::unordered_map<std::string, ArchiveLocation> Installer::archive_entries(
    const CdnInfo& cdn, const ConfigFile& config) const {
    std::unordered_map<std::string, ArchiveLocation> entries;
    const auto hashes = config.get("archives");
    if (hashes.empty()) return entries;
    std::cout << "Loading " << hashes.size() << " archive indexes.\n";
    std::mutex merge_mutex;
    run_parallel(8, hashes.size(), [&](std::size_t i) {
        const auto index = ArchiveIndex::parse(archive_index_bytes(cdn, hashes[i]));
        std::lock_guard lock(merge_mutex);
        for (const auto& [key, location] : index.entries()) {
            auto resolved = location;
            resolved.archive_key = hashes[i];
            entries.try_emplace(key, std::move(resolved));
        }
    });
    std::cout << "Archive index entries: " << entries.size() << "\n";
    return entries;
}

std::vector<std::uint8_t> Installer::fetch_encoded(
    const CdnInfo& cdn, const std::unordered_map<std::string, ArchiveLocation>& archives,
    const std::string& encoding_key) const {
    const auto location = archives.find(encoding_key);
    if (location == archives.end()) return catalog_.fetch_data(cdn, encoding_key);
    try {
        return catalog_.fetch_archive_range(cdn, location->second.archive_key,
                                            location->second.offset, location->second.encoded_size);
    } catch (const std::exception& archive_error) {
        try {
            return catalog_.fetch_data(cdn, encoding_key);
        } catch (const std::exception& direct_error) {
            throw std::runtime_error("archive object failed (" + std::string(archive_error.what()) +
                                     "); direct object failed (" + direct_error.what() + ")");
        }
    }
}

std::vector<std::uint8_t> Installer::content(
    const CdnInfo& cdn, const std::unordered_map<std::string, ArchiveLocation>& archives,
    const std::string& encoding_key, const KeyRing& keyring) const {
    const auto path = cache_path(encoding_key);
    if (std::filesystem::exists(path)) return read_file(path);
    const auto decoded = BlteDecoder::decode(fetch_encoded(cdn, archives, encoding_key), keyring);
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
            auto candidate = content(plan.cdn, plan.archive_entries, encoding_key, plan.keyring);
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
    std::atomic<std::size_t> completed{0};
    std::mutex output_mutex;
    run_parallel(jobs, plan.selected_entries.size(), [&](std::size_t index) {
        const auto downloaded = install_one(plan, plan.selected_entries[index], directory);
        const auto done = completed.fetch_add(1) + 1;
        std::lock_guard lock(output_mutex);
        std::cout << "[" << done << "/" << plan.selected_entries.size() << "] "
                  << plan.selected_entries[index].path
                  << (downloaded ? "" : " (already verified)") << '\n';
    });
    if (plan.casc) {
        std::cout << "Populating CASC storage: " << plan.data_objects.size() << " objects, "
                  << format_mib(plan.data_bytes) << ".\n";
        const auto stored = install_data(plan, directory, jobs);
        write_storage_metadata(plan, directory);
        std::cout << "CASC objects stored: " << stored << '\n';
        completed += stored;
    }
    return completed.load();
}

std::size_t Installer::install_data(const InstallPlan& plan, const std::filesystem::path& directory,
                                    std::size_t jobs) const {
    CascStorage storage(directory / "Data");
    storage.open();
    if (storage.salvaged() > 0) {
        std::cout << "Recovered " << storage.salvaged()
                  << " CASC objects left unindexed by an interrupted run.\n";
    }

    struct Piece {
        const DataObject* object;
        const ArchiveLocation* location;   // null for loose CDN objects
    };
    struct Task {
        std::string archive_key;           // empty for a loose object
        std::uint64_t offset{};
        std::uint64_t size{};
        std::vector<Piece> pieces;
    };

    std::unordered_map<std::string, std::vector<Piece>> by_archive;
    std::vector<Task> tasks;
    std::uint64_t pending_bytes = 0;
    std::size_t pending_count = 0;
    for (const auto& object : plan.data_objects) {
        if (storage.contains(object.encoding_key)) continue;
        ++pending_count;
        pending_bytes += object.encoded_size;
        const auto location = plan.archive_entries.find(object.encoding_key);
        if (location == plan.archive_entries.end()) {
            tasks.push_back(Task{{}, 0, object.encoded_size, {Piece{&object, nullptr}}});
        } else {
            by_archive[location->second.archive_key].push_back(Piece{&object, &location->second});
        }
    }
    // Coalesce neighbouring archive objects into large range requests.
    for (auto& [archive_key, pieces] : by_archive) {
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
            return a.location->offset < b.location->offset;
        });
        Task current;
        for (const auto& piece : pieces) {
            const auto begin = piece.location->offset;
            const auto end = begin + piece.location->encoded_size;
            const bool fits = !current.pieces.empty() && begin >= current.offset + current.size &&
                              begin - (current.offset + current.size) <= kRangeMaxGapBytes &&
                              end - current.offset <= kRangeChunkBytes;
            if (!fits) {
                if (!current.pieces.empty()) tasks.push_back(std::move(current));
                current = Task{archive_key, begin, 0, {}};
            }
            current.size = std::max(current.size, end - current.offset);
            current.pieces.push_back(piece);
        }
        if (!current.pieces.empty()) tasks.push_back(std::move(current));
    }
    std::cout << "CASC objects to download: " << pending_count << " (" << format_mib(pending_bytes)
              << ") in " << tasks.size() << " requests; already stored: "
              << (plan.data_objects.size() - pending_count) << ".\n";
    if (tasks.empty()) {
        storage.commit();
        return 0;
    }

    std::atomic<std::size_t> stored{0};
    std::atomic<std::uint64_t> stored_bytes{0};
    std::atomic<std::uint64_t> since_commit{0};
    std::mutex output_mutex;
    std::mutex commit_mutex;
    std::mutex failure_mutex;
    std::vector<std::string> failures;   // per-object errors; the run continues and reports them at the end
    const auto record_failure = [&](const DataObject& object, const std::string& what) {
        std::lock_guard lock(failure_mutex);
        failures.push_back(object.encoding_key + " (" + object.source + "): " + what);
    };
    auto last_report = std::chrono::steady_clock::now();

    const auto store = [&](const DataObject& object, std::vector<std::uint8_t> encoded, bool allow_retry) {
        if (!verify_encoded(object.encoding_key, encoded)) {
            if (!allow_retry) throw std::runtime_error("encoded object " + object.encoding_key + " failed verification");
            encoded = catalog_.fetch_data(plan.cdn, object.encoding_key);
            if (!verify_encoded(object.encoding_key, encoded)) {
                throw std::runtime_error("encoded object " + object.encoding_key + " failed verification (archive and direct)");
            }
        }
        if (storage.append(object.encoding_key, encoded)) {
            stored.fetch_add(1);
            stored_bytes.fetch_add(encoded.size());
            if (since_commit.fetch_add(encoded.size()) + encoded.size() >= kCommitEveryBytes) {
                std::lock_guard lock(commit_mutex);
                if (since_commit.load() >= kCommitEveryBytes) {
                    storage.commit();
                    since_commit.store(0);
                }
            }
        }
        std::lock_guard lock(output_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2) || stored.load() == pending_count) {
            last_report = now;
            const auto bytes = stored_bytes.load();
            std::cout << "[casc] " << stored.load() << "/" << pending_count << " objects, " << format_mib(bytes)
                      << " / " << format_mib(pending_bytes) << " ("
                      << (pending_bytes ? bytes * 100 / pending_bytes : 100) << "%)\n";
        }
    };

    try {
        run_parallel(jobs, tasks.size(), [&](std::size_t index) {
            const auto& task = tasks[index];
            if (task.archive_key.empty()) {
                const auto& object = *task.pieces.front().object;
                try {
                    store(object, catalog_.fetch_data(plan.cdn, object.encoding_key), false);
                } catch (const std::exception& error) {
                    record_failure(object, error.what());
                }
                return;
            }
            // A transient network error on the range request must not turn
            // into a fatal 404: archive-only objects do not exist as direct
            // CDN files, so retry the range first and only then fall back.
            std::vector<std::uint8_t> block;
            std::string range_error;
            for (int attempt = 1; attempt <= kRangeAttempts && block.empty(); ++attempt) {
                try {
                    block = catalog_.fetch_archive_range(plan.cdn, task.archive_key, task.offset,
                                                         static_cast<std::uint32_t>(task.size));
                } catch (const std::exception& error) {
                    range_error = error.what();
                    block.clear();
                    if (attempt < kRangeAttempts) std::this_thread::sleep_for(std::chrono::seconds(attempt));
                }
            }
            for (const auto& piece : task.pieces) {
                const auto begin = piece.location->offset - task.offset;
                const auto end = begin + piece.location->encoded_size;
                try {
                    if (block.size() >= end) {
                        store(*piece.object, std::vector<std::uint8_t>(block.begin() + static_cast<std::ptrdiff_t>(begin),
                                                                       block.begin() + static_cast<std::ptrdiff_t>(end)), true);
                    } else {
                        try {
                            store(*piece.object, catalog_.fetch_data(plan.cdn, piece.object->encoding_key), false);
                        } catch (const std::exception& direct_error) {
                            throw std::runtime_error(std::string(direct_error.what()) +
                                                     (range_error.empty() ? "" : "; archive range: " + range_error));
                        }
                    }
                } catch (const std::exception& error) {
                    record_failure(*piece.object, error.what());
                }
            }
        });
    } catch (...) {
        try { storage.commit(); } catch (...) {}   // keep what was downloaded so the next run resumes
        throw;
    }
    storage.commit();
    if (!failures.empty()) {
        std::ostringstream message;
        message << failures.size() << " of " << pending_count << " CASC objects could not be downloaded; "
                << stored.load() << " were stored and will not be fetched again. First error: " << failures.front()
                << ". Run the same install command again to retry the missing objects.";
        throw std::runtime_error(message.str());
    }
    return stored.load();
}

void Installer::write_storage_metadata(const InstallPlan& plan, const std::filesystem::path& directory) const {
    const auto data_dir = directory / "Data";
    write_casc_config(data_dir, plan.version.build_config, plan.build_config_bytes);
    write_casc_config(data_dir, plan.version.cdn_config, plan.cdn_config_bytes);
    for (const auto& hash : plan.cdn_config.get("archives")) {
        const auto target = data_dir / "indices" / (hash + ".index");
        if (!std::filesystem::exists(target)) write_atomic(target, archive_index_bytes(plan.cdn, hash));
    }

    BuildInfoRecord record;
    record.branch = plan.version.region;
    record.build_key = plan.version.build_config;
    record.cdn_key = plan.version.cdn_config;
    if (const auto install = plan.build_config.pair("install"); install) record.install_key = install->encoding_key;
    if (const auto sizes = plan.build_config.get("install-size"); sizes.size() > 1) record.install_size = std::stoull(sizes[1]);
    record.cdn_path = plan.cdn.path;
    record.cdn_hosts = plan.cdn.hosts;
    record.cdn_servers = plan.cdn.servers;
    record.tags = build_info_tags(plan.version.region, plan.locale);
    record.version = plan.version.version_name;
    record.product = plan.version.product;
    write_atomic(directory / ".build.info", format_build_info(record));
}

std::vector<std::string> Installer::verify(const InstallPlan& plan,
                                           const std::filesystem::path& directory,
                                           const std::string& locale, bool deep) const {
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
    if (!plan.casc) return failures;

    const auto data_dir = directory / "Data";
    if (!std::filesystem::exists(directory / ".build.info")) failures.push_back(".build.info: missing");
    for (const auto& hash : {plan.version.build_config, plan.version.cdn_config}) {
        if (!std::filesystem::exists(data_dir / "config" / hash.substr(0, 2) / hash.substr(2, 2) / hash)) {
            failures.push_back("Data/config/" + hash + ": missing");
        }
    }
    if (!std::filesystem::is_directory(data_dir / "data")) {
        failures.push_back("Data/data: missing (" + std::to_string(plan.data_objects.size()) + " objects)");
        return failures;
    }
    CascStorage storage(data_dir);
    try {
        storage.open();
    } catch (const std::exception& error) {
        failures.push_back(std::string("Data/data: ") + error.what());
        return failures;
    }
    std::size_t missing = 0;
    for (const auto& object : plan.data_objects) {
        const auto entry = storage.find(object.encoding_key);
        if (!entry) {
            if (missing++ < 20) failures.push_back("casc " + object.encoding_key + " (" + object.source + "): missing");
            continue;
        }
        if (object.encoded_size != 0 && entry->size != object.encoded_size + kCascDataHeaderSize) {
            failures.push_back("casc " + object.encoding_key + " (" + object.source + "): size mismatch");
            continue;
        }
        if (deep) {
            try {
                if (!verify_encoded(object.encoding_key, storage.read(*entry))) {
                    failures.push_back("casc " + object.encoding_key + " (" + object.source + "): hash mismatch");
                }
            } catch (const std::exception& error) {
                failures.push_back("casc " + object.encoding_key + ": " + error.what());
            }
        }
    }
    if (missing > 20) failures.push_back("casc: " + std::to_string(missing - 20) + " more objects missing");
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
    // Missing objects are simply downloaded again by install_data; damaged
    // ones are dropped from the journals first so they get re-appended.
    if (plan.casc && std::filesystem::is_directory(directory / "Data" / "data")) {
        CascStorage storage(directory / "Data");
        storage.open();
        bool changed = false;
        for (const auto& failure : failures) {
            if (failure.rfind("casc ", 0) != 0) continue;
            if (failure.find("size mismatch") == std::string::npos && failure.find("hash mismatch") == std::string::npos) continue;
            const auto key = failure.substr(5, failure.find(' ', 5) - 5);
            if (is_hex_hash(key, 16) && storage.erase(key)) changed = true;
        }
        if (changed) storage.commit();
    }
    return install(repair_plan, directory, locale, jobs);
}

} // namespace openblizz
