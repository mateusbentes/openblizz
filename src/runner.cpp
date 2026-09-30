#include "openblizz/runner.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace openblizz {
namespace {

std::string shell_quote(const std::string& value) {
    std::string quoted{"'"};
    for (const char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

std::string env_assignment(const char* name, const std::string& value) {
    return std::string(name) + "=" + shell_quote(value) + " ";
}

} // namespace

int Runner::launch(const LaunchOptions& options) {
    if (options.directory.empty()) throw std::runtime_error("launch requires --directory");
    if (options.executable.empty()) throw std::runtime_error("launch requires --exe");
    if (!std::filesystem::exists(options.directory)) {
        throw std::runtime_error("game directory does not exist: " + options.directory.string());
    }

    const auto executable = options.executable.is_absolute()
        ? options.executable
        : options.directory / options.executable;
    if (!std::filesystem::exists(executable)) {
        throw std::runtime_error("game executable does not exist: " + executable.string());
    }

    std::string command;
    if (options.backend == "proton" || options.backend == "umu") {
        command += env_assignment("WINEPREFIX", options.prefix.empty()
            ? (std::filesystem::current_path() / ".openblizz-prefix").string()
            : options.prefix.string());
        command += env_assignment("PROTONPATH", options.proton_path.empty() ? "GE-Proton" : options.proton_path);
        command += env_assignment("GAMEID", "umu-openblizz");
        command += "umu-run ";
    } else if (options.backend == "wine") {
        command += env_assignment("WINEPREFIX", options.prefix.empty()
            ? (std::filesystem::current_path() / ".openblizz-prefix").string()
            : options.prefix.string());
        command += "wine ";
    } else if (options.backend != "native") {
        throw std::runtime_error("unknown runner backend: " + options.backend);
    }

    command += shell_quote(executable.string());
    for (const auto& argument : options.arguments) command += " " + shell_quote(argument);
    std::cout << "Launching: " << command << '\n';
    return std::system(command.c_str());
}

} // namespace openblizz
