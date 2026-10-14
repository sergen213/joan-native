#include "install/install.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// known_versions.json is embedded verbatim (NUL-terminated) so the binaries need no data files.
asm(".pushsection .rodata\n"
    ".global joa_known_versions_json\n"
    "joa_known_versions_json:\n"
    ".incbin \"" JOA_KNOWN_VERSIONS_PATH "\"\n"
    ".byte 0\n"
    ".popsection\n");
extern "C" const char joa_known_versions_json[];

namespace joa::install {
namespace {
using nlohmann::json;
const char* kMarker = ".joa-install.json";

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void report(const Progress& progress, std::size_t done, std::size_t total, const std::string& message) {
    if (progress) progress(total ? static_cast<int>(done * 100 / total) : 100, message);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    for (std::size_t start = 0; start <= dirs.size();) {
        const auto end = std::min(dirs.find(':', start), dirs.size());
        const fs::path candidate = fs::path(dirs.substr(start, end - start)) / name;
        if (end > start && ::access(candidate.c_str(), X_OK) == 0) return candidate;
        start = end + 1;
    }
    return {};
}

// Runs argv without a shell; child stdout goes to our stderr so --json stdout stays clean.
void run(const std::vector<std::string>& argv, const std::vector<std::string>& extra_env = {}) {
    std::vector<std::string> env_storage(extra_env);
    for (char** e = environ; *e; ++e) env_storage.emplace_back(*e);
    std::vector<char*> args, envp;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    for (auto& e : env_storage) envp.push_back(e.data());
    args.push_back(nullptr);
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, 2, 1);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) fail("cannot start " + argv[0] + ": " + std::strerror(rc));
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fail(argv[0] + " failed (status " + std::to_string(status) + ")");
}

bool has_inno_signature(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string head(4 << 20, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return head.starts_with("MZ") && (head.find("Inno Setup") != std::string::npos || head.find("JR.Inno.Setup") != std::string::npos);
}

std::string timestamp() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

struct Lock {
    int fd = -1;
    explicit Lock(const fs::path& root) {
        fs::create_directories(root);
        fd = ::open((root / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) fail("another joa-install operation holds " + (root / ".lock").string());
    }
    ~Lock() { if (fd >= 0) ::close(fd); }
};

struct TempDir {
    fs::path path;
    ~TempDir() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); }
};

// Reflink when the filesystem allows it, otherwise a regular copy. dst must not exist.
void clone_file(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst.parent_path());
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) fail("cannot read " + src.string() + ": " + std::strerror(errno));
    const int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) { ::close(in); fail("cannot create " + dst.string() + ": " + std::strerror(errno)); }
    const bool cloned = ::ioctl(out, FICLONE, in) == 0;
    ::close(in);
    ::close(out);
    if (!cloned) fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

const KnownVersion& require_version(const Detection& d) {
    if (d.kind != SourceKind::InstalledDir) fail("not an installed game directory: " + d.path.string());
    if (!d.version) fail("unknown joan.exe (sha256 " + d.exe_sha256 + "); this release is not in known_versions.json");
    if (!d.missing.empty()) fail(std::to_string(d.missing.size()) + " required files missing in source, first: " + d.missing.front());
    if (!d.wrong_size.empty()) fail(std::to_string(d.wrong_size.size()) + " source files have unexpected size, first: " + d.wrong_size.front());
    return *d.version;
}

bool inside(const fs::path& child, const fs::path& parent) {
    const auto c = fs::weakly_canonical(child), p = fs::weakly_canonical(parent);
    auto [pe, ce] = std::mismatch(p.begin(), p.end(), c.begin(), c.end());
    return pe == p.end();
}

// Runs before anything is created, so a bad --dest never writes into the source.
void check_disjoint(const fs::path& source, const fs::path& game_root) {
    if (inside(game_root, source)) fail("destination " + game_root.string() + " lies inside the source; refusing to write to the source");
    if (inside(source, game_root)) fail("source lies inside the destination " + game_root.string());
}

// Resolved import source: an installed directory, extracting an installer into a temp dir first.
struct Source {
    TempDir temp;
    Detection detection;
};

std::unique_ptr<Source> open_source(const fs::path& input, const Paths& paths, const Options& opt, const Catalog& catalog) {
    auto src = std::make_unique<Source>();
    src->detection = detect(input, catalog);
    if (src->detection.kind != SourceKind::GogInstaller) return src;
    src->temp.path = paths.game_root / (".extract-" + std::to_string(::getpid()));
    fs::remove_all(src->temp.path);
    fs::create_directories(src->temp.path);
    const auto exe = fs::absolute(input);
    if (opt.use_wine) {
        const auto wine = find_on_path("wine");
        if (wine.empty()) fail("wine not found on PATH");
        std::string dir = "Z:" + (src->temp.path / "app").string();
        std::replace(dir.begin(), dir.end(), '/', '\\');
        if (opt.progress) opt.progress(0, "Running the original installer under Wine (private prefix)");
        run({wine.string(), exe.string(), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/NOICONS", "/DIR=" + dir},
            {"WINEPREFIX=" + (src->temp.path / "prefix").string(), "WINEDEBUG=-all", "WINEDLLOVERRIDES=mscoree,mshtml="});
    } else {
        const auto tool = find_on_path("innoextract");
        if (tool.empty()) fail("innoextract not found on PATH (install it, e.g. `pacman -S innoextract`), or opt in to --use-wine");
        if (opt.progress) opt.progress(0, "Extracting installer with innoextract");
        run({tool.string(), "--extract", "--exclude-temp", "--silent", "--output-dir", src->temp.path.string(), exe.string()});
    }
    const auto app = fs::is_directory(src->temp.path / "app") ? src->temp.path / "app" : src->temp.path;
    src->detection = detect(app, catalog);
    return src;
}

json read_marker(const fs::path& dir) {
    std::ifstream in(dir / kMarker);
    if (!in) fail("no joan-native game installation at " + dir.string() + " (run import first)");
    return json::parse(in);
}

const KnownVersion& installed_version(const Paths& paths, const Catalog& catalog) {
    const auto id = read_marker(paths.current()).at("version").get<std::string>();
    for (const auto& v : catalog) if (v.id == id) return v;
    fail("installed version " + id + " is not in known_versions.json");
}

// Removes entries this tool created under game_root (never foreign files), except `keep`.
void prune(const Paths& paths, const Catalog& catalog, const std::string& keep) {
    std::error_code ec;
    if (!fs::exists(paths.game_root)) return;
    for (const auto& e : fs::directory_iterator(paths.game_root)) {
        const auto name = e.path().filename().string();
        if (name == keep || name == "current" || name == ".lock" || e.is_symlink()) continue;
        bool ours = name.starts_with(".extract-") || fs::exists(e.path() / kMarker);
        for (const auto& v : catalog) ours = ours || (name.starts_with(v.id + "-") && name.ends_with(".staging"));
        if (ours && e.is_directory()) fs::remove_all(e.path(), ec);
    }
}
} // namespace

const char* to_string(SourceKind k) {
    switch (k) {
    case SourceKind::InstalledDir: return "installed_dir";
    case SourceKind::GogInstaller: return "gog_installer";
    case SourceKind::Unknown: break;
    }
    return "unknown";
}

Catalog parse_catalog(const std::string& text) {
    const auto doc = json::parse(text);
    Catalog out;
    for (const auto& v : doc.at("versions")) {
        KnownVersion k;
        k.id = v.at("id");
        k.label = v.value("label", k.id);
        k.exe = v.value("exe", "joan.exe");
        k.exe_sha256 = v.at("exe_sha256");
        k.copy_if_present = v.value("copy_if_present", std::vector<std::string>{});
        for (const auto& f : v.at("files")) k.files.push_back({f.at(0), f.at(1), f.at(2)});
        out.push_back(std::move(k));
    }
    return out;
}

const Catalog& known_versions() {
    static const Catalog catalog = parse_catalog(joa_known_versions_json);
    return catalog;
}

Paths Paths::from_env() {
    const auto xdg = [](const char* var, const char* fallback) {
        const char* v = std::getenv(var);
        if (v && *v == '/') return fs::path(v);
        const char* home = std::getenv("HOME");
        if (!home || !*home) fail(std::string("neither ") + var + " nor HOME is set");
        return fs::path(home) / fallback;
    };
    const auto data = xdg("XDG_DATA_HOME", ".local/share");
    return {data / "joan-native" / "game", xdg("XDG_CONFIG_HOME", ".config") / "joan-native",
            xdg("XDG_STATE_HOME", ".local/state") / "joan-native", data / "applications"};
}

std::string sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("cannot read " + file.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    unsigned char md[32];
    unsigned len = 0;
    EVP_DigestFinal_ex(ctx.get(), md, &len);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < len; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 15]; }
    return out;
}

Detection detect(const fs::path& source, const Catalog& catalog) {
    Detection d;
    d.path = source;
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        const auto exe = source / (catalog.empty() ? "joan.exe" : catalog.front().exe);
        if (!fs::is_regular_file(exe)) {
            d.notes.push_back("no joan.exe found; retail CD layouts are unsupported until a sample has been examined");
            return d;
        }
        d.kind = SourceKind::InstalledDir;
        d.exe_sha256 = sha256_file(exe);
        for (const auto& v : catalog) if (v.exe_sha256 == d.exe_sha256) d.version = &v;
        if (!d.version) { d.notes.push_back("joan.exe sha256 not in known_versions.json"); return d; }
        // ponytail: exact-case paths only; add case-insensitive lookup when a differently-cased release appears.
        for (const auto& f : d.version->files) {
            const auto size = fs::file_size(source / f.path, ec);
            if (ec) d.missing.push_back(f.path);
            else if (size != f.size) d.wrong_size.push_back(f.path);
        }
        return d;
    }
    if (fs::is_regular_file(source, ec)) {
        const auto name = lower(source.filename().string());
        if (name.ends_with(".exe") && (name.starts_with("setup_wars_and_warriors_joan_of_arc") || has_inno_signature(source))) {
            d.kind = SourceKind::GogInstaller;
            d.extractor_available = !find_on_path("innoextract").empty();
            d.notes.push_back("Inno Setup installer; game version is detected after extraction");
            if (!d.extractor_available) d.notes.push_back("innoextract not on PATH; install it or opt in to Wine-assisted import (--use-wine)");
            const auto stem = source.stem().string();
            for (const auto& e : fs::directory_iterator(source.parent_path().empty() ? "." : source.parent_path(), ec))
                if (e.path().extension() == ".bin" && e.path().filename().string().starts_with(stem)) d.notes.push_back("data part: " + e.path().filename().string());
            return d;
        }
        d.notes.push_back("not a recognised installer (expected GOG setup_wars_and_warriors_joan_of_arc_*.exe)");
        return d;
    }
    d.notes.push_back("path does not exist");
    return d;
}

VerifyReport verify_tree(const fs::path& root, const KnownVersion& version, const Progress& progress) {
    VerifyReport r;
    r.version_id = version.id;
    // ponytail: sequential hashing (source may be an HDD); parallelise per file if SSD verify time matters.
    for (const auto& f : version.files) {
        report(progress, r.checked, version.files.size(), "Verifying " + f.path);
        std::error_code ec;
        const auto p = root / f.path;
        const auto size = fs::file_size(p, ec);
        if (ec) r.missing.push_back(f.path);
        else if (size != f.size || sha256_file(p) != f.sha256) r.mismatched.push_back(f.path);
        ++r.checked;
    }
    report(progress, 1, 1, "Verified");
    return r;
}

fs::path import_game(const fs::path& source, const Paths& paths, const Options& opt) {
    const auto& catalog = opt.catalog ? *opt.catalog : known_versions();
    check_disjoint(source, paths.game_root);
    Lock lock(paths.game_root);
    auto src = open_source(source, paths, opt, catalog);
    const auto& version = require_version(src->detection);
    const auto& from = src->detection.path;

    auto name = version.id + "-" + timestamp();
    for (int n = 1; fs::exists(paths.game_root / name) || fs::exists(paths.game_root / (name + ".staging")); ++n)
        name = version.id + "-" + timestamp() + "-" + std::to_string(n);
    const auto staging = paths.game_root / (name + ".staging"), final_dir = paths.game_root / name;
    bool published = false;
    try {
        fs::create_directory(staging);
        std::size_t done = 0;
        for (const auto& f : version.files) {
            report(opt.progress, done, version.files.size(), "Copying " + f.path);
            clone_file(from / f.path, staging / f.path);
            if (fs::file_size(staging / f.path) != f.size || sha256_file(staging / f.path) != f.sha256)
                fail("checksum mismatch for " + f.path + " (source modified or damaged)");
            ++done;
            if (opt.fail_hook) opt.fail_hook(done);
        }
        for (const auto& p : version.copy_if_present) {
            if (!fs::exists(from / p)) continue;
            fs::create_directories((staging / p).parent_path());
            fs::copy(from / p, staging / p, fs::copy_options::recursive);
        }
        std::ofstream(staging / kMarker) << json{{"version", version.id}, {"label", version.label},
            {"source", fs::absolute(source).string()}, {"imported", timestamp()}}.dump(2) << "\n";
        fs::rename(staging, final_dir);
        published = true;
        const auto tmp_link = paths.game_root / (".current.tmp-" + std::to_string(::getpid()));
        fs::remove(tmp_link);
        fs::create_directory_symlink(name, tmp_link);
        fs::rename(tmp_link, paths.current()); // rename(2) atomically replaces the old symlink
    } catch (...) {
        std::error_code ec;
        fs::remove_all(staging, ec);
        if (published && fs::read_symlink(paths.current(), ec) != name) fs::remove_all(final_dir, ec);
        throw;
    }
    prune(paths, catalog, name);
    if (opt.desktop_entry) write_desktop_entry(paths, opt.launcher_exec);
    report(opt.progress, 1, 1, "Installed " + version.label);
    return final_dir;
}

VerifyReport verify_installed(const Paths& paths, const Options& opt) {
    const auto& catalog = opt.catalog ? *opt.catalog : known_versions();
    return verify_tree(paths.current(), installed_version(paths, catalog), opt.progress);
}

VerifyReport repair(const fs::path& source, const Paths& paths, const Options& opt) {
    const auto& catalog = opt.catalog ? *opt.catalog : known_versions();
    check_disjoint(source, paths.game_root);
    Lock lock(paths.game_root);
    const auto& version = installed_version(paths, catalog);
    const auto before = verify_tree(paths.current(), version, opt.progress);
    if (before.ok()) return before;
    auto src = open_source(source, paths, opt, catalog);
    if (src->detection.kind != SourceKind::InstalledDir || src->detection.version != &version)
        fail("repair source is not the installed version " + version.id);
    const auto dir = fs::canonical(paths.current());
    auto broken = before.missing;
    broken.insert(broken.end(), before.mismatched.begin(), before.mismatched.end());
    for (const auto& path : broken) {
        const auto& entry = *std::find_if(version.files.begin(), version.files.end(), [&](const FileEntry& f) { return f.path == path; });
        if (opt.progress) opt.progress(0, "Repairing " + path);
        if (sha256_file(src->detection.path / path) != entry.sha256) fail("repair source copy of " + path + " is also damaged");
        auto tmp = dir / path;
        tmp += ".joa-repair";
        fs::remove(tmp);
        clone_file(src->detection.path / path, tmp);
        fs::rename(tmp, dir / path);
    }
    return verify_tree(paths.current(), version, opt.progress);
}

void uninstall(const Paths& paths) {
    std::error_code ec;
    if (fs::exists(paths.game_root)) {
        Lock lock(paths.game_root);
        prune(paths, known_versions(), "");
        if (fs::is_symlink(paths.current())) fs::remove(paths.current());
        fs::remove(paths.game_root / ".lock", ec);
    }
    fs::remove(paths.game_root, ec); // only if empty: foreign files are never deleted
    fs::remove(paths.applications / "joan-native.desktop", ec);
}

fs::path write_desktop_entry(const Paths& paths, const fs::path& launcher_exec) {
    // Desktop Entry Spec: quote the argument, escape "`$\ inside quotes, then escape \ again for the string type.
    std::string quoted = "\"";
    for (char c : launcher_exec.empty() ? std::string("joa-launcher") : launcher_exec.string()) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += "\\\\";
        if (c == '%') quoted += '%';
        quoted += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    quoted += '"';
    fs::create_directories(paths.applications);
    const auto file = paths.applications / "joan-native.desktop";
    auto tmp = file;
    tmp += ".tmp";
    std::ofstream(tmp) << "[Desktop Entry]\nType=Application\nName=Wars and Warriors: Joan of Arc (Native)\n"
                          "Comment=Native Linux port launcher\nExec=" << quoted << "\nIcon=applications-games\n"
                          "Terminal=false\nCategories=Game;StrategyGame;\nStartupWMClass=joa-launcher\n";
    fs::rename(tmp, file);
    return file;
}

std::string generate_manifest(const fs::path& dir, const std::string& id, const std::string& label) {
    // Port/repo files, GOG installer metadata and redistributables are not game content.
    static const std::vector<std::string> skip = {".git", ".gitignore", ".github", "CMakeLists.txt", "README.md", "goal.txt",
        "src", "tests", "tools", "docs", "work", "launcher", "updater", "packaging", "cmake", "shaders", "third_party",
        "goglog.ini", "gog.ico", "support.ico", "weblinks", "util", "Launch Wars and Warriors - Joan of Arc.lnk"};
    // Mutable (written by the original game at runtime): copied, never verified. S1L1.PLN appears after
    // the first run of scenario 1 and is absent from GOG's goggame-*.hashdb.
    static const std::vector<std::string> user_files = {"CONFIG.DAT", "Save", "map/scenario1/S1L1.PLN"};
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) {
        const auto rel = fs::relative(it->path(), dir);
        const auto top = rel.begin()->string();
        if (std::find(skip.begin(), skip.end(), top) != skip.end() || std::find(user_files.begin(), user_files.end(), rel.generic_string()) != user_files.end() ||
            top.starts_with("build") || top.starts_with("unins000.") || top.starts_with("goggame-")) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file()) files.push_back(rel);
    }
    std::sort(files.begin(), files.end());
    std::string out = "{\"schema\":1,\"versions\":[\n{\"id\":" + json(id).dump() + ",\"label\":" + json(label).dump() +
        ",\"exe\":\"joan.exe\",\"exe_sha256\":\"" + sha256_file(dir / "joan.exe") + "\",\"copy_if_present\":" + json(user_files).dump() + ",\"files\":[\n";
    for (std::size_t i = 0; i < files.size(); ++i)
        out += json::array({files[i].generic_string(), fs::file_size(dir / files[i]), sha256_file(dir / files[i])}).dump() + (i + 1 < files.size() ? ",\n" : "\n");
    return out + "]}\n]}\n";
}
} // namespace joa::install
