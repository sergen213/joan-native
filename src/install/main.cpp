// joa-install: command-line front end of joa_install. Contract: docs/INSTALLER.md.
#include "install/install.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

namespace {
using namespace joa::install;
using nlohmann::json;

int usage() {
    std::cerr << "usage: joa-install [--json] [--dest GAME_ROOT] <command>\n"
                 "  detect <path>                      identify a game directory or GOG installer\n"
                 "  import <path> [--use-wine] [--no-desktop]  copy original game into GAME_ROOT/current\n"
                 "  verify                             check the current install against known hashes\n"
                 "  repair <source> [--use-wine]       re-copy missing/damaged files from source\n"
                 "  uninstall                          remove imported game data and desktop entry\n"
                 "  manifest <dir> --id ID --label L   (dev) print known_versions.json for dir\n"
                 "GAME_ROOT defaults to $XDG_DATA_HOME/joan-native/game. Progress: stderr lines `progress N message`.\n";
    return 2;
}

json to_json(const VerifyReport& r) {
    return {{"ok", r.ok()}, {"version", r.version_id}, {"checked", r.checked}, {"missing", r.missing}, {"mismatched", r.mismatched}};
}

json to_json(const Detection& d) {
    json j = {{"kind", to_string(d.kind)}, {"path", d.path.string()}, {"exe_sha256", d.exe_sha256}, {"version", nullptr},
              {"missing", d.missing}, {"wrong_size", d.wrong_size}, {"extractor_available", d.extractor_available}, {"notes", d.notes}};
    if (d.version) j["version"] = {{"id", d.version->id}, {"label", d.version->label}, {"required_files", d.version->files.size()}};
    j["ok"] = (d.kind == SourceKind::InstalledDir && d.version && d.missing.empty() && d.wrong_size.empty()) ||
              (d.kind == SourceKind::GogInstaller && d.extractor_available);
    return j;
}

void print_text(const json& j) {
    for (const auto& [key, value] : j.items()) {
        if (value.is_array() && value.size() > 20) std::cout << key << ": " << value.size() << " entries, first " << value.front() << "\n";
        else std::cout << key << ": " << (value.is_string() ? value.get<std::string>() : value.dump()) << "\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    bool as_json = false, use_wine = false, desktop = true;
    std::vector<std::string> args;
    std::string dest, id, label;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](std::string& out) { if (i + 1 >= argc) return false; out = argv[++i]; return true; };
        if (a == "--json") as_json = true;
        else if (a == "--use-wine") use_wine = true;
        else if (a == "--no-desktop") desktop = false;
        else if (a == "--dest") { if (!value(dest)) return usage(); }
        else if (a == "--id") { if (!value(id)) return usage(); }
        else if (a == "--label") { if (!value(label)) return usage(); }
        else if (a == "-h" || a == "--help") return usage(), 0;
        else if (a.starts_with("--")) return usage();
        else args.push_back(a);
    }
    if (args.empty()) return usage();
    const auto& cmd = args[0];
    const bool needs_path = cmd == "detect" || cmd == "import" || cmd == "repair" || cmd == "manifest";
    if (needs_path != (args.size() == 2) || args.size() > 2) return usage();

    int last = -1;
    Options opt;
    opt.use_wine = use_wine;
    opt.desktop_entry = desktop;
    opt.progress = [&](int percent, const std::string& message) {
        if (percent == last) return;
        last = percent;
        std::cerr << "progress " << percent << " " << message << std::endl;
    };
    std::error_code ec;
    const auto launcher = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path() / "joa-launcher";
    if (!ec && std::filesystem::exists(launcher)) opt.launcher_exec = launcher;

    json out;
    int code = 0;
    try {
        auto paths = Paths::from_env();
        if (!dest.empty()) paths.game_root = std::filesystem::absolute(dest);
        if (cmd == "detect") {
            const auto d = detect(args[1]);
            out = to_json(d);
            code = out["ok"] ? 0 : 1;
        } else if (cmd == "import") {
            const auto dir = import_game(args[1], paths, opt);
            std::ifstream marker(dir / ".joa-install.json");
            out = {{"ok", true}, {"installed", dir.string()}, {"current", paths.current().string()},
                   {"version", json::parse(marker).at("version")}};
            if (desktop) out["desktop_entry"] = (paths.applications / "joan-native.desktop").string();
        } else if (cmd == "verify" || cmd == "repair") {
            const auto r = cmd == "verify" ? verify_installed(paths, opt) : repair(args[1], paths, opt);
            out = to_json(r);
            code = r.ok() ? 0 : 1;
        } else if (cmd == "uninstall") {
            uninstall(paths);
            out = {{"ok", true}, {"removed", paths.game_root.string()}};
        } else if (cmd == "manifest") {
            if (id.empty()) return usage();
            std::cout << generate_manifest(args[1], id, label.empty() ? id : label);
            return 0;
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        out = {{"ok", false}, {"error", e.what()}};
        code = 1;
    }
    if (as_json) std::cout << out.dump(2) << "\n";
    else print_text(out);
    return code;
}
