// joa-updater: signed GitHub-release updates for app/ (see docs/RELEASE_PROCESS.md).
#include "update.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace joa;
constexpr const char* usage =
    "Usage: joa-updater COMMAND [--channel stable|experimental] [--json]\n"
    "Commands:\n"
    "  check     Query GitHub releases of " JOA_UPDATE_REPO " for the newest version in the channel\n"
    "  apply     Verify the Ed25519-signed manifest and every artifact SHA-256, stage, then\n"
    "            atomically switch app/current to the new version (downgrades are refused)\n"
    "  rollback  Reactivate the previous installed version\n"
    "  status    Print current, previous and installed versions\n"
    "  --version, --help\n"
    "Only ${XDG_DATA_HOME:-~/.local/share}/joan-native/app/ changes; game/, saves, settings and\n"
    "asset packs never do. Progress goes to stderr as \"progress N message\"; results to stdout.\n";
struct Options { std::string command, channel = "stable"; bool json = false; };
void require(bool good, const std::string& message) {
    if (!good) throw std::runtime_error(message);
}
void progress(int percent, const std::string& message) { std::cerr << "progress " << percent << ' ' << message << std::endl; }
void emit(const Options& options, const json& result, const std::string& text) {
    if (options.json) std::cout << result.dump() << '\n';
    else std::cout << text << '\n';
}
std::string currentVersion(const fs::path& app) {
    const auto active = update::active_version(app);
    return active.empty() ? JOA_VERSION : active; // nothing installed: this binary's own version
}
const update::Release* latest(const Options& options, std::vector<update::Release>& list) {
    list = update::parse_releases(update::fetch(update::api_base() + "/repos/" JOA_UPDATE_REPO "/releases?per_page=50", 8 << 20));
    return update::select(list, options.channel);
}
const update::Asset& asset(const update::Release& release, const std::string& name) {
    for (const auto& item : release.assets) if (item.name == name) return item;
    throw std::runtime_error("Release " + release.version + " lacks asset " + name);
}
int check(const Options& options) {
    const auto current = currentVersion(update::app_dir());
    std::vector<update::Release> list;
    const auto* release = latest(options, list);
    const bool available = release && update::compare_versions(release->version, current) > 0;
    emit(options, {{"current", current}, {"latest", release ? json(release->version) : json()}, {"available", available},
                   {"notes", release ? release->notes : ""}},
         !release ? "No " + options.channel + " release found; installed " + current
                  : "Installed " + current + ", latest " + options.channel + ' ' + release->version + (available ? ": update available" : ": up to date"));
    return 0;
}
// Removes stage and downloads on failure; a committed stage was renamed away.
struct Scratch {
    fs::path stage;
    std::vector<fs::path> downloads;
    ~Scratch() {
        std::error_code error;
        for (const auto& path : downloads) fs::remove(path, error);
        if (!stage.empty()) fs::remove_all(stage, error);
    }
};
int apply(const Options& options) {
    const auto app = update::app_dir();
    update::ensure_app(app);
    update::Lock lock(app);
    progress(0, "Recovering interrupted updates");
    update::recover(app);
    const auto current = currentVersion(app);
    progress(5, "Querying " JOA_UPDATE_REPO " (" + options.channel + ")");
    std::vector<update::Release> list;
    const auto* release = latest(options, list);
    require(release, "No " + options.channel + " release found");
    const int order = update::compare_versions(release->version, current);
    require(order >= 0, "Refusing downgrade from " + current + " to " + release->version + " (use rollback)");
    if (order == 0 && !update::active_version(app).empty()) {
        emit(options, {{"applied", false}, {"from", current}, {"to", current}}, "Already up to date (" + current + ")");
        return 0;
    }
    progress(8, "Verifying signed manifest for " + release->version);
    const auto text = update::fetch(asset(*release, "manifest.json").url, 256 << 10);
    const auto signature = update::fetch(asset(*release, "manifest.json.sig").url, 4 << 10);
    const auto manifest = update::verify_manifest(text, signature, update::public_key());
    require(manifest.version == release->version, "Signed manifest version " + manifest.version + " does not match release " + release->version);
    require(options.channel == "experimental" || manifest.channel == "stable", "Signed manifest is not a stable release");
    update::require_glibc(manifest.minGlibc);
    Scratch scratch;
    scratch.stage = update::stage_dir(app, manifest.version);
    // Every artifact is downloaded and hash-checked before anything is extracted.
    int step = 0;
    for (const auto& artifact : manifest.artifacts) {
        const auto& remote = asset(*release, artifact.name);
        require(!remote.size || remote.size == artifact.size, "Release asset size differs from signed manifest: " + artifact.name);
        scratch.downloads.push_back(scratch.stage.string() + "-" + artifact.component + ".tar.gz"); // .stage-* prefix: recover() removes leftovers
        progress(10 + 60 * step++ / static_cast<int>(manifest.artifacts.size()), "Downloading " + artifact.name);
        const auto digest = update::download(remote.url, scratch.downloads.back(), artifact.size);
        require(digest == artifact.sha256, "Artifact SHA-256 mismatch: " + artifact.name);
    }
    progress(75, "Extracting");
    update::FileIndex files;
    for (const auto& path : scratch.downloads) update::extract_artifact(path, scratch.stage, files);
    struct stat info{};
    require(::lstat((scratch.stage/"bin/joa-updater").c_str(), &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0100),
        "Update lacks executable bin/joa-updater");
    json inventory = {{"format", 1}, {"version", manifest.version}, {"files", json::array()}};
    for (const auto& [path, item] : files) inventory["files"].push_back({{"path", path}, {"size", item.first}, {"sha256", item.second}});
    {
        std::ofstream out(scratch.stage/".joan-native-payload.json", std::ios::binary);
        out << inventory.dump() << '\n';
        require(bool(out.flush()), "Cannot write payload inventory");
    }
    progress(95, "Activating " + manifest.version);
    update::commit(app, scratch.stage, manifest.version);
    scratch.stage.clear();
    progress(100, "Updated " + current + " -> " + manifest.version);
    emit(options, {{"applied", true}, {"from", current}, {"to", manifest.version}}, "Updated " + current + " -> " + manifest.version);
    return 0;
}
int rollback(const Options& options) {
    const auto app = update::app_dir();
    update::ensure_app(app);
    update::Lock lock(app);
    update::recover(app);
    const auto from = update::active_version(app);
    update::rollback(app);
    const auto to = update::active_version(app);
    emit(options, {{"from", from}, {"to", to}}, "Rolled back " + from + " -> " + to);
    return 0;
}
int status(const Options& options) {
    const auto app = update::app_dir();
    const auto active = update::active_version(app), previous = update::previous_version(app);
    const auto installed = update::installed_versions(app);
    std::string text = "current: " + (active.empty() ? "none" : active) + "\nprevious: " + (previous.empty() ? "none" : previous) + "\ninstalled:";
    for (const auto& version : installed) text += ' ' + version;
    emit(options, {{"current", active.empty() ? json() : json(active)}, {"previous", previous.empty() ? json() : json(previous)},
                   {"installed", installed}, {"app_dir", app.string()}}, text);
    return 0;
}
std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { std::cout << usage; return std::nullopt; }
        if (arg == "--version") { std::cout << "joa-updater " JOA_VERSION " (" JOA_UPDATE_REPO ")\n"; return std::nullopt; }
        if (arg == "check" || arg == "apply" || arg == "rollback" || arg == "status") {
            if (!options.command.empty()) throw std::invalid_argument("Choose exactly one command");
            options.command = arg;
        } else if (arg == "--channel") {
            if (i + 1 >= argc) throw std::invalid_argument("--channel requires a value");
            options.channel = argv[++i];
        } else if (arg == "--json") options.json = true;
        else throw std::invalid_argument("Unknown argument: " + arg);
    }
    if (options.command.empty()) throw std::invalid_argument("Choose a command");
    if (options.channel != "stable" && options.channel != "experimental") throw std::invalid_argument("--channel must be stable or experimental");
    return options;
}
}

int main(int argc, char** argv) {
    std::optional<Options> options;
    try { options = parse(argc, argv); }
    catch (const std::invalid_argument& error) { std::cerr << "joa-updater: " << error.what() << '\n' << usage; return 2; }
    if (!options) return 0;
    try {
        require(::geteuid() != 0, "Run the updater as your ordinary user, not root");
        require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Cannot initialize libcurl");
        if (options->command == "check") return check(*options);
        if (options->command == "apply") return apply(*options);
        if (options->command == "rollback") return rollback(*options);
        return status(*options);
    } catch (const std::exception& error) {
        std::cerr << "joa-updater: " << error.what() << '\n';
        if (options->json) std::cout << json{{"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
