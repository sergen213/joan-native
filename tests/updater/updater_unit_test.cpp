// Version ordering, release selection, signed-manifest validation and payload extraction (no network).
#include "update.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace joa::update;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static std::string b64(const unsigned char* data, std::size_t size) {
    std::string out(sodium_base64_ENCODED_LEN(size, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), data, size, sodium_base64_VARIANT_ORIGINAL);
    out.pop_back();
    return out;
}
static bool rejects(const std::string& manifest, const std::string& signature, const std::string& key, const char* reason) {
    try { verify_manifest(manifest, signature, key); }
    catch (const std::exception& error) { return std::string(error.what()).find(reason) != std::string::npos; }
    return false;
}

// tar.gz of {path, symlink target ("" = regular file)} into a fresh temp dir; true if extract_artifact accepts it.
static bool extracts(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string tmpl = (fs::temp_directory_path() / "joa-updater-unit-XXXXXX").string();
    CHECK(::mkdtemp(tmpl.data()));
    const fs::path dir(tmpl), tarball = dir / "payload.tar.gz", stage = dir / "stage";
    fs::create_directory(stage);
    archive* out = archive_write_new();
    archive_write_add_filter_gzip(out);
    archive_write_set_format_pax_restricted(out);
    CHECK(archive_write_open_filename(out, tarball.c_str()) == ARCHIVE_OK);
    for (const auto& [path, link] : entries) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, path.c_str());
        archive_entry_set_filetype(e, link.empty() ? AE_IFREG : AE_IFLNK);
        archive_entry_set_perm(e, link.empty() ? 0644 : 0777);
        if (!link.empty()) archive_entry_set_symlink(e, link.c_str());
        archive_entry_set_size(e, link.empty() ? 1 : 0);
        CHECK(archive_write_header(out, e) == ARCHIVE_OK);
        if (link.empty()) CHECK(archive_write_data(out, "x", 1) == 1);
        archive_entry_free(e);
    }
    CHECK(archive_write_close(out) == ARCHIVE_OK);
    archive_write_free(out);
    bool ok = true;
    try { FileIndex files; extract_artifact(tarball, stage, files); }
    catch (const std::exception&) { ok = false; }
    fs::remove_all(dir);
    return ok;
}

int main() {
    CHECK(sodium_init() >= 0);
    CHECK(compare_versions("1.0.0", "1.0.0") == 0);
    CHECK(compare_versions("1.0.0-beta.2", "1.0.0-beta.11") < 0);
    CHECK(compare_versions("1.0.0-rc.1", "1.0.0") < 0);
    CHECK(compare_versions("0.10.0", "0.9.9") > 0);
    CHECK(compare_versions("1.0.0+build.5", "1.0.0") == 0);
    CHECK(!valid_version("01.0.0") && !valid_version("1.0") && !valid_version("../1.0.0"));

    const auto releases = parse_releases(R"([
        {"tag_name":"v0.3.0-beta.1","prerelease":true,"body":"b","assets":[]},
        {"tag_name":"v0.2.0","prerelease":false,"body":"s","assets":[{"name":"manifest.json","browser_download_url":"https://x/m","size":5}]},
        {"tag_name":"v9.0.0","draft":true,"assets":[]},
        {"tag_name":"nightly","assets":[]}])");
    CHECK(releases.size() == 2);
    CHECK(select(releases, "stable")->version == "0.2.0" && select(releases, "stable")->assets[0].size == 5);
    CHECK(select(releases, "experimental")->version == "0.3.0-beta.1");

    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES], other[crypto_sign_PUBLICKEYBYTES], otherSk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    crypto_sign_keypair(other, otherSk);
    const auto sign = [&](const std::string& text) {
        unsigned char signature[crypto_sign_BYTES];
        crypto_sign_detached(signature, nullptr, reinterpret_cast<const unsigned char*>(text.data()), text.size(), sk);
        return b64(signature, sizeof signature);
    };
    const std::string hash(64, 'a');
    const std::string good = R"({"format":1,"product":"joan-native","version":"0.2.0","channel":"stable","min_glibc":"2.17","artifacts":[)"
        R"({"name":"joan-native-0.2.0-linux-x86_64-engine.tar.gz","component":"engine","sha256":")" + hash + R"(","size":10},)"
        R"({"name":"joan-native-0.2.0-linux-x86_64-updater.tar.gz","component":"updater","sha256":")" + hash + R"(","size":3}]})";
    const auto manifest = verify_manifest(good, sign(good), b64(pk, sizeof pk));
    CHECK(manifest.version == "0.2.0" && manifest.artifacts.size() == 2 && manifest.artifacts[1].component == "updater");
    CHECK(rejects(good, sign(good), b64(other, sizeof other), "signature is invalid"));
    auto tampered = good;
    tampered.replace(tampered.find("2.17"), 4, "2.16");
    CHECK(rejects(tampered, sign(good), b64(pk, sizeof pk), "signature is invalid"));
    CHECK(rejects(good, "not base64!", b64(pk, sizeof pk), "base64"));
    // Validly signed but semantically wrong manifests.
    const auto signedReject = [&](std::string text, const char* reason) { return rejects(text, sign(text), b64(pk, sizeof pk), reason); };
    auto wrongProduct = good; wrongProduct.replace(wrongProduct.find("joan-native\""), 11, "other-thing");
    CHECK(signedReject(wrongProduct, "not for joan-native"));
    auto wrongName = good; wrongName.replace(wrongName.find("0.2.0-linux-x86_64-engine"), 5, "0.1.0");
    CHECK(signedReject(wrongName, "Unexpected artifact name"));
    auto badComponent = good; badComponent.replace(badComponent.find("x86_64-updater"), 14, "x86_64-engine"); badComponent.replace(badComponent.find("\"updater\""), 9, "\"engine\"");
    CHECK(signedReject(badComponent, "duplicate component"));
    CHECK(signedReject(R"({"format":2})", "Unsupported manifest format"));

    // Symlink targets: shipped same-directory sonames and descending paths extract; any ".." is refused,
    // since it can cancel a component that is itself a payload symlink. The pair below normalizes
    // lexically to lib/... and bin/x, yet lib/t resolves through s (-> bin) five levels up, out of the install.
    CHECK(extracts({{"lib/libfoo.so.1.2", ""}, {"lib/libfoo.so.1", "libfoo.so.1.2"}, {"share/doc/x", "sub/y"}}));
    CHECK(!extracts({{"bin/x", ""}, {"lib/a/b/c/s", "../../../../bin"}, {"lib/t", "a/b/c/s/../../../../../bin/x"}}));
    CHECK(!extracts({{"lib/t", "a/b/c/s/../../../../../bin/x"}}));
    CHECK(!extracts({{"bin/x", ""}, {"lib/y", "../bin/x"}}));
    CHECK(!extracts({{"lib/y", "./libfoo.so"}}));
    CHECK(!extracts({{"lib/y", "/etc/passwd"}}));
    CHECK(!extracts({{"lib/y", "../../escape"}}));
    std::puts("updater unit checks passed");
}
