#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Signed GitHub-release updates for the native Joan of Arc port.
// Network I/O is HTTPS-only with TLS verification. The test-only build
// (JOA_UPDATER_TEST_TRANSPORT, never installed) also admits http://127.0.0.1 and
// env-supplied API base/public key.
namespace joa::update {
namespace fs = std::filesystem;

// ---- install layout: <data root>/app/{<version>/, current -> <version>, .state, .lock} ----
// Only entries inside app/ are ever created, replaced or deleted; game/, saves,
// settings, HD packs and every other data-root entry are never touched.
// ${XDG_DATA_HOME:-~/.local/share}/joan-native/app (XDG_DATA_HOME only if absolute).
fs::path app_dir();
// "MAJOR.MINOR.PATCH[-pre][+build]", at most 64 characters.
bool valid_version(std::string_view version);
// Semantic-version precedence: <0, 0, >0. Throws on invalid versions.
int compare_versions(std::string_view a, std::string_view b);
// Target of app/current; empty when nothing is installed.
std::string active_version(const fs::path& app);
// Rollback target; empty when none is installed.
std::string previous_version(const fs::path& app);
std::vector<std::string> installed_versions(const fs::path& app);
// Creates app/ (and parents) if missing; throws unless app is a real directory.
void ensure_app(const fs::path& app);
// Interrupted-update recovery (call under Lock): deletes leftover stage/download/temp
// entries and versions other than active, previous and the running one.
void recover(const fs::path& app);
// New private (0700) directory inside app/ to fill with bin/lib/share.
fs::path stage_dir(const fs::path& app, std::string_view version);
// Publishes staged as app/<version>, swaps current atomically (commit point),
// records .state (previous = old active) and prunes older versions.
void commit(const fs::path& app, const fs::path& staged, std::string_view version);
// Swaps current back to the previous version; active becomes the new previous.
void rollback(const fs::path& app);
struct Lock {
    explicit Lock(const fs::path& app); // throws if another process holds it
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    int fd;
};

// ---- releases, manifest, transport ----
struct Asset { std::string name, url; std::uint64_t size = 0; };
struct Release { std::string version, notes; bool prerelease = false; std::vector<Asset> assets; };
// GitHub /releases JSON; drafts and tags that are not "[v]SEMVER" are skipped.
std::vector<Release> parse_releases(std::string_view json);
// Highest release in channel (stable excludes prereleases); null if none.
const Release* select(const std::vector<Release>& releases, std::string_view channel);
struct Artifact { std::string name, component, sha256; std::uint64_t size = 0; };
struct Manifest { std::string version, channel, minGlibc; std::vector<Artifact> artifacts; };
// Verifies the detached Ed25519 signature over the exact bytes BEFORE parsing;
// throws on any defect.
Manifest verify_manifest(std::string_view json, std::string_view signatureBase64, std::string_view publicKeyBase64);
// Throws unless the running glibc is at least minimum ("MAJOR.MINOR").
void require_glibc(std::string_view minimum);
// Extracts a bin/lib/share tar.gz into stage, refusing absolute/../escaping paths and
// links, hard links, devices, setuid/setgid/sticky bits and paths already extracted
// by another artifact (recorded in files: path -> {size, sha256}).
using FileIndex = std::map<std::string, std::pair<std::uint64_t, std::string>>;
void extract_artifact(const fs::path& archive, const fs::path& stage, FileIndex& files);
std::string sha256_file(const fs::path& path);
// HTTPS GET into memory, bounded by limit bytes.
std::string fetch(const std::string& url, std::size_t limit);
// HTTPS GET into a new file; size must equal expected; returns lowercase hex SHA-256.
std::string download(const std::string& url, const fs::path& target, std::uint64_t expected);
// "https://api.github.com" (test build: $JOA_UPDATER_TEST_API).
std::string api_base();
// Build-time key from packaging/release/update_pubkey.txt (test build: $JOA_UPDATER_TEST_PUBKEY).
std::string public_key();
}
