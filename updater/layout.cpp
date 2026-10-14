// Versioned app/ layout. Adapted from the Kabus22 native updater (same author).
#include "update.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace joa::update {
namespace {
void require(bool good, const std::string& message) {
    if (!good) throw std::runtime_error(message);
}
[[noreturn]] void fail(const std::string& what, const fs::path& path) {
    throw std::runtime_error(what + ": " + path.string() + ": " + std::strerror(errno));
}
bool present(const fs::path& path) { return fs::symlink_status(path).type() != fs::file_type::not_found; }
bool realDir(const fs::path& path) { return fs::symlink_status(path).type() == fs::file_type::directory; }
void syncDir(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) fail("Cannot open directory", path);
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) fail("Cannot sync directory", path);
}
bool scratchName(const std::string& name) {
    return name.starts_with(".stage-") || name.starts_with(".tmp-") || name.starts_with(".download-");
}
// The temporary symlink lives in app/ (same filesystem); rename(2) replaces current atomically.
void swapCurrent(const fs::path& app, const std::string& version) {
    const auto temporary = app/(".tmp-current-" + std::to_string(::getpid()));
    ::unlink(temporary.c_str());
    if (::symlink(version.c_str(), temporary.c_str()) != 0) fail("Cannot create symlink", temporary);
    if (::rename(temporary.c_str(), (app/"current").c_str()) != 0) {
        const int error = errno;
        ::unlink(temporary.c_str());
        errno = error;
        fail("Cannot replace", app/"current");
    }
    syncDir(app);
}
void writeState(const fs::path& app, std::string_view active, std::string_view previous) {
    const auto state = app/".state", temporary = app/(".tmp-state-" + std::to_string(::getpid()));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "active=" << active << "\nprevious=" << previous << '\n';
        out.flush();
        require(bool(out), "Cannot write " + temporary.string());
    }
    const int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) { if (fd >= 0) ::close(fd); fail("Cannot sync", temporary); }
    ::close(fd);
    if (::rename(temporary.c_str(), state.c_str()) != 0) fail("Cannot replace", state);
    syncDir(app);
}
std::pair<std::string, std::string> readState(const fs::path& app) {
    std::ifstream in(app/".state");
    std::string line, active, previous;
    while (std::getline(in, line)) {
        if (line.starts_with("active=")) active = line.substr(7);
        else if (line.starts_with("previous=")) previous = line.substr(9);
    }
    return {valid_version(active) ? active : "", valid_version(previous) ? previous : ""};
}
bool versionDir(const fs::path& app, const std::string& version) {
    return valid_version(version) && realDir(app/version);
}
std::string runningVersion(const fs::path& app) {
    std::error_code error;
    const auto self = fs::read_symlink("/proc/self/exe", error);
    const auto base = fs::weakly_canonical(app, error);
    if (error) return {};
    const auto relative = self.lexically_relative(base);
    if (relative.empty() || *relative.begin() == "..") return {};
    const auto name = relative.begin()->string();
    return valid_version(name) ? name : "";
}
void prune(const fs::path& app, const std::vector<std::string>& keep) {
    for (const auto& entry : fs::directory_iterator(app)) {
        const auto name = entry.path().filename().string();
        if ((!scratchName(name) && !valid_version(name)) || std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
        fs::remove_all(entry.path());
    }
    syncDir(app);
}
} // namespace

fs::path app_dir() {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = data && *data && fs::path(data).is_absolute() ? fs::path(data)
        : home && *home && fs::path(home).is_absolute() ? fs::path(home)/".local/share" : fs::path();
    require(!base.empty(), "Neither XDG_DATA_HOME nor HOME is an absolute path");
    return (base/"joan-native/app").lexically_normal();
}
bool valid_version(std::string_view version) {
    static const std::regex pattern(R"((0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?)");
    return version.size() <= 64 && std::regex_match(version.begin(), version.end(), pattern);
}
std::string active_version(const fs::path& app) {
    std::error_code error;
    const auto target = fs::read_symlink(app/"current", error).string();
    return !error && versionDir(app, target) ? target : "";
}
std::string previous_version(const fs::path& app) {
    const auto active = active_version(app);
    const auto [recorded, previous] = readState(app);
    // A crash after the current swap but before .state leaves recorded == old active.
    const auto candidate = recorded == active ? previous : recorded;
    return candidate != active && versionDir(app, candidate) ? candidate : "";
}
std::vector<std::string> installed_versions(const fs::path& app) {
    std::vector<std::string> result;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(app, error))
        if (const auto name = entry.path().filename().string(); versionDir(app, name)) result.push_back(name);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return compare_versions(a, b) < 0; });
    return result;
}
void ensure_app(const fs::path& app) {
    if (!present(app)) {
        fs::create_directories(app.parent_path());
        if (::mkdir(app.c_str(), 0755) != 0 && errno != EEXIST) fail("Cannot create", app);
    }
    require(realDir(app), "Update directory is not a real directory: " + app.string());
}
void recover(const fs::path& app) {
    prune(app, {active_version(app), previous_version(app), runningVersion(app)});
}
fs::path stage_dir(const fs::path& app, std::string_view version) {
    require(valid_version(version), "Invalid version: " + std::string(version));
    auto pattern = (app/(".stage-" + std::string(version) + "-XXXXXX")).string();
    require(::mkdtemp(pattern.data()) != nullptr, "Cannot create staging directory: " + std::string(std::strerror(errno)));
    return pattern;
}
void commit(const fs::path& app, const fs::path& staged, std::string_view versionView) {
    const std::string version(versionView);
    require(valid_version(version), "Invalid version: " + version);
    require(staged.parent_path() == app && staged.filename().string().starts_with(".stage-") && realDir(staged),
        "Staged payload must come from stage_dir: " + staged.string());
    require(realDir(staged/"bin"), "Staged payload lacks bin/");
    const auto old = active_version(app);
    require(version != old, "Version " + version + " is already active");
    const auto target = app/version;
    if (present(target)) {
        require(version != runningVersion(app), "Refusing to replace the running version " + version);
        fs::remove_all(target);
    }
    if (::rename(staged.c_str(), target.c_str()) != 0) fail("Cannot publish version", target);
    syncDir(app);
    swapCurrent(app, version);
    writeState(app, version, old);
    prune(app, {version, old, runningVersion(app)});
}
void rollback(const fs::path& app) {
    const auto previous = previous_version(app), active = active_version(app);
    require(!previous.empty(), "No previous version to roll back to");
    swapCurrent(app, previous);
    writeState(app, previous, active);
}
Lock::Lock(const fs::path& app) : fd(-1) {
    const auto path = app/".lock";
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) fail("Cannot open lock", path);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); throw std::runtime_error("Another joan-native update is in progress"); }
}
Lock::~Lock() { if (fd >= 0) ::close(fd); }
}
