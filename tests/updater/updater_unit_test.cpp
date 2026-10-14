// Version ordering, release selection and signed-manifest validation (no network).
#include "update.hpp"
#include <sodium.h>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    std::puts("updater unit checks passed");
}
