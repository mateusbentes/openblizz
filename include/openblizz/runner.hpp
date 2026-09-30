#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace openblizz {

struct LaunchOptions {
    std::filesystem::path directory;
    std::filesystem::path prefix;
    std::filesystem::path executable;
    std::string proton_path;
    std::string backend{"proton"};
    std::vector<std::string> arguments;
};

class Runner {
public:
    static int launch(const LaunchOptions& options);
};

} // namespace openblizz
