// Synthetic fake install (no game data): detect, import, rollback, verify/repair, installer path, settings, uninstall.
#include "install/install.hpp"
#include "install/settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>

using namespace joa::install;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}
static std::string get(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
template <class F> static bool throws(F f, const std::string& needle = {}) {
    try { f(); } catch (const std::exception& e) { return std::string(e.what()).find(needle) != std::string::npos; }
    return false;
}
// path -> content, used to prove the source is never modified.
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(root))
        out[fs::relative(e.path(), root).string()] = e.is_regular_file() ? get(e.path()) : "<dir>";
    return out;
}
static std::size_t count_entries(const fs::path& dir, const std::string& suffix) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) n += e.path().filename().string().ends_with(suffix);
    return n;
}

int main() {
    char tmpl[] = "/tmp/joa-install-test-XXXXXX";
    CHECK(::mkdtemp(tmpl));
    const fs::path tmp = tmpl;
    const auto src = tmp / "src";
    put(src / "joan.exe", "MZ fake joan");
    put(src / "Resource/A.RES", "resource bytes");
    put(src / "texture/b.dds", std::string(70000, 'x'));
    put(src / "CONFIG.DAT", "cfg");
    put(src / "Save/Mission1.SAV", "save");
    put(src / "unins000.dat", "not game content");

    Catalog catalog(1);
    auto& v = catalog[0];
    v.id = "test-v1";
    v.label = "Synthetic test version";
    v.exe_sha256 = sha256_file(src / "joan.exe");
    v.copy_if_present = {"CONFIG.DAT", "Save"};
    for (const char* p : {"joan.exe", "Resource/A.RES", "texture/b.dds"}) v.files.push_back({p, fs::file_size(src / p), sha256_file(src / p)});

    const Paths paths{tmp / "xdg/data/joan-native/game", tmp / "xdg/config/joan-native", tmp / "xdg/state/joan-native", tmp / "xdg/data/applications"};
    const auto original = snapshot(src);

    // detect
    auto d = detect(src, catalog);
    CHECK(d.kind == SourceKind::InstalledDir && d.version == &v && d.missing.empty() && d.wrong_size.empty());
    CHECK(detect(tmp / "nope", catalog).kind == SourceKind::Unknown);

    // import
    Options opt;
    opt.catalog = &catalog;
    opt.launcher_exec = tmp / "my dir/joa-launcher";
    const auto installed = import_game(src, paths, opt);
    CHECK(fs::is_symlink(paths.current()) && fs::equivalent(paths.current(), installed));
    CHECK(fs::read_symlink(paths.current()).is_relative());
    CHECK(get(paths.current() / "texture/b.dds") == get(src / "texture/b.dds"));
    CHECK(get(paths.current() / "Save/Mission1.SAV") == "save" && get(paths.current() / "CONFIG.DAT") == "cfg");
    CHECK(!fs::exists(paths.current() / "unins000.dat"));
    CHECK(count_entries(paths.game_root, ".staging") == 0);
    const auto desktop = get(paths.applications / "joan-native.desktop");
    CHECK(desktop.find("Exec=\"" + (tmp / "my dir/joa-launcher").string() + "\"\n") != std::string::npos);
    CHECK(verify_installed(paths, opt).ok() && verify_installed(paths, opt).checked == 3);

    // rollback: failure mid-copy leaves `current` and the version set untouched
    const auto target = fs::read_symlink(paths.current());
    Options failing = opt;
    failing.fail_hook = [](std::size_t done) { if (done == 2) throw std::runtime_error("injected failure"); };
    CHECK(throws([&] { import_game(src, paths, failing); }, "injected failure"));
    CHECK(fs::read_symlink(paths.current()) == target && count_entries(paths.game_root, ".staging") == 0);
    CHECK(verify_installed(paths, opt).ok());

    // a same-size but damaged source is caught by the checksum during copy and rolled back
    fs::copy(src, tmp / "damaged", fs::copy_options::recursive);
    put(tmp / "damaged/Resource/A.RES", "resource BYTES");
    CHECK(throws([&] { import_game(tmp / "damaged", paths, opt); }, "checksum mismatch"));
    CHECK(fs::read_symlink(paths.current()) == target && count_entries(paths.game_root, ".staging") == 0);

    // destination inside the source is refused before anything is written
    Paths inner = paths;
    inner.game_root = src / "inner";
    CHECK(throws([&] { import_game(src, inner, opt); }, "inside the source") && !fs::exists(src / "inner"));

    // verify detects a corrupted and a missing file; repair re-copies them from the source
    put(paths.current() / "Resource/A.RES", "corrupted!!!!!");
    fs::remove(paths.current() / "texture/b.dds");
    auto r = verify_installed(paths, opt);
    CHECK(!r.ok() && r.mismatched == std::vector<std::string>{"Resource/A.RES"} && r.missing == std::vector<std::string>{"texture/b.dds"});
    CHECK(throws([&] { repair(tmp / "damaged", paths, opt); }, "also damaged"));
    CHECK(repair(src, paths, opt).ok() && verify_installed(paths, opt).ok());

    // installer: fake innoextract on PATH extracts the fake install to <dir>/app
    const auto installer = tmp / "dl/setup_wars_and_warriors_joan_of_arc_1.0_(1).exe";
    put(installer, "MZ Inno Setup stub");
    put(tmp / "dl/setup_wars_and_warriors_joan_of_arc_1.0_(1)-1.bin", "");
    put(tmp / "bin/innoextract", "#!/bin/sh\nwhile [ $# -gt 1 ]; do [ \"$1\" = --output-dir ] && out=$2; shift; done\ncp -r '" + src.string() + "' \"$out/app\"\n");
    ::chmod((tmp / "bin/innoextract").c_str(), 0755);
    const std::string old_path = std::getenv("PATH");
    ::setenv("PATH", "/nonexistent", 1);
    d = detect(installer, catalog);
    CHECK(d.kind == SourceKind::GogInstaller && !d.extractor_available && d.notes.size() == 3);
    CHECK(throws([&] { import_game(installer, paths, opt); }, "innoextract not found"));
    ::setenv("PATH", ((tmp / "bin").string() + ":" + old_path).c_str(), 1);
    CHECK(detect(installer, catalog).extractor_available);
    const auto from_installer = import_game(installer, paths, opt);
    CHECK(fs::equivalent(paths.current(), from_installer) && verify_installed(paths, opt).ok());
    CHECK(count_entries(paths.game_root, "") == 3); // .lock, current, one version dir (old ones pruned, temp removed)

    CHECK(snapshot(src) == original);

    // settings: defaults, round trip, invalid values fall back
    Settings s;
    s.backend = "vulkan"; s.rt_gi = true; s.width = 2560; s.height = 1440; s.mode = "borderless"; s.deadzone = 0.25; s.channel = "experimental";
    s.save(paths.settings());
    const auto loaded = Settings::load(paths.settings());
    CHECK(loaded.serialize() == s.serialize());
    const auto bad = Settings::parse("[renderer]\nbackend=directx\n[display]\nwidth=800\nmode=windowed\n[controller]\ndeadzone=7\n");
    CHECK(bad.backend == "opengl" && bad.width == 0 && bad.height == 0 && bad.mode == "windowed" && bad.deadzone == 0.15);
    CHECK(Settings::load(tmp / "missing.ini").serialize() == Settings{}.serialize());
    // [graphics] image-quality keys round-trip; unset = preset default (not written); other [graphics] lines survive.
    const auto gfx = Settings::parse("[graphics]\naa=smaa\nmsaa=4\nanisotropy=16\nrender_scale=1.5\nsharpness=0.25\nhd_packs=a, b\n"
                                     "ssao=false\n[renderer]\npreset=original\n");
    CHECK(gfx.aa == "smaa" && gfx.msaa == "4" && gfx.anisotropy == "16" && gfx.render_scale == "1.5" && gfx.sharpness == "0.25");
    CHECK(gfx.graphics_extra == "hd_packs=a, b\nssao=false\n" && gfx.preset == "original");
    CHECK(Settings::parse(gfx.serialize()).serialize() == gfx.serialize());
    const auto bad_gfx = Settings::parse("[graphics]\naa=msaa\nmsaa=3\nanisotropy=0\nrender_scale=4\nsharpness=-1\n");
    CHECK(bad_gfx.aa.empty() && bad_gfx.msaa.empty() && bad_gfx.anisotropy.empty() && bad_gfx.render_scale.empty() && bad_gfx.sharpness.empty());
    CHECK(Settings{}.serialize().find("aa=") == std::string::npos);

    // uninstall removes only what we created
    put(paths.game_root / "foreign.txt", "user file");
    uninstall(paths);
    CHECK(!fs::exists(paths.current()) && fs::exists(paths.game_root / "foreign.txt") && count_entries(paths.game_root, "") == 1);
    CHECK(!fs::exists(paths.applications / "joan-native.desktop") && fs::exists(paths.settings()));

    fs::remove_all(tmp);
    std::puts("install.import: ok");
    return 0;
}
