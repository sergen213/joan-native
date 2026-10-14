// Asset-backed: the original install at JOA_GAME_DIR is a known version with every required file (read-only).
#include "install/install.hpp"

#include <cstdio>
#include <cstdlib>

int main() {
    const char* dir = std::getenv("JOA_GAME_DIR");
    if (!dir || !std::filesystem::exists(std::filesystem::path(dir) / "joan.exe")) return 77;
    const auto d = joa::install::detect(dir);
    std::printf("kind=%s sha256=%s version=%s missing=%zu wrong_size=%zu\n", joa::install::to_string(d.kind), d.exe_sha256.c_str(),
                d.version ? d.version->id.c_str() : "unknown", d.missing.size(), d.wrong_size.size());
    if (d.kind != joa::install::SourceKind::InstalledDir || !d.version || !d.missing.empty() || !d.wrong_size.empty()) return 1;
    return d.version->files.size() > 9000 ? 0 : 1;
}
