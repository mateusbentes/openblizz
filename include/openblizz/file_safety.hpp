#pragma once

// Linux/POSIX filesystem helpers. Directory components are opened one by one
// with O_NOFOLLOW, so checks cannot be bypassed by replacing a parent symlink.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace openblizz::file_safety {
inline std::filesystem::path windows_relative(const std::string& raw) {
    auto normalized = raw;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path path(normalized);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        throw std::runtime_error("manifest contains an absolute path: " + raw);
    }
    for (const auto& part : path) {
        auto component = part.string();
        if (component == "..") throw std::runtime_error("manifest path escapes install directory: " + raw);
        if (component == ".") continue;
        if (component.empty() || component.back() == '.' || component.back() == ' ' ||
            std::any_of(component.begin(), component.end(), [](unsigned char c) {
                return c < 32 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
            })) throw std::runtime_error("unsafe Windows manifest path: " + raw);
        auto device = component.substr(0, component.find('.'));
        std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return std::tolower(c); });
        if (device == "con" || device == "prn" || device == "aux" || device == "nul" ||
            (device.size() == 4 && (device.rfind("com", 0) == 0 || device.rfind("lpt", 0) == 0) &&
             device[3] >= '1' && device[3] <= '9')) {
            throw std::runtime_error("reserved Windows manifest path: " + raw);
        }
    }
    path = path.lexically_normal();
    if (path.empty() || path == "." || normalized.back() == '/') throw std::runtime_error("empty install file path: " + raw);
    return path;
}

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = other.release();
        }
        return *this;
    }
    int get() const { return value_; }
    int release() { const auto result = value_; value_ = -1; return result; }
private:
    int value_;
};

[[noreturn]] inline void fail(const std::filesystem::path& path) {
    throw std::runtime_error("unsafe or inaccessible filesystem path: " + path.string() + ": " + std::strerror(errno));
}

inline Fd directory(const std::filesystem::path& path, bool create = false) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) fail(path);
    for (const auto& component : absolute.relative_path()) {
        if (component == "." || component.empty()) continue;
        if (component == "..") { errno = EINVAL; fail(path); }
        if (create && ::mkdirat(current.get(), component.c_str(), 0755) != 0 && errno != EEXIST) fail(path);
        const int next = ::openat(current.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) fail(path);
        ::close(current.release());
        current = Fd(next);
    }
    return current;
}

inline Fd open_file(const std::filesystem::path& path, int flags, bool create_parents = false) {
    auto parent = directory(path.parent_path(), create_parents);
    Fd result(::openat(parent.get(), path.filename().c_str(), flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644));
    if (result.get() < 0) fail(path);
    struct stat info{};
    if (::fstat(result.get(), &info) != 0) fail(path);
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; fail(path); }
    if ((flags & O_ACCMODE) != O_RDONLY && info.st_nlink != 1) { errno = EMLINK; fail(path); }
    return result;
}

inline void check_target(const std::filesystem::path& path, bool create_parents = false) {
    auto parent = directory(path.parent_path(), create_parents);
    struct stat info{};
    if (::fstatat(parent.get(), path.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        fail(path);
    }
    if (!S_ISREG(info.st_mode)) { errno = ELOOP; fail(path); }
}

inline std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    auto file = open_file(path, O_RDONLY);
    struct stat info{};
    if (::fstat(file.get(), &info) != 0 || info.st_size < 0) fail(path);
    if (static_cast<std::uint64_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) { errno = EFBIG; fail(path); }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::read(file.get(), data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { errno = EIO; fail(path); }
        offset += static_cast<std::size_t>(count);
    }
    return data;
}

inline void write_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    auto parent = directory(path.parent_path(), true);
    // Never replace a symlink/device/directory as an installation side effect.
    struct stat info{};
    if (::fstatat(parent.get(), path.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(info.st_mode)) { errno = ELOOP; fail(path); }
    } else if (errno != ENOENT) fail(path);
    static std::atomic<std::uint64_t> sequence{0};
    const auto temporary = path.filename().string() + ".part." + std::to_string(::getpid()) + "." +
        std::to_string(sequence.fetch_add(1));
    Fd file(::openat(parent.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (file.get() < 0) fail(path);
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = ::write(file.get(), data.data() + offset, data.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { errno = EIO; fail(path); }
            offset += static_cast<std::size_t>(count);
        }
        if (::close(file.release()) != 0) fail(path);
        if (::renameat(parent.get(), temporary.c_str(), parent.get(), path.filename().c_str()) != 0) fail(path);
    } catch (...) {
        ::unlinkat(parent.get(), temporary.c_str(), 0);
        throw;
    }
}
} // namespace openblizz::file_safety
