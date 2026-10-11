#undef NDEBUG
#include "openblizz/file_safety.hpp"
#include "openblizz/casc.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace safe = openblizz::file_safety;
template<class Function> bool throws(Function&& function) {
    try { function(); } catch (const std::exception&) { return true; }
    return false;
}
int main() {
    const auto root = fs::temp_directory_path() / ("ob-safety-" + std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "game");
    fs::create_directories(root / "outside");
    const std::vector<std::uint8_t> original{'s','a','f','e'};
    safe::write_atomic(root / "outside/sentinel", original);
    for (const auto& path : {"../escape", "a/../escape", "/absolute", "C:escape", "C:\\escape", "file:stream", "CON.txt", "nul", "a.", "a ", "LPT1.log", "", ".", "a/", "a\nfile"}) {
        assert(throws([&] { (void)safe::windows_relative(path); }));
    }
    assert(safe::windows_relative("x86_64\\Warcraft III.exe").generic_string() == "x86_64/Warcraft III.exe");
    assert(safe::windows_relative("./game.bin").generic_string() == "game.bin");
    fs::create_directory_symlink(root / "outside", root / "game/link");
    assert(throws([&] { safe::write_atomic(root / "game/link/sentinel", {'x'}); }));
    assert(safe::read(root / "outside/sentinel") == original);
    assert(throws([&] { (void)safe::read(root / "game/link/sentinel"); }));
    fs::create_symlink(root / "outside/sentinel", root / "game/file-link");
    assert(throws([&] { safe::write_atomic(root / "game/file-link", {'x'}); }));
    assert(safe::read(root / "outside/sentinel") == original);
    assert(throws([&] { (void)safe::read(root / "game/file-link"); }));
    // Exclusive .part creation must not follow a pre-existing symlink.
    const auto occupied = root / "game" / ("output.part." + std::to_string(::getpid()) + ".1");
    fs::create_symlink(root / "outside/sentinel", occupied);
    assert(throws([&] { safe::write_atomic(root / "game/output", {'x'}); }));
    assert(safe::read(root / "outside/sentinel") == original);
    fs::remove(occupied);
    safe::write_atomic(root / "game/output", {'o','k'});
    assert(safe::read(root / "game/output") == std::vector<std::uint8_t>({'o','k'}));
    assert(safe::read(root / "outside/sentinel") == original);
    fs::create_directories(root / "casc/Data");
    fs::create_directory_symlink(root / "outside", root / "casc/Data/data");
    openblizz::CascStorage redirected(root / "casc/Data");
    assert(throws([&] { redirected.open(); }));
    fs::create_directories(root / "hard/Data/data");
    fs::create_hard_link(root / "outside/sentinel", root / "hard/Data/data/data.000");
    openblizz::CascStorage linked(root / "hard/Data");
    assert(throws([&] { linked.open(); }));
    assert(safe::read(root / "outside/sentinel") == original);
    fs::remove_all(root);
    std::cout << "OpenBlizz filesystem safety tests passed\n";
}
