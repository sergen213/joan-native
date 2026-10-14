#pragma once
// Original-game import/verify/repair for the native port. Never writes to a source tree.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace joa::install {
namespace fs = std::filesystem;

struct FileEntry { std::string path; std::uint64_t size = 0; std::string sha256; };

// One known original release, identified by the sha256 of its executable.
struct KnownVersion {
    std::string id, label, exe = "joan.exe", exe_sha256;
    std::vector<FileEntry> files;               // required, size+sha256 verified
    std::vector<std::string> copy_if_present;   // mutable user files (config/saves), copied unverified
};
using Catalog = std::vector<KnownVersion>;
const Catalog& known_versions();               // embedded src/install/known_versions.json
Catalog parse_catalog(const std::string& json);

enum class SourceKind { InstalledDir, GogInstaller, Unknown };
const char* to_string(SourceKind);

struct Detection {
    SourceKind kind = SourceKind::Unknown;
    fs::path path;
    std::string exe_sha256;                     // installed dir only
    const KnownVersion* version = nullptr;      // null = unknown/unsupported
    std::vector<std::string> missing, wrong_size; // installed dir: cheap presence+size check
    bool extractor_available = false;           // installer: innoextract on PATH
    std::vector<std::string> notes;
};
Detection detect(const fs::path& source, const Catalog& catalog = known_versions());

// XDG layout. game_root = $XDG_DATA_HOME/joan-native/game (holds <id>-<ts>/ dirs and the `current` symlink).
struct Paths {
    fs::path game_root, config_dir, state_dir, applications;
    static Paths from_env();
    fs::path current() const { return game_root / "current"; }
    fs::path settings() const { return config_dir / "settings.ini"; }
    fs::path logs() const { return state_dir / "logs"; }
    fs::path hd_root() const { return game_root.parent_path() / "hd"; }  // engine `hd_packs` names live here
};

using Progress = std::function<void(int percent, const std::string& message)>;

struct Options {
    Progress progress;
    const Catalog* catalog = nullptr;           // null = known_versions()
    bool use_wine = false;                      // opt-in: run a GOG installer under a private Wine prefix
    bool desktop_entry = true;                  // import: write $XDG_DATA_HOME/applications/joan-native.desktop
    fs::path launcher_exec;                     // desktop entry Exec target; empty = joa-launcher via PATH
    std::function<void(std::size_t files_done)> fail_hook; // tests: throw to simulate a failure mid-import
};

struct VerifyReport {
    std::string version_id;
    std::size_t checked = 0;
    std::vector<std::string> missing, mismatched;
    bool ok() const { return missing.empty() && mismatched.empty(); }
};

std::string sha256_file(const fs::path& file);
VerifyReport verify_tree(const fs::path& root, const KnownVersion& version, const Progress& progress = {});

// Copies a verified original game into game_root/<id>-<ts> via a .staging dir, then atomically
// switches `current`. Any failure removes the staging dir and leaves `current` untouched.
fs::path import_game(const fs::path& source, const Paths& paths, const Options& options = {});
VerifyReport verify_installed(const Paths& paths, const Options& options = {});
// Re-copies missing/mismatched files of the current install from source (dir or installer).
VerifyReport repair(const fs::path& source, const Paths& paths, const Options& options = {});
// Removes game_root and the desktop entry. Settings, logs and saves are left alone.
void uninstall(const Paths& paths);
fs::path write_desktop_entry(const Paths& paths, const fs::path& launcher_exec);

// Dev helper: manifest JSON object for the original files found under dir.
std::string generate_manifest(const fs::path& dir, const std::string& id, const std::string& label);

// Copies texture/** .dds/.png files of an extracted HD pack (e.g. the community HD Update) into
// hd_root()/<name> via a staging dir, skipping files byte-identical to the current install. Never
// copies executables, CONFIG.DAT, models or int8x6/ (docs/HD_ASSETS.md). <name> must not exist yet.
struct HdImport { fs::path pack; std::size_t copied = 0, identical = 0; };
HdImport import_hd_pack(const fs::path& source, const std::string& name, const Paths& paths, const Options& options = {});
} // namespace joa::install
