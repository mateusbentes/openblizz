#include "openblizz/auth.hpp"
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
  openblizz products
  openblizz versions <product> [--region us]
  openblizz cdns <product> [--region us]
  openblizz plan <product> [--region us] [--locale enUS]
  openblizz vfs manifests <product> [--region us]
  openblizz vfs list <product> [--region us] [--root war3.w3mod] [--manifests-only] [--summary]
  openblizz install <product> --directory DIR [--prefix PREFIX] [--token-file PATH] [--region us] [--locale enUS] [--jobs 4] [--limit N] [--force]
  openblizz update <product> --directory DIR [--prefix PREFIX] [--token-file PATH] [--region us] [--locale enUS] [--jobs 4]
  openblizz verify <product> --directory DIR [--region us] [--locale enUS]
  openblizz repair <product> --directory DIR [--prefix PREFIX] [--token-file PATH] [--region us] [--locale enUS] [--jobs 4]
  openblizz login --client-id ID --redirect-uri URI [--scope openid] [--secret-env ENV]
                  [--token-file PATH]
  openblizz agent-login --prefix PREFIX [--backend auto|umu|wine] [--proton GE-Proton]
                        [--installer Battle.Net-Setup.exe]
  openblizz auth-status --prefix PREFIX
  openblizz agent-info --prefix PREFIX [--product PRODUCT]
  openblizz account [--token-env OPENBLIZZ_OAUTH_TOKEN]
  openblizz library add <product> [--library-file PATH]
  openblizz library remove <product> [--library-file PATH]
  openblizz library list [--all] [--library-file PATH]
  openblizz library scan [--cookie-file cookies.txt | --cookie-header 'JSESSIONID=...'] [--no-cookie-jar]
                         [--cookie-jar PATH] [--account-host account.battle.net] [--dump PATH]
                         [--entitlement-url HTTPS_URL] [--token-file PATH]
  openblizz launch --directory DIR --exe GAME.exe [--prefix PREFIX]
                   [--backend proton|umu|wine|native] [--proton GE-Proton]

The login command authenticates in the browser through official Battle.net OAuth.
The agent-login command is the optional legacy Battle.net UI fallback.
The vfs commands mount the TVFS manifests of the current build (vfs-root plus
the nested vfs-N manifests) and list the virtual files they describe.
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
}

ob::InstallPlan make_plan(ob::Installer& installer, const std::string& product,
                          const std::vector<std::string>& args) {
    return installer.plan(product, option(args, "--region", "us"), option(args, "--locale", "enUS"));
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
            for (const auto& product : catalog.products()) {
                std::cout << product.id << "\t" << product.name << '\n';
            }
            return 0;
        }

        if (command == "account") {
            ob::AuthOptions auth;
            const auto token_env = option(args, "--token-env", "OPENBLIZZ_OAUTH_TOKEN");
            if (const auto* token = std::getenv(token_env.c_str()); token != nullptr) auth.oauth_token = token;
            auth.oauth_token_file = option(args, "--token-file");
            return ob::AuthManager::account(auth);
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
                ob::AuthOptions auth;
                auth.oauth_token_file = option(args, "--token-file");
                const auto token_env = option(args, "--token-env", "OPENBLIZZ_OAUTH_TOKEN");
                if (const auto* token = std::getenv(token_env.c_str()); token != nullptr) auth.oauth_token = token;
                auto endpoint = option(args, "--entitlement-url");
                if (endpoint.empty()) {
                    if (const auto* configured = std::getenv("OPENBLIZZ_ENTITLEMENT_URL"); configured != nullptr) {
                        endpoint = configured;
                    }
                }
                ob::LibraryScanOptions scan_options;
                scan_options.entitlement_url = endpoint;
                scan_options.web_session.host = option(args, "--account-host", "account.battle.net");
                scan_options.dump_path = option(args, "--dump");
                auto cookie_file = option(args, "--cookie-file");
                if (cookie_file.empty()) {
                    if (const auto* configured = std::getenv("OPENBLIZZ_ACCOUNT_COOKIE_FILE"); configured != nullptr) {
                        cookie_file = configured;
                    }
                }
                const auto default_jar = ob::LibraryManager::default_cookie_jar();
                if (cookie_file.empty() && option(args, "--cookie-header").empty() &&
                    std::filesystem::is_regular_file(default_jar)) {
                    cookie_file = default_jar.string();   // reuse the saved session automatically
                }
                if (!cookie_file.empty()) {
                    if (!std::filesystem::is_regular_file(cookie_file)) {
                        throw std::runtime_error("cookie file not found: " + cookie_file);
                    }
                    scan_options.web_session.cookie_file = cookie_file;
                    auto jar = option(args, "--cookie-jar");
                    if (jar.empty() && !has_flag(args, "--no-cookie-jar")) jar = default_jar.string();
                    if (!jar.empty() && jar != cookie_file) {
                        std::filesystem::create_directories(std::filesystem::path(jar).parent_path());
                    }
                    scan_options.web_session.cookie_jar = jar;
                } else {
                    scan_options.web_session.cookie_header = option(args, "--cookie-header");
                }
                return ob::LibraryManager::scan(catalog, auth, path, scan_options);
            }
            throw std::runtime_error("unknown library command: " + subcommand);
        }

        if (command == "agent-info") {
            ob::AuthOptions auth;
            auth.prefix = option(args, "--prefix");
            return ob::AuthManager::agent_info(auth, option(args, "--product"));
        }

        if (command == "login" || command == "oauth-login") {
            ob::AuthOptions auth;
            auth.oauth_client_id = option(args, "--client-id");
            if (auth.oauth_client_id.empty()) {
                if (const auto* value = std::getenv("OPENBLIZZ_CLIENT_ID"); value != nullptr) auth.oauth_client_id = value;
            }
            const auto secret_env = option(args, "--secret-env", "OPENBLIZZ_CLIENT_SECRET");
            if (const auto* value = std::getenv(secret_env.c_str()); value != nullptr) auth.oauth_client_secret = value;
            auth.oauth_redirect_uri = option(args, "--redirect-uri");
            if (auth.oauth_redirect_uri.empty()) {
                if (const auto* value = std::getenv("OPENBLIZZ_REDIRECT_URI"); value != nullptr) auth.oauth_redirect_uri = value;
            }
            auth.oauth_scope = option(args, "--scope", "openid");
            auth.oauth_token_file = option(args, "--token-file");
            return ob::AuthManager::oauth_login(auth);
        }

        if (command == "agent-login" || command == "auth-status") {
            ob::AuthOptions auth;
            auth.prefix = option(args, "--prefix");
            auth.proton_path = option(args, "--proton", "GE-Proton");
            auth.backend = option(args, "--backend", "auto");
            auth.installer = option(args, "--installer");
            return command == "agent-login" ? ob::AuthManager::agent_login(auth) : ob::AuthManager::status(auth);
        }

        if (command == "launch") {
            ob::LaunchOptions launch;
            launch.directory = option(args, "--directory");
            launch.prefix = option(args, "--prefix");
            launch.executable = option(args, "--exe");
            launch.proton_path = option(args, "--proton", "GE-Proton");
            launch.backend = option(args, "--backend", "proton");
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
                ob::AuthOptions auth;
                auth.prefix = option(args, "--prefix");
                auth.proton_path = option(args, "--proton", "GE-Proton");
                auth.oauth_token_file = option(args, "--token-file");
                const auto token_env = option(args, "--token-env", "OPENBLIZZ_OAUTH_TOKEN");
                if (const auto* token = std::getenv(token_env.c_str()); token != nullptr) auth.oauth_token = token;
                ob::AuthManager::require_authenticated(auth, product);
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
                    std::cout << "Warning: ownership of " << product << " is unknown (Blizzard has no public entitlement API); "
                                 "run `openblizz library scan --cookie-file cookies.txt` once with your account session.\n";
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
                const auto failures = installer.verify(plan, directory, locale);
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
