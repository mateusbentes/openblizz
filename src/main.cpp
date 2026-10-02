#include "openblizz/browser_login.hpp"
#include "openblizz/catalog.hpp"
#include "openblizz/installer.hpp"
#include "openblizz/library.hpp"
#include "openblizz/runner.hpp"
#include "openblizz/tvfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ob = openblizz;

namespace {

void usage() {
    std::cout << R"(OpenBlizz - independent Blizzard game client

Usage:
  openblizz products [--all|--shop]    (--all: every NGDP product code; --shop: storefront highlights)
  openblizz versions <product> [--region us]
  openblizz cdns <product> [--region us]
  openblizz plan <product> [--region us] [--locale enUS] [--all-locales] [--no-data]
  openblizz vfs manifests <product> [--region us]
  openblizz vfs list <product> [--region us] [--root war3.w3mod] [--manifests-only] [--summary]
  openblizz install <product> --directory DIR [--prefix PREFIX] [--region us] [--locale enUS]
                    [--all-locales] [--no-data] [--jobs 4] [--limit N] [--data-limit BYTES] [--force]
  openblizz update <product> --directory DIR [--prefix PREFIX] [--region us] [--locale enUS] [--jobs 4]
  openblizz verify <product> --directory DIR [--region us] [--locale enUS] [--all-locales] [--deep]
  openblizz repair <product> --directory DIR [--prefix PREFIX] [--region us] [--locale enUS] [--jobs 4]
  openblizz login [--browser-exe PATH] [--timeout 600] [--keep-browser] [--cookie-jar PATH]
  openblizz logout [--cookie-jar PATH]
  openblizz library add <product> [--library-file PATH]
  openblizz library remove <product> [--library-file PATH]
  openblizz library list [--all] [--library-file PATH]
  openblizz library scan [--dump PATH] [--cookie-jar PATH]
  openblizz launch --directory DIR --exe GAME.exe [--prefix PREFIX]
                   [--backend proton|umu|wine|native] [--proton GE-Proton] [-- GAME_ARGS...]

The login command opens an isolated window of your default browser (Firefox
family via WebDriver BiDi, Chromium family via DevTools) on the official
Battle.net login page, waits until you finish (password, MFA, captcha), keeps the
session cookies with owner-only permissions, closes the window and scans your
library. Every later command (library scan, install, update, repair) reuses that
saved session and renews it automatically; logout deletes it. OpenBlizz never
sees your password and does not use the Battle.net app, Agent or OAuth API.
The vfs commands mount the TVFS manifests of the current build (vfs-root plus
the nested vfs-N manifests) and list the virtual files they describe.
For products with a TVFS root (Warcraft III: Reforged), install also fills the
local CASC storage (Data/data, Data/config, Data/indices, .build.info) with the
game data for enUS plus --locale; --all-locales keeps every language and
--no-data restores the old executables-only behaviour. Warcraft III expects
"-launch" to start without the Battle.net app: launch ... -- -launch
)";
}

std::string option(const std::vector<std::string>& args, const std::string& name,
                   const std::string& fallback = {}) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return fallback;
}

bool has_option(const std::vector<std::string>& args, const std::string& name) {
    for (const auto& arg : args) if (arg == name) return true;
    return false;
}

std::size_t jobs(const std::vector<std::string>& args) {
    const auto value = option(args, "--jobs", "4");
    return static_cast<std::size_t>(std::stoull(value));
}

bool has_flag(const std::vector<std::string>& args, const std::string& name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

std::filesystem::path default_cache() {
    if (const auto* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache/openblizz";
    }
    return std::filesystem::current_path() / ".openblizz-cache";
}

std::filesystem::path library_file(const std::vector<std::string>& args) {
    const auto configured = option(args, "--library-file");
    return configured.empty() ? ob::LibraryManager::default_file() : std::filesystem::path(configured);
}

std::string body_text(const ob::HttpResponse& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

void print_plan(const ob::InstallPlan& plan) {
    std::cout << "product: " << plan.product.id << " (" << plan.product.name << ")\n";
    std::cout << "version: " << plan.version.version_name << " (build " << plan.version.build_id << ")\n";
    std::cout << "build config: " << plan.version.build_config << '\n';
    std::cout << "cdn path: " << plan.cdn.path << "\n";
    std::cout << "manifest entries: " << plan.install_manifest.entries.size() << '\n';
    std::cout << "selected files: " << plan.selected_entries.size() << '\n';
    std::cout << "selected bytes: " << plan.total_bytes << '\n';
    std::cout << "encoding mappings: " << plan.mappings.size() << '\n';
    if (plan.casc) {
        std::cout << "casc locales:";
        for (const auto& locale : plan.selected_locales) std::cout << ' ' << locale;
        std::cout << '\n';
        std::cout << "casc virtual files: " << plan.vfs_files.size() << '\n';
        std::cout << "casc objects: " << plan.data_objects.size() << '\n';
        std::cout << "casc bytes: " << plan.data_bytes << '\n';
        if (plan.unresolved_spans != 0) std::cout << "casc unresolved spans: " << plan.unresolved_spans << '\n';
    }
}

ob::InstallPlan make_plan(ob::Installer& installer, const std::string& product,
                          const std::vector<std::string>& args) {
    ob::PlanOptions options;
    options.all_locales = has_flag(args, "--all-locales");
    options.skip_data = has_flag(args, "--no-data");
    if (const auto limit = option(args, "--data-limit"); !limit.empty()) options.data_limit = std::stoull(limit);
    return installer.plan(product, option(args, "--region", "us"), option(args, "--locale", "enUS"), options);
}

int vfs_command(ob::Installer& installer, const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("vfs requires manifests or list followed by a product id");
    const auto subcommand = args[0];
    const auto product = args[1];
    const auto context = installer.context(product, option(args, "--region", "us"));
    std::cout << "product: " << context.product.id << " (" << context.product.name << ")\n";
    std::cout << "version: " << context.version.version_name << " (build " << context.version.build_id << ")\n";
    if (subcommand == "manifests") {
        const auto refs = ob::vfs_manifest_refs(context.build_config);
        if (refs.empty()) throw std::runtime_error("product " + product + " exposes no TVFS manifests");
        std::cout << "name\tcontent_key\tencoding_key\tcontent_size\tencoded_size\n";
        for (const auto& ref : refs) {
            std::cout << ref.name << '\t' << ref.content_key << '\t' << ref.encoding_key << '\t'
                      << ref.content_size << '\t' << ref.encoded_size << '\n';
        }
        return 0;
    }
    if (subcommand != "list") throw std::runtime_error("unknown vfs command: " + subcommand);

    const auto files = installer.vfs_files(context);
    auto root = option(args, "--root");
    std::transform(root.begin(), root.end(), root.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool manifests_only = has_flag(args, "--manifests-only");
    const bool summary = has_flag(args, "--summary");
    std::size_t listed = 0;
    std::size_t nested = 0;
    std::uint64_t content_bytes = 0;
    std::uint64_t encoded_bytes = 0;
    if (!summary) std::cout << "path\tmanifest\tcontent_size\tencoded_size\tekey\tckey\n";
    for (const auto& file : files) {
        const bool inside_root = root.empty() || file.path == root || file.path.rfind(root + ":", 0) == 0;
        if (!inside_root) continue;
        if (!file.nested_manifest.empty()) ++nested;
        if (manifests_only && file.nested_manifest.empty()) continue;
        ++listed;
        if (file.nested_manifest.empty()) {
            content_bytes += file.content_size();
            encoded_bytes += file.encoded_size();
        }
        if (summary) continue;
        std::cout << file.path << '\t'
                  << (file.nested_manifest.empty() ? file.manifest : file.manifest + "->" + file.nested_manifest) << '\t'
                  << file.content_size() << '\t' << file.encoded_size() << '\t';
        for (std::size_t i = 0; i < file.spans.size(); ++i) {
            if (i != 0) std::cout << ',';
            std::cout << file.spans[i].encoding_key;
        }
        std::cout << '\t';
        for (std::size_t i = 0; i < file.spans.size(); ++i) {
            if (i != 0) std::cout << ',';
            std::cout << file.spans[i].content_key;
        }
        std::cout << '\n';
    }
    std::cout << "virtual files: " << listed << '\n';
    std::cout << "nested manifests: " << nested << '\n';
    std::cout << "content bytes: " << content_bytes << '\n';
    std::cout << "encoded bytes: " << encoded_bytes << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            usage();
            return argc < 2 ? 1 : 0;
        }
        const std::string command = argv[1];
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

        if (command == "products") {
            ob::HttpClient http;
            ob::Catalog catalog(http);
            if (has_flag(args, "--shop")) {
                // Public storefront highlights (including third-party titles sold
                // on Battle.net). Not a full catalog: the shop renders the rest
                // client-side behind a login.
                ob::CookieSession shop(std::string{});  // the storefront needs cookies across its login redirects
                const auto page = shop.get("https://us.shop.battle.net/en-us", {
                    "Accept: text/html",
                });
                for (const auto& card : ob::LibraryManager::parse_shop_cards(page.body.empty() ? std::string{} : std::string(page.body.begin(), page.body.end()))) {
                    std::cout << card.slug << "\t" << card.name << "\t" << card.franchise
                              << (card.app_game_code.empty() ? "" : "\tcode=" + card.app_game_code) << '\n';
                }
                return 0;
            }
            if (has_flag(args, "--all")) {
                // Every product code published by Ribbit; most are PTR/beta/internal.
                for (const auto& entry : catalog.summary(option(args, "--region").empty() ? "us" : option(args, "--region"))) {
                    if (!entry.flags.empty()) continue;
                    const auto title = ob::expected_title_id(entry.product);
                    std::cout << entry.product << "\tseqn=" << entry.seqn
                              << (title > 0 ? "\ttitleId=" + std::to_string(title) : std::string{}) << '\n';
                }
                return 0;
            }
            for (const auto& product : catalog.products()) {
                std::cout << product.id << "\t" << product.name << '\n';
            }
            return 0;
        }

        if (command == "library") {
            if (args.empty()) throw std::runtime_error("library requires list, add, remove, or scan");
            ob::HttpClient http;
            ob::Catalog catalog(http);
            const auto subcommand = args.front();
            const auto path = library_file(args);
            if (subcommand == "list") {
                ob::LibraryManager::auto_refresh(catalog, path, ob::LibraryManager::default_cookie_jar(), 6 * 3600);
                return ob::LibraryManager::list(catalog, path, has_flag(args, "--all"));
            }
            if (subcommand == "add" || subcommand == "remove") {
                if (args.size() < 2) throw std::runtime_error("library " + subcommand + " requires a product id");
                return subcommand == "add"
                    ? ob::LibraryManager::add(catalog, path, args[1])
                    : ob::LibraryManager::remove(catalog, path, args[1]);
            }
            if (subcommand == "scan") {
                // Uses the Battle.net session captured by `openblizz login`.
                ob::LibraryScanOptions scan_options;
                scan_options.web_session.host = option(args, "--account-host", "account.battle.net");
                scan_options.dump_path = option(args, "--dump");
                const auto jar = option(args, "--cookie-jar", ob::LibraryManager::default_cookie_jar().string());
                if (!std::filesystem::is_regular_file(jar)) {
                    throw std::runtime_error("no saved Battle.net session at " + jar + "; run `openblizz login` first");
                }
                scan_options.web_session.cookie_file = jar;
                scan_options.web_session.cookie_jar = jar;   // keep rotated cookies
                return ob::LibraryManager::scan(catalog, path, scan_options);
            }
            throw std::runtime_error("unknown library command: " + subcommand);
        }

        if (command == "login") {
            // steamcmd-like interactive login: isolated window of the default
            // browser on the official Battle.net page; password, MFA and captcha
            // happen there. OpenBlizz only keeps the resulting session cookies.
            ob::BrowserLoginOptions browser;
            browser.browser = option(args, "--browser-exe");
            browser.account_host = option(args, "--account-host", "account.battle.net");
            browser.timeout_seconds = std::stoi(option(args, "--timeout", "600"));
            browser.keep_browser_open = has_flag(args, "--keep-browser");
            if (const auto jar = option(args, "--cookie-jar"); !jar.empty()) browser.cookie_jar = jar;
            if (const auto profile = option(args, "--profile-dir"); !profile.empty()) browser.profile_dir = profile;
            const auto jar = ob::BrowserLogin::run(browser);
            ob::HttpClient http;
            ob::Catalog catalog(http);
            ob::LibraryScanOptions scan_options;
            scan_options.web_session.cookie_file = jar;
            scan_options.web_session.cookie_jar = jar;
            scan_options.web_session.host = browser.account_host;
            std::cout << "Scanning your Battle.net library.\n";
            return ob::LibraryManager::scan(catalog, library_file(args), scan_options);
        }

        if (command == "logout") {
            const auto jar = option(args, "--cookie-jar", ob::LibraryManager::default_cookie_jar().string());
            std::error_code ec;
            const bool removed = std::filesystem::remove(jar, ec);
            std::cout << (removed ? "Saved Battle.net session removed.\n" : "No saved session found.\n");
            return 0;
        }

        if (command == "launch") {
            ob::LaunchOptions launch;
            launch.directory = option(args, "--directory");
            launch.prefix = option(args, "--prefix");
            launch.executable = option(args, "--exe");
            launch.proton_path = option(args, "--proton", "GE-Proton");
            launch.backend = option(args, "--backend", "proton");
            if (const auto separator = std::find(args.begin(), args.end(), "--"); separator != args.end()) {
                launch.arguments.assign(separator + 1, args.end());
            }
            return ob::Runner::launch(launch);
        }

        ob::HttpClient http;
        ob::Catalog catalog(http);
        if (command == "vfs") {
            ob::Installer installer(catalog, default_cache());
            return vfs_command(installer, args);
        }
        if (command == "versions" || command == "cdns") {
            if (args.empty()) throw std::runtime_error(command + " requires a product id");
            const auto product = args.front();
            const auto region = option(args, "--region", "us");
            if (command == "versions") {
                const auto info = catalog.version(product, region);
                std::cout << "product=" << info.product << '\n'
                          << "region=" << info.region << '\n'
                          << "build_config=" << info.build_config << '\n'
                          << "cdn_config=" << info.cdn_config << '\n'
                          << "build_id=" << info.build_id << '\n'
                          << "version=" << info.version_name << '\n';
            } else {
                for (const auto& cdn : catalog.cdns(product, region)) {
                    std::cout << cdn.path << "\t";
                    for (const auto& host : cdn.hosts) std::cout << host << ' ';
                    std::cout << '\n';
                }
            }
            return 0;
        }

        if (args.empty()) throw std::runtime_error(command + " requires a product id");
        const auto product = args.front();
        ob::Installer installer(catalog, default_cache());
        if (command == "plan") {
            print_plan(make_plan(installer, product, args));
            return 0;
        }

        if (command == "install" || command == "update" || command == "verify" || command == "repair") {
            const auto directory = option(args, "--directory");
            if (directory.empty()) throw std::runtime_error(command + " requires --directory");
            if (command == "install" || command == "update" || command == "repair") {
                const auto jar = option(args, "--cookie-jar", ob::LibraryManager::default_cookie_jar().string());
                if (!ob::BrowserLogin::session_authenticated(jar, "account.battle.net")) {
                    throw std::runtime_error("no valid Battle.net account session; run `openblizz login` first");
                }
                std::cout << "Authentication: Battle.net account session verified\n";
                const auto library_path = library_file(args);
                ob::LibraryManager::auto_refresh(catalog, library_path, ob::LibraryManager::default_cookie_jar(), 6 * 3600);
                const auto ownership = ob::LibraryManager::ownership_of(library_path, product);
                if (ownership == ob::OwnershipState::NotOwned && !has_flag(args, "--force")) {
                    throw std::runtime_error("your Battle.net account does not own " + product +
                                             " (library state: not_owned); use --force to override");
                }
                if (ownership == ob::OwnershipState::Owned) {
                    std::cout << "Ownership: " << product << " is in your Battle.net account library.\n";
                } else if (ownership == ob::OwnershipState::Manual) {
                    std::cout << "Ownership: " << product << " was added manually to the library.\n";
                } else {
                    std::cout << "Warning: ownership of " << product << " is unknown; run `openblizz library scan` "
                                 "(or `openblizz login` again) to refresh your account library.\n";
                }
            }
            auto plan = make_plan(installer, product, args);
            if (const auto limit = option(args, "--limit"); !limit.empty()) {
                const auto count = std::min<std::size_t>(std::stoull(limit), plan.selected_entries.size());
                plan.selected_entries.resize(count);
                plan.total_bytes = 0;
                for (const auto& entry : plan.selected_entries) plan.total_bytes += entry.file_size;
            }
            print_plan(plan);
            const auto locale = option(args, "--locale", "enUS");
            if (command == "install" || command == "update") {
                std::cout << "Starting content download.\n";
                const auto completed = installer.install(plan, directory, locale, jobs(args));
                std::cout << "Files processed: " << completed << '\n';
            } else if (command == "verify") {
                const auto failures = installer.verify(plan, directory, locale, has_flag(args, "--deep"));
                for (const auto& failure : failures) std::cout << failure << '\n';
                return failures.empty() ? 0 : 2;
            } else {
                const auto repaired = installer.repair(plan, directory, locale, jobs(args));
                std::cout << "Files repaired: " << repaired << '\n';
            }
            return 0;
        }

        usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "OpenBlizz error: " << error.what() << '\n';
        return 1;
    }
}
