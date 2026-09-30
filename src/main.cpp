#include "openblizz/auth.hpp"
#include "openblizz/catalog.hpp"
#include "openblizz/installer.hpp"
#include "openblizz/runner.hpp"

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
    std::cout << R"(OpenBlizz - independent Warcraft game client

Usage:
  openblizz products
  openblizz versions <product> [--region us]
  openblizz cdns <product> [--region us]
  openblizz plan <product> [--region us] [--locale enUS]
  openblizz install <product> --directory DIR --prefix PREFIX [--region us] [--locale enUS] [--jobs 4] [--limit N]
  openblizz update <product> --directory DIR --prefix PREFIX [--region us] [--locale enUS] [--jobs 4]
  openblizz verify <product> --directory DIR [--region us] [--locale enUS]
  openblizz repair <product> --directory DIR --prefix PREFIX [--region us] [--locale enUS] [--jobs 4]
  openblizz login --prefix PREFIX [--proton GE-Proton] [--installer Battle.net-Setup.exe]
  openblizz auth-status --prefix PREFIX
  openblizz launch --directory DIR --exe GAME.exe [--prefix PREFIX]
                   [--backend proton|umu|wine|native] [--proton GE-Proton]

The login command opens the official Battle.net UI and never asks OpenBlizz
for a password or MFA code.
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

std::filesystem::path default_cache() {
    if (const auto* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "openblizz";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache/openblizz";
    }
    return std::filesystem::current_path() / ".openblizz-cache";
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

        if (command == "login" || command == "auth-status") {
            ob::AuthOptions auth;
            auth.prefix = option(args, "--prefix");
            auth.proton_path = option(args, "--proton", "GE-Proton");
            auth.installer = option(args, "--installer");
            return command == "login" ? ob::AuthManager::login(auth) : ob::AuthManager::status(auth);
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
                ob::AuthManager::require_authenticated(auth, product);
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
