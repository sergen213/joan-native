// Releases, signed manifest, transport and extraction. Adapted from the Kabus22
// native updater (same author); crypto is libsodium.
#include "update.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <gnu/libc-version.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace joa::update {
namespace {
using json = nlohmann::json;
constexpr std::uint64_t maxPayload = 2ULL * 1024 * 1024 * 1024;
const std::set<std::string> components{"engine", "launcher", "updater", "shaders", "tools"};
void require(bool good, const std::string& message) {
    if (!good) throw std::runtime_error(message);
}
void sodium() { require(sodium_init() >= 0, "Cannot initialize libsodium"); }
std::string base64(std::string_view text) {
    sodium();
    require(!text.empty() && text.size() <= 4096, "Invalid base64 length");
    std::string out(text.size(), '\0');
    std::size_t length = 0;
    const char* end = nullptr;
    require(sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), text.data(), text.size(),
        " \t\r\n", &length, &end, sodium_base64_VARIANT_ORIGINAL) == 0 && end == text.data() + text.size(), "Invalid base64 data");
    out.resize(length);
    return out;
}
struct Sha256 {
    crypto_hash_sha256_state state;
    Sha256() { sodium(); crypto_hash_sha256_init(&state); }
    void update(const void* data, std::size_t size) { crypto_hash_sha256_update(&state, static_cast<const unsigned char*>(data), size); }
    std::string finish() {
        unsigned char digest[crypto_hash_sha256_BYTES];
        crypto_hash_sha256_final(&state, digest);
        char hex[crypto_hash_sha256_BYTES * 2 + 1];
        sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
        return hex;
    }
};
std::vector<std::string> identifiers(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t dot; (dot = text.find('.', start)) != std::string_view::npos; start = dot + 1) out.emplace_back(text.substr(start, dot - start));
    out.emplace_back(text.substr(start));
    return out;
}
bool numeric(const std::string& value) { return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }); }
int compareIdentifier(const std::string& a, const std::string& b) {
    const bool na = numeric(a), nb = numeric(b);
    if (na && nb) return a.size() != b.size() ? (a.size() < b.size() ? -1 : 1) : a.compare(b) < 0 ? -1 : a == b ? 0 : 1;
    if (na != nb) return na ? -1 : 1;
    return a < b ? -1 : a == b ? 0 : 1;
}
// A bundled libcurl's compiled-in CA path need not exist on the host distribution;
// use the host's trust bundle (verification itself is never relaxed).
const char* caBundle() {
    static const std::string found = [] {
        if (const char* file = std::getenv("SSL_CERT_FILE"); file && *file && fs::is_regular_file(file)) return std::string(file);
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"})
            if (fs::is_regular_file(path)) return std::string(path);
        return std::string();
    }();
    return found.empty() ? nullptr : found.c_str();
}
struct Transfer {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{curl_easy_init(), curl_easy_cleanup};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{nullptr, curl_slist_free_all};
    char error[CURL_ERROR_SIZE]{};
    explicit Transfer(const std::string& url) {
        require(bool(handle), "Cannot initialize HTTPS client");
#ifdef JOA_UPDATER_TEST_TRANSPORT
        const char* protocols = "https,http";
        require(url.starts_with("https://") || url.starts_with("http://127.0.0.1:"), "Refusing non-HTTPS URL: " + url);
#else
        const char* protocols = "https";
        require(url.starts_with("https://"), "Refusing non-HTTPS URL: " + url);
#endif
        static const std::string agent = std::string("joa-updater/") + JOA_VERSION;
        headers.reset(curl_slist_append(nullptr, "Accept: application/vnd.github+json, application/octet-stream"));
        auto* h = handle.get();
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, protocols);
        curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 512L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(h, CURLOPT_USERAGENT, agent.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error);
        if (const char* bundle = caBundle()) curl_easy_setopt(h, CURLOPT_CAINFO, bundle);
    }
    void perform(const std::string& url) {
        const auto code = curl_easy_perform(handle.get());
        require(code == CURLE_OK, "Download failed: " + url + ": " + (*error ? error : curl_easy_strerror(code)));
    }
};
struct Sink { std::string* text; std::size_t limit; };
std::size_t toString(char* data, std::size_t, std::size_t size, void* user) {
    auto* sink = static_cast<Sink*>(user);
    if (size > sink->limit - sink->text->size()) return 0;
    sink->text->append(data, size);
    return size;
}
struct FileSink { int fd; std::uint64_t written, expected; Sha256 hash; };
std::size_t toFile(char* data, std::size_t, std::size_t size, void* user) {
    auto* sink = static_cast<FileSink*>(user);
    if (size > sink->expected - sink->written) return 0;
    for (std::size_t done = 0; done < size;) {
        const auto count = ::write(sink->fd, data + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        done += static_cast<std::size_t>(count);
    }
    sink->hash.update(data, size);
    sink->written += size;
    return size;
}
std::string field(const json& object, const char* key) {
    const auto found = object.find(key);
    require(found != object.end() && found->is_string(), std::string("Missing string field: ") + key);
    return found->get<std::string>();
}
std::string optionalString(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : "";
}
bool safeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) if (part == ".." || part == "." || part.empty()) return false;
    const auto top = path.begin()->string();
    return top == "bin" || top == "lib" || top == "share";
}
const char* archiveError(archive* handle) {
    const char* text = archive_error_string(handle);
    return text ? text : "";
}
} // namespace

int compare_versions(std::string_view a, std::string_view b) {
    require(valid_version(a) && valid_version(b), "Invalid semantic version");
    const auto split = [](std::string_view v) {
        v = v.substr(0, v.find('+'));
        const auto dash = v.find('-');
        const auto core = identifiers(v.substr(0, dash));
        return std::pair{core, dash == std::string_view::npos ? std::vector<std::string>{} : identifiers(v.substr(dash + 1))};
    };
    const auto [ca, pa] = split(a);
    const auto [cb, pb] = split(b);
    for (int i = 0; i < 3; ++i) if (const int c = compareIdentifier(ca[i], cb[i])) return c;
    if (pa.empty() || pb.empty()) return pa.empty() == pb.empty() ? 0 : pa.empty() ? 1 : -1;
    for (std::size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) if (const int c = compareIdentifier(pa[i], pb[i])) return c;
    return pa.size() == pb.size() ? 0 : pa.size() < pb.size() ? -1 : 1;
}
std::vector<Release> parse_releases(std::string_view text) {
    json document;
    try { document = json::parse(text); }
    catch (const json::exception& error) { throw std::runtime_error(std::string("Invalid GitHub releases JSON: ") + error.what()); }
    require(document.is_array(), "GitHub releases response is not an array");
    std::vector<Release> result;
    for (const auto& item : document) {
        require(item.is_object(), "GitHub release entry is not an object");
        if (item.value("draft", false)) continue;
        auto tag = optionalString(item, "tag_name");
        if (tag.starts_with('v')) tag.erase(0, 1);
        if (!valid_version(tag)) continue;
        Release release{tag, optionalString(item, "body"), item.value("prerelease", false), {}};
        if (const auto assets = item.find("assets"); assets != item.end() && assets->is_array())
            for (const auto& asset : *assets) {
                require(asset.is_object(), "GitHub asset entry is not an object");
                const auto size = asset.find("size");
                release.assets.push_back({field(asset, "name"), field(asset, "browser_download_url"),
                    size != asset.end() && size->is_number_unsigned() ? size->get<std::uint64_t>() : 0});
            }
        result.push_back(std::move(release));
    }
    return result;
}
const Release* select(const std::vector<Release>& releases, std::string_view channel) {
    const Release* best = nullptr;
    for (const auto& release : releases) {
        if (channel == "stable" && release.prerelease) continue;
        if (!best || compare_versions(release.version, best->version) > 0) best = &release;
    }
    return best;
}
Manifest verify_manifest(std::string_view text, std::string_view signatureBase64, std::string_view publicKeyBase64) {
    const auto key = base64(publicKeyBase64), signature = base64(signatureBase64);
    require(key.size() == crypto_sign_PUBLICKEYBYTES, "Update public key must be 32 raw bytes");
    require(signature.size() == crypto_sign_BYTES, "Manifest signature must be 64 raw bytes");
    require(crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(signature.data()),
        reinterpret_cast<const unsigned char*>(text.data()), text.size(),
        reinterpret_cast<const unsigned char*>(key.data())) == 0, "Update manifest signature is invalid");
    // Only signed bytes are parsed from here on.
    json document;
    try { document = json::parse(text); }
    catch (const json::exception& error) { throw std::runtime_error(std::string("Invalid manifest JSON: ") + error.what()); }
    require(document.is_object() && document.value("format", json()) == json(1), "Unsupported manifest format");
    require(document.value("product", json()) == json("joan-native"), "Manifest is not for joan-native");
    Manifest manifest{field(document, "version"), field(document, "channel"), field(document, "min_glibc"), {}};
    require(valid_version(manifest.version), "Invalid manifest version");
    require(manifest.channel == "stable" || manifest.channel == "experimental", "Invalid manifest channel");
    require(std::regex_match(manifest.minGlibc, std::regex("[0-9]{1,3}\\.[0-9]{1,3}")), "Invalid manifest min_glibc");
    const auto artifacts = document.find("artifacts");
    require(artifacts != document.end() && artifacts->is_array() && !artifacts->empty() && artifacts->size() <= components.size(),
        "Manifest needs 1-5 artifacts");
    std::uint64_t total = 0;
    std::set<std::string> seen;
    for (const auto& item : *artifacts) {
        require(item.is_object(), "Invalid manifest artifact");
        Artifact artifact{field(item, "name"), field(item, "component"), field(item, "sha256"), 0};
        const auto size = item.find("size");
        require(size != item.end() && size->is_number_unsigned(), "Missing artifact size");
        artifact.size = size->get<std::uint64_t>();
        require(components.contains(artifact.component) && seen.insert(artifact.component).second, "Invalid or duplicate component: " + artifact.component);
        require(artifact.name == "joan-native-" + manifest.version + "-linux-x86_64-" + artifact.component + ".tar.gz", "Unexpected artifact name: " + artifact.name);
        require(std::regex_match(artifact.sha256, std::regex("[0-9a-f]{64}")), "Invalid artifact sha256");
        require(artifact.size > 0 && artifact.size <= maxPayload - total, "Artifact size out of bounds");
        total += artifact.size;
        manifest.artifacts.push_back(std::move(artifact));
    }
    return manifest;
}
void require_glibc(std::string_view minimum) {
    const auto parse = [](std::string_view text) {
        unsigned major = 0, minor = 0;
        std::sscanf(std::string(text).c_str(), "%u.%u", &major, &minor);
        return std::pair{major, minor};
    };
    const std::string running = gnu_get_libc_version();
    require(parse(running) >= parse(minimum), "Update requires glibc " + std::string(minimum) + " or newer; this system has " + running);
}
std::string sha256_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "Cannot read " + path.string());
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) hash.update(buffer.data(), static_cast<std::size_t>(in.gcount()));
    return hash.finish();
}
void extract_artifact(const fs::path& archivePath, const fs::path& stageInput, FileIndex& files) {
    // libarchive's symlink guard lstat()s every component; resolve ancestors first.
    const auto stage = fs::canonical(stageInput);
    std::unique_ptr<archive, decltype(&archive_read_free)> in(archive_read_new(), archive_read_free);
    std::unique_ptr<archive, decltype(&archive_write_free)> out(archive_write_disk_new(), archive_write_free);
    require(in && out, "Cannot initialize archive extraction");
    archive_read_support_filter_gzip(in.get());
    archive_read_support_format_tar(in.get());
    archive_write_disk_set_options(out.get(), ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_NO_OVERWRITE);
    require(archive_read_open_filename(in.get(), archivePath.c_str(), 1 << 16) == ARCHIVE_OK, "Cannot open artifact: " + std::string(archiveError(in.get())));
    std::uint64_t total = 0;
    for (const auto& [path, item] : files) total += item.first;
    archive_entry* entry = nullptr;
    for (int status; (status = archive_read_next_header(in.get(), &entry)) != ARCHIVE_EOF;) {
        require(status == ARCHIVE_OK, "Corrupt artifact archive: " + std::string(archiveError(in.get())));
        require(files.size() < 16384, "Payload has too many entries");
        std::string name = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
        while (name.starts_with("./")) name.erase(0, 2);
        while (name.ends_with('/')) name.pop_back();
        const fs::path relative(name);
        require(safeRelative(relative), "Unsafe payload path: " + name);
        for (unsigned char c : name) require(c >= 32 && c != 127 && c != '\\', "Unsafe payload path: " + name);
        const auto type = archive_entry_filetype(entry);
        require(type == AE_IFREG || type == AE_IFDIR || type == AE_IFLNK, "Payload entry is not a file, directory or symlink: " + name);
        require(!archive_entry_hardlink(entry), "Payload hard links are not allowed: " + name);
        require(!(archive_entry_mode(entry) & (S_ISUID | S_ISGID | S_ISVTX)), "Payload setuid/setgid/sticky bit: " + name);
        if (type == AE_IFLNK) {
            // Same-directory or descending targets only (shipped: lib/libX.so.N -> libX.so.N.M). A ".." cannot be
            // checked lexically: it may cancel a component that is itself a payload symlink (lib/a/s -> ../../bin,
            // lib/t -> a/s/../../../x lexically stays in lib/ but resolves outside the install).
            const fs::path target(archive_entry_symlink(entry) ? archive_entry_symlink(entry) : "");
            bool plain = !target.empty() && !target.is_absolute();
            for (const auto& part : target) plain = plain && part != ".." && part != "." && !part.empty();
            require(plain && safeRelative(relative.parent_path()/target), "Payload symlink escapes: " + name);
        }
        if (type != AE_IFDIR) {
            // NO_OVERWRITE silently skips existing files; overlapping artifacts are an error.
            require(!files.contains(relative.generic_string()), "Payload path appears twice: " + name);
            files[relative.generic_string()] = {0, type == AE_IFLNK ? "symlink" : ""};
        }
        if (type == AE_IFREG) {
            const auto size = archive_entry_size(entry);
            require(size >= 0 && static_cast<std::uint64_t>(size) <= maxPayload - total, "Payload exceeds size bound");
            total += static_cast<std::uint64_t>(size);
        }
        archive_entry_set_pathname(entry, (stage/relative).c_str());
        archive_entry_set_perm(entry, archive_entry_perm(entry) & 0755);
        require(archive_write_header(out.get(), entry) == ARCHIVE_OK, "Cannot extract " + name + ": " + archiveError(out.get()));
        if (type == AE_IFREG) {
            Sha256 hash;
            std::uint64_t size = 0;
            const void* block; std::size_t length; la_int64_t offset;
            for (int read; (read = archive_read_data_block(in.get(), &block, &length, &offset)) != ARCHIVE_EOF;) {
                require(read == ARCHIVE_OK && archive_write_data_block(out.get(), block, length, offset) == ARCHIVE_OK, "Cannot extract data: " + name);
                hash.update(block, length);
                size += length;
            }
            files[relative.generic_string()] = {size, hash.finish()};
        }
        require(archive_write_finish_entry(out.get()) == ARCHIVE_OK, "Cannot finish " + name);
    }
    require(archive_write_close(out.get()) == ARCHIVE_OK, "Cannot finish extraction");
}
std::string fetch(const std::string& url, std::size_t limit) {
    Transfer transfer(url);
    std::string text;
    Sink sink{&text, limit};
    curl_easy_setopt(transfer.handle.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(limit));
    curl_easy_setopt(transfer.handle.get(), CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(transfer.handle.get(), CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(transfer.handle.get(), CURLOPT_WRITEDATA, &sink);
    transfer.perform(url);
    return text;
}
std::string download(const std::string& url, const fs::path& target, std::uint64_t expected) {
    Transfer transfer(url);
    const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(fd >= 0, "Cannot create download file " + target.string() + ": " + std::strerror(errno));
    FileSink sink{fd, 0, expected, {}};
    curl_easy_setopt(transfer.handle.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(expected));
    curl_easy_setopt(transfer.handle.get(), CURLOPT_WRITEFUNCTION, toFile);
    curl_easy_setopt(transfer.handle.get(), CURLOPT_WRITEDATA, &sink);
    try { transfer.perform(url); }
    catch (...) { ::close(fd); throw; }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    require(synced, "Cannot sync download " + target.string());
    require(sink.written == expected, "Artifact size mismatch: expected " + std::to_string(expected) + ", got " + std::to_string(sink.written));
    return sink.hash.finish();
}
std::string api_base() {
#ifdef JOA_UPDATER_TEST_TRANSPORT
    if (const char* value = std::getenv("JOA_UPDATER_TEST_API"); value && *value) return value;
#endif
    return "https://api.github.com";
}
std::string public_key() {
#ifdef JOA_UPDATER_TEST_TRANSPORT
    if (const char* value = std::getenv("JOA_UPDATER_TEST_PUBKEY"); value && *value) return value;
#endif
    return JOA_UPDATE_PUBKEY;
}
}
